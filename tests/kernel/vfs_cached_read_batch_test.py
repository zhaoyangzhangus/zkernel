"""真实缓存读取：跨页持锁上限、缺页解锁、EOF 与失败进度。"""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/page_cache.c').read_text()
body = source[source.index('kstatus_t vfs_read_cached_kernel('):
              source.index('static void vfs_page_cache_zero_range(')]
program = r'''
#include <assert.h>
/* 单句柄测试只桩化偏移锁；共享句柄并发由专门的回归测试覆盖。 */
#define vfs_file_position_init(file) ((void)(file))
#define vfs_file_position_lock(file) ((void)(file), K_OK)
#define vfs_file_position_unlock(file) ((void)(file))
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#define PAGE_SIZE 4096U
#define KOBJECT_TYPE_FILE 1
#define KOBJECT_TYPE_VNODE 2
#define VFS_OPEN_READ 1
enum {K_OK=0,K_EIO=-5,K_EINVAL=-22};
typedef int kstatus_t;
typedef struct {unsigned char data[PAGE_SIZE];} page_t;
typedef struct {page_t *page;} vfs_cached_page_t;
typedef struct {vfs_cached_page_t entries[64];} vfs_page_cache_t;
typedef struct {int read;} vfs_backend_node_t;
typedef struct {struct {unsigned type;} object;uint64_t size;void *fs_private,*page_cache;} vnode_t;
typedef struct {struct {unsigned type;} object;vnode_t *vnode;unsigned rights;uint64_t position;} file_t;
static page_t pages[128];
static unsigned page_stride=1;
static vfs_page_cache_t cache;
static _Thread_local bool held;
static _Thread_local unsigned locks,copies,batch_copies,gets,lookups;
static pthread_mutex_t guard=PTHREAD_MUTEX_INITIALIZER;
static int fail_page,shrink_page;
static uint64_t shrink_size;
static void vfs_vnode_lock(vnode_t *v){
 (void)v;assert(!held);assert(!pthread_mutex_lock(&guard));held=true;++locks;batch_copies=0;
}
static void vfs_vnode_unlock(vnode_t *v){
 (void)v;assert(held&&batch_copies<=16);held=false;assert(!pthread_mutex_unlock(&guard));
}
static vfs_cached_page_t *vfs_page_cache_lookup(vfs_page_cache_t *c,uint64_t i){
 assert(held&&i<64);++lookups;return c&&c->entries[i].page?&c->entries[i]:NULL;
}
static int vfs_file_page_get_batch(vnode_t *v,uint64_t i,unsigned maximum,page_t **out){
 assert(maximum>=1 && maximum<=17);
 assert(!held&&i<64);++gets;
 if((int)i==fail_page)return K_EIO;
 if((int)i==shrink_page)v->size=shrink_size;
 cache.entries[i].page=&pages[i*page_stride];*out=cache.entries[i].page;return K_OK;
}
static uintptr_t page_to_phys(page_t *p){assert(held);return (uintptr_t)p;}
static void *phys_to_direct(uintptr_t p){assert(held);return (void *)p;}
static void *copy(void *d,const void *s,size_t n){
 assert(held&&n&&n<=16*PAGE_SIZE);++copies;
 size_t offset=((uintptr_t)s-(uintptr_t)pages)%PAGE_SIZE;
 batch_copies+=(unsigned)((offset+n+PAGE_SIZE-1)/PAGE_SIZE);assert(batch_copies<=16);
 return memcpy(d,s,n);
}
#define __builtin_memcpy copy
''' + body + r'''
static atomic_bool writer_ready,writer_stop;
static void *mutate(void *arg){
 vnode_t *v=arg;unsigned generation=0;
 while(!atomic_load(&writer_stop)){
  vfs_vnode_lock(v);
  for(unsigned i=0;i<16;++i)memset(pages[i].data,(unsigned char)generation,PAGE_SIZE);
  vfs_vnode_unlock(v);++generation;atomic_store(&writer_ready,true);
 }
 return NULL;
}
int main(void){
 static unsigned char out[64*PAGE_SIZE+32],oracle[64*PAGE_SIZE];
 for(unsigned i=0;i<sizeof(oracle);++i)oracle[i]=(unsigned char)(i*17+i/4096);
 for(unsigned i=0;i<64;++i)memcpy(pages[i].data,oracle+i*PAGE_SIZE,PAGE_SIZE);
 const unsigned counts[]={1,15,16,17,31,32,33,63};
 unsigned cases=0;
 for(unsigned layout=0;layout<6;++layout)for(unsigned start=0;start<2;++start)
 for(unsigned n=0;n<sizeof(counts)/sizeof(*counts);++n)for(unsigned fault=0;fault<3;++fault){
  page_stride=layout>=3?2:1;
  for(unsigned i=0;i<64;++i){
   memcpy(pages[i*page_stride].data,oracle+i*PAGE_SIZE,PAGE_SIZE);
   cache.entries[i].page=layout%3==0||
       (layout%3==1&&i%3)?&pages[i*page_stride]:NULL;
  }
  vfs_backend_node_t backend={1};
  vnode_t v={{KOBJECT_TYPE_VNODE},sizeof(oracle),&backend,&cache};
  file_t f={{KOBJECT_TYPE_FILE},&v,VFS_OPEN_READ,start?13:0};
  unsigned initial=f.position;size_t length=counts[n]*PAGE_SIZE-initial;
  fail_page=fault==1?5:-1;shrink_page=fault==2?5:-1;shrink_size=5*PAGE_SIZE+7;
  if(fault)cache.entries[5].page=NULL;
  memset(out,0xa5,sizeof(out));locks=copies=gets=batch_copies=lookups=0;held=false;
  uint64_t expected=length;
  int status=K_OK;
  if(counts[n]>5&&fault){expected=(fault==1?5*PAGE_SIZE:shrink_size)-initial;if(fault==1)status=K_EIO;}
  uint64_t bytes=UINT64_MAX;
  assert(vfs_read_cached_kernel(&f,out+16,length,&bytes)==status);
  assert(!held&&bytes==expected&&f.position==initial+expected);
  assert(!memcmp(out+16,oracle+initial,bytes));
  for(unsigned i=0;i<16;++i)assert(out[i]==0xa5);
  for(size_t i=16+bytes;i<sizeof(out);++i)assert(out[i]==0xa5);
  if(layout%3==0&&!fault){
   assert(!gets&&locks==(counts[n]+15)/16);
   assert(copies==(layout==0?(counts[n]+15)/16:counts[n]));
   assert(lookups==counts[n]);
  }
  ++cases;
 }
 /* 混合布局每四页有一处物理间隙，借用结果后仍可继续合并相邻页。 */
 for(unsigned i=0;i<63;++i){
  cache.entries[i].page=&pages[i+i/4];
  memcpy(cache.entries[i].page->data,oracle+i*PAGE_SIZE,PAGE_SIZE);
 }
 {
  vfs_backend_node_t backend={1};
  vnode_t v={{KOBJECT_TYPE_VNODE},63*PAGE_SIZE,&backend,&cache};
  file_t f={{KOBJECT_TYPE_FILE},&v,VFS_OPEN_READ,13};uint64_t bytes;
  locks=copies=gets=batch_copies=lookups=0;
  assert(vfs_read_cached_kernel(&f,out,63*PAGE_SIZE-13,&bytes)==K_OK);
  assert(bytes==63*PAGE_SIZE-13&&!memcmp(out,oracle+13,bytes));
  assert(lookups==63&&copies==16&&locks==4&&!gets&&!held);
 }
 /* 批次内复制与修改串行；不要求跨批次形成整个文件的原子快照。 */
 page_stride=1;
 vfs_backend_node_t backend={1};
 vnode_t v={{KOBJECT_TYPE_VNODE},16*PAGE_SIZE,&backend,&cache};
 for(unsigned i=0;i<16;++i)cache.entries[i].page=&pages[i];
 pthread_t writer;assert(!pthread_create(&writer,NULL,mutate,&v));
 while(!atomic_load(&writer_ready)){}
 for(unsigned i=0;i<2000;++i){
  file_t f={{KOBJECT_TYPE_FILE},&v,VFS_OPEN_READ,0};uint64_t bytes;
  unsigned before=locks;
  assert(vfs_read_cached_kernel(&f,out,16*PAGE_SIZE,&bytes)==K_OK);
  assert(bytes==16*PAGE_SIZE&&locks==before+1);
  for(unsigned j=0;j<16*PAGE_SIZE;++j)assert(out[j]==out[0]);
 }
 atomic_store(&writer_stop,true);assert(!pthread_join(writer,NULL));
 printf("Cached read batches: %u contiguous/fragmented, warm/cold/sparse, 16-page boundaries, unaligned, fault/truncate/canary cases PASS\n",cases);
 puts("Warm contiguous 63-page read: 4 copies; fragmented: 63 copies; both retain 4 bounded lock batches PASS");
 puts("Warm fragmented 63-page read: 63 index lookups instead of 122; contiguous and fragmented each probe every page once PASS");
 puts("Mixed physical runs: 63 unaligned pages, 63 lookups, 16 copies and 4 bounded lock batches PASS");
 puts("Cached batch concurrency: 2000 whole 16-page reads vs concurrent mutation preserve one-lock coherent copies PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-cached-read-batch-') as directory:
    command = ['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-x', 'c', '-', '-o', directory + '/test']
    subprocess.run(command, input=program, text=True, check=True)
    subprocess.run([directory + '/test'], check=True, timeout=30)
    for label, old, new in (
        ('unbounded lock batch', 'batch_pages == 16U', 'batch_pages == 64U'),
        ('per-page copies', 'while (chunk < remaining &&', 'while (false && chunk < remaining &&'),
        ('repeated fragmented lookup', 'lookahead != 0 ? lookahead : vfs_page_cache_lookup(', 'lookahead != 0 && false ? lookahead : vfs_page_cache_lookup('),
    ):
        broken = program.replace(old, new)
        assert broken != program
        subprocess.run(command, input=broken, text=True, check=True)
        result = subprocess.run([directory + '/test'], capture_output=True, timeout=30,
            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        assert result.returncode == -signal.SIGABRT
        print('Negative control rejected:', label)
