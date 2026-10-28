"""Actual allocation paths: one payload fill, unchanged poison/slack/guards."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/mm/kmalloc.c').read_text()
layout = source[source.index('#define KMEM_MAGIC'):source.index('static void bytes_fill(')]
fill = source[source.index('static void bytes_fill('):source.index('static void cache_lock(')]
fill = fill.replace('__builtin_memset(memory, value, size)', 'tracked_fill(memory, value, size)')
headers = source[source.index('static uint16_t class_index_for_size('):source.index('static kmem_magazine_t *current_magazine(')]
guards = source[source.index('static uint64_t allocation_cookie('):source.index('static slab_page_t *create_slab(')]
allocate = source[source.index('static void *activate_payload('):source.index('bool kmalloc_try_grow(')]
take = source[source.index('static void *cache_take_payload_locked('):source.index('static bool magazine_refill(')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define CACHELINE_SIZE 64
#define MAX_CPUS 1
#define PAGE_SIZE 4096
#define PAGE_SHIFT 12
#define BUDDY_MAX_ORDER 18
#define PAGE_OWNER_SLAB 7
typedef struct { unsigned owner; } page_t;
typedef struct { atomic_uint state; } spinlock_t;
''' + layout + r'''
static unsigned preempt, locked, errors, mode, hits, caller_preempt;
static unsigned allocation_attempts,last_order;
static bool fail_page, fail_direct, available;
static uint8_t slot[4096] __attribute__((aligned(64)));
static uint8_t arena[1024*1024] __attribute__((aligned(4096)));
static page_t page;
static slab_page_t slab;
static uintptr_t tracked_start;
static size_t tracked_size, filled;
static void tracked_fill(void *p,uint8_t v,size_t n) {
 if(n>2048)assert(preempt==caller_preempt && !locked);
 uintptr_t begin=(uintptr_t)p,end=begin+n;
 uintptr_t low=begin>tracked_start?begin:tracked_start;
 uintptr_t high=end<tracked_start+tracked_size?end:tracked_start+tracked_size;
 if(high>low) filled+=high-low;
 memset(p,v,n);
}
''' + fill + headers + guards + r'''
static void record_error(void) { ++errors; }
static uint32_t x86_current_cpu_index(void) { return 0; }
static void sched_preempt_disable(void) { ++preempt; }
static void sched_preempt_enable(void) { assert(preempt && !locked);--preempt; }
static void initialize_allocator(void) { assert(preempt); }
static kmem_magazine_t *current_magazine(uint16_t i,uint32_t cpu) {
 assert(preempt&&cpu==0);return mode>=2?0:&g_magazines[0].classes[i];
}
static bool magazine_refill(uint16_t i,kmem_magazine_t *m) {
 assert(preempt);(void)i;if(!available)return false;
 m->head=header_payload((allocation_header_t*)slot);m->count=1;return true;
}
static void record_fastpath_hit(uint32_t cpu) { assert(cpu==0);++hits; }
static void cache_lock(kmem_cache_t *c) { (void)c;assert(preempt && !locked);++locked; }
static void cache_unlock(kmem_cache_t *c) { (void)c;assert(locked==1);--locked; }
static bool spinlock_try_lock(spinlock_t *lock) {
 unsigned expected=0;return atomic_compare_exchange_strong(&lock->state,&expected,1U);
}
static void spinlock_unlock(spinlock_t *lock) {
 assert(atomic_load(&lock->state)==1U);atomic_store(&lock->state,0U);
}
static slab_page_t *cache_partial_head(kmem_cache_t *c) { return c->slabs; }
static slab_page_t *create_slab(uint16_t i) { (void)i;return available?&slab:0; }
static void cache_partial_insert(kmem_cache_t *c,slab_page_t *s) { c->slabs=s; }
static void cache_partial_remove_head(kmem_cache_t *c,slab_page_t *s) {
 assert(c->slabs==s);c->slabs=0;
}
static page_t *page_alloc(unsigned order,unsigned flags) {
 assert(preempt==caller_preempt && !locked && !flags);++allocation_attempts;last_order=order;if(fail_page)return 0;
 assert(((size_t)PAGE_SIZE<<order)<=sizeof(arena));return &page;
}
static void page_free(page_t *p) { assert(p==&page); }
static uintptr_t page_to_phys(page_t *p) { assert(p==&page);return (uintptr_t)arena; }
static void *phys_to_direct(uintptr_t p) { return fail_direct?0:(void*)p; }
''' + take + allocate + r'''
static void prepare(unsigned c,size_t size) {
 memset(g_magazines,0,sizeof(g_magazines));memset(g_caches,0,sizeof(g_caches));
 memset(slot,0x69,sizeof(slot));memset(arena,0x69,sizeof(arena));
 allocation_header_t *h=(void*)slot;h->magic=KMEM_MAGIC;h->state=KMEM_STATE_FREE;
 void *payload=header_payload(h);memset(payload,KMEM_POISON_FREE,class_capacity(c));
 *(void**)payload=0;
 slab=(slab_page_t){.free_list=payload,.free_count=1};
 if(mode==3) g_caches[c].slabs=&slab;
 if(mode==0) {g_magazines[0].classes[c].head=payload;g_magazines[0].classes[c].count=1;}
 tracked_start=(uintptr_t)payload;tracked_size=size;filled=0;available=true;
}
int main(void) {
 unsigned trailer_cases=0;
 for(unsigned offset=0;offset<64;++offset)for(unsigned pattern=0;pattern<64;++pattern) {
  uint8_t buffer[96];memset(buffer,0x59,sizeof(buffer));
  uint64_t cookie=UINT64_C(0x0102030405060708)*pattern;
  write_trailer(buffer,offset,cookie);
  for(unsigned i=0;i<sizeof(buffer);++i)
   assert(buffer[i]==(i>=offset && i<offset+8?
       (uint8_t)(cookie>>((i-offset)*8)):0x59));
  assert(trailer_valid(buffer,offset,cookie));
  for(unsigned byte=0;byte<8;++byte)for(unsigned bit=0;bit<8;++bit) {
   buffer[offset+byte]^=(uint8_t)(1U<<bit);
   assert(!trailer_valid(buffer,offset,cookie));
   buffer[offset+byte]^=(uint8_t)(1U<<bit);
  }
  ++trailer_cases;
 }
 assert(trailer_cases==4096);
 unsigned cases=0;
 for(mode=0;mode<4;++mode) for(unsigned c=0;c<KMEM_CLASS_COUNT;++c) {
  size_t first=c?class_capacity(c-1)+1:1;
  for(size_t size=first;size<=class_capacity(c);++size) for(unsigned zero=0;zero<2;++zero) {
   prepare(c,size);unsigned before=errors;
   uint8_t *p=zero?kzalloc(size,0x1234):kmalloc(size,0x1234);
   assert(p==header_payload((allocation_header_t*)slot) && filled==size);
   for(size_t j=0;j<size;++j)assert(p[j]==(zero?0:KMEM_POISON_ALLOC));
   allocation_header_t *h=payload_header(p);
   assert(h->flags==0x34 && h->state==KMEM_STATE_ALLOCATED && h->requested==size);
   assert(header_redzones_valid(h,p) && errors==before && !preempt && !locked);
   for(size_t j=size+KMEM_TRAILER_SIZE;j<class_capacity(c);++j)assert(p[j]==KMEM_POISON_ALLOC);
   p[size]^=1;assert(!header_redzones_valid(h,p));
   ++cases;
  }
 }
 /* Free poison corruption must still be detected before zeroing it away. */
 mode=0;prepare(0,17);slot[KMEM_PAYLOAD_OFFSET+9]^=1;
 unsigned before=errors;assert(kzalloc(17,0));assert(errors==before+1);
 for(caller_preempt=0;caller_preempt<4;++caller_preempt)
 for(unsigned zero=0;zero<2;++zero) {
  preempt=caller_preempt;
  const size_t sizes[]={2048,4096,5000,65536,512*1024};
  for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
   prepare(0,1);tracked_start=(uintptr_t)arena+KMEM_PAYLOAD_OFFSET;
   tracked_size=sizes[i];filled=0;
   uint8_t *p=zero?kzalloc(sizes[i],0x42):kmalloc(sizes[i],0x42);
   assert(p==(void*)tracked_start && filled==sizes[i]);
   for(size_t j=0;j<sizes[i];++j)assert(p[j]==(zero?0:KMEM_POISON_ALLOC));
   assert(header_redzones_valid(payload_header(p),p) && preempt==caller_preempt && !locked);
  }
 }
 caller_preempt=preempt=0;
 prepare(0,1);mode=2;available=false;assert(!kzalloc(1,0) && !preempt && !locked);
 fail_page=true;assert(!kzalloc(5000,0) && !preempt);fail_page=false;
 fail_direct=true;assert(!kzalloc(5000,0) && !preempt);fail_direct=false;
 /* Use the old loop as an independent size/order oracle. Fail the backing
  * allocation deliberately, so upper boundary cases need no huge buffers. */
 fail_page=true;
 unsigned order_cases=0;
 for(unsigned kind=0;kind<3;++kind) {
  unsigned count=kind==0?65536U:kind==1?33U*5U:10000U;
  uint64_t seed=19;
  for(unsigned index=0;index<count;++index) {
   uint64_t size;
   if(kind==0)size=2000ULL+index*67ULL;
   else if(kind==1) {
    uint64_t boundary=1ULL<<(index/5U);
    unsigned variant=index%5U;
    size=variant==0?boundary:variant==1?boundary-1:variant==2?boundary+1:
         variant==3?(boundary>KMEM_PAYLOAD_OFFSET+KMEM_TRAILER_SIZE?
         boundary-KMEM_PAYLOAD_OFFSET-KMEM_TRAILER_SIZE:1):
         boundary+1-KMEM_PAYLOAD_OFFSET-KMEM_TRAILER_SIZE;
   } else {
    seed=seed*UINT64_C(6364136223846793005)+1;size=seed;
   }
   if(size<=class_capacity(KMEM_CLASS_COUNT-1U))continue;
   unsigned expected=0;
   bool allowed=size<=UINT32_MAX;
   if(allowed) {
    uint64_t needed=size+KMEM_PAYLOAD_OFFSET+KMEM_TRAILER_SIZE;
    while(expected<=BUDDY_MAX_ORDER && (PAGE_SIZE*(1ULL<<expected))<needed)++expected;
    allowed=expected<=BUDDY_MAX_ORDER;
   }
   unsigned before=allocation_attempts;
   assert(!kmalloc((size_t)size,0) && !preempt);
   assert(allocation_attempts-before==(unsigned)allowed);
   if(allowed)assert(last_order==expected);
   ++order_cases;
  }
 }
 fail_page=false;
 assert(!kzalloc(0,0) && !kmalloc(0,0));assert(!kzalloc(UINT64_MAX,0) && !preempt);
 printf("%u slab activation cases and %u large-order oracle cases: magazine hit/refill/cache, single fill, poison/slack/guards; large and failure paths passed\n",cases,order_cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-kzalloc-init-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-variable',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True)
    old = ('    sched_preempt_enable();\n'
           '    return large ? allocate_large(size, flags, zero) : result;')
    new = ('    if (large) result = allocate_large(size, flags, zero);\n'
           '    sched_preempt_enable();\n    return result;')
    negative=code.replace(old,new)
    assert negative!=code
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-variable',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True,text=True,
                          preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode!=0 and 'Assertion' in result.stderr
    print('Negative control rejected: large allocation holds an extra preemption guard')
