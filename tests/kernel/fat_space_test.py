"""编译真实 FAT 空间查询和表项写入，覆盖缓存维护及错误回滚。"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/nativefs/fat_table.c').read_text()
def function(signature):
    a=source.index(signature)
    return source[a:source.index('\n}',a)+2]
code=r'''
#include <assert.h>
#include <string.h>
#include <kernel/fat32.h>
#define FAT_TABLE_TYPE_16 16U
static unsigned reads,writes,locked;
static bool read_failure,write_failure;
static UINT8 image[32][4096];
void fat32_mutation_lock(OS_FAT32 *f){(void)f;assert(!locked);locked=1;}
void fat32_mutation_unlock(OS_FAT32 *f){(void)f;assert(locked);locked=0;}
BOOLEAN fat32_read_sector(const OS_FAT32 *f,UINT64 lba,UINT8 *out){
 assert(locked);++reads;if(read_failure || lba>=32)return 0;
 memcpy(out,image[lba],f->BytesPerSector);return 1;
}
BOOLEAN fat32_write_sector(OS_FAT32 *f,UINT64 lba,const UINT8 *in){
 assert(locked);++writes;if(write_failure || lba>=32)return 0;
 memcpy(image[lba],in,f->BytesPerSector);return 1;
}
static UINT16 fat32_read_u16(const UINT8 *p){return p[0]|(UINT16)p[1]<<8;}
static UINT32 fat32_read_u32(const UINT8 *p){return fat32_read_u16(p)|(UINT32)fat32_read_u16(p+2)<<16;}
static void fat32_write_u16(UINT8 *p,UINT16 n){p[0]=n;p[1]=n>>8;}
static void fat32_write_u32(UINT8 *p,UINT32 n){fat32_write_u16(p,n);fat32_write_u16(p+2,n>>16);}
BOOLEAN fat32_cluster_valid(const OS_FAT32 *f,UINT32 c){return c>=2 && c<f->ClusterCount+2;}
''' + function('BOOLEAN fat32_write_fat_entry(') + function('BOOLEAN fat32_space(') + r'''
int main(void){
 for(unsigned type=16;type<=32;type+=16)for(unsigned sector=512;sector<=4096;sector*=8){
  memset(image,0xff,sizeof(image));reads=writes=0;
  OS_FAT32 fs={.Mounted=1,.FatType=type,.BytesPerSector=sector,.SectorsPerCluster=8,
   .FatCount=2,.FatSectors=8,.FatStartLba=1,.ClusterCount=700};
  unsigned width=type/8;
  for(unsigned c=2;c<702;++c){
   UINT8 *p=image[1+c*width/sector]+c*width%sector;
   if(c%3==0)memset(p,0,width);
  }
  UINT64 total=99,free_blocks=99,size=99;
  assert(fat32_space(&fs,&total,&free_blocks,&size));
  assert(total==700 && free_blocks==233 && size==sector*8);
  assert(fs.FreeClusterSearchStart==3);
  unsigned first=reads;assert(first==(702*width+sector-1)/sector);
  assert(fat32_space(&fs,&total,&free_blocks,&size)&&reads==first);
  fat32_mutation_lock(&fs);
  assert(fat32_write_fat_entry(&fs,1,3,0xfffffff));
  assert(fs.FreeClusterSearchStart==4);
  assert(fs.FreeClusterCount==232);
  assert(fat32_write_fat_entry(&fs,1,3,7)&&fs.FreeClusterCount==232);
  assert(fat32_write_fat_entry(&fs,1,3,0)&&fs.FreeClusterCount==233);
  assert(fs.FreeClusterSearchStart==3);
  assert(fat32_write_fat_entry(&fs,2,3,1)&&fs.FreeClusterCount==233);
  write_failure=true;assert(!fat32_write_fat_entry(&fs,1,3,1));write_failure=false;
  assert(!fs.FreeClusterCountValid);fat32_mutation_unlock(&fs);
  assert(fs.FreeClusterSearchStart==2);
  read_failure=true;total=free_blocks=size=99;
  assert(!fat32_space(&fs,&total,&free_blocks,&size)&&!locked);
  assert(total==99&&free_blocks==99&&size==99&&!fs.FreeClusterCountValid);
  read_failure=false;assert(fat32_space(&fs,&total,&free_blocks,&size)&&free_blocks==233);
  assert(fs.FreeClusterSearchStart==3);
  fs.FatSectors=1;assert(!fat32_space(&fs,&total,&free_blocks,&size) || sector==4096);
  fs.Mounted=0;assert(!fat32_space(&fs,&total,&free_blocks,&size));
 }
 assert(!fat32_space(0,0,0,0)&&!locked);
}
'''
with tempfile.TemporaryDirectory(prefix='fat-space-') as directory:
    binary=Path(directory)/'test'
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),'-x','c','-','-o',str(binary)],
                   input=code,text=True,check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: FAT16/32 real free-space scan, one read per sector, cached queries, primary/mirror writes, failed I/O and geometry validation')
