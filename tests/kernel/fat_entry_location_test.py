"""验证实际 FAT 表项读写的公共定位与无效几何拒绝。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/nativefs/fat_table.c').read_text()
def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]

actual = '\n'.join(function(signature) for signature in (
    'BOOLEAN fat32_cluster_valid(', 'static BOOLEAN fat32_fat_entry_location(',
    'BOOLEAN fat32_read_fat_entry(', 'BOOLEAN fat32_write_fat_entry('))
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/fat32.h>
#define FAT_TABLE_TYPE_16 16U
static unsigned reads,writes;
static UINT64 expected_lba;
static UINT8 saved[4096],expected[4096];
static UINT16 fat32_read_u16(const UINT8 *p){return p[0]|(UINT16)p[1]<<8;}
static UINT32 fat32_read_u32(const UINT8 *p){return fat32_read_u16(p)|(UINT32)fat32_read_u16(p+2)<<16;}
static void fat32_write_u16(UINT8 *p,UINT16 v){p[0]=v;p[1]=v>>8;}
static void fat32_write_u32(UINT8 *p,UINT32 v){fat32_write_u16(p,v);fat32_write_u16(p+2,v>>16);}
static BOOLEAN fat32_read_sector(const OS_FAT32 *fs,UINT64 lba,UINT8 *out){
 assert(lba==expected_lba&&fs->BytesPerSector<=4096);++reads;
 memcpy(out,saved,fs->BytesPerSector);return 1;
}
static BOOLEAN fat32_write_sector(OS_FAT32 *fs,UINT64 lba,const UINT8 *in){
 assert(lba==expected_lba&&fs->BytesPerSector<=4096);++writes;
 assert(!memcmp(in,expected,fs->BytesPerSector));return 1;
}
'''
suffix = r'''
int main(void){
 unsigned cases=0;
 for(unsigned bits=16;bits<=32;bits+=16)
 for(unsigned bps=512;bps<=4096;bps*=2)
 for(unsigned copy=1;copy<=3;++copy)
 for(unsigned cluster=2;cluster<1026;++cluster){
  OS_FAT32 fs={.FatType=bits,.BytesPerSector=bps,.FatCount=3,.FatSectors=97,
   .FatStartLba=123456789012ULL,.ClusterCount=1024,.FreeClusterSearchStart=2};
  unsigned width=bits/8,offset=(cluster*width)%bps;
  expected_lba=fs.FatStartLba+(UINT64)(copy-1)*97+(cluster*width)/bps;
  for(unsigned i=0;i<bps;++i)saved[i]=(UINT8)(cluster*13+i*7+copy);
  UINT32 value=0,reference=0;
  for(unsigned b=0;b<width;++b)reference|=(UINT32)saved[offset+b]<<(8*b);
  reference&=bits==16?0xffff:0x0fffffff;
  reads=writes=0;
  assert(fat32_read_fat_entry(&fs,copy,cluster,&value)&&value==reference&&reads==1&&!writes);
  memcpy(expected,saved,bps);
  value=(cluster*1664525U+1013904223U)&(bits==16?0xffff:0x0fffffff);
  for(unsigned b=0;b<width;++b)expected[offset+b]=(UINT8)(value>>(8*b));
  assert(fat32_write_fat_entry(&fs,copy,cluster,value)&&reads==2&&writes==1);++cases;
 }
 OS_FAT32 fs={.FatType=32,.BytesPerSector=512,.FatCount=3,.FatSectors=97,.ClusterCount=1024};
 const unsigned invalid_sizes[]={0,1,4097,8192};
 reads=writes=0;UINT32 value=0xdeadbeef;
 for(unsigned i=0;i<4;++i){
  fs.BytesPerSector=invalid_sizes[i];
  assert(!fat32_read_fat_entry(&fs,1,2,&value)&&value==0xdeadbeef);
  assert(!fat32_write_fat_entry(&fs,1,2,0));
 }
 fs.BytesPerSector=512;
 assert(!fat32_read_fat_entry(0,1,2,&value));assert(!fat32_write_fat_entry(0,1,2,0));
 assert(!fat32_read_fat_entry(&fs,1,2,0));
 const unsigned invalid_clusters[]={0,1,1026,UINT32_MAX};
 for(unsigned i=0;i<4;++i){
  assert(!fat32_read_fat_entry(&fs,1,invalid_clusters[i],&value));
  assert(!fat32_write_fat_entry(&fs,1,invalid_clusters[i],0));
 }
 for(unsigned copy=0;copy<=4;copy+=4){
  assert(!fat32_read_fat_entry(&fs,copy,2,&value));
  assert(!fat32_write_fat_entry(&fs,copy,2,0));
 }
 assert(!reads&&!writes&&value==0xdeadbeef);
 printf("FAT entry location: %u FAT16/32 and mirror read/write cases, sector boundaries and invalid geometry without I/O PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='fat-entry-location-') as directory:
    binary = str(Path(directory) / 'test')
    def run(code):
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I'+str(root/'include'), '-x', 'c', '-', '-o', binary],
                       input=prefix+code+suffix, text=True, check=True)
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        return subprocess.run([binary], capture_output=True, text=True, preexec_fn=no_core)
    result = run(actual)
    assert result.returncode == 0, result.stderr
    print(result.stdout, end='')
    for old, new in (
        ('(UINT64)(fat_number - 1U) * filesystem->FatSectors', '0U'),
        ('filesystem->BytesPerSector == 0U || ', ''),
        ('filesystem->BytesPerSector > 4096U', 'false')):
        assert old in actual
        result = run(actual.replace(old, new))
        assert result.returncode != 0
    print('反例被拒绝：忽略镜像偏移、零扇区除法、超过栈缓冲区的扇区大小')
