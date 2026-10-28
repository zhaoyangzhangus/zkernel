"""Real dispatcher -> slow allocation -> local pool drain -> retry transaction.

The Buddy boundary and hardware guards are mocked; production drain traversal,
retry bounds, zone fallback and accounting execute together.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
pools = (root / 'kernel/mm/phys/percpu_page.c').read_text()
slow = (root / 'kernel/mm/phys/page_alloc.c').read_text()
drain = pools[pools.index('static void pool_drain_one('):
              pools.index('void pool_init_cpu_state(')]
dispatch = pools[pools.index('page_t *page_alloc('):pools.index('void page_free(')]
slow = slow[slow.index('PAGE_NOINLINE page_t *page_alloc_slow('):
            slow.index('PAGE_NOINLINE void page_free_non_o0(')]
program = '#include "kernel/mm/phys/internal.h"\n' + r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static page_local_pool_t pools[MAX_CPUS][PAGE_ZONE_COUNT][PAGE_POOL_ORDER_COUNT];
static page_pool_group_t groups[3];
static page_t pages[3];
static unsigned cpu,depth,initial_depth,locked,drains,freed,accounted,allocations;
static unsigned reclaim_zone,cache_owner,held_cpu=MAX_CPUS;
static bool irq_enabled,initial_irq,buddy_available,pinned;
static atomic_uint_fast64_t g_pool_reclaim_passes;
static atomic_uint_fast64_t g_pool_remote_borrows;
static unsigned char memory[PAGE_SIZE];
static uint64_t test_irq_save(void){
 uint64_t saved=irq_enabled?512:0;irq_enabled=false;return saved;
}
static void test_irq_restore(uint64_t saved){assert(!irq_enabled);irq_enabled=(saved&512)!=0;}
static void test_disable(void){++depth;}
static void test_enable(void){assert(depth);--depth;}
static uint32_t test_cpu(void){assert(depth==initial_depth+1);return cpu;}
static void test_lock(void){assert(!locked&&!irq_enabled);locked=1;}
static void test_unlock(void){assert(locked);locked=0;}
void pool_cpu_lock(uint32_t c){
 assert(c<MAX_CPUS&&!irq_enabled&&depth>=initial_depth+1&&!locked&&held_cpu==MAX_CPUS);
 held_cpu=c;
}
void pool_cpu_unlock(uint32_t c){assert(c==held_cpu&&!locked);held_cpu=MAX_CPUS;}
static page_local_pool_t *pool_for_cpu(uint32_t c,uint8_t z,uint8_t order){
 assert(c==held_cpu&&depth>=initial_depth+1&&!irq_enabled);
 return &pools[c][z][order];
}
static void pool_drain_remote(uint32_t c,uint8_t z,uint8_t o,page_local_pool_t *p){
 assert(c==held_cpu&&p==&pools[c][z][o]&&!irq_enabled&&depth>=initial_depth+1);
 if(z==0&&o==0)++drains;
}
static page_pool_group_t *pool_group_by_id(uint32_t id){return id<3?&groups[id]:0;}
static uint32_t pool_group_id(page_pool_group_t *g){return (uint32_t)(g-groups);}
static page_pool_group_t *pool_group_for_base(uint64_t p,uint8_t order){
 assert(order==0&&p>=4096&&p<4099);return &groups[p-4096];
}
static uint32_t pool_partial_pop(page_local_pool_t *p){
 unsigned id=p->partial_head;if(id==PAGE_POOL_GROUP_NONE)return id;
 p->partial_head=groups[id].partial_next;groups[id].flags=0;return id;
}
static void pool_partial_push(page_local_pool_t *p,uint32_t id,page_pool_group_t *g){
 assert(!g->flags);g->partial_next=p->partial_head;p->partial_head=id;g->flags=1;
}
static uint64_t pool_group_pinned_mask(uint64_t p,uint8_t order,uint64_t candidates){
 assert(order==0);return pinned&&p==4097?(candidates&1):0;
}
page_t *page_for_pfn(pfn_t p){assert(p>=4096&&p<4099);return &pages[p-4096];}
paddr_t page_to_phys(const page_t *p){return (paddr_t){(4096+(uint64_t)(p-pages))<<PAGE_SHIFT};}
void free_block_locked(page_t *p,uint8_t order){
 assert(locked&&!irq_enabled&&p==&pages[1]&&order==6&&!buddy_available);
 buddy_available=true;++freed;
}
page_t *pool_alloc_small(uint8_t zone,uint8_t order,uint32_t c){
 assert(c==held_cpu&&order==0&&!irq_enabled&&depth==initial_depth+1&&!locked);
 ++allocations;
 if(!buddy_available||zone!=reclaim_zone)return 0;
 buddy_available=false;return &pages[1];
}
static page_t *pool_alloc_o0_normal(uint32_t c){return pool_alloc_small(PAGE_ZONE_NORMAL,0,c);}
page_t *take_from_zone_locked(uint8_t z,uint8_t o){(void)z;(void)o;assert(0);return 0;}
void page_stats_note_alloc(uint8_t order){
 assert(!order);++accounted;
 /* Hot success is counted pinned; slow success is counted after restoration. */
 assert(depth==initial_depth||depth==initial_depth+1);
}
void *phys_to_direct(paddr_t p){assert(p.value==(4097ULL<<PAGE_SHIFT));return memory;}
static void test_zero(void *p,size_t n){
 assert(p==memory&&n==PAGE_SIZE&&depth==initial_depth&&irq_enabled==initial_irq);
 memset(p,0,n);
}
void page_free(page_t *p){(void)p;assert(0);}
#define page_pool_irq_save test_irq_save
#define page_pool_irq_restore test_irq_restore
#define x86_preempt_disable_fast test_disable
#define x86_preempt_enable_fast test_enable
#define x86_current_cpu_index_fast test_cpu
#define lock_pages test_lock
#define unlock_pages test_unlock
#define page_memory_zero test_zero
''' + drain + slow + dispatch + r'''
int main(void){
 unsigned cases=0;
 for(unsigned zero=0;zero<2;++zero)
 for(unsigned zone=0;zone<PAGE_ZONE_COUNT;++zone)
 for(unsigned irq=0;irq<2;++irq)
 for(unsigned nesting=0;nesting<4;++nesting)
 for(unsigned last_cpu=0;last_cpu<2;++last_cpu)
 for(unsigned pin=0;pin<2;++pin)
 for(unsigned remote=0;remote<2;++remote){
  cpu=last_cpu?MAX_CPUS-1:0;initial_depth=depth=nesting;
  initial_irq=irq_enabled=irq;reclaim_zone=zone;pinned=pin;
  buddy_available=false;drains=freed=accounted=allocations=locked=0;
  held_cpu=MAX_CPUS;cache_owner=remote?(cpu==0?MAX_CPUS-1:0):cpu;
  atomic_store(&g_pool_reclaim_passes,0);
  memset(pools,0,sizeof(pools));memset(groups,0,sizeof(groups));
  memset(memory,0x75,sizeof(memory));
  for(unsigned c=0;c<MAX_CPUS;++c)
   for(unsigned z=0;z<PAGE_ZONE_COUNT;++z)
    for(unsigned o=0;o<PAGE_POOL_ORDER_COUNT;++o)pools[c][z][o].partial_head=PAGE_POOL_GROUP_NONE;
  pools[cache_owner][zone][0].partial_head=0;
  for(unsigned i=0;i<3;++i){
   groups[i].base_pfn=4096+i;groups[i].owner_cpu=cache_owner;groups[i].pool_order=0;
   groups[i].partial_next=i==2?PAGE_POOL_GROUP_NONE:i+1;groups[i].flags=1;
   groups[i].free_mask=i==1?UINT64_MAX:i+1;
  }
  page_t *p=page_alloc(0,zero?PAGE_ALLOC_ZERO:0);
  assert(drains==1U+((pin||remote)?MAX_CPUS:0U));
  assert(atomic_load(&g_pool_reclaim_passes)==(unsigned)(pin||remote));
  assert(!locked&&held_cpu==MAX_CPUS&&depth==initial_depth&&irq_enabled==initial_irq);
  if(pin){assert(!p&&!freed&&!accounted);}
  else{
   assert(p==&pages[1]&&freed==1&&accounted==1&&!buddy_available);
   assert(groups[1].owner_cpu==UINT16_MAX&&groups[1].pool_order==UINT8_MAX);
  }
  for(unsigned i=0;i<PAGE_SIZE;++i)assert(memory[i]==(zero&&!pin?0:0x75));
  assert(groups[0].free_mask==1&&groups[2].free_mask==3);
  /* No unbounded retry. Hot misses add two attempts before the slow loop. */
  assert(allocations==(zero?0U:2U)+2U+(pin||remote?2U:0U)+
         ((pin?2U:remote?1U:0U)*(MAX_CPUS-1)*2U)+
         (pin||zone==PAGE_ZONE_DMA32?2U:1U));
  ++cases;
 }
 printf("Integrated reclaim: %u real dispatcher/slow/local/global drain transactions; remote CPU recovery, NORMAL/DMA32, pinned groups, bounded retry, single owner lock, exact accounting and IRQ/preemption restoration PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='page-reclaim-integration-') as directory:
    path = Path(directory)
    for negative in (False, True):
        candidate = program
        if negative:
            omitted = ('        if (PAGE_UNLIKELY(page == 0) && cpu < MAX_CPUS)\n'
                       '            return page_alloc_slow(order, flags);\n')
            assert omitted in candidate
            candidate = candidate.replace(omitted, '')
        (path / 'test.c').write_text(candidate)
        subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I', str(root), '-I', str(root / 'include'),
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        result = subprocess.run([str(path / 'test')], capture_output=negative)
        if negative:
            assert result.returncode != 0, 'old dispatcher must miss reclaimed pages'
        else:
            result.check_returncode()
print('Negative control rejected: free memory remains stranded without the dispatcher recovery path')
