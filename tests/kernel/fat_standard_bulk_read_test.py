"""真实标准 FAT 簇链、游标、文件读取、块缓存到设备的集成对照。"""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
table = (root / 'kernel/fs/nativefs/fat_table.c').read_text()
files = (root / 'kernel/fs/nativefs/file_lifecycle.c').read_text()
cache = (root / 'kernel/fs/pagecache/cache.c').read_text()
# 主机进程不能访问 COM1；仅替换诊断输出，保留全部缓存与错误处理逻辑。
diag_start = cache.index('static atomic_flag g_block_cache_diag_lock')
diag_end = cache.index('static VOID copy_bytes(', diag_start)
cache = cache[:diag_start] + '''static VOID block_cache_diag(UINT32 stage,
    UINT64 lba,const OS_BLOCK_CACHE *cache) {
    (void)stage;(void)lba;(void)cache;
}
''' + cache[diag_end:]

def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2]

locate = function(files, 'static BOOLEAN locate_file_cluster(')
reference_locate = locate.replace('locate_file_cluster(', 'reference_locate_file_cluster(', 1)
reference_locate = reference_locate.replace(': probe);', ': 16U);')
# 参考版本保持旧的固定探测上限；保留其余游标和检查点逻辑。
reference_locate = reference_locate.replace('        UINT32 length =',
                                           '        (void)probe;\n        UINT32 length =', 1)

program = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <kernel/fat32.h>
#define FAT_TABLE_TYPE_16 16U
#define FAT_TABLE16_EOC_MIN 0xfff8U
#define FAT_TABLE16_BAD_CLUSTER 0xfff7U
#define FAT_TABLE_EOC_MIN 0x0ffffff8U
#define FAT_TABLE_BAD_CLUSTER 0x0ffffff7U
static UINT8 fat[65536];
static unsigned bps,data_reads,metadata_reads,largest,run;
static unsigned sector_calls;
static unsigned failed_lba;
static const unsigned data_start=4096,fat_start=16;
static unsigned physical(unsigned logical) {
 return 2+logical+(run?logical/run*3:0);
}
static UINT8 pixel(UINT64 lba,unsigned byte) {
 return (UINT8)((lba*17+byte)^(lba>>8));
}
static BOOLEAN read_disk(VOID *unused,UINT64 lba,UINT32 count,VOID *out) {
 (void)unused;
 if(lba<data_start){
  ++metadata_reads;
  assert(lba>=fat_start && (lba-fat_start+count)*bps<=sizeof(fat));
  if(lba<=failed_lba && failed_lba<lba+count)return 0;
  memcpy(out,fat+(lba-fat_start)*bps,(size_t)count*bps);
 }else{
  ++data_reads;if(count>largest)largest=count;
  if(lba<=failed_lba && failed_lba<lba+count)return 0;
  for(unsigned s=0;s<count;++s)for(unsigned b=0;b<bps;++b)
   ((UINT8*)out)[(size_t)s*bps+b]=pixel(lba+s,b);
 }
 return 1;
}
static BOOLEAN write_disk(VOID *unused,UINT64 lba,UINT32 count,const VOID *in) {
 (void)unused;(void)lba;(void)count;(void)in;assert(0);return 0;
}
static BOOLEAN fat32_cluster_valid(const OS_FAT32 *fs,UINT32 c) {
 return fs && c>=2 && c<=fs->ClusterCount+1;
}
static UINT16 fat32_read_u16(const UINT8 *p) {
 return (UINT16)p[0]|(UINT16)p[1]<<8;
}
static UINT32 fat32_read_u32(const UINT8 *p) {
 return (UINT32)fat32_read_u16(p)|(UINT32)fat32_read_u16(p+2)<<16;
}
static BOOLEAN fat32_read_sector(const OS_FAT32 *fs,UINT64 lba,UINT8 *out) {
 ++sector_calls;
 return block_cache_read((OS_BLOCK_CACHE*)&fs->Cache,lba,out);
}
static UINT64 fat32_cluster_lba(const OS_FAT32 *fs,UINT32 c) {
 return fs->DataStartLba+(UINT64)(c-2)*fs->SectorsPerCluster;
}
static void fat32_diag_failure(const char *tag,UINT64 a,UINT64 b,UINT64 c) {
 (void)tag;(void)a;(void)b;(void)c;
}
''' + '\n'.join((
    function(table, 'BOOLEAN fat32_read_fat_entry('),
    function(table, 'BOOLEAN fat32_read_next_cluster('),
    function(table, 'UINT32 fat32_extent_length('),
    locate,
    reference_locate,
    function(files, 'static UINT64 fat32_contiguous_extent_bytes('),
    function(files, 'static BOOLEAN fat32_read('),
)) + r'''
int main(void) {
 UINT8 *out=malloc(OS_BLOCK_CACHE_MAX_READ_BYTES+32);assert(out);
 unsigned cases=0;
 for(unsigned bits=16;bits<=32;bits+=16)
 for(bps=512;bps<=4096;bps*=2)
 for(unsigned geometry=0;geometry<3;++geometry)
 for(unsigned layout=0;layout<3;++layout)
 for(unsigned align=0;align<3;++align)
 for(unsigned small=0;small<2;++small) {
  run=layout==0?0:layout==1?4:37;
  unsigned spc=geometry==0?1:geometry==1?8:32;
  unsigned width=bits/8,cluster_bytes=bps*spc;
  unsigned clusters=OS_BLOCK_CACHE_MAX_READ_BYTES/cluster_bytes+32;
  memset(fat,0,sizeof(fat));
  for(unsigned i=0;i<clusters;++i){
   unsigned value=i+1<clusters?physical(i+1):
       bits==16?FAT_TABLE16_EOC_MIN:FAT_TABLE_EOC_MIN;
   if(bits==32)value|=0xa0000000U;
   for(unsigned b=0;b<width;++b)fat[physical(i)*width+b]=(UINT8)(value>>(8*b));
  }
  OS_BLOCK_DEVICE device={.Registered=1,.BlockSize=bps,.BlockCount=1000000,
   .Read=read_disk,.Write=write_disk};
  OS_FAT32 fs={.Device=&device,.BytesPerSector=bps,.SectorsPerCluster=spc,
   .FatCount=2,.FatType=bits,.FatStartLba=fat_start,.FatSectors=32,
   .ClusterCount=10000,.DataStartLba=data_start};
  assert(block_cache_init(&fs.Cache,&device));
  OS_FAT32_FILE file={.FileSystem=&fs,.Used=1,.FirstCluster=2,
   .Size=(UINT64)clusters*cluster_bytes};
  unsigned offset=align==0?0:align==1?1:cluster_bytes-1;
  unsigned size=small?8192:OS_BLOCK_CACHE_MAX_READ_BYTES;
  memset(out,0x9b,OS_BLOCK_CACHE_MAX_READ_BYTES+32);
  failed_lba=UINT32_MAX;data_reads=metadata_reads=largest=0;
  UINT32 got=0;assert(fat32_read(&file,offset,out+16,size,&got) && got==size);
  for(unsigned b=0;b<got;++b){
   UINT64 position=(UINT64)offset+b;
   unsigned c=physical((unsigned)(position/cluster_bytes));
   UINT64 lba=data_start+(UINT64)(c-2)*spc+position%cluster_bytes/bps;
   assert(out[16+b]==pixel(lba,position%bps));
  }
  for(unsigned b=0;b<16;++b)assert(out[b]==0x9b && out[16+got+b]==0x9b);
  if(!layout&&!align&&!small){
   assert(data_reads==1 && largest==size/bps);
   unsigned needed=size/cluster_bytes;
   assert(file.CursorLength==(needed>16?needed:16));
  }
  /* 小请求不应为了潜在后续读取扩大簇链扫描。 */
  if(!layout&&!align&&small&&cluster_bytes>=4096)
   assert(file.CursorLength==16 && metadata_reads==1);
  if(!layout&&!align&&!small&&bps==512&&spc==8){
   /* 扩展探测跨 FAT 扇区失败时，只消费已验证前缀；恢复后从进度继续。 */
   assert(block_cache_destroy(&fs.Cache));assert(block_cache_init(&fs.Cache,&device));
   file.CursorValid=0;failed_lba=fat_start+1;got=0;
   assert(!fat32_read(&file,0,out+16,size,&got) && got>0 && got<size);
   unsigned progress=got;failed_lba=UINT32_MAX;
   UINT32 rest=0;
   assert(fat32_read(&file,progress,out+16+progress,size-progress,&rest));
   assert(progress+rest==size);
   for(unsigned b=0;b<size;++b)assert(out[16+b]==pixel(data_start+b/bps,b%bps));
   /* 数据请求失败不能推进进度，保留的已验证游标允许安全重试。 */
   assert(block_cache_destroy(&fs.Cache));assert(block_cache_init(&fs.Cache,&device));
   failed_lba=data_start;got=123;
   assert(!fat32_read(&file,0,out+16,size,&got) && got==0);
   failed_lba=UINT32_MAX;
   assert(fat32_read(&file,0,out+16,size,&got) && got==size);
  }
  assert(block_cache_destroy(&fs.Cache));++cases;
 }
 unsigned long actual_calls=0,previous_calls=0;
 unsigned seeks=0;
 for(unsigned bits=16;bits<=32;bits+=16)
 for(bps=512;bps<=4096;bps*=2)
 for(unsigned layout=0;layout<3;++layout) {
  run=layout==0?0:layout==1?4:37;
  unsigned width=bits/8;
  memset(fat,0,sizeof(fat));
  for(unsigned i=0;i<4096;++i) {
   unsigned value=i==4095?(bits==16?FAT_TABLE16_EOC_MIN:FAT_TABLE_EOC_MIN):physical(i+1);
   for(unsigned b=0;b<width;++b)fat[physical(i)*width+b]=(UINT8)(value>>(8*b));
  }
  OS_BLOCK_DEVICE device={.Registered=1,.BlockSize=bps,.BlockCount=1000000,
   .Read=read_disk,.Write=write_disk};
  OS_FAT32 fs={.Device=&device,.BytesPerSector=bps,.SectorsPerCluster=8,
   .FatCount=2,.FatType=bits,.FatStartLba=fat_start,.FatSectors=64,
   .ClusterCount=10000,.DataStartLba=data_start};
  failed_lba=UINT32_MAX;
  unsigned targets[]={0,15,16,254,255,256,1024,4095,128,4094,16,2048,4096,8192};
  for(unsigned cold=0;cold<2;++cold) {
   OS_FAT32_FILE a={.FileSystem=&fs,.Used=1,.FirstCluster=2,.Size=4096ULL*bps*8};
   OS_FAT32_FILE previous=a;
   for(unsigned t=0;t<sizeof(targets)/sizeof(*targets);++t) {
    if(cold)a.CursorValid=previous.CursorValid=0;
    assert(block_cache_init(&fs.Cache,&device));
    UINT32 actual=0,expected=0;
    sector_calls=0;BOOLEAN ok=locate_file_cluster(&a,targets[t],&actual);
    actual_calls+=sector_calls;
    assert(block_cache_destroy(&fs.Cache));
    assert(block_cache_init(&fs.Cache,&device));
    sector_calls=0;BOOLEAN old_ok=reference_locate_file_cluster(&previous,targets[t],&expected);
    previous_calls+=sector_calls;
    assert(ok==old_ok && ok==(targets[t]<4096));
    assert(actual==expected && (!ok || actual==physical(targets[t])));
    assert(block_cache_destroy(&fs.Cache));++seeks;
   }
  }
 }
 assert(actual_calls<previous_calls);
 printf("Standard FAT seek: %u cold/warm forward/backward byte-location comparisons; metadata sector accesses %lu -> %lu\n",seeks,previous_calls,actual_calls);
 free(out);
 printf("Standard FAT full read: %u FAT16/32 geometry/fragmentation/alignment/size byte-oracle cases; real 1MiB data reads 16 -> 1 at 4KiB clusters; small requests retain short probe\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='fat-standard-bulk-') as directory:
    binary = str(Path(directory) / 'test')
    cache_file = Path(directory) / 'cache.c'
    cache_file.write_text(cache)
    command = ['cc', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-I'+str(root/'include'), '-x', 'c', '-',
               str(root/'kernel/block/bio.c'),
               str(cache_file), '-o', binary]
    subprocess.run(command, input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
    broken = program.replace('!filesystem->Fat3Available && extent_bytes < wanted',
                             'filesystem->Fat3Available && extent_bytes < wanted')
    assert broken != program
    subprocess.run(command, input=broken, text=True, check=True)
    result = subprocess.run([binary], capture_output=True, timeout=30,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0,0)))
    assert result.returncode == -signal.SIGABRT and b'data_reads==1' in result.stderr
    print('Negative control rejected: fixed short extent splits real contiguous reads')
    broken = program.replace(': probe);', ': 16U);')
    broken = broken.replace('        UINT32 length = fat32_extent_length',
                            '        (void)probe;\n        UINT32 length = fat32_extent_length', 1)
    assert broken != program
    subprocess.run(command, input=broken, text=True, check=True)
    result = subprocess.run([binary], capture_output=True, timeout=30,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0,0)))
    assert result.returncode == -signal.SIGABRT and b'actual_calls<previous_calls' in result.stderr
    print('Negative control rejected: distant seeks repeat fixed 16-cluster probes')
