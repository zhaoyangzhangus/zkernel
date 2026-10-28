"""串联真实 VFS、FAT 和块缓存，对照逐页读取与动态批量读取的设备请求。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
io = (root / 'kernel/fs/vfs/file_io.c').read_text()
read = io[io.index('static kstatus_t vfs_read_backend_chunk('):
          io.index('kstatus_t vfs_read_kernel(')]
fat = (root / 'kernel/fs/nativefs/file_lifecycle.c').read_text()
fat = fat[fat.index('static UINT64 fat32_contiguous_extent_bytes('):fat.index('static BOOLEAN fat32_write_locked(')]
prefix = r'''
#include <assert.h>
/* 非缓存测试只桩化读取合并；映射一致性由单独的真实代码回归测试覆盖。 */
#define vfs_page_cache_present(vnode) ((void)(vnode), false)
#define vfs_page_cache_buffer_alias(vnode,buffer,length) \
    ((void)(vnode),(void)(buffer),(void)(length),false)
#define vfs_backend_consistent_read_lock(backend) ((void)(backend), K_OK)
#define vfs_backend_consistent_read_unlock(backend) ((void)(backend))
#define vfs_page_cache_overlay_read(vnode,offset,buffer,length) \
    ((void)(vnode),(void)(offset),(void)(buffer),(void)(length),K_OK)
/* 单句柄测试只桩化偏移锁；共享句柄并发由专门的回归测试覆盖。 */
#define vfs_file_position_init(file) ((void)(file))
#define vfs_file_position_lock(file) ((void)(file), K_OK)
#define vfs_file_position_unlock(file) ((void)(file))
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/fat32.h>
#define __user
enum {KOBJECT_TYPE_FILE=1,VFS_OPEN_READ=1};
typedef struct {void *context;uint64_t size;
 kstatus_t (*read)(void *,uint64_t,void *,size_t,uint64_t *);} vfs_backend_node_t;
typedef struct {void *fs_private;} vnode_t;
typedef struct {struct {unsigned type;} object;vnode_t *vnode;unsigned rights;uint64_t position;} file_t;
static unsigned requests,sectors,backend_calls,held,allocations,live;
static bool oom,fragmented;
static void *kmalloc(size_t n,unsigned flags){
 (void)flags;assert(!held&&n<=65536);++allocations;if(oom)return NULL;
 void *p=malloc(n);assert(p);++live;return p;
}
static void kfree(void *p){assert(p&&live&&!held);--live;free(p);}
static bool x86_user_range_valid(const void *p,size_t n){return p||!n;}
static void vfs_backend_lock(vfs_backend_node_t *b){(void)b;assert(!held);held=1;}
static void vfs_backend_unlock(vfs_backend_node_t *b){(void)b;assert(held);held=0;}
static void vfs_read_kernel_yield(void){assert(!held);}
static int copy_to_user(void *d,const void *s,size_t n){assert(!held);memcpy(d,s,n);return K_OK;}
static BOOLEAN disk_read(VOID *ctx,UINT64 lba,UINT32 count,VOID *out){
 (void)ctx;++requests;sectors+=count;
 for(unsigned i=0;i<count*512U;++i)((unsigned char *)out)[i]=(unsigned char)((lba*512U+i)*17U+(lba+i/512U));
 return 1;
}
static BOOLEAN disk_write(VOID *ctx,UINT64 lba,UINT32 count,const VOID *in){
 (void)ctx;(void)lba;(void)count;(void)in;return 1;
}
static VOID fat32_diag_failure(const char *tag,UINT64 a,UINT64 b,UINT64 c){
 (void)tag;(void)a;(void)b;(void)c;assert(false);
}
static UINT64 fat32_cluster_lba(const OS_FAT32 *fs,UINT32 cluster){
 return fs->DataStartLba+(UINT64)(cluster-2U)*fs->SectorsPerCluster;
}
static BOOLEAN locate_file_cluster(OS_FAT32_FILE *file,UINT32 logical,UINT32 *physical){
 unsigned extent=fragmented?2U:16U;
 file->CursorValid=1;file->CursorLogicalStart=logical/extent*extent;
 file->CursorPhysicalStart=2U+(logical/extent)*(fragmented?4U:16U);
 file->CursorLength=extent;*physical=file->CursorPhysicalStart+logical%extent;return 1;
}
static BOOLEAN fat32_read_sector(const OS_FAT32 *fs,UINT64 lba,UINT8 *out){
 return block_cache_read((OS_BLOCK_CACHE *)&fs->Cache,lba,out);
}
static UINT32 fat32_extent_length(const OS_FAT32 *fs,UINT32 cluster,UINT32 maximum){
 (void)fs;(void)cluster;(void)maximum;return 1U;
}
''' + fat + r'''
static kstatus_t read_backend(void *ctx,uint64_t offset,void *out,size_t size,uint64_t *bytes){
 assert(held);++backend_calls;UINT32 count=0;
 if(!fat32_read(ctx,offset,out,(UINT32)size,&count))return K_EIO;
 *bytes=count;return K_OK;
}
'''
main = r'''
int main(void){
 static unsigned char output[2*65536+513];
 OS_BLOCK_DEVICE disk={.Registered=1,.BlockSize=512,.BlockCount=4096,.Read=disk_read,.Write=disk_write};
 const unsigned lengths[]={128*1024U,16384U,65536U+13U};
 for(unsigned f=0;f<2;++f)for(unsigned n=0;n<3;++n)for(unsigned offset=0;offset<2;++offset){
  fragmented=f;requests=sectors=backend_calls=allocations=0;
  OS_FAT32 fs={.Device=&disk,.BytesPerSector=512,.SectorsPerCluster=8,.DataStartLba=64};
  assert(block_cache_init(&fs.Cache,&disk));
  OS_FAT32_FILE fat_file={.FileSystem=&fs,.Used=1,.Size=3*65536};
  vfs_backend_node_t backend={&fat_file,fat_file.Size,read_backend};vnode_t v={&backend};
  file_t file={{KOBJECT_TYPE_FILE},&v,VFS_OPEN_READ,offset?13:0};
  uint64_t initial=file.position,bytes=0;memset(output,0xcc,sizeof(output));
  assert(vfs_read(&file,output+1,lengths[n],&bytes)==K_OK);
  assert(bytes==lengths[n]&&file.position==initial+bytes&&!live&&!held);
  assert(output[0]==0xcc&&output[1+bytes]==0xcc);
  for(unsigned i=0;i<bytes;++i){
   uint64_t position=initial+i,logical=position/4096U;
   unsigned extent=fragmented?2U:16U;
   uint64_t physical=(logical/extent)*(fragmented?4U:16U)+logical%extent;
   uint64_t disk_position=64U*512U+physical*4096U+position%4096U;
   assert(output[i+1U]==(unsigned char)(disk_position*17U+disk_position/512U));
  }
  printf("%u %u %u %u %u %u\n",f,lengths[n],offset,backend_calls,requests,sectors);
  assert(block_cache_destroy(&fs.Cache));
 }
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-fat-user-read-') as directory:
    results = []
    for old in (True, False):
        binary = str(Path(directory) / ('page' if old else 'batch'))
        body = read
        if old:
            body = body.replace('if (chunk > capacity) chunk = capacity;',
                                '(void)capacity; if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;')
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I'+str(root/'include'), '-x', 'c', '-',
                        str(root/'kernel/block/bio.c'), str(root/'kernel/fs/pagecache/cache.c'),
                        '-o', binary], input=prefix+body+main, text=True, check=True)
        results.append([list(map(int, line.split())) for line in
                        subprocess.check_output([binary], text=True, timeout=30).splitlines()])
    for before, after in zip(*results):
        assert before[:3] == after[:3]
        assert after[3] < before[3] and after[4] <= before[4], (before, after)
    before, after = results[0][0], results[1][0]
    assert before[3] == 32 and after[3] == 2 and after[4] == 2
    print(f'VFS/FAT/cache: 12 contiguous/fragmented/unaligned fixtures match disk oracle; '
          f'128KiB backend calls {before[3]} -> {after[3]}, device reads {before[4]} -> {after[4]} PASS')
