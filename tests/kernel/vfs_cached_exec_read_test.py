"""Production cached reads and resize: EOF, reuse, updates and zeroed regrowth."""
from pathlib import Path
import resource
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/vfs/page_cache.c').read_text()
begin=source.index('kstatus_t vfs_read_cached_kernel(')
end=source.index('void vfs_file_page_mark_dirty(',begin)
copy=source[source.index('static void vfs_page_cache_copy_bytes('):]
start=source.index('static inline bool vfs_page_cache_use_range_index(')
strategy=source[start:source.index('\n}',start)+2]
resize = source[begin:end].replace('uint64_t base = entry->index * PAGE_SIZE;',
                                 '++resize_visits; uint64_t base = entry->index * PAGE_SIZE;')
resize=resize.replace('memset(', 'liteos_memset(')
copy=copy.replace('memmove(', 'liteos_memmove(')
program=r'''
#include <assert.h>
/* 单句柄测试只桩化偏移锁；共享句柄并发由专门的回归测试覆盖。 */
#define vfs_file_position_init(file) ((void)(file))
#define vfs_file_position_lock(file) ((void)(file), K_OK)
#define vfs_file_position_unlock(file) ((void)(file))
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define PAGE_SIZE 4096U
#define KOBJECT_TYPE_FILE 1
#define KOBJECT_TYPE_VNODE 2
#define VFS_OPEN_READ 1
#define K_OK 0
#define K_EIO -5
#define K_EINVAL -22
typedef int kstatus_t;
void *liteos_memset(void *,int,size_t);
void *liteos_memmove(void *,const void *,size_t);
typedef struct { unsigned char data[4096]; } page_t;
typedef struct vfs_cached_page {
 uint64_t index;page_t *page;bool dirty;struct vfs_cached_page *next;
} vfs_cached_page_t;
typedef struct { vfs_cached_page_t *head; uint64_t entry_count; } vfs_page_cache_t;
static uint32_t vfs_page_cache_bucket_count(vfs_page_cache_t *c) { (void)c;return 256; }
typedef struct { int read; } vfs_backend_node_t;
typedef struct { uint64_t size;void *page_cache;struct {unsigned type;} object;
 void *fs_private; } vnode_t;
typedef struct { struct { unsigned type; } object;vnode_t *vnode;
 uint64_t position;unsigned rights;
} file_t;
static page_t pages[3];static vfs_cached_page_t entries[3];
static bool resident[3];static unsigned loads,depth,copies,resize_visits,locks,page_gets;
static int fail_index=-1;
static int64_t shrink_to=-1;
static bool missing_page;
static int missing_index=-1,bad_direct_index=-1;
static void vfs_vnode_lock(vnode_t *v) {
 assert(!depth);++depth;++locks;
 if(shrink_to>=0)v->size=(uint64_t)shrink_to;
}
static void vfs_vnode_unlock(vnode_t *v) { (void)v;assert(depth==1);--depth; }
static uintptr_t page_to_phys(page_t *page) { return (uintptr_t)page; }
static bool vfs_page_cache_still_mapped(vfs_cached_page_t *entry) {(void)entry;return false;}
static void *phys_to_direct(uintptr_t address) {
 if(bad_direct_index>=0 && address==(uintptr_t)&pages[bad_direct_index])return 0;
 return (void*)address;
}
static vfs_cached_page_t *vfs_page_cache_lookup(vfs_page_cache_t *cache,uint64_t index) {
 // Fault injection models a missing cache entry, not a failing resident hit.
 if(!cache || (int)index==fail_index || missing_page || (int)index==missing_index)return 0;
 for(vfs_cached_page_t *e=cache->head;e;e=e->next)if(e->index==index)return e;
 return 0;
}
static kstatus_t vfs_file_page_get_batch(vnode_t *v,uint64_t index,unsigned maximum,page_t **out) {
 (void)maximum;
 assert(!depth && index<3);++page_gets;if((int)index==fail_index)return K_EIO;
 if(shrink_to>=0) v->size=(uint64_t)shrink_to;
 if(missing_page || (int)index==missing_index) { *out=0;return K_OK; }
 if(!resident[index]) {
  ++loads;resident[index]=true;
  for(unsigned j=0;j<PAGE_SIZE;++j)pages[index].data[j]=(unsigned char)(index*PAGE_SIZE+j);
  entries[index]=(vfs_cached_page_t){index,&pages[index],false,((vfs_page_cache_t*)v->page_cache)->head};
  ((vfs_page_cache_t*)v->page_cache)->head=&entries[index];
 }
 *out=&pages[index];return K_OK;
}
static void *checked_copy(void *destination,const void *source,size_t length) {
 assert(depth==1 && length>0 && length<=PAGE_SIZE);
 uintptr_t address=(uintptr_t)source;bool bounded=false;
 for(unsigned i=0;i<3;++i) {
  uintptr_t first=(uintptr_t)pages[i].data;
  if(address>=first && address-first<PAGE_SIZE &&
     length<=PAGE_SIZE-(address-first)) bounded=true;
 }
 assert(bounded);++copies;return memcpy(destination,source,length);
}
#define __builtin_memcpy checked_copy
''' + strategy+resize+copy+r'''
int main(void) {
 vfs_backend_node_t backend={.read=1};
 vfs_page_cache_t cache={0};vnode_t node={.size=9000,.page_cache=&cache,
  .object={KOBJECT_TYPE_VNODE},.fs_private=&backend};
 file_t file={.object={KOBJECT_TYPE_FILE},.vnode=&node,.position=13,.rights=VFS_OPEN_READ};
 unsigned char out[10000];uint64_t got=123;
 assert(vfs_read_cached_kernel(&file,out,6000,&got)==K_OK && got==6000 && loads==2 && copies==2);
 for(unsigned i=0;i<6000;++i)assert(out[i]==(unsigned char)(13+i));
 file.position=13;locks=page_gets=0;
 assert(vfs_read_cached_kernel(&file,out,6000,&got)==K_OK && loads==2);
 assert(locks==1 && page_gets==0); // 连续常驻页共享一次有界保护。
 unsigned char update[]={9,8,7};vfs_page_cache_copy_in(&node,31,update,3);
 file.position=31;assert(vfs_read_cached_kernel(&file,out,3,&got)==K_OK && !memcmp(out,update,3));
 file.position=8990;assert(vfs_read_cached_kernel(&file,out,100,&got)==K_OK && got==10);
 assert(vfs_read_cached_kernel(&file,out,100,&got)==K_OK && got==0);
 file.position=UINT64_MAX;assert(vfs_read_cached_kernel(&file,out,100,&got)==K_OK && got==0);
 file.position=0;assert(vfs_read_cached_kernel(&file,out,0,&got)==K_OK && got==0);
 memset(out,0x69,sizeof(out));file.position=4090;
 assert(vfs_read_cached_kernel(&file,out+17,1000,&got)==K_OK && got==1000);
 for(unsigned i=0;i<17;++i)assert(out[i]==0x69);
 for(unsigned i=0;i<1000;++i)assert(out[i+17]==(unsigned char)(4090+i));
 for(unsigned i=1017;i<sizeof(out);++i)assert(out[i]==0x69);
 shrink_to=4093;file.position=4090;
 assert(vfs_read_cached_kernel(&file,out,1000,&got)==K_OK && got==3 && file.position==4093);
 shrink_to=4090;node.size=9000;file.position=4090;
 assert(vfs_read_cached_kernel(&file,out,1000,&got)==K_OK && got==0 && file.position==4090);
 shrink_to=-1;node.size=9000;missing_page=true;file.position=0;
 assert(vfs_read_cached_kernel(&file,out,1,&got)==K_EIO && got==0 && !depth);
 missing_page=false;
 vfs_page_cache_resize(&node,37);
 resize_visits=0;vfs_page_cache_resize(&node,37);
 assert(!resize_visits && !depth);
 for(unsigned i=37;i<PAGE_SIZE;++i)assert(pages[0].data[i]==0);
 for(unsigned i=0;i<PAGE_SIZE;++i)assert(pages[1].data[i]==0 && pages[2].data[i]==0);
 vfs_page_cache_resize(&node,9000);
 file.position=37;assert(vfs_read_cached_kernel(&file,out,8963,&got)==K_OK && got==8963);
 for(unsigned i=0;i<got;++i)assert(out[i]==0);
 assert(pages[0].data[31]==9);vfs_page_cache_resize(&node,9000);assert(pages[0].data[31]==9);
 fail_index=1;file.position=0;
 assert(vfs_read_cached_kernel(&file,out,6000,&got)==K_EIO && got==4096 && file.position==4096);
 fail_index=-1;
 assert(vfs_read_cached_kernel(&file,out,6000,&got)==K_OK && !depth);
 const unsigned starts[]={0,13,4090};
 for(unsigned kind=0;kind<3;++kind)for(unsigned failed=0;failed<3;++failed)
 for(unsigned s=0;s<3;++s) {
  fail_index=kind==0?(int)failed:-1;
  missing_index=kind==1?(int)failed:-1;
  bad_direct_index=kind==2?(int)failed:-1;
  file.position=starts[s];got=UINT64_MAX;memset(out,0x69,sizeof(out));
  unsigned progress=failed?failed*PAGE_SIZE-starts[s]:0;
  assert(vfs_read_cached_kernel(&file,out,9000-starts[s],&got)==K_EIO);
  assert(got==progress && file.position==starts[s]+progress && !depth);
  for(unsigned i=0;i<progress;++i) {
   unsigned position=starts[s]+i;
   assert(out[i]==pages[position/PAGE_SIZE].data[position%PAGE_SIZE]);
  }
  for(unsigned i=progress;i<sizeof(out);++i)assert(out[i]==0x69);
 }
 fail_index=missing_index=bad_direct_index=-1;
 // Invalid page-get state must still fail on a resident hit, without progress.
 for(unsigned invalid=0;invalid<3;++invalid) {
  file.position=0;got=123;memset(out,0x69,sizeof(out));
  node.object.type=invalid==0?0:KOBJECT_TYPE_VNODE;
  node.fs_private=invalid==1?0:&backend;backend.read=invalid==2?0:1;
  assert(vfs_read_cached_kernel(&file,out,1,&got)==K_EINVAL && !got && !file.position && !depth);
  assert(out[0]==0x69);
 }
 node.object.type=KOBJECT_TYPE_VNODE;node.fs_private=&backend;backend.read=1;
 // A broken resident entry must not masquerade as a cache miss or copy data.
 entries[0].page=0;file.position=0;got=123;locks=page_gets=0;
 assert(vfs_read_cached_kernel(&file,out,1,&got)==K_EIO && !got && !file.position);
 assert(locks==1 && page_gets==0 && !depth);
 entries[0].page=&pages[0];
 /* Cache writes span page boundaries without touching surrounding bytes. */
 memset(out,0x75,4096);vfs_page_cache_copy_in(&node,4090,out,4096);
 assert(pages[0].data[4089]==0);
 for(unsigned i=4090;i<PAGE_SIZE;++i) assert(pages[0].data[i]==0x75);
 for(unsigned i=0;i<4090;++i) assert(pages[1].data[i]==0x75);
 assert(pages[1].data[4090]==0);
 for(unsigned direction=0;direction<2;++direction) {
  for(unsigned i=0;i<PAGE_SIZE;++i) pages[0].data[i]=(unsigned char)i;
  unsigned from=direction?1:0,to=direction?0:1;
  memcpy(out,pages[0].data,PAGE_SIZE);memmove(out+to,out+from,PAGE_SIZE-1);
  vfs_page_cache_copy_in(&node,to,pages[0].data+from,PAGE_SIZE-1);
  assert(!memcmp(out,pages[0].data,PAGE_SIZE) && !depth);
 }
 file.rights=0;assert(vfs_read_cached_kernel(&file,out,1,&got)==K_EINVAL);
 assert(vfs_read_cached_kernel(0,out,1,&got)==K_EINVAL);
 vfs_page_cache_resize(&node,0);assert(node.size==0 && !depth);
 puts("Cached exec reads: resident pages use one lock and no page-get; cold reads, validation, writes, EOF, error progress, shrink/regrowth and lock ownership passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-cached-exec-') as directory:
 binary=str(Path(directory)/'test')
 obj=str(Path(directory)/'string.o')
 subprocess.run(['cc','-O2','-fno-builtin','-mno-sse','-mno-mmx','-fno-tree-vectorize',
                 '-I'+str(root/'include'),'-Dmemcpy=liteos_memcpy',
                 '-Dmemmove=liteos_memmove','-Dmemset=liteos_memset','-Dmemcmp=liteos_memcmp',
                 '-c',str(root/'kernel/string.c'),'-o',obj],check=True)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                 '-fsanitize=address,undefined','-fno-pie','-no-pie',
                 '-x','c','-','-x','none',obj,'-o',binary],input=program,text=True,check=True)
 subprocess.run([binary],check=True)
 # A green byte comparison alone would also accept the old repeated lookup.
 # Reject removal of the warm path with the actual lock/page-get assertions.
 guard='vfs_cached_page_t *cached = vfs_page_cache_lookup(\n            (vfs_page_cache_t *)vnode->page_cache, position / PAGE_SIZE);'
 assert guard in program
 negative=program.replace(guard,'vfs_cached_page_t *cached = 0;')
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                 '-fsanitize=address,undefined','-fno-pie','-no-pie',
                 '-x','c','-','-x','none',obj,'-o',binary],input=negative,text=True,check=True)
 result=subprocess.run([binary],capture_output=True,text=True,timeout=10,
                       preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
 assert result.returncode != 0 and 'locks==1 && page_gets==0' in result.stderr,result.stderr
 print('Negative control rejected: redundant resident page-get/lock cycle')
