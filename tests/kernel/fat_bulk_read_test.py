"""Production FAT reader and cache: fragmented extents and dirty coherence."""
from pathlib import Path
import subprocess
import tempfile
import resource
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/nativefs/file_lifecycle.c').read_text()
begin = source.index('static UINT64 fat32_contiguous_extent_bytes(')
end = source.index('static BOOLEAN fat32_write_locked(', begin)
code = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <kernel/fat32.h>
static unsigned reads, max_count;
static BOOLEAN fail_read;
static BOOLEAN contiguous;
static unsigned block_size=512;
static int fail_at=-1;
static BOOLEAN disk_read(VOID *ctx, UINT64 lba, UINT32 count, VOID *buffer) {
    (void)ctx; ++reads; if (count > max_count) max_count = count;
    if (fail_read || (int)(reads-1)==fail_at) return 0;
    for (unsigned i=0;i<count;++i)
        memset((UINT8*)buffer+block_size*i,(int)((lba+i)&255),block_size);
    return 1;
}
static BOOLEAN disk_write(VOID *ctx, UINT64 lba, UINT32 count, const VOID *buffer) {
    (void)ctx;(void)lba;(void)count;(void)buffer;return 1;
}
static VOID fat32_diag_failure(const char *tag, UINT64 a, UINT64 b, UINT64 c) {
    (void)tag;(void)a;(void)b;(void)c;
}
static UINT64 fat32_cluster_lba(const OS_FAT32 *fs, UINT32 cluster) {
    return fs->DataStartLba+(UINT64)(cluster-2)*fs->SectorsPerCluster;
}
static BOOLEAN locate_file_cluster(OS_FAT32_FILE *file, UINT32 logical,
                                   UINT32 *physical) {
    if(contiguous){
        file->CursorValid=1;file->CursorLogicalStart=0;
        file->CursorPhysicalStart=2;file->CursorLength=64;
        *physical=2+logical;return 1;
    }
    file->CursorValid=1;file->CursorLogicalStart=(logical/4)*4;
    file->CursorPhysicalStart=2+(logical/4)*8;file->CursorLength=4;
    *physical=file->CursorPhysicalStart+logical%4;return 1;
}
static UINT32 fat32_extent_length(const OS_FAT32 *fs,UINT32 cluster,UINT32 maximum) {
    (void)fs;(void)cluster;(void)maximum;return 1U;
}
static BOOLEAN fat32_read_sector(const OS_FAT32 *fs, UINT64 lba, UINT8 *buffer) {
    return block_cache_read((OS_BLOCK_CACHE*)&fs->Cache,lba,buffer);
}
''' + source[begin:end] + r'''
int main(void) {
    OS_BLOCK_DEVICE disk={.Registered=1,.BlockSize=512,.BlockCount=4096,
                             .Read=disk_read,.Write=disk_write};
    OS_FAT32 fs={.Device=&disk,.BytesPerSector=512,.SectorsPerCluster=32,
                    .DataStartLba=64};
    assert(block_cache_init(&fs.Cache,&disk));
    UINT8 *out=malloc(OS_BLOCK_CACHE_MAX_READ_BYTES+2);assert(out);
    UINT8 dirty[512];memset(dirty,0xa5,sizeof(dirty));
    assert(block_cache_write(&fs.Cache,100,dirty));
    reads=0;
    assert(block_cache_read_range(&fs.Cache,64,128,out));
    assert(reads==2 && out[(100-64)*512]==0xa5);
    for(unsigned i=0;i<128;++i)
        assert(out[i*512]==(i==36?0xa5:((64+i)&255)));
    assert(!block_cache_read_range(&fs.Cache,64,OS_BLOCK_CACHE_MAX_READ_BYTES/512+1,out));
    assert(!block_cache_read_range(&fs.Cache,4095,2,out));
    assert(!block_cache_read_range(&fs.Cache,UINT64_MAX,2,out));
    assert(!block_cache_read_range(&fs.Cache,64,0,out));
    fail_read=1;assert(!block_cache_read_range(&fs.Cache,200,2,out));
    fail_read=0;
    /* A failed backend read must release the lock and preserve dirty data. */
    assert(block_cache_read_range(&fs.Cache,100,1,out) && out[0]==0xa5);
    assert(block_cache_flush(&fs.Cache));
    assert(block_cache_destroy(&fs.Cache));
    assert(block_cache_init(&fs.Cache,&disk));
    OS_FAT32_FILE file={.FileSystem=&fs,.Used=1,.Size=19*16384+99};
    const unsigned capacities[]={1,511,512,513,4097,65536};
    for(unsigned offset=0;offset<1024;offset+=31)
        for(unsigned i=0;i<sizeof(capacities)/sizeof(capacities[0]);++i) {
            unsigned cap=capacities[i];UINT32 got=0;
            memset(out,0xcc,128*512+2);
            assert(fat32_read(&file,offset,out+1,cap,&got) && got==cap);
            assert(out[0]==0xcc && out[cap+1]==0xcc);
            for(unsigned j=0;j<got;++j) {
                UINT64 pos=offset+j;unsigned logical=pos/16384;
                UINT64 lba=64+(logical/4)*8*32+(logical%4)*32+(pos%16384)/512;
                assert(out[j+1]==(lba&255));
            }
        }
    UINT32 got;
    assert(fat32_read(&file,file.Size-31,out,512,&got) && got==31);
    assert(fat32_read(&file,file.Size,out,512,&got) && got==0);
    fail_read=1;
    assert(!fat32_read(&file,8*16384,out,65536,&got));
    fail_read=0;
    assert(fat32_read(&file,8*16384,out,65536,&got) && got==65536);
    assert(max_count==128);
    /* 一整段连续 1 MiB 必须交给设备一次，而不是仍在 FAT 中切成 16 个 64 KiB 请求。 */
    assert(block_cache_destroy(&fs.Cache));assert(block_cache_init(&fs.Cache,&disk));
    contiguous=1;file.CursorValid=0;file.Size=OS_BLOCK_CACHE_MAX_READ_BYTES;
    reads=max_count=0;memset(out,0xcc,OS_BLOCK_CACHE_MAX_READ_BYTES+2);
    assert(fat32_read(&file,0,out+1,OS_BLOCK_CACHE_MAX_READ_BYTES,&got));
    assert(got==OS_BLOCK_CACHE_MAX_READ_BYTES&&reads==1&&max_count==2048);
    assert(out[0]==0xcc&&out[got+1]==0xcc);
    for(unsigned i=0;i<got;++i)assert(out[i+1]==((64+i/512)&255));
    assert(block_cache_destroy(&fs.Cache));
    /* 与逐扇区独立对照：命中顺序、重复项、脏数据和边界都不能随批次扩大发生变化。 */
    unsigned cases=0;
    for(block_size=512;block_size<=4096;block_size*=2){
        disk.BlockSize=block_size;
        unsigned maximum=OS_BLOCK_CACHE_MAX_READ_BYTES/block_size;
        const unsigned counts[]={1,15,16,17,127,128,129,maximum-1,maximum};
        for(unsigned layout=0;layout<5;++layout)
        for(unsigned c=0;c<sizeof(counts)/sizeof(*counts);++c){
            unsigned count=counts[c];assert(block_cache_init(&fs.Cache,&disk));
            for(unsigned i=0;i<OS_BLOCK_CACHE_ENTRY_COUNT;++i){
                OS_BLOCK_CACHE_ENTRY *entry=&fs.Cache.Entries[i];
                entry->Valid=layout!=0;entry->Dirty=i%2;
                /* 逆序命中、相邻命中、重复 LBA 和完全不相交。 */
                entry->Lba=layout==1?64+(15-i)*count/16:
                    layout==2?64+i:layout==3?64+(i%4)*count/4:4090+i;
                entry->LastUse=i+1;memset(entry->Data,0xa0+i,block_size);
            }
            memset(out,0xcc,OS_BLOCK_CACHE_MAX_READ_BYTES+2);reads=max_count=0;
            assert(block_cache_read_range(&fs.Cache,64,count,out+1));
            unsigned expected_requests=0;bool previous_miss=false;
            for(unsigned sector=0;sector<count;++sector){
                int slot=-1;
                for(unsigned i=0;i<OS_BLOCK_CACHE_ENTRY_COUNT;++i)
                    if(fs.Cache.Entries[i].Valid&&fs.Cache.Entries[i].Lba==64+sector){slot=(int)i;break;}
                bool miss=slot<0;
                if(miss&&!previous_miss)++expected_requests;
                previous_miss=miss;
                unsigned char expected=miss?(unsigned char)(64+sector):(unsigned char)(0xa0+slot);
                for(unsigned byte=0;byte<block_size;++byte)
                    assert(out[1+(size_t)sector*block_size+byte]==expected);
            }
            assert(reads==expected_requests&&out[0]==0xcc&&out[1+count*block_size]==0xcc);
            assert(!block_cache_read_range(&fs.Cache,64,maximum+1,out));
            if(expected_requests>1){
                reads=0;fail_at=1;assert(!block_cache_read_range(&fs.Cache,64,count,out+1));
                fail_at=-1;assert(block_cache_read_range(&fs.Cache,64,count,out+1));
            }
            /* 不回写这些人工构造的脏项，仅确认读取未改其有效位、脏位与数据。 */
            for(unsigned i=0;i<OS_BLOCK_CACHE_ENTRY_COUNT;++i){
                OS_BLOCK_CACHE_ENTRY *entry=&fs.Cache.Entries[i];
                assert(entry->Valid==(layout!=0)&&entry->Dirty==(BOOLEAN)(i%2)&&entry->Data[0]==0xa0+i);
                entry->Dirty=0;
            }
            assert(block_cache_destroy(&fs.Cache));++cases;
        }
    }
    printf("Cache range: %u block-size/layout/length oracles, dirty/duplicate precedence, exact gap requests and intermediate failure recovery PASS\n",cases);
    free(out);
    puts("FAT bulk reads: dirty coherence, fragmentation, unaligned tails, EOF and failure passed; contiguous 1MiB device requests 16 -> 1");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fat-bulk-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),'-x','c','-',
                    str(root/'kernel/block/bio.c'),
                    str(root/'kernel/fs/pagecache/cache.c'),'-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True)
    # 恢复旧的 FAT 限制时，字节仍正确，但必须被设备请求数量断言拒绝。
    limit='UINT32 maximum = OS_BLOCK_CACHE_MAX_READ_BYTES / filesystem->BytesPerSector;'
    assert limit in code
    old=code.replace(limit,'UINT32 maximum = 128U;')
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),'-x','c','-',
                    str(root/'kernel/block/bio.c'),
                    str(root/'kernel/fs/pagecache/cache.c'),'-o',binary],
                   input=old,text=True,check=True)
    failed=subprocess.run([binary],capture_output=True,text=True,timeout=20,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert failed.returncode!=0 and 'reads==1&&max_count==2048' in failed.stderr,failed.stderr
    print('Negative control rejected: FAT splits contiguous 1MiB into 128-sector requests')
