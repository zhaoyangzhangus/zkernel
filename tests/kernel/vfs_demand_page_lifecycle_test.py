"""Execute complete production page-get/cache/destroy paths with fault injection."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/page_cache.c').read_text()

def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

helpers = source[source.index('static uint32_t vfs_page_cache_bucket('):
                 source.index('static void vfs_vm_mapping_retain(')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096U
#define PAGE_OWNER_FILE 4U
#define PAGE_COMPOUND_TAIL 32U
#define KOBJECT_TYPE_VNODE 7U
#define KOBJECT_TYPE_FILE 8U
#define VFS_OPEN_READ 1U
#define VFS_PAGE_CACHE_BUCKETS 256U
#define K_OK 0
#define K_EINVAL -22
#define K_ENOMEM -12
#define K_EIO -5
typedef bool BOOLEAN;
typedef int kstatus_t;
typedef struct {uint64_t value;} paddr_t;
typedef struct page {
 unsigned owner,flags,order;bool live;
 union {struct {void *mapping;uint64_t index;} file;} u;
} page_t;
typedef struct entry {
 uint64_t index;page_t *page;bool dirty;struct entry *next,*hash_next;
} vfs_cached_page_t;
typedef struct {
 vfs_cached_page_t *head,*buckets[VFS_PAGE_CACHE_BUCKETS],**expanded_buckets;
 uint32_t bucket_count;uint64_t entry_count,growth_retry_at;
} vfs_page_cache_t;
typedef struct {uint64_t size;bool detached;void *context;
 kstatus_t (*read)(void *,uint64_t,void *,size_t,uint64_t *);
} vfs_backend_node_t;
typedef struct {struct {unsigned type;} object;void *fs_private,*page_cache;uint64_t size;} vnode_t;
typedef struct {struct {unsigned type;} object;vnode_t *vnode;unsigned rights;uint64_t position;} file_t;
static page_t pages[1024];static unsigned char data[1024][PAGE_SIZE];
static unsigned used,live_pages,live_heap,heap_calls,page_calls,reads,lock_depth;
static unsigned fail_heap_call,fail_order,read_mode,bad_tail;
static unsigned physical_queries;
static bool bad_direct;
static paddr_t paddr_make(uint64_t value){return (paddr_t){value};}
static void *kzalloc(size_t n,unsigned flags){
 assert(!flags);++heap_calls;
 if(heap_calls==fail_heap_call)return 0;
 void *p=calloc(1,n);assert(p);++live_heap;return p;
}
static void kfree(void *p){if(p){assert(live_heap);--live_heap;free(p);}}
static page_t *page_alloc(unsigned order,unsigned flags){
 assert(!flags && order<=4);++page_calls;
 if(fail_order & (1U<<order))return 0;
 unsigned count=1U<<order;assert(used+count<=1024);
 page_t *p=&pages[used];
 for(unsigned i=0;i<count;++i){
  pages[used+i]=(page_t){.live=true,.flags=i?PAGE_COMPOUND_TAIL:0};
  memset(data[used+i],0x75,PAGE_SIZE);
 }
 p->order=order;used+=count;live_pages+=count;
 if(used<1024)memset(data[used],0x3c,PAGE_SIZE);
 return p;
}
static void page_free(page_t *p){
 if(p->flags&PAGE_COMPOUND_TAIL)return;
 assert(p>=pages && p<pages+used && p->live);
 unsigned count=1U<<p->order;
 for(unsigned i=0;i<count;++i)if(p[i].live){p[i].live=false;--live_pages;}
}
static paddr_t page_to_phys(page_t *p){++physical_queries;return paddr_make((uint64_t)(p-pages)*PAGE_SIZE);}
static void *phys_to_direct(paddr_t p){assert(p.value<sizeof(data));return bad_direct?0:(char*)data+p.value;}
static page_t *phys_to_page(paddr_t p){
 if(bad_tail==1)return 0;
 if(bad_tail>=3 && p.value/PAGE_SIZE==bad_tail-3)return 0;
 assert(p.value/PAGE_SIZE<used);
 page_t *page=&pages[p.value/PAGE_SIZE];
 if(bad_tail==2)page->flags&=~PAGE_COMPOUND_TAIL;
 return page;
}
static void vfs_vnode_lock(vnode_t *v){assert(v&&!lock_depth);lock_depth=1;}
static void vfs_vnode_unlock(vnode_t *v){assert(v&&lock_depth==1);lock_depth=0;}
static void vfs_backend_lock(vfs_backend_node_t *b){assert(b&&lock_depth==1);lock_depth=2;}
static void vfs_backend_unlock(vfs_backend_node_t *b){assert(b&&lock_depth==2);lock_depth=1;}
static kstatus_t read_data(void *context,uint64_t offset,void *out,size_t n,uint64_t *bytes){
 (void)context;assert(lock_depth==2 && n<=16U*PAGE_SIZE);++reads;
 for(size_t i=0;i<n;++i)((unsigned char*)out)[i]=(unsigned char)((offset+i)*19+7);
 *bytes=read_mode==1?n-1:read_mode==2?n+1:n;
 return read_mode==3?K_ENOMEM:K_OK;
}
static kstatus_t vfs_memory_backend_read(void *c,uint64_t o,void *p,size_t n,uint64_t *b){
 return read_data(c,o,p,n,b);
}
'''
body = helpers + function('void vfs_page_cache_destroy(') + '\n' + \
    function('static kstatus_t vfs_file_page_get_batch(') + '\n' + \
    function('kstatus_t vfs_file_page_get(')
suffix = r'''
static void reset(void){
 assert(!live_pages&&!live_heap&&!lock_depth);
 if(used && used<1024)for(unsigned i=0;i<PAGE_SIZE;++i)assert(data[used][i]==0x3c);
 used=heap_calls=page_calls=reads=physical_queries=0;bad_direct=false;
 fail_heap_call=fail_order=read_mode=bad_tail=0;
}
static void check_bytes(page_t *p,uint64_t index,uint64_t size){
 unsigned char *memory=phys_to_direct(page_to_phys(p));
 uint64_t offset=index*PAGE_SIZE;
 for(unsigned i=0;i<PAGE_SIZE;++i)
  assert(memory[i]==(offset+i<size?(unsigned char)((offset+i)*19+7):0));
 assert(p->owner==PAGE_OWNER_FILE && p->u.file.index==index);
}
int main(void){
 unsigned cases=0;
 for(unsigned kind=0;kind<3;++kind){
  reset();vfs_backend_node_t b={.size=2097152,.detached=kind==2,
    .read=kind==0?vfs_memory_backend_read:read_data};
  vnode_t v={.object={KOBJECT_TYPE_VNODE},.fs_private=&b,.size=b.size};
  for(unsigned i=0;i<512;i+=2){
   page_t *p=0;assert(vfs_file_page_get(&v,i,&p)==K_OK && p);
   check_bytes(p,i,b.size);
   unsigned old_heap=heap_calls,old_pages=page_calls,old_reads=reads;
   page_t *again=0;assert(vfs_file_page_get(&v,i,&again)==K_OK && again==p);
   assert(heap_calls==old_heap&&page_calls==old_pages&&reads==old_reads);
  }
  assert(live_pages==(kind==1?512U:256U));
  assert(((vfs_page_cache_t*)v.page_cache)->entry_count==live_pages);
  for(unsigned i=1;i<512;i+=2){
   page_t *p=0;assert(vfs_file_page_get(&v,i,&p)==K_OK);check_bytes(p,i,b.size);
  }
  assert(live_pages==512 && (kind==1?reads<=36U:reads==512U));
  if(kind==1)printf("Sequential demand: 512 pages, %u device reads (fixed two-page baseline: 256)\n",reads);
  vfs_page_cache_destroy(&v);assert(!v.page_cache);reset();++cases;
 }
 /* 孤立缺页不能把每次随机访问都放大为十六页。 */
 {
  reset();vfs_backend_node_t b={.size=512U*PAGE_SIZE,.read=read_data};
  vnode_t v={.object={KOBJECT_TYPE_VNODE},.fs_private=&b,.size=b.size};
  for(unsigned i=0;i<256;i+=32){
   page_t *p=0;assert(vfs_file_page_get(&v,i,&p)==K_OK);
   check_bytes(p,i,b.size);
  }
  assert(reads==8 && live_pages==16);
  vfs_page_cache_destroy(&v);reset();++cases;
 }
 /* 已识别顺序访问仍须遵守显式读取范围；大块分配失败可缩小，短读不能发布半批页面。 */
 for(unsigned mode=0;mode<4;++mode){
  reset();vfs_backend_node_t b={.size=64U*PAGE_SIZE,.read=read_data};
  vnode_t v={.object={KOBJECT_TYPE_VNODE},.fs_private=&b,.size=b.size};page_t *first=0;
  assert(vfs_file_page_get(&v,0,&first)==K_OK && live_pages==2);
  if(mode==1)fail_order=1U<<4;
  if(mode==2)read_mode=1;
  if(mode==3)bad_tail=1;
  page_t sentinel,*out=&sentinel;
  kstatus_t status=mode==0?vfs_file_page_get_batch(&v,2,1,&out):
                         vfs_file_page_get(&v,2,&out);
  if(mode<2){
   assert(status==K_OK && live_pages==(mode==0?3U:10U));
   check_bytes(out,2,b.size);
  }else{
   assert(status==K_EIO && out==&sentinel && live_pages==2);
   assert(((vfs_page_cache_t*)v.page_cache)->entry_count==2);
   assert(!vfs_page_cache_lookup(v.page_cache,2));
  }
  assert(vfs_page_cache_lookup(v.page_cache,0)->page==first);
  vfs_page_cache_destroy(&v);reset();++cases;
 }
 /* Every heap/page/read/publication failure must keep output untouched and
  * release temporary ownership. Destroy also reclaims an empty cache. */
 for(unsigned kind=0;kind<3;++kind)for(unsigned failure=0;failure<10;++failure){
  reset();vfs_backend_node_t b={.size=8193,.detached=kind==2,
   .read=kind==0?vfs_memory_backend_read:read_data};
  vnode_t v={.object={KOBJECT_TYPE_VNODE},.fs_private=&b,.size=b.size};
  if(failure<3)fail_heap_call=failure+1;
  if(failure==3)fail_order=2; /* Optional order1 fails; demand order0 survives. */
  if(failure==4)fail_order=3;
  if(failure>=5&&failure<=7)read_mode=failure-4;
  if(failure==8)bad_tail=1;
  page_t sentinel,*out=&sentinel;
  kstatus_t status=vfs_file_page_get(&v,0,&out);
  bool device=kind==1;
  bool fails=failure<2 || failure==4 || (failure>=5&&failure<=7) ||
             (device&&failure==8);
  assert((status!=K_OK)==fails);
  kstatus_t expected=!fails?K_OK:
   (failure==5||failure==6||(device&&failure==8))?K_EIO:K_ENOMEM;
  assert(status==expected);
  if(fails){assert(out==&sentinel && !live_pages);}
  else {assert(out!=&sentinel);check_bytes(out,0,b.size);}
  assert(!lock_depth);vfs_page_cache_destroy(&v);reset();++cases;
 }
 /* A failed compound-page check must preserve a colliding resident entry,
  * both indexes and the caller's output, for null and malformed tails. */
 for(unsigned failure=1;failure<=2;++failure){
  reset();vfs_backend_node_t b={.size=259U*PAGE_SIZE,.read=vfs_memory_backend_read};
  vnode_t v={.object={KOBJECT_TYPE_VNODE},.fs_private=&b,.size=b.size};page_t *original=0;
  assert(vfs_file_page_get(&v,0,&original)==K_OK);
  vfs_page_cache_t *cache=v.page_cache;
  vfs_cached_page_t *head=cache->head;
  assert(vfs_page_cache_bucket(0,256)==vfs_page_cache_bucket(257,256));
  unsigned old_heap=live_heap,old_pages=live_pages;
  b.read=read_data;bad_tail=failure;
  page_t sentinel,*out=&sentinel;
  assert(vfs_file_page_get(&v,257,&out)==K_EIO && out==&sentinel);
  assert(cache->head==head && cache->entry_count==1 && !head->next && !head->hash_next);
  assert(vfs_page_cache_lookup(cache,0)==head && !vfs_page_cache_lookup(cache,257));
  assert(live_heap==old_heap && live_pages==old_pages && !lock_depth);
  page_t *again=0;
  assert(vfs_file_page_get(&v,0,&again)==K_OK && again==original);
  check_bytes(original,0,b.size);
  vfs_page_cache_destroy(&v);reset();++cases;
 }
 const uint64_t lengths[]={0,1,4095,4096,4097,8191,8192,8193};
 for(unsigned kind=0;kind<3;++kind)
 for(unsigned s=0;s<sizeof(lengths)/sizeof(*lengths);++s){
  reset();vfs_backend_node_t b={.size=lengths[s],.detached=kind==2,
   .read=kind==0?vfs_memory_backend_read:read_data};
  vnode_t v={.object={KOBJECT_TYPE_VNODE},.fs_private=&b,.size=b.size};page_t *out=0;
  assert(vfs_file_page_get(&v,0,&out)==K_OK);check_bytes(out,0,b.size);
  vfs_page_cache_destroy(&v);reset();++cases;
 }
 printf("PASS: %u complete page-get/cache/destroy cases; sparse RAM 256 vs device 512 physical pages, cache hits, full reads, EOF and failure cleanup\n",cases);
}
'''
def main():
 with tempfile.TemporaryDirectory(prefix='vfs-demand-page-') as directory:
    binary = str(Path(directory) / 'test')
    def compile_body(code):
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-x', 'c', '-', '-o', binary],
                       input=prefix+code+suffix, text=True, check=True)
    compile_body(body)
    subprocess.run([binary], check=True, timeout=20)
    for old, new in (
        ('backend->read != vfs_memory_backend_read &&', ''),
        ('!__atomic_load_n(&backend->detached, __ATOMIC_ACQUIRE) &&', ''),
        ('status != K_OK || bytes != read_length', 'status != K_OK'),
        ('vfs_file_page_get_batch(vnode, page_index, 0U, out)',
         'vfs_file_page_get_batch(vnode, page_index, 2U, out)'),
    ):
        assert old in body
        compile_body(body.replace(old, new))
        result = subprocess.run([binary], capture_output=True, text=True, timeout=20)
        assert result.returncode != 0 and 'Assertion' in result.stderr
    print('Negative controls rejected: RAM/snapshot over-allocation, short-read publication and fixed small demand reads')

if __name__ == '__main__':
    main()
