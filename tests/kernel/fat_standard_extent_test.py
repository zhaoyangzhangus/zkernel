"""真实 FAT 连续簇探测：逐项参考对照、扇区复用、跨界和失败回退。"""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/nativefs/fat_table.c').read_text()
begin=source.index('UINT32 fat32_extent_length(')
end=source.index('\n}',begin)+2
entry_begin=source.index('BOOLEAN fat32_read_fat_entry(')
entry=source[entry_begin:source.index('\n}',entry_begin)+2]
next_begin=source.index('BOOLEAN fat32_read_next_cluster(')
next_cluster=source[next_begin:source.index('\n}',next_begin)+2]
code=r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef int BOOLEAN;
#define FAT_TABLE_TYPE_16 16U
#define FAT_TABLE16_EOC_MIN 0xfff8U
#define FAT_TABLE16_BAD_CLUSTER 0xfff7U
#define FAT_TABLE_EOC_MIN 0x0ffffff8U
#define FAT_TABLE_BAD_CLUSTER 0x0ffffff7U
typedef struct {
 BOOLEAN Fat3Available;
 UINT32 ClusterCount,FatType,FatCount,BytesPerSector,FatSectors;
 UINT64 FatStartLba;
} OS_FAT32;
static UINT8 disk[131072];
static unsigned calls;
static UINT64 fail_lba;
static BOOLEAN fat32_cluster_valid(const OS_FAT32 *fs, UINT32 c) {
 return fs && c>=2 && c<=fs->ClusterCount+1;
}
static UINT16 fat32_read_u16(const UINT8 *p) {
 return (UINT16)p[0] | (UINT16)p[1]<<8;
}
static UINT32 fat32_read_u32(const UINT8 *p) {
 return (UINT32)fat32_read_u16(p) | (UINT32)fat32_read_u16(p+2)<<16;
}
static BOOLEAN fat32_read_sector(const OS_FAT32 *fs,UINT64 lba,UINT8 *out) {
 ++calls;if(lba==fail_lba)return 0;
 UINT64 offset=(lba-fs->FatStartLba)*fs->BytesPerSector;
 assert(offset+fs->BytesPerSector<=sizeof(disk));
 memcpy(out,disk+offset,fs->BytesPerSector);return 1;
}
''' +entry+next_cluster+source[begin:end]+r'''
static UINT32 reference(const OS_FAT32 *fs,UINT32 cluster) {
 UINT32 length=1;
 while(length<16 && fat32_cluster_valid(fs,cluster+length)) {
  UINT32 next;
  if(!fat32_read_next_cluster(fs,cluster+length-1,&next) ||
     next!=cluster+length)break;
  ++length;
 }
 return length;
}
static void store(unsigned index,unsigned width,UINT32 value) {
 for(unsigned b=0;b<width;++b)disk[index*width+b]=(UINT8)(value>>(8*b));
}
int main(void) {
 unsigned cases=0;
 for(unsigned bits=16;bits<=32;bits+=16)
 for(unsigned bps=512;bps<=4096;bps*=2) {
  OS_FAT32 fs={.ClusterCount=4096,.FatType=bits,.FatCount=2,
   .BytesPerSector=bps,.FatSectors=8,.FatStartLba=123};
  unsigned width=bits/8,per_sector=bps/width;
  unsigned starts[]={2,per_sector-15,per_sector-7,per_sector-1,
                     per_sector,4090,4096,4097};
  for(unsigned s=0;s<sizeof(starts)/sizeof(*starts);++s)
  for(unsigned stop=0;stop<16;++stop)
  for(unsigned kind=0;kind<6;++kind)
  for(unsigned failed=0;failed<3;++failed) {
   memset(disk,0,sizeof(disk));
   for(unsigned i=2;i<=4097;++i)
    store(i,width,bits==32?0xa0000000U|(i+1):i+1);
   UINT32 value=kind==0?0:kind==1?1:kind==2?4099:
                kind==3?(bits==16?FAT_TABLE16_EOC_MIN:FAT_TABLE_EOC_MIN):
                kind==4?(bits==16?FAT_TABLE16_BAD_CLUSTER:FAT_TABLE_BAD_CLUSTER):37;
   store(starts[s]+stop,width,value);
   UINT64 first=fs.FatStartLba+(UINT64)starts[s]*width/bps;
   fail_lba=failed?first+failed-1:UINT64_MAX;
   calls=0;unsigned expected=reference(&fs,starts[s]);
   unsigned reference_calls=calls;
   calls=0;unsigned actual=fat32_extent_length(&fs,starts[s],16);
   assert(actual==expected && actual>=1 && actual<=16);
   unsigned expected_calls=0;UINT64 previous=UINT64_MAX;
   for(unsigned i=0;i<reference_calls;++i) {
    UINT64 lba=fs.FatStartLba+(UINT64)(starts[s]+i)*width/bps;
    if(lba!=previous){++expected_calls;previous=lba;}
   }
   assert(calls==expected_calls);++cases;
  }
  memset(disk,0,sizeof(disk));
  for(unsigned i=2;i<20;++i)store(i,width,i+1);
  fail_lba=UINT64_MAX;calls=0;
  assert(fat32_extent_length(&fs,2,16)==16 && calls==1);
  fs.Fat3Available=1;fs.FatCount=3;
  store(2+2*fs.FatSectors*bps/width,width,8);
  calls=0;assert(fat32_extent_length(&fs,2,16)==8 && calls==1);
  assert(fat32_extent_length(&fs,2,3)==3);
  calls=0;assert(fat32_extent_length(&fs,2,0)==0 && calls==0);
 }
 printf("Standard FAT: %u FAT16/32 geometry, fragmentation, EOC/bad/reserved links, sector failure and volume-edge comparisons passed; contiguous probe 15 -> 1 sector reads\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fat-extent-') as directory:
 binary=str(Path(directory)/'test')
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                 '-fno-pie','-no-pie','-x','c','-','-o',binary],input=code,text=True,check=True)
 subprocess.run([binary],check=True)
 broken=code.replace('!loaded || loaded_lba != lba','1 || !loaded || loaded_lba != lba')
 assert broken!=code
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                 '-fno-pie','-no-pie','-x','c','-','-o',binary],input=broken,text=True,check=True)
 result=subprocess.run([binary],capture_output=True,timeout=15,
     preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
 assert result.returncode==-signal.SIGABRT and b'Assertion' in result.stderr
 print('Negative control rejected: repeated FAT sector reads')
print('Standard FAT: bounded contiguous prefix, EOF, read failure, volume boundary and third-table compatibility passed')
