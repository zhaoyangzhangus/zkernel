"""Run actual truncate, RAM backing and cache resize under writeback failure.

Discarding all bytes must not allocate/write them first. A failed backend
truncate must leave both the dirty cache and its old backing intact.
"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
io = (root / 'kernel/fs/vfs/file_io.c').read_text()
pages = (root / 'kernel/fs/vfs/page_cache.c').read_text()
backend = (root / 'kernel/fs/vfs/backend.c').read_text()


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


cache_helpers = pages[pages.index('static uint32_t vfs_page_cache_bucket('):
                      pages.index('static void vfs_page_cache_grow(')]
# Insertion/growth are not involved; use the real bucket lookup and geometry.
cache_helpers += function(pages, 'static vfs_cached_page_t *vfs_page_cache_lookup(')
cache_helpers += pages[pages.index('static kstatus_t vfs_page_cache_flush_entry('):
                       pages.index('kstatus_t vfs_page_cache_sync(')]
cache_helpers += pages[pages.index('static void vfs_page_cache_zero_range('):
                       pages.index('void vfs_file_page_mark_dirty(')]
ram = backend[backend.index('struct {\n    uint64_t failures;'):
              backend.index('kstatus_t vfs_fat32_backend_read(')]
truncate = '\n'.join(function(io, signature) for signature in (
    'static kstatus_t vfs_write_transaction_begin(',
    'static void vfs_write_transaction_end(',
    'kstatus_t vfs_truncate_node('))
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096U
#define VFS_PAGE_CACHE_BUCKETS 4U
#define KOBJECT_TYPE_VNODE 7U
enum {K_OK=0,K_EINVAL=-22,K_ENOMEM=-12,K_EIO=-5,K_EISDIR=-21,
      K_EACCES=-13,K_EAGAIN=-11,K_EOVERFLOW=-75};
typedef int kstatus_t;
typedef struct {unsigned unused;} kmutex_t;
typedef struct page {unsigned char memory[PAGE_SIZE];} page_t;
typedef struct entry {
 uint64_t index;page_t *page;bool dirty;struct entry *next,*hash_next;uint64_t writeback_generation;
} vfs_cached_page_t;
typedef struct {
 vfs_cached_page_t *head,*buckets[VFS_PAGE_CACHE_BUCKETS],**expanded_buckets;
 uint32_t bucket_count;uint64_t entry_count,growth_retry_at,writeback_generation;
} vfs_page_cache_t;
typedef struct {
 uint8_t *data;uint64_t size,data_capacity;bool directory;void *context;
 kmutex_t write_mutex;
 kstatus_t (*write)(void *,uint64_t,const void *,size_t,uint64_t *);
 kstatus_t (*truncate)(void *,uint64_t);
} vfs_backend_node_t;
typedef struct {
 struct {unsigned type;} object;void *fs_private;uint64_t size;void *page_cache;
} vnode_t;
static unsigned transaction_lock,vnode_lock,backend_lock;
static unsigned allocations,frees,writes,truncates,stats;
static int write_error,truncate_error,lock_error;
static bool boot;
static void *kmalloc(size_t n,unsigned flags){
 assert(!flags);++allocations;void *p=malloc(n);assert(p);return p;
}
static void *kzalloc(size_t n,unsigned flags){void *p=kmalloc(n,flags);if(p)memset(p,0,n);return p;}
static void kfree(void *p){if(p){++frees;free(p);}}
static bool kmalloc_try_grow(void *p,size_t n){(void)p;(void)n;return false;}
static kstatus_t page_memory_get_stats(uint64_t *t,uint64_t *f){
 ++stats;*t=1048576;*f=0;return K_OK;
}
static void *sched_current_thread(void){return boot?NULL:(void*)1;}
static int kmutex_try_lock(kmutex_t *m){
 assert(m && !transaction_lock && !vnode_lock && !backend_lock);
 if(lock_error)return K_EAGAIN;transaction_lock=1;return K_OK;
}
static int kmutex_lock(kmutex_t *m,uint64_t timeout){
 assert(m && timeout==UINT64_MAX && lock_error && !transaction_lock);return lock_error;
}
static int kmutex_unlock(kmutex_t *m){
 assert(m && transaction_lock && !vnode_lock && !backend_lock);transaction_lock=0;return K_OK;
}
static int io_mutex_lock(kmutex_t *m){
 if(!sched_current_thread())return K_OK;
 int status=kmutex_try_lock(m);return status==K_EAGAIN?kmutex_lock(m,UINT64_MAX):status;
}
static void io_mutex_unlock_required(kmutex_t *m){
 if(sched_current_thread())assert(kmutex_unlock(m)==K_OK);
}
static void vfs_vnode_lock(vnode_t *v){
 assert(v && !vnode_lock && !backend_lock && (transaction_lock || boot));vnode_lock=1;
}
static void vfs_vnode_unlock(vnode_t *v){assert(v && vnode_lock && !backend_lock);vnode_lock=0;}
static void vfs_backend_lock(vfs_backend_node_t *b){
 assert(b && !backend_lock && (transaction_lock || boot));backend_lock=1;
}
static void vfs_backend_unlock(vfs_backend_node_t *b){assert(b && backend_lock);backend_lock=0;}
static void *page_to_phys(page_t *p){return p->memory;}
static bool vfs_page_cache_still_mapped(vfs_cached_page_t *entry){(void)entry;return false;}
static void *phys_to_direct(void *p){return p;}
''' + ram + cache_helpers + truncate + r'''
static int write_backend(void *context,uint64_t offset,const void *data,size_t n,uint64_t *bytes){
 assert(backend_lock && vnode_lock);++writes;
 return write_error?write_error:vfs_memory_backend_write(context,offset,data,n,bytes);
}
static int truncate_backend(void *context,uint64_t size){
 assert(backend_lock && !vnode_lock);++truncates;
 return truncate_error?truncate_error:vfs_memory_backend_truncate(context,size);
}
int main(void){
 const uint64_t old_size=3U*PAGE_SIZE-17U;
 const uint64_t sizes[]={0,1,PAGE_SIZE-1U,PAGE_SIZE,PAGE_SIZE+1U,
                         2U*PAGE_SIZE-1U,2U*PAGE_SIZE,2U*PAGE_SIZE+1U,
                         old_size,old_size+17U,old_size+34U};
 const int errors[]={K_OK,K_ENOMEM,K_EIO};
 unsigned cases=0;
 for(unsigned early=0;early<2;++early)for(unsigned mask=0;mask<8;++mask)
 for(unsigned s=0;s<sizeof(sizes)/sizeof(sizes[0]);++s)
 for(unsigned w=0;w<3;++w)for(unsigned t=0;t<3;++t){
  boot=early;write_error=truncate_error=lock_error=0;
  allocations=frees=writes=truncates=0;
  vfs_backend_node_t b={0};b.context=&b;b.write=write_backend;b.truncate=truncate_backend;
  assert(vfs_memory_backend_truncate(&b,old_size)==K_OK);
  memset(b.data,0x11,old_size);
  uint8_t *old=b.data;uint64_t old_capacity=b.data_capacity;
  page_t data[3];vfs_cached_page_t entries[3];vfs_page_cache_t cache={0};
  for(unsigned i=0;i<3;++i){
   memset(data[i].memory,0x80+i,PAGE_SIZE);
   entries[i]=(vfs_cached_page_t){.index=i,.page=&data[i],.dirty=(mask&(1U<<i))!=0,
                                .next=cache.head};
   cache.head=&entries[i];cache.buckets[vfs_page_cache_bucket(i,4)]=&entries[i];++cache.entry_count;
  }
  vnode_t v={{KOBJECT_TYPE_VNODE},&b,old_size,&cache};
  write_error=errors[w];truncate_error=errors[t];unsigned before=allocations;
  int status=vfs_truncate_node(&v,sizes[s]);
  assert(!transaction_lock && !vnode_lock && !backend_lock);
  if(!sizes[s]){
   assert(!writes && allocations==before && truncates==1);
   assert(status==truncate_error);
   if(status==K_OK){
    assert(!b.data && !b.size && !b.data_capacity && !v.size && frees==1);
    for(unsigned i=0;i<3;++i){
     assert(!entries[i].dirty);
     for(unsigned j=0;j<PAGE_SIZE;++j)assert(data[i].memory[j]==0);
    }
   }else{
    assert(b.data==old && b.size==old_size && b.data_capacity==old_capacity && v.size==old_size && !frees);
    for(unsigned j=0;j<old_size;++j)assert(b.data[j]==0x11);
    for(unsigned i=0;i<3;++i){
     assert(entries[i].dirty==((mask&(1U<<i))!=0));
     for(unsigned j=0;j<PAGE_SIZE;++j)assert(data[i].memory[j]==0x80+i);
    }
   }
  }else if(sizes[s]==old_size){
   assert(status==truncate_error && !writes && allocations==before && truncates==1);
   assert(b.data==old && b.size==old_size && b.data_capacity==old_capacity && v.size==old_size && !frees);
   for(unsigned j=0;j<old_size;++j)assert(b.data[j]==0x11);
   for(unsigned i=0;i<3;++i){
    assert(entries[i].dirty==((mask&(1U<<i))!=0));
    for(unsigned j=0;j<PAGE_SIZE;++j)assert(data[i].memory[j]==0x80+i);
   }
  }else{
   unsigned retained=0,runs=0;bool previous=false;
   for(unsigned i=0;i<3;++i){
    bool current=(uint64_t)i*PAGE_SIZE<sizes[s] && (mask&(1U<<i));
    if(current){++retained;if(!previous)++runs;}
    previous=current;
   }
   int expected=(retained && write_error)?write_error:truncate_error;
   assert(status==expected && truncates==(unsigned)!(retained && write_error));
   /* 连续的脏页现在共用一次后端调用，数据与失败保留语义不变。 */
   assert(writes==(retained && write_error?1:runs));
   if(status==K_OK){assert(b.size==sizes[s] && v.size==sizes[s]);}
   else assert(b.size==old_size && v.size==old_size);
   /* Check actual retained bytes, not just backend call counts: dirty edge
    * pages must persist their prefix before EOF changes. Failed writeback
    * must leave the backing unchanged, and successful growth exposes zeros. */
   for(uint64_t j=0;j<b.size;++j){
    unsigned i=j/PAGE_SIZE;
    bool persisted=j<old_size && (uint64_t)i*PAGE_SIZE<sizes[s] &&
                   (mask&(1U<<i)) && !write_error;
    assert(b.data[j]==(persisted?0x80+i:(j<old_size?0x11:0)));
   }
   for(unsigned i=0;i<3;++i)if((uint64_t)i*PAGE_SIZE<sizes[s]){
    assert(entries[i].dirty==((mask&(1U<<i))!=0 && write_error!=K_OK));
    for(unsigned j=0;j<PAGE_SIZE;++j){
     uint64_t offset=(uint64_t)i*PAGE_SIZE+j;
     bool zeroed=status==K_OK && (sizes[s]<old_size ? offset>=sizes[s] :
                                  offset>=old_size && offset<sizes[s]);
     assert(data[i].memory[j]==(zeroed?0:0x80+i));
    }
   }
   for(unsigned i=0;i<3;++i)if((uint64_t)i*PAGE_SIZE>=sizes[s]){
    assert(entries[i].dirty==(status!=K_OK && (mask&(1U<<i))!=0));
    for(unsigned j=0;j<PAGE_SIZE;++j)
     assert(data[i].memory[j]==(status==K_OK?0:0x80+i));
   }
  }
  write_error=truncate_error=0;
  assert(vfs_memory_backend_truncate(&b,0)==K_OK);
  assert(frees==allocations);++cases;
 }
 /* Lock/backend validation failures do not touch data or call writeback. */
 boot=false;vfs_backend_node_t b={.truncate=truncate_backend};
 vnode_t v={{KOBJECT_TYPE_VNODE},&b,0,NULL};
 writes=truncates=0;lock_error=K_EIO;
 assert(vfs_truncate_node(&v,0)==K_EIO && !writes && !truncates);
 assert(vfs_truncate_node(NULL,0)==K_EINVAL);
 v.object.type=0;assert(vfs_truncate_node(&v,0)==K_EINVAL);v.object.type=KOBJECT_TYPE_VNODE;
 b.directory=true;assert(vfs_truncate_node(&v,0)==K_EISDIR);b.directory=false;
 b.truncate=NULL;assert(vfs_truncate_node(&v,0)==K_EACCES);
 assert(!transaction_lock && !vnode_lock && !backend_lock && !stats);
 printf("PASS: %u truncate/RAM/cache transactions; prefix-only writeback, discarded-page failure preservation, unchanged EOF, backend errors and boot/lock cleanup\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-truncate-discard-') as directory:
    binary = str(Path(directory) / 'test')
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-Wno-misleading-indentation', '-fsanitize=address,undefined',
               '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary]
    def run(code):
        subprocess.run(command, input=code, text=True, check=True)
        return subprocess.run([binary], capture_output=True, text=True, timeout=20,
                              preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    result = run(program)
    assert result.returncode == 0, result.stderr
    print(result.stdout, end='')
    for name, old, new in (
        # Entering the prefix helper with size zero is harmless now: it is
        # explicitly a no-op. Inject actual writeback to test the regression,
        # rather than requiring rejection of a behaviorally equivalent guard.
        ('discard unnecessarily flushes dirty pages', 'if (size != 0U) {',
         'if (size == 0U) {\n'
         '    vfs_vnode_lock(vnode);\n'
         '    status = vfs_page_cache_flush(vnode);\n'
         '    vfs_vnode_unlock(vnode);\n'
         '    if (status != K_OK) {\n'
         '        vfs_write_transaction_end(backend, locked);\n'
         '        return status;\n'
         '    }\n'
         '}\nif (size != 0U) {'),
        ('unchanged EOF unnecessarily flushes dirty pages',
         'size == vnode->size ? K_OK :\n            vfs_page_cache_flush_prefix(vnode, size)',
         'vfs_page_cache_flush_prefix(vnode, size)'),
        ('discarded suffix unnecessarily written back',
         'vfs_page_cache_flush_prefix(vnode, size);',
         'vfs_page_cache_flush(vnode);'),
        ('failed backend truncate destroys cache contents',
         'if (status == K_OK) {\n        vfs_page_cache_resize(vnode, size);',
         'if (true) {\n        vfs_page_cache_resize(vnode, size);'),
    ):
        negative = program.replace(old, new)
        assert negative != program, name
        result = run(negative)
        assert result.returncode != 0 and 'Assertion' in result.stderr, (name, result)
        print('Negative control rejected:', name)
