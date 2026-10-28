"""Real inbox and pool reclamation under controlled producer interleavings."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/phys/percpu_page.c').read_text()
by_id = source[source.index('static page_pool_group_t *pool_group_by_id('):
               source.index('static page_pool_group_t *pool_group_for_base(')]
lists = source[source.index('static void pool_partial_push('):
               source.index('static void pool_drain_remote(')]
remote = source[source.index('static void pool_drain_remote('):
                source.index('static PAGE_NOINLINE page_t *pool_alloc_slow(')]
drain = source[source.index('static void pool_drain_one('):
               source.index('void pool_reclaim_all(')]
remote = remote.replace('atomic_exchange_explicit(&group->remote_mask, 0,',
                        'exchange_with_producer(&group->remote_mask, 0,')
code = '#include "kernel/mm/phys/internal.h"\n' + r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static page_pool_group_t groups[2];
page_pool_group_t *g_pool_groups=groups;
uint32_t g_pool_group_count=2;
page_remote_heads_t g_remote_heads[MAX_CPUS];
static page_local_pool_t pools[PAGE_ZONE_COUNT][PAGE_POOL_ORDER_COUNT];
static page_t pages[128];
static unsigned freed,locked;
static bool irq_enabled,publication_complete,inject_producer;
static bool pool_held;
void pool_cpu_lock(uint32_t cpu){assert(!cpu&&!irq_enabled&&!pool_held);pool_held=true;}
void pool_cpu_unlock(uint32_t cpu){assert(!cpu&&!irq_enabled&&pool_held);pool_held=false;}
static uint64_t irq_save(void){uint64_t old=irq_enabled?512:0;irq_enabled=false;return old;}
static void irq_restore(uint64_t old){assert(!irq_enabled);irq_enabled=(old&512)!=0;}
static page_local_pool_t *pool_for_cpu(uint32_t cpu,uint8_t zone,uint8_t order){
 assert(!cpu&&!irq_enabled&&pool_held);return &pools[zone][order];
}
paddr_t page_to_phys(const page_t *page){return (paddr_t){(uint64_t)(page-pages)<<PAGE_SHIFT};}
page_t *page_for_pfn(pfn_t pfn){assert(pfn<128);return &pages[pfn];}
static page_pool_group_t *pool_group_for_base(uint64_t pfn,uint8_t order){
 assert(!order&&pfn%64==0&&pfn<128);return &groups[pfn/64];
}
static uint64_t pool_group_pinned_mask(uint64_t pfn,uint8_t order,uint64_t mask){
 (void)pfn;(void)mask;assert(!order);return 0;
}
static void test_lock(void){assert(!locked&&!irq_enabled);locked=1;}
static void test_unlock(void){assert(locked);locked=0;}
void free_block_locked(page_t *page,uint8_t order){
 assert(locked&&page==pages&&order==6&&publication_complete);++freed;
}
''' + by_id + lists + r'''
/* Producer 2 arrives immediately after the consumer takes the first mask,
 * before it applies those bits. Its new node must survive for the next drain. */
static uint64_t exchange_with_producer(atomic_uint_fast64_t *mask,uint64_t value,
                                      memory_order order){
 uint64_t bits=atomic_exchange_explicit(mask,value,order);
 if(inject_producer){
  inject_producer=false;
  assert(atomic_fetch_or_explicit(mask,1ULL<<7,memory_order_acq_rel)==0);
  pool_remote_notify(0,PAGE_ZONE_NORMAL,0,0,&groups[0]);
 }
 return bits;
}
''' + remote + r'''
#define page_pool_irq_save irq_save
#define page_pool_irq_restore irq_restore
#define lock_pages test_lock
#define unlock_pages test_unlock
''' + drain + r'''
static void reset(bool active,bool two_bits,bool irq){
 memset(groups,0,sizeof(groups));memset(pools,0,sizeof(pools));
 freed=locked=0;irq_enabled=irq;publication_complete=inject_producer=false;
 for(unsigned z=0;z<PAGE_ZONE_COUNT;++z)
  for(unsigned o=0;o<PAGE_POOL_ORDER_COUNT;++o){
   pools[z][o].partial_head=PAGE_POOL_GROUP_NONE;
   atomic_store(&g_remote_heads[0].head[z][o],PAGE_POOL_GROUP_NONE);
  }
 for(unsigned i=0;i<2;++i){
  groups[i].base_pfn=i*64;groups[i].owner_cpu=0;groups[i].pool_order=0;
  groups[i].zone=PAGE_ZONE_NORMAL;
  groups[i].remote_next=groups[i].partial_next=PAGE_POOL_GROUP_NONE;
 }
 uint64_t missing=(1ULL<<3)|(two_bits?(1ULL<<7):0);
 page_local_pool_t *pool=&pools[PAGE_ZONE_NORMAL][0];
 if(active){pool->base=pages;pool->free_mask=UINT64_MAX&~missing;}
 else{groups[0].free_mask=UINT64_MAX&~missing;pool_partial_push(pool,0,&groups[0]);}
}
int main(void){
 unsigned cases=0;
 for(unsigned irq=0;irq<2;++irq)
 for(unsigned active=0;active<2;++active)
 for(unsigned two=0;two<2;++two){
  reset(active,two,irq);
  /* Producer 1 pauses between bitmap publication and inbox publication. */
  assert(atomic_fetch_or(&groups[0].remote_mask,1ULL<<3)==0);
  if(two)assert(atomic_fetch_or(&groups[0].remote_mask,1ULL<<7)!=0);
  pool_drain_cpu(0);
  assert(!freed&&!locked&&irq_enabled==(bool)irq&&groups[0].owner_cpu==0);
  assert(atomic_load(&groups[0].remote_mask)==((1ULL<<3)|(two?(1ULL<<7):0)));
  /* Resume the sole notifier. The group can now be consumed and recycled. */
  publication_complete=true;pool_remote_notify(0,PAGE_ZONE_NORMAL,0,0,&groups[0]);
  pool_drain_cpu(0);
  assert(freed==1&&groups[0].owner_cpu==UINT16_MAX&&!locked);
  assert(!atomic_load(&groups[0].remote_mask));
  assert(atomic_load(&g_remote_heads[0].head[PAGE_ZONE_NORMAL][0])==PAGE_POOL_GROUP_NONE);
  pool_drain_cpu(0);assert(freed==1&&irq_enabled==(bool)irq);++cases;
 }
 for(unsigned irq=0;irq<2;++irq){
  reset(false,true,irq);publication_complete=true;
  atomic_store(&groups[0].remote_mask,1ULL<<3);
  pool_remote_notify(0,PAGE_ZONE_NORMAL,0,0,&groups[0]);inject_producer=true;
  pool_drain_cpu(0);
  assert(!freed&&atomic_load(&groups[0].remote_mask)==(1ULL<<7));
  assert(atomic_load(&g_remote_heads[0].head[PAGE_ZONE_NORMAL][0])==0);
  pool_drain_cpu(0);
  assert(freed==1&&!atomic_load(&groups[0].remote_mask));
  assert(atomic_load(&g_remote_heads[0].head[PAGE_ZONE_NORMAL][0])==PAGE_POOL_GROUP_NONE);
  assert(irq_enabled==(bool)irq);++cases;
 }
 printf("Remote publication: %u real inbox/drain/reclaim transactions; delayed notifier, coalesced producer, active/parked pool, requeue during consumption, exact recycle and IRQ restoration PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='pool-remote-publish-') as directory:
    path = Path(directory)
    for negative in (False, True):
        candidate = code
        if negative:
            candidate = candidate.replace('uint64_t bits = group->free_mask;',
                'uint64_t bits = group->free_mask | atomic_exchange(&group->remote_mask, 0); group->free_mask = bits;')
            candidate = candidate.replace('group->free_mask |= pool->free_mask;',
                'group->free_mask |= pool->free_mask | atomic_exchange(&group->remote_mask, 0);')
        (path / 'test.c').write_text(candidate)
        subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I', str(root), '-I', str(root / 'include'),
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        result = subprocess.run([str(path / 'test')], capture_output=negative)
        if negative:
            assert result.returncode != 0, 'consuming unpublished bits must fail'
        else:
            result.check_returncode()
print('Negative control rejected: premature bitmap consumption recycles a group before its notifier finishes')
