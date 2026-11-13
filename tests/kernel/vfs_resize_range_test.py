"""Production cache resize: bounded growth visits and unchanged zeroing."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/page_cache.c').read_text()
lookup = source[source.index('static uint32_t vfs_page_cache_bucket('):
                source.index('static void vfs_vm_mapping_retain(')]
lookup = lookup.replace('if (entry->index == page_index)',
                        '++hash_visits; if (entry->index == page_index)')
resize = source[source.index('static void vfs_page_cache_zero_range('):
                source.index('void vfs_file_page_mark_dirty(')]
resize = resize.replace('vfs_page_cache_zero_range(entry, start, end, size);',
                        '++list_visits; vfs_page_cache_zero_range(entry, start, end, size);')
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096U
#define VFS_PAGE_CACHE_BUCKETS 256U
typedef struct entry { uint64_t index; void *page; bool dirty;
 struct entry *next, *hash_next; } vfs_cached_page_t;
typedef struct { vfs_cached_page_t *head, *buckets[256];
 vfs_cached_page_t **expanded_buckets; uint32_t bucket_count;
 uint64_t entry_count, growth_retry_at; } vfs_page_cache_t;
typedef struct { void *page_cache; uint64_t size; } vnode_t;
static unsigned hash_visits, list_visits, depth;
static void *kzalloc(size_t n,int f) { (void)f;return calloc(1,n); }
static void kfree(void *p) { free(p); }
static void vfs_vnode_lock(vnode_t *v) { (void)v;assert(!depth);++depth; }
static void vfs_vnode_unlock(vnode_t *v) { (void)v;assert(depth==1);--depth; }
static void *page_to_phys(void *p) { return p; }
static void *phys_to_direct(void *p) { assert(depth==1);return p; }
''' + lookup + resize + r'''
enum { N=65538, SAMPLES=68 };
static vfs_cached_page_t entries[N];
static unsigned char data[SAMPLES][PAGE_SIZE];
static uint64_t indexes[SAMPLES];
static void check(vnode_t *node,uint64_t old,uint64_t size,bool indexed,unsigned budget) {
 node->size=old;hash_visits=list_visits=0;
 for(unsigned i=0;i<N;++i) entries[i].dirty=true;
 memset(data,0x75,sizeof(data));
 vfs_page_cache_resize(node,size);
 assert(node->size==size && !depth);
 uint64_t start=size<old?size:old,end=size<old?UINT64_MAX:size;
 for(unsigned i=0;i<SAMPLES;++i) {
  uint64_t base=indexes[i]*PAGE_SIZE;
  for(unsigned j=0;j<PAGE_SIZE;++j) {
   uint64_t position=base+j;
   unsigned char expected=old!=size && position>=start && position<end?0:0x75;
   assert(data[i][j]==expected);
  }
 }
 for(unsigned i=0;i<N;++i) {
  uint64_t base=entries[i].index*PAGE_SIZE;
  bool cleared=old!=size && base>=size && base<end &&
               (base>=start || start-base<PAGE_SIZE);
  assert(entries[i].dirty==!cleared);
 }
 if(indexed) assert(!list_visits && hash_visits<=budget);
 else assert(!hash_visits && list_visits==N);
 printf("resize %llu -> %llu: hash visits=%u, list visits=%u\n",
        (unsigned long long)old,(unsigned long long)size,hash_visits,list_visits);
}
int main(void) {
 vfs_page_cache_t cache={0};vnode_t node={&cache,0};
 indexes[0]=0;
 for(unsigned i=1;i<=64;++i) indexes[i]=10000+i-1;
 indexes[65]=65535;indexes[66]=100000;indexes[67]=UINT64_MAX/PAGE_SIZE;
 for(unsigned i=0;i<N;++i) {
  entries[i].index=i<65536?i:(i==65536?100000:UINT64_MAX/PAGE_SIZE);
  for(unsigned j=0;j<SAMPLES;++j)
   if(entries[i].index==indexes[j]) entries[i].page=data[j];
  vfs_page_cache_insert(&cache,&entries[i]);vfs_page_cache_grow(&cache);
 }
 uint64_t base=10000ULL*PAGE_SIZE;
 check(&node,base+7,base+8,true,4);
 check(&node,base+7,base+17*PAGE_SIZE-5,true,68);
 check(&node,base,base,true,0);
 check(&node,base-1,base+1,true,8);
 check(&node,100000ULL*PAGE_SIZE,100032ULL*PAGE_SIZE,true,132);
 check(&node,UINT64_MAX-1,UINT64_MAX,true,8);
 check(&node,0,UINT64_MAX,false,0);
 check(&node,65536ULL*PAGE_SIZE,37,false,0);
 check(&node,37,0,false,0);
 free(cache.expanded_buckets);
 /* 稀疏/大范围扩展仍只遍历一次链表，而不是探测数百万次。 */
 cache=(vfs_page_cache_t){0};
 for(unsigned i=0;i<8;++i) {
  entries[i]=(vfs_cached_page_t){.index=i,.page=data[i]};
  vfs_page_cache_insert(&cache,&entries[i]);
 }
 node.size=0;hash_visits=list_visits=0;
 vfs_page_cache_resize(&node,1000000ULL*PAGE_SIZE);
 assert(list_visits==8 && !hash_visits && !depth);
 node.page_cache=0;node.size=0;
 vfs_page_cache_resize(&node,UINT64_MAX);assert(node.size==UINT64_MAX && !depth);
 vfs_page_cache_resize(0,1);assert(!depth);
 puts("Indexed growth, holes, shrink beyond EOF, dirty flags and overflow boundaries passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-resize-range-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
