"""实际 FAT 搜索和写入联测：跨文件复用已占用前缀，不遗漏释放和回滚的空洞。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/fs/nativefs/fat_table.c').read_text()


def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start)+2]


search = function('BOOLEAN fat32_find_free_cluster_from(')
baseline = search.replace('fat32_find_free_cluster_from(', 'reference_search(', 1)
baseline = baseline.replace('UINT32 floor = filesystem->FreeClusterSearchStart;',
                            'UINT32 floor = 2U;')
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/fat32.h>
#define FAT_TABLE_TYPE_16 16U
static UINT8 media[131072], reference_media[131072];
static unsigned reads,writes;
static UINT64 failed_lba=UINT64_MAX;
static bool failed_write;
static UINT16 fat32_read_u16(const UINT8 *p) { return p[0]|(UINT16)p[1]<<8; }
static UINT32 fat32_read_u32(const UINT8 *p) { return fat32_read_u16(p)|(UINT32)fat32_read_u16(p+2)<<16; }
static void fat32_write_u16(UINT8 *p,UINT16 n) { p[0]=n;p[1]=n>>8; }
static void fat32_write_u32(UINT8 *p,UINT32 n) { fat32_write_u16(p,n);fat32_write_u16(p+2,n>>16); }
static BOOLEAN fat32_read_sector(const OS_FAT32 *fs,UINT64 lba,UINT8 *out) {
    ++reads;
    if (lba==failed_lba) return 0;
    assert((lba+1)*fs->BytesPerSector<=sizeof(media));
    memcpy(out,media+lba*fs->BytesPerSector,fs->BytesPerSector);return 1;
}
static BOOLEAN fat32_write_sector(OS_FAT32 *fs,UINT64 lba,const UINT8 *in) {
    ++writes;
    if (failed_write) return 0;
    assert((lba+1)*fs->BytesPerSector<=sizeof(media));
    memcpy(media+lba*fs->BytesPerSector,in,fs->BytesPerSector);return 1;
}
''' + '\n'.join((function('BOOLEAN fat32_cluster_valid('),
                 function('BOOLEAN fat32_write_fat_entry('), search, baseline,
                 function('BOOLEAN fat32_find_free_cluster('))) + r'''
static UINT32 value(const OS_FAT32 *fs,unsigned cluster) {
    const UINT8 *p=media+fs->FatStartLba*fs->BytesPerSector+cluster*(fs->FatType/8);
    return fs->FatType==16?fat32_read_u16(p):fat32_read_u32(p)&0x0fffffff;
}
static void reset(OS_FAT32 *fs) {
    memset(media,0,sizeof(media));reads=writes=0;failed_lba=UINT64_MAX;failed_write=false;
    fs->FreeClusterSearchStart=2;
    for(unsigned c=2;c<7002;++c) {
        UINT8 *p=media+fs->FatStartLba*fs->BytesPerSector+c*(fs->FatType/8);
        if(fs->FatType==16)fat32_write_u16(p,0xffff);
        else fat32_write_u32(p,0x0fffffff);
    }
}
int main(void) {
    for(unsigned bits=16;bits<=32;bits+=16)
    for(unsigned bps=512;bps<=4096;bps*=2) {
        OS_FAT32 fs={.ClusterCount=8192,.FatType=bits,.BytesPerSector=bps,
            .FatCount=2,.FatSectors=(8194*(bits/8)+bps-1)/bps,.FatStartLba=1};
        unsigned old_reads=0,new_reads=0;
        for(unsigned old=0;old<2;++old) {
            reset(&fs);
            for(unsigned i=0;i<128;++i) {
                UINT32 cluster=0;
                unsigned before=reads;
                assert((old?reference_search(&fs,2,&cluster):fat32_find_free_cluster(&fs,&cluster)));
                if(old)old_reads+=reads-before;else new_reads+=reads-before;
                assert(cluster==7002+i);
                for(unsigned mirror=1;mirror<=2;++mirror)
                    assert(fat32_write_fat_entry(&fs,mirror,cluster,0x0fffffff));
            }
            if(!old)memcpy(reference_media,media,sizeof(media));
            else assert(!memcmp(media,reference_media,sizeof(media)));
        }
        assert(new_reads<old_reads/3);
        printf("FAT%u/%u, 128 separate allocations: search sector reads %u -> %u; identical media\n",
               bits,bps,old_reads,new_reads);

        /* 任意搜索起点与独立逐项参考结果相同；主 FAT 释放涵盖删除、截断和事务回滚。 */
        UINT32 random=31;
        for(unsigned i=0;i<2000;++i) {
            random=random*1664525U+1013904223U;
            unsigned cluster=2+(random%8192);
            assert(fat32_write_fat_entry(&fs,1,cluster,(random&0x80000000)?0:0x0fffffff));
            for(unsigned c=2;c<fs.FreeClusterSearchStart;++c)assert(value(&fs,c)!=0);
            unsigned first=i%3?2:2+((random>>5)%8192);
            UINT32 expected=0xdeadbeef,actual=expected;
            for(unsigned c=first;c<=fs.ClusterCount+1;++c)
                if(!value(&fs,c)){expected=c;break;}
            BOOLEAN found=fat32_find_free_cluster_from(&fs,first,&actual);
            assert(found==(expected!=0xdeadbeef) && actual==expected);
        }
        reset(&fs);UINT32 cluster=123;
        assert(fat32_find_free_cluster_from(&fs,7500,&cluster)&&cluster==7500);
        assert(fs.FreeClusterSearchStart==2);
        failed_lba=1;
        assert(!fat32_find_free_cluster(&fs,&cluster)&&fs.FreeClusterSearchStart==2);
        failed_lba=UINT64_MAX;
        assert(fat32_find_free_cluster(&fs,&cluster)&&cluster==7002);
        failed_write=true;
        assert(!fat32_write_fat_entry(&fs,1,5,0)&&fs.FreeClusterSearchStart==2);
        failed_write=false;
        assert(fat32_find_free_cluster(&fs,&cluster)&&cluster==7002);
        assert(fat32_write_fat_entry(&fs,1,5,0)&&fs.FreeClusterSearchStart==5);
        assert(fat32_find_free_cluster(&fs,&cluster)&&cluster==5);
        assert(fat32_write_fat_entry(&fs,1,5,0x0fffffff));
        assert(fat32_find_free_cluster(&fs,&cluster)&&cluster==7002);

        /* 完整扫描确认已满后可直接失败；释放最后一簇必须立刻恢复可分配性。 */
        for(unsigned c=2;c<=fs.ClusterCount+1;++c)
            assert(fat32_write_fat_entry(&fs,1,c,0x0fffffff));
        cluster=123;
        assert(!fat32_find_free_cluster(&fs,&cluster)&&cluster==123);
        assert(fs.FreeClusterSearchStart==fs.ClusterCount+2);
        unsigned before=reads;
        assert(!fat32_find_free_cluster(&fs,&cluster)&&reads==before);
        assert(fat32_write_fat_entry(&fs,1,8193,0));
        assert(fat32_find_free_cluster(&fs,&cluster)&&cluster==8193);
        for(unsigned invalid=0;invalid<3;++invalid) {
            fs.FreeClusterSearchStart=invalid==0?0:invalid==1?1:UINT32_MAX;
            assert(fat32_find_free_cluster(&fs,&cluster)&&cluster==8193);
        }
    }
    puts("random mutations, explicit search start, read/write failure, rollback holes, full volume and invalid floor PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='fat-free-search-floor-') as directory:
    binary = str(Path(directory)/'test')
    flags = ['cc', '-O2', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
             '-I'+str(root/'include'), '-x', 'c', '-', '-o', binary]
    subprocess.run(flags, input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
    # 丢掉释放时的下界修正必须被随机变更及回滚空洞断言拒绝。
    broken = program.replace('floor = cluster;', '(void)cluster;')
    assert broken != program
    subprocess.run(flags, input=broken, text=True, check=True)
    failed = subprocess.run([binary], capture_output=True, text=True, timeout=30,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0,0)))
    assert failed.returncode != 0 and 'Assertion' in failed.stderr
    print('Negative control rejected: freed earlier clusters skipped by stale search floor')
