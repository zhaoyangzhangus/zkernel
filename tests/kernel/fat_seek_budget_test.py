"""真实 FAT 定位按已遍历簇数计费，损坏链不能重复扫描整个卷。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
table = (root/'kernel/fs/nativefs/fat_table.c').read_text()
files = (root/'kernel/fs/nativefs/file_lifecycle.c').read_text()
def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start)+2]

prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/fat32.h>
#define FAT_TABLE_TYPE_16 16U
#define FAT_TABLE16_EOC_MIN 0xfff8U
#define FAT_TABLE16_BAD_CLUSTER 0xfff7U
#define FAT_TABLE_EOC_MIN 0x0ffffff8U
#define FAT_TABLE_BAD_CLUSTER 0x0ffffff7U
static UINT8 media[524288];
static unsigned reads,verified,probes;
static UINT16 fat32_read_u16(const UINT8 *p){return p[0]|(UINT16)p[1]<<8;}
static UINT32 fat32_read_u32(const UINT8 *p){return fat32_read_u16(p)|(UINT32)fat32_read_u16(p+2)<<16;}
static BOOLEAN fat32_read_sector(const OS_FAT32 *fs,UINT64 lba,UINT8 *out){
 assert(lba>=fs->FatStartLba);++reads;
 UINT64 offset=(lba-fs->FatStartLba)*fs->BytesPerSector;
 assert(offset+fs->BytesPerSector<=sizeof(media));
 memcpy(out,media+offset,fs->BytesPerSector);return 1;
}
'''
actual = '\n'.join(function(table, signature) for signature in (
    'BOOLEAN fat32_cluster_valid(', 'static BOOLEAN fat32_fat_entry_location(',
    'BOOLEAN fat32_read_fat_entry(', 'BOOLEAN fat32_read_next_cluster('))
actual += function(table, 'UINT32 fat32_extent_length(').replace(
    'fat32_extent_length(', 'real_extent_length(', 1)
actual += r'''
static UINT32 fat32_extent_length(const OS_FAT32 *fs,UINT32 cluster,UINT32 maximum){
 ++probes;UINT32 length=real_extent_length(fs,cluster,maximum);verified+=length;
 return length;
}
'''
actual += function(files, 'static void fat32_extend_seek_hint(')
locate = function(files, 'static BOOLEAN locate_file_cluster(')
suffix = r'''
static void store(const OS_FAT32 *fs,unsigned cluster,unsigned next){
 unsigned width=fs->FatType/8;
 for(unsigned b=0;b<width;++b)media[cluster*width+b]=(UINT8)(next>>(8*b));
}
int main(void){
 unsigned cases=0;
 for(unsigned bits=16;bits<=32;bits+=16)
 for(unsigned bps=512;bps<=4096;bps*=2)
 for(unsigned ext=0;ext<2;++ext){
  OS_FAT32 fs={.FatType=bits,.BytesPerSector=bps,.SectorsPerCluster=1,
   .FatCount=ext?3:2,.Fat3Available=ext,.FatSectors=32,.FatStartLba=17,.ClusterCount=1003};
  for(unsigned cluster=2;cluster<=fs.ClusterCount+1;++cluster){
   store(&fs,cluster,cluster==fs.ClusterCount+1?(bits==16?0xfff8:0x0ffffff8):cluster+1);
   unsigned width=bits/8,off=2*fs.FatSectors*bps+cluster*width;
   for(unsigned b=0;b<width;++b)media[off+b]=(UINT8)((fs.ClusterCount+2-cluster)>>(8*b));
  }
  for(unsigned target=0;target<fs.ClusterCount;++target){
   OS_FAT32_FILE file={.FileSystem=&fs,.FirstCluster=2,.Size=(UINT64)fs.ClusterCount*bps};
   UINT32 out=99;reads=verified=probes=0;
   assert(locate_file_cluster(&file,target,&out)&&out==2+target);
   assert(verified<=fs.ClusterCount);++cases;
  }
  /* 文件大小元数据损坏且整条链回环，不能按 extent 数把工作预算放大。 */
  for(unsigned first=2;first<fs.ClusterCount;first+=fs.ClusterCount/2){
   store(&fs,fs.ClusterCount+1,first);
   const unsigned targets[]={fs.ClusterCount,65535,UINT32_MAX};
   for(unsigned i=0;i<3;++i){
   OS_FAT32_FILE file={.FileSystem=&fs,.FirstCluster=first,.Size=UINT32_MAX};
   UINT32 out=99;reads=verified=probes=0;
   assert(!locate_file_cluster(&file,targets[i],&out));
   assert(verified<=fs.ClusterCount&&reads<=32&&probes<=64);++cases;
   }
  }
 }
 printf("FAT seek budget: %u real FAT16/32 seek/cycle cases, valid byte locations and volume-sized work bound PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='fat-seek-budget-') as directory:
    binary = str(Path(directory)/'test')
    def run(code):
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I'+str(root/'include'), '-x', 'c', '-', '-o', binary],
                       input=prefix+actual+code+suffix, text=True, check=True)
        result = subprocess.run([binary], capture_output=True, text=True,
            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        return result
    result = run(locate)
    assert result.returncode == 0, result.stderr
    print(result.stdout, end='')
    for label, old, new in (
        ('按连续段数量而非簇数计费', 'hops += length;', '++hops;'),
        ('标准 FAT 探测超过剩余预算', 'if (probe > remaining) probe = remaining;', ''),
        ('扩展 FAT 提示超过剩余预算', 'filesystem->Fat3Available ? remaining : probe',
         'filesystem->Fat3Available ? UINT32_MAX : probe')):
        assert old in locate
        result = run(locate.replace(old,new))
        assert result.returncode != 0, label
        print('反例被拒绝：'+label)
