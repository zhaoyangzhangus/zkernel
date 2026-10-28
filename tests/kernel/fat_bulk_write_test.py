"""真实 FAT 簇链和块缓存：连续/碎片写入、边界及部分设备失败重试。"""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
files=(root/'kernel/fs/nativefs/file_lifecycle.c').read_text()
table=(root/'kernel/fs/nativefs/fat_table.c').read_text()
cache=(root/'kernel/fs/pagecache/cache.c').read_text()
diag_start=cache.index('static atomic_flag g_block_cache_diag_lock')
diag_end=cache.index('static VOID copy_bytes(',diag_start)
cache=cache[:diag_start]+'''static VOID block_cache_diag(UINT32 stage, UINT64 lba, const OS_BLOCK_CACHE *cache) {
 (void)stage;(void)lba;(void)cache;
}
'''+cache[diag_end:]
def function(source,signature):
 start=source.index(signature)
 return source[start:source.index('\n}',start)+2]
program=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/fat32.h>
#define FAT_TABLE_TYPE_16 16U
#define FAT_TABLE16_EOC_MIN 0xfff8U
#define FAT_TABLE16_BAD_CLUSTER 0xfff7U
#define FAT_TABLE_EOC_MIN 0x0ffffff8U
#define FAT_TABLE_BAD_CLUSTER 0x0ffffff7U
static UINT8 *disk,*expected,*input;
static unsigned bps,run,data_writes,largest,fail_data;
static const unsigned data_start=64,block_count=8192;
static OS_FAT32 fs;
static unsigned physical(unsigned logical){return 2+logical+(run?logical/run*3:0);}
static BOOLEAN read_disk(VOID *ctx,UINT64 lba,UINT32 count,VOID *out){
 (void)ctx;assert(lba+count<=block_count);memcpy(out,disk+lba*bps,(size_t)count*bps);return 1;
}
static BOOLEAN write_disk(VOID *ctx,UINT64 lba,UINT32 count,const VOID *in){
 (void)ctx;assert(lba+count<=block_count);
 if(lba>=data_start){++data_writes;if(count>largest)largest=count;
  if(fail_data&&count>=16){memcpy(disk+lba*bps,in,(size_t)count*bps/2);return 0;}}
 memcpy(disk+lba*bps,in,(size_t)count*bps);return 1;
}
static BOOLEAN fat32_cluster_valid(const OS_FAT32 *f,UINT32 c){return c>=2&&c<=f->ClusterCount+1;}
static UINT16 fat32_read_u16(const UINT8 *p){return (UINT16)p[0]|(UINT16)p[1]<<8;}
static UINT32 fat32_read_u32(const UINT8 *p){return (UINT32)fat32_read_u16(p)|(UINT32)fat32_read_u16(p+2)<<16;}
static BOOLEAN fat32_read_sector(const OS_FAT32 *f,UINT64 lba,UINT8 *out){
 return block_cache_read((OS_BLOCK_CACHE *)&f->Cache,lba,out);
}
static UINT64 fat32_cluster_lba(const OS_FAT32 *f,UINT32 cluster){
 return f->DataStartLba+(UINT64)(cluster-2U)*f->SectorsPerCluster;
}
static BOOLEAN extend_file_to(OS_FAT32_FILE *file,UINT64 size,UINT64 zero_until){
 (void)zero_until;return size<=file->Size;
}
'''+'\n'.join((
 function(table,'BOOLEAN fat32_read_fat_entry('),
 function(table,'BOOLEAN fat32_read_next_cluster('),
 function(table,'UINT32 fat32_extent_length('),
 function(files,'static BOOLEAN locate_file_cluster('),
 function(files,'static UINT64 fat32_contiguous_extent_bytes('),
 function(files,'static BOOLEAN fat32_write_locked('),
))+r'''
static void init_media(unsigned bits,unsigned layout){
 run=layout==0?0:layout==1?4:13;
 memset(disk,0xa7,(size_t)block_count*bps);
 unsigned width=bits/8;
 for(unsigned i=0;i<512;i++){
  unsigned value=i==511?(bits==16?FAT_TABLE16_EOC_MIN:FAT_TABLE_EOC_MIN):physical(i+1);
  unsigned pos=physical(i)*width;
  for(unsigned j=0;j<width;j++)disk[bps+pos+j]=(UINT8)(value>>(8*j));
 }
}
static void apply_oracle(unsigned offset,unsigned size,unsigned cluster_bytes){
 for(unsigned i=0;i<size;i++){
  UINT64 pos=(UINT64)offset+i;
  unsigned cluster=physical((unsigned)(pos/cluster_bytes));
  UINT64 lba=data_start+(UINT64)(cluster-2)*8+(pos%cluster_bytes)/bps;
  expected[lba*bps+pos%bps]=input[3+i];
 }
}
int main(void){
 unsigned cases=0;
 input=malloc(OS_BLOCK_CACHE_MAX_WRITE_BYTES+16);assert(input);
 for(size_t i=0;i<OS_BLOCK_CACHE_MAX_WRITE_BYTES+16;i++)input[i]=(UINT8)(i*37+(i>>9));
 for(unsigned bits=16;bits<=32;bits+=16)for(bps=512;bps<=4096;bps*=8){
  size_t disk_bytes=(size_t)block_count*bps;disk=malloc(disk_bytes);expected=malloc(disk_bytes);assert(disk&&expected);
  for(unsigned layout=0;layout<3;layout++)for(unsigned alignment=0;alignment<4;alignment++)
  for(unsigned trial=0;trial<4;trial++){
   init_media(bits,layout);memcpy(expected,disk,disk_bytes);
   OS_BLOCK_DEVICE dev={.Registered=1,.BlockSize=bps,.BlockCount=block_count,.Read=read_disk,.Write=write_disk};
   memset(&fs,0,sizeof(fs));fs.Device=&dev;fs.BytesPerSector=bps;fs.SectorsPerCluster=8;
   fs.FatCount=2;fs.FatType=bits;fs.FatStartLba=1;fs.FatSectors=32;fs.ClusterCount=2048;fs.DataStartLba=data_start;
   assert(block_cache_init(&fs.Cache,&dev));
   unsigned cluster_bytes=bps*8;
   unsigned offsets[]={0,1,bps-1,cluster_bytes-1};
   unsigned lengths[]={1,8193,65536,OS_BLOCK_CACHE_MAX_WRITE_BYTES};
   unsigned off=offsets[alignment],size=lengths[trial];
   OS_FAT32_FILE f={.FileSystem=&fs,.Used=1,.FirstCluster=2,.Size=(UINT64)512*cluster_bytes};
   apply_oracle(off,size,cluster_bytes);data_writes=largest=fail_data=0;UINT32 done=0;
   assert(fat32_write_locked(&f,off,input+3,size,&done)&&done==size);
   if(!layout&&!alignment&&trial==3){assert(data_writes==1&&largest==size/bps);}
   assert(block_cache_flush(&fs.Cache));
   assert(!memcmp(disk,expected,disk_bytes));assert(block_cache_destroy(&fs.Cache));++cases;
  }
  /* 设备直写部分完成后失败：进度只包含完成的前缀，重试恢复完整字节。 */
  for(unsigned unaligned=0;unaligned<2;unaligned++){
   init_media(bits,0);memcpy(expected,disk,disk_bytes);
   OS_BLOCK_DEVICE dev={.Registered=1,.BlockSize=bps,.BlockCount=block_count,.Read=read_disk,.Write=write_disk};
   memset(&fs,0,sizeof(fs));fs.Device=&dev;fs.BytesPerSector=bps;fs.SectorsPerCluster=8;
   fs.FatCount=2;fs.FatType=bits;fs.FatStartLba=1;fs.FatSectors=32;fs.ClusterCount=2048;fs.DataStartLba=data_start;
   assert(block_cache_init(&fs.Cache,&dev));
   OS_FAT32_FILE f={.FileSystem=&fs,.Used=1,.FirstCluster=2,.Size=(UINT64)512*bps*8};
   unsigned off=unaligned?1:0,size=131072;apply_oracle(off,size,bps*8);
   data_writes=largest=0;fail_data=1;UINT32 done=0;
   assert(!fat32_write_locked(&f,off,input+3,size,&done));
   assert(done==(unaligned?bps-1:0));
   fail_data=0;UINT32 rest=0;
   assert(fat32_write_locked(&f,off+done,input+3+done,size-done,&rest)&&done+rest==size);
   assert(block_cache_flush(&fs.Cache)&&!memcmp(disk,expected,disk_bytes));
   assert(block_cache_destroy(&fs.Cache));
  }
  free(disk);free(expected);
 }
 free(input);
 printf("FAT streaming write: %u FAT16/32 sector/fragment/alignment cases, 1MiB single request, partial failure and retry PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='fat-stream-write-') as directory:
 binary=str(Path(directory)/'test')
 cache_path=Path(directory)/'cache.c';cache_path.write_text(cache)
 subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
  '-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),
  '-x','c','-',str(root/'kernel/block/bio.c'),str(cache_path),
  '-o',binary],input=program,text=True,check=True)
 subprocess.run([binary],check=True,timeout=45)
