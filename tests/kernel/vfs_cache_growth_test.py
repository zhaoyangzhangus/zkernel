#!/usr/bin/env python3
"""Exercise the production adaptive cache index with allocation failures."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root / 'kernel/fs/vfs/internal.h').read_text()
types = header[header.index('typedef struct vfs_cached_page {'):
              header.index('static inline void vfs_state_lock(')]
source = (root / 'kernel/fs/vfs/page_cache.c').read_text()
helpers = source[source.index('static uint32_t vfs_page_cache_bucket('):
                 source.index('static void vfs_vm_mapping_retain(')]
helpers = helpers.replace('if (entry->index == page_index)',
                          '++visits; if (entry->index == page_index)')
helpers = helpers.replace('uint32_t bucket = vfs_page_cache_bucket(entry->index, new_count);',
                          '++rehash_visits; uint32_t bucket = vfs_page_cache_bucket(entry->index, new_count);')
code = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
typedef void page_t;
static unsigned allocations, failures, live, visits, rehash_visits;
static void *kzalloc(size_t n, int flags) {
 (void)flags; ++allocations;
 if (failures) { --failures; return NULL; }
 void *p=calloc(1,n); assert(p); ++live; return p;
}
static void kfree(void *p) { if(p) { assert(live); --live; free(p); } }
''' + types + helpers + r'''
static void verify(vfs_page_cache_t *cache, vfs_cached_page_t *entries, unsigned n) {
 unsigned list_count=0;
 for(vfs_cached_page_t *e=cache->head;e;e=e->next) {
  assert(e>=entries && e<entries+n); assert(++list_count<=n);
 }
 assert(list_count==n && cache->entry_count==n);
 for(unsigned i=0;i<n;++i) assert(vfs_page_cache_lookup(cache,i)==entries+i);
 assert(vfs_page_cache_lookup(cache,UINT64_MAX)==NULL);
}
int main(void) {
 vfs_page_cache_t cache={0};
 vfs_cached_page_t *entries=calloc(65536,sizeof(*entries)); assert(entries);
 assert(!vfs_page_cache_lookup(NULL,0));
 for(unsigned i=0;i<1023;++i) {
  entries[i].index=i; vfs_page_cache_insert(&cache,entries+i);
  vfs_page_cache_grow(&cache);
 }
 assert(!allocations && !cache.expanded_buckets);
 failures=2;
 for(unsigned i=1023;i<1280;++i) {
  entries[i].index=i; vfs_page_cache_insert(&cache,entries+i);
  vfs_page_cache_grow(&cache);
 }
 assert(allocations==2 && !live && cache.growth_retry_at==1536);
 verify(&cache,entries,1280);
 /* Demand + read-ahead can cross a threshold by two; odd counts must grow. */
 for(unsigned i=1280;i<65536;i+=2) {
  entries[i].index=i; vfs_page_cache_insert(&cache,entries+i);
  if(i+1<65536) {
   entries[i+1].index=i+1; vfs_page_cache_insert(&cache,entries+i+1);
  }
  vfs_page_cache_grow(&cache);
 }
 verify(&cache,entries,65536);
 assert(cache.bucket_count==16384 && live==1);
 visits=0; assert(vfs_page_cache_lookup(&cache,12345)==entries+12345);
 assert(visits<=4); printf("65536 cached pages: lookup visits=%u, buckets=%u\n",visits,cache.bucket_count);
 unsigned before=allocations; vfs_page_cache_grow(&cache); assert(allocations==before);
 kfree(cache.expanded_buckets); free(entries); assert(!live);
 /* Crossing the very first threshold with paired insertions from an odd base. */
 cache=(vfs_page_cache_t){0}; entries=calloc(1025,sizeof(*entries)); assert(entries);
 for(unsigned i=0;i<1025;++i) { entries[i].index=i; vfs_page_cache_insert(&cache,entries+i); }
 vfs_page_cache_grow(&cache); assert(cache.bucket_count==512);
 verify(&cache,entries,1025); kfree(cache.expanded_buckets); free(entries);
 /* Simulate repeated OOM while a file accumulates resident pages. Recovery
  * must select the same capacity as uninterrupted growth in one allocation
  * and one walk, including recovery beyond the maximum index capacity. */
 const unsigned recover_at[]={4096,8192,32768,65536};
 for(unsigned scenario=0;scenario<4;++scenario) {
  cache=(vfs_page_cache_t){0};
  unsigned count=recover_at[scenario];
  entries=calloc(count,sizeof(*entries));assert(entries);
  failures=100000;
  for(unsigned i=0;i<count;++i) {
   entries[i].index=i;vfs_page_cache_insert(&cache,entries+i);
   if(i+1<count)vfs_page_cache_grow(&cache);
  }
  assert(!live && !cache.expanded_buckets && !cache.bucket_count);
  assert(cache.growth_retry_at==count); /* Final insertion reaches the next retry. */
  verify(&cache,entries,count);
  failures=0;before=allocations;rehash_visits=0;
  vfs_page_cache_grow(&cache);
  unsigned expected=512;
  while(expected<16384 && count>=(uint64_t)expected*4U)expected*=2U;
  assert(cache.bucket_count==expected && allocations==before+1 && live==1);
  assert(rehash_visits==count && !cache.growth_retry_at);
  verify(&cache,entries,count);
  before=allocations;vfs_page_cache_grow(&cache);
  assert(allocations==before && rehash_visits==count);
  printf("OOM recovery: %u pages -> %u buckets, one allocation and %u rehash visits\n",
         count,cache.bucket_count,rehash_visits);
  kfree(cache.expanded_buckets);free(entries);assert(!live);
 }
 /* Recovery must also replace an already-expanded index without losing its
  * ownership or modifying it on failed allocation. */
 const unsigned initial_counts[]={1024,4096};
 for(unsigned scenario=0;scenario<2;++scenario) {
  cache=(vfs_page_cache_t){0};
  unsigned initial=initial_counts[scenario],count=initial*8U;
  entries=calloc(count,sizeof(*entries));assert(entries);
  failures=0;
  for(unsigned i=0;i<initial;++i) {
   entries[i].index=i;vfs_page_cache_insert(&cache,entries+i);
   vfs_page_cache_grow(&cache);
  }
  assert(live==1 && cache.expanded_buckets);
  vfs_cached_page_t **old_index=cache.expanded_buckets;
  unsigned old_count=cache.bucket_count;
  failures=100000;
  for(unsigned i=initial;i<count;++i) {
   entries[i].index=i;vfs_page_cache_insert(&cache,entries+i);
   if(i+1<count)vfs_page_cache_grow(&cache);
  }
  assert(live==1 && cache.expanded_buckets==old_index && cache.bucket_count==old_count);
  assert(cache.growth_retry_at==count);
  verify(&cache,entries,count);
  before=allocations;rehash_visits=0;
  vfs_page_cache_grow(&cache); /* One more failed allocation at the boundary. */
  assert(allocations==before+1 && !rehash_visits && live==1);
  assert(cache.expanded_buckets==old_index && cache.bucket_count==old_count);
  assert(cache.growth_retry_at==count+256U);
  failures=0;before=allocations;
  for(unsigned i=0;i<8;++i)vfs_page_cache_grow(&cache);
  assert(allocations==before && !rehash_visits); /* Retry throttle is retained. */
  verify(&cache,entries,count);
  /* Reach the retry threshold with real insertions, not synthetic counters. */
  vfs_cached_page_t extra[256]={0};
  for(unsigned i=0;i<256;++i) {
   extra[i].index=count+i;vfs_page_cache_insert(&cache,extra+i);
   vfs_page_cache_grow(&cache);
  }
  unsigned expected=old_count*2U;
  while(expected<16384U && count+256U>=(uint64_t)expected*4U)expected*=2U;
  assert(cache.bucket_count==expected && allocations==before+1 && live==1);
  assert(rehash_visits==count+256U && !cache.growth_retry_at);
  for(unsigned i=0;i<count;++i)assert(vfs_page_cache_lookup(&cache,i)==entries+i);
  for(unsigned i=0;i<256;++i)assert(vfs_page_cache_lookup(&cache,count+i)==extra+i);
  printf("Expanded-index OOM recovery: %u -> %u buckets, ownership and retry throttle passed\n",
         old_count,cache.bucket_count);
  kfree(cache.expanded_buckets);free(entries);assert(!live);
 }
 assert(!live); puts("Growth, OOM retry, list ownership, paired insertion and cap passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-cache-growth-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-x', 'c', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
    catch_up = '''while (new_count < 16384U &&
           cache->entry_count >= (uint64_t)new_count * 4U)
        new_count *= 2U;'''
    assert code.count(catch_up) == 1
    old_code = code.replace(catch_up, '/* Negative control: old one-step growth. */')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-x', 'c', '-', '-o', binary],
                   input=old_code, text=True, check=True)
    def disable_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    result = subprocess.run([binary], capture_output=True, text=True,
                            preexec_fn=disable_core, timeout=30)
    assert result.returncode != 0 and 'cache.bucket_count==expected' in result.stderr, result
    print('Negative control rejected: one-step growth cannot catch up after repeated OOM')
