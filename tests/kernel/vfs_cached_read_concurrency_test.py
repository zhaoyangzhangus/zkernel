"""Exercise production warm cached reads against real concurrent mutation."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/page_cache.c').read_text()
start = source.index('kstatus_t vfs_read_cached_kernel(')
end = source.index('\nstatic void vfs_page_cache_zero_range(', start)
body = source[start:end]
program = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define PAGE_SIZE 4096U
#define KOBJECT_TYPE_FILE 1
#define KOBJECT_TYPE_VNODE 2
#define VFS_OPEN_READ 1U
enum {K_OK=0,K_EINVAL=-22,K_EIO=-5};
typedef int kstatus_t;
typedef struct {unsigned char data[PAGE_SIZE];} page_t;
typedef struct {int read;} vfs_backend_node_t;
typedef struct {page_t *page;} vfs_cached_page_t;
typedef struct {vfs_cached_page_t entry;} vfs_page_cache_t;
typedef struct {
 struct {unsigned type;} object;
 uint64_t size;
 void *fs_private,*page_cache;
 pthread_mutex_t guard;
} vnode_t;
typedef struct {
 struct {unsigned type;} object;
 vnode_t *vnode;
 unsigned rights;
 uint64_t position;
 pthread_mutex_t position_lock;
} file_t;
static _Thread_local unsigned held,acquisitions,position_held;
static kstatus_t vfs_file_position_lock(file_t *f) {
 assert(!position_held);assert(!pthread_mutex_lock(&f->position_lock));position_held=1;return K_OK;
}
static void vfs_file_position_unlock(file_t *f) {
 assert(position_held && !held);position_held=0;assert(!pthread_mutex_unlock(&f->position_lock));
}
static atomic_uint ready,phase,mutations;
static atomic_bool stop;
static page_t page;
static vfs_backend_node_t backend={1};
static vfs_page_cache_t cache={{&page}};
static vnode_t vnode={.object={KOBJECT_TYPE_VNODE},.size=PAGE_SIZE,
 .fs_private=&backend,.page_cache=&cache,.guard=PTHREAD_MUTEX_INITIALIZER};
static void vfs_vnode_lock(vnode_t *v) {
 assert(!held); assert(!pthread_mutex_lock(&v->guard)); ++held; ++acquisitions;
}
static void vfs_vnode_unlock(vnode_t *v) {
 assert(held==1); --held; assert(!pthread_mutex_unlock(&v->guard));
}
static vfs_cached_page_t *vfs_page_cache_lookup(vfs_page_cache_t *c,uint64_t index) {
 assert(held==1 && index==0);return c?&c->entry:0;
}
static kstatus_t vfs_file_page_get_batch(vnode_t *v,uint64_t index,unsigned maximum,page_t **out) {
 (void)maximum;
 (void)v;(void)index;(void)out;
 // Every test page is resident. Entering page-get is a regression here.
 assert(!"resident read called page-get");return K_EIO;
}
static uintptr_t page_to_phys(page_t *p) {assert(held==1);return (uintptr_t)p;}
static void *phys_to_direct(uintptr_t p) {assert(held==1);return (void*)p;}
static void *guarded_copy(void *dst,const void *src,size_t size) {
 assert(position_held==1 && held==1 && size>0 && size<=PAGE_SIZE);return memcpy(dst,src,size);
}
#define __builtin_memcpy guarded_copy
''' + body + r'''
static void *writer(void *arg) {
 (void)arg;
 unsigned n=0;
 atomic_store_explicit(&ready,1,memory_order_release);
 while(!atomic_load_explicit(&stop,memory_order_acquire)) {
  vfs_vnode_lock(&vnode);
  unsigned mode=atomic_load_explicit(&phase,memory_order_relaxed);
  if(mode==0) {
   vnode.size=PAGE_SIZE;memset(page.data,(unsigned char)++n,PAGE_SIZE);
  } else {
   memset(page.data,0x5a,PAGE_SIZE);
   // Stable data, but change EOF across the reader's unlock/relock boundary.
   vnode.size=(n++&1)?127U:PAGE_SIZE;
  }
  vfs_vnode_unlock(&vnode);
  atomic_fetch_add_explicit(&mutations,1,memory_order_relaxed);
 }
 return 0;
}
int main(void) {
 pthread_t worker;assert(!pthread_create(&worker,0,writer,0));
 while(!atomic_load_explicit(&ready,memory_order_acquire)){}
 unsigned char output[PAGE_SIZE+32];
 file_t file={.object={KOBJECT_TYPE_FILE},.vnode=&vnode,.rights=VFS_OPEN_READ,
              .position_lock=PTHREAD_MUTEX_INITIALIZER};
 for(unsigned mode=0;mode<2;++mode) {
  vfs_vnode_lock(&vnode);
  atomic_store_explicit(&phase,mode,memory_order_relaxed);
  if(mode){memset(page.data,0x5a,PAGE_SIZE);vnode.size=PAGE_SIZE;}
  vfs_vnode_unlock(&vnode);
  for(unsigned iteration=0;iteration<20000;++iteration) {
   memset(output,0xa5,sizeof(output));file.position=13;
   uint64_t bytes=UINT64_MAX;acquisitions=0;
   assert(vfs_read_cached_kernel(&file,output+16,PAGE_SIZE-13,&bytes)==K_OK);
   assert(!held && !position_held && file.position==13+bytes && bytes<=PAGE_SIZE-13);
   if(mode==0) assert(bytes==PAGE_SIZE-13 && acquisitions==1);
   else assert(bytes>=114 && acquisitions<=3);
   for(size_t i=0;i<16;++i)assert(output[i]==0xa5);
   for(size_t i=0;i<bytes;++i)assert(output[16+i]==(mode?0x5a:output[16]));
   for(size_t i=16+bytes;i<sizeof(output);++i)assert(output[i]==0xa5);
  }
 }
 atomic_store_explicit(&stop,true,memory_order_release);
 assert(!pthread_join(worker,0));assert(atomic_load(&mutations)>0);
 puts("Cached reads: 40000 concurrent reads vs whole-page writes/EOF changes; "
      "one lock per full resident page, coherent guarded copies, offsets and canaries PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-cached-concurrency-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=30)
