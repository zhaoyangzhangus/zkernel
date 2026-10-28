#!/usr/bin/env python3
"""Exercise production range writeback and count hash-chain visits."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/page_cache.c').read_text()
lookup = source[source.index('static uint32_t vfs_page_cache_bucket('):
                source.index('static void vfs_vm_mapping_retain(')]
lookup = lookup.replace('if (entry->index == page_index)',
                        '++visits; if (entry->index == page_index)')
flush = source[source.index('static kstatus_t vfs_page_cache_flush_entry('):
               source.index('kstatus_t vfs_page_cache_flush(vnode_t')]
flush = flush.replace('if (!entry->dirty || entry->index < first_page ||',
                      '++list_visits; if (!entry->dirty || entry->index < first_page ||')
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#define PAGE_SIZE 4096U
#define VFS_PAGE_CACHE_BUCKETS 256U
typedef int kstatus_t;
enum { K_OK, K_EINVAL, K_EOVERFLOW, K_EACCES, K_EIO };
typedef struct entry { uint64_t index; void *page; bool dirty;
 struct entry *next, *hash_next; uint64_t writeback_generation; } vfs_cached_page_t;
typedef struct { vfs_cached_page_t *head, *buckets[256];
 vfs_cached_page_t **expanded_buckets; uint32_t bucket_count;
 uint64_t entry_count, growth_retry_at, writeback_generation; } vfs_page_cache_t;
static void *kzalloc(size_t n, int flags) { (void)flags; return calloc(1,n); }
static void kfree(void *p) { free(p); }
typedef struct { void *context; int (*write)(void *, uint64_t, void *, size_t, uint64_t *); } vfs_backend_node_t;
typedef struct { void *fs_private, *page_cache; uint64_t size; } vnode_t;
static unsigned visits, list_visits, writes, fail, fail_at, short_write, locks;
static uint64_t last_offset; static size_t last_length;
static void vfs_backend_lock(void *b) { (void)b; ++locks; }
static void vfs_backend_unlock(void *b) { (void)b; --locks; }
static void *page_to_phys(void *p) { return p; }
static bool vfs_page_cache_still_mapped(vfs_cached_page_t *entry) { (void)entry; return false; }
static void *phys_to_direct(void *p) { return p; }
static int write_page(void *c, uint64_t o, void *p, size_t n, uint64_t *w) {
 (void)c; (void)p; assert(locks==1); ++writes; last_offset=o; last_length=n;
 *w=short_write ? n-1 : n; return fail || writes==fail_at ? K_EIO : K_OK;
}
''' + lookup + flush + r'''
int main(void) {
 const uint64_t counts[]={0,1,15,16,17,255,256,257,512,16384,16385,UINT64_MAX};
 const uint64_t residents[]={0,1,63,64,67,68,1023,1024,65536,UINT64_MAX};
 const uint32_t buckets[]={0,256,512,16384};
 unsigned policy_cases=0;
 for(unsigned b=0;b<4;++b)for(unsigned r=0;r<10;++r)for(unsigned n=0;n<12;++n) {
  vfs_page_cache_t policy={.bucket_count=buckets[b],.entry_count=residents[r]};
  uint32_t effective=buckets[b]?buckets[b]:256;
  bool expected=counts[n]<=16 || (counts[n]<=effective && counts[n]<=residents[r]/4);
  assert(vfs_page_cache_use_range_index(&policy,counts[n])==expected);++policy_cases;
 }
 assert(policy_cases==480);
 static vfs_cached_page_t entries[65536]; vfs_page_cache_t cache={0};
 vfs_backend_node_t backend={.write=write_page};
 vnode_t vnode={&backend, &cache, 65536ULL*PAGE_SIZE};
 for(unsigned i=0;i<65536;++i) {
  vfs_cached_page_t *e=&entries[i]; e->index=i; e->dirty=true;
  vfs_page_cache_insert(&cache,e);
  vfs_page_cache_grow(&cache);
 }
 assert(vfs_page_cache_flush_locked(&vnode,12345,1)==K_OK);
 assert(writes==1 && !entries[12345].dirty && entries[12344].dirty);
 assert(visits<=4);
 printf("single-page sync: 65536 list entries -> %u hash entries inspected\n",visits);
 for(unsigned count=17;count<=1088;count*=4) {
  visits=list_visits=0;unsigned before=writes;
  assert(vfs_page_cache_flush_locked(&vnode,20000,count)==K_OK);
  unsigned dirty=count==17?count:count-count/4;
  assert(writes-before==dirty && !list_visits && visits<=count*4);
  assert(entries[19999].dirty && entries[20000+count].dirty);
  printf("%u-page sync: list visits=%u hash visits=%u\n",count,list_visits,visits);
 }
 /* Empty index buckets are valid holes, not backend writes. */
 visits=list_visits=0;unsigned hole_writes=writes;
 assert(vfs_page_cache_flush_locked(&vnode,100000,32)==K_OK);
 assert(!list_visits && writes==hole_writes && visits<=128);
 /* Indexed multi-page failure retains failed/unvisited dirty pages; retry
  * writes only the remaining range, without flushing adjacent pages. */
 fail_at=writes+5;
 assert(vfs_page_cache_flush_locked(&vnode,30000,32)==K_EIO);
 assert(!locks && !entries[30003].dirty && entries[30004].dirty && entries[30031].dirty);
 unsigned retry_writes=writes;fail_at=0;
 assert(vfs_page_cache_flush_locked(&vnode,30000,32)==K_OK);
 assert(writes-retry_writes==28 && entries[29999].dirty && entries[30032].dirty);
 fail=1; assert(vfs_page_cache_flush_locked(&vnode,12346,1)==K_EIO);
 assert(entries[12346].dirty && !locks); fail=0;
 short_write=1; assert(vfs_page_cache_flush_locked(&vnode,12346,1)==K_EIO);
 assert(entries[12346].dirty); short_write=0;
 vnode.size=12346ULL*PAGE_SIZE+17;
 assert(vfs_page_cache_flush_locked(&vnode,12346,2)==K_OK);
 assert(last_offset==12346ULL*PAGE_SIZE && last_length==17);
 assert(!entries[12346].dirty && !entries[12347].dirty);
 unsigned before=writes;
 assert(vfs_page_cache_flush_locked(&vnode,UINT64_MAX,2)==K_EOVERFLOW);
 assert(vfs_page_cache_flush_locked(&vnode,1,UINT64_MAX)==K_EINVAL);
 assert(vfs_page_cache_flush_locked(&vnode,0,0)==K_EINVAL);
 assert(writes==before);
 list_visits=0;
 assert(vfs_page_cache_flush_locked(&vnode,0,UINT64_MAX)==K_OK);
 assert(list_visits==65536);
 for(unsigned i=0;i<65536;++i) assert(!entries[i].dirty);
 assert(!locks); free(cache.expanded_buckets);
 /* A range wider than a small cache should still use one list walk. */
 cache=(vfs_page_cache_t){0};
 for(unsigned i=0;i<8;++i) {entries[i]=(vfs_cached_page_t){.index=i,.dirty=true};
  vfs_page_cache_insert(&cache,&entries[i]);}
 vnode.size=8ULL*PAGE_SIZE;list_visits=visits=0;
 assert(vfs_page_cache_flush_locked(&vnode,0,32)==K_OK);
 assert(list_visits==8 && !visits);
 puts("range isolation, EOF, errors, overflow and full flush passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-range-sync-') as directory:
    binary = str(pathlib.Path(directory) / 'test')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
