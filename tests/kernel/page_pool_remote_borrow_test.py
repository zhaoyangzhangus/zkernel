"""Actual pressure allocation, remote borrowing and original-owner free route."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
pools = (root / 'kernel/mm/phys/percpu_page.c').read_text()
slow = (root / 'kernel/mm/phys/page_alloc.c').read_text()
borrow = pools[pools.index('page_t *pool_alloc_remote('):pools.index('void pool_init_cpu_state(')]
allocate = slow[slow.index('PAGE_NOINLINE page_t *page_alloc_slow('):
                slow.index('PAGE_NOINLINE void page_free_non_o0(')]
notify = pools[pools.index('static void pool_remote_notify('):pools.index('static void pool_drain_remote(')]
free_slow = pools[pools.index('static PAGE_NOINLINE bool pool_free_slow('):
                  pools.index('static bool pool_page_in_local_group(')]
free_small = pools[pools.index('static bool pool_page_in_local_group('):
                   pools.index('static PAGE_NOINLINE void pool_free_o0_slow(')]
program = '#include "kernel/mm/phys/internal.h"\n' + r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static page_t page;
static page_pool_group_t group;
static page_local_pool_t local[MAX_CPUS][PAGE_ZONE_COUNT][PAGE_POOL_ORDER_COUNT];
page_remote_heads_t g_remote_heads[MAX_CPUS];
static atomic_uint_fast64_t g_pool_remote_borrows;
static unsigned requester,owner,requested_order,available_zone,depth,outer;
static unsigned held=MAX_CPUS,local_drains,global_drains,accounted,tries;
static bool irq_enabled,initial_irq;
static unsigned char bytes[PAGE_SIZE*32+32];
static uint64_t irq_save(void){uint64_t saved=irq_enabled?512:0;irq_enabled=false;return saved;}
static void irq_restore(uint64_t saved){assert(!irq_enabled&&held==MAX_CPUS&&depth==outer);irq_enabled=(saved&512)!=0;}
static void disable(void){++depth;}
static void enable(void){assert(depth==outer+1&&held==MAX_CPUS);--depth;}
static uint32_t current_cpu(void){assert(depth==outer+1);return requester;}
void pool_cpu_lock(uint32_t cpu){assert(cpu<MAX_CPUS&&held==MAX_CPUS&&!irq_enabled&&depth==outer+1);held=cpu;}
void pool_cpu_unlock(uint32_t cpu){assert(held==cpu);held=MAX_CPUS;}
page_t *pool_alloc_small(uint8_t zone,uint8_t order,uint32_t cpu){
 assert(cpu==held&&order==requested_order&&!irq_enabled);++tries;
 if(cpu!=owner||zone!=available_zone||!group.free_mask)return 0;
 group.free_mask=0;page.order=order;page.zone=zone;atomic_store(&page.refs,1);return &page;
}
void pool_drain_cpu(uint32_t cpu){assert(cpu==requester&&held==MAX_CPUS);++local_drains;}
void pool_reclaim_all(void){assert(held==MAX_CPUS);++global_drains;}
page_t *take_from_zone_locked(uint8_t zone,uint8_t order){(void)zone;(void)order;assert(0);return 0;}
void page_stats_note_alloc(uint8_t order){assert(order==requested_order&&depth==outer&&irq_enabled==initial_irq&&held==MAX_CPUS);++accounted;}
paddr_t page_to_phys(const page_t *p){assert(p==&page);return (paddr_t){4096ULL<<PAGE_SHIFT};}
void *phys_to_direct(paddr_t p){assert(p.value==(4096ULL<<PAGE_SHIFT));return bytes+16;}
static void zero_memory(void *p,size_t n){assert(p==bytes+16&&n==((size_t)PAGE_SIZE<<requested_order));memset(p,0,n);}
void page_free(page_t *p){(void)p;assert(0);}
static page_local_pool_t *pool_for_cpu(uint32_t cpu,uint8_t zone,uint8_t order){assert(cpu==held);return &local[cpu][zone][order];}
static uint64_t pool_group_base_for_pfn(uint64_t p,uint8_t order){return p&~((64ULL<<order)-1);}
static page_pool_group_t *pool_group_for_base(uint64_t p,uint8_t order){assert(p==4096&&order==requested_order);return &group;}
static uint32_t pool_group_id(page_pool_group_t *g){assert(g==&group);return 0;}
static void pool_install_base(page_local_pool_t *p,uint64_t base,uint8_t order){(void)p;(void)base;(void)order;assert(0);}
static void pool_partial_push(page_local_pool_t *p,uint32_t id,page_pool_group_t *g){(void)p;(void)id;(void)g;assert(0);}
#define page_pool_irq_save irq_save
#define page_pool_irq_restore irq_restore
#define x86_preempt_disable_fast disable
#define x86_preempt_enable_fast enable
#define x86_current_cpu_index_fast current_cpu
#define page_memory_zero zero_memory
#define lock_pages() assert(0)
#define unlock_pages() assert(0)
''' + borrow + allocate + notify + free_slow + free_small + r'''
int main(void){
 unsigned cases=0;
 for(unsigned order=0;order<=PAGE_POOL_SMALL_MAX_ORDER;++order)
 for(unsigned flags=0;flags<4;++flags)
 for(unsigned zone=0;zone<PAGE_ZONE_COUNT;++zone)
 for(unsigned irq=0;irq<2;++irq)
 for(unsigned nested=0;nested<4;++nested)
 for(unsigned last=0;last<2;++last){
  requester=last?MAX_CPUS-1:0;owner=last?0:MAX_CPUS-1;
  requested_order=order;available_zone=zone;initial_irq=irq_enabled=irq;outer=depth=nested;
  held=MAX_CPUS;local_drains=global_drains=accounted=tries=0;
  memset(&group,0,sizeof(group));memset(bytes,0x75,sizeof(bytes));
  group.base_pfn=4096;group.owner_cpu=owner;group.pool_order=order;group.zone=zone;
  group.free_mask=1;group.remote_next=PAGE_POOL_GROUP_NONE;
  atomic_store(&g_pool_remote_borrows,0);
  atomic_store(&g_remote_heads[owner].head[zone][order],PAGE_POOL_GROUP_NONE);
  page_t *result=page_alloc_slow(order,flags);
  bool allowed=!(flags&PAGE_ALLOC_DMA32)||zone==PAGE_ZONE_DMA32;
  assert(local_drains==1&&global_drains==(unsigned)!allowed&&depth==outer&&irq_enabled==initial_irq&&held==MAX_CPUS);
  assert(accounted==(unsigned)allowed&&atomic_load(&g_pool_remote_borrows)==(unsigned)allowed);
  assert(group.owner_cpu==owner&&group.pool_order==order);
  if(allowed){
   assert(result==&page&&!group.free_mask&&atomic_load(&page.refs)==1);
   for(size_t i=0;i<((size_t)PAGE_SIZE<<order);++i)assert(bytes[16+i]==((flags&PAGE_ALLOC_ZERO)?0:0x75));
   for(unsigned i=0;i<16;++i)assert(bytes[i]==0x75&&bytes[16+((size_t)PAGE_SIZE<<order)+i]==0x75);
   uint64_t saved=irq_save();disable();pool_cpu_lock(requester);
   assert(pool_free_small(result,order,requester));
   pool_cpu_unlock(requester);enable();irq_restore(saved);
   assert(!atomic_load(&page.refs)&&!group.free_mask&&atomic_load(&group.remote_mask)==1);
   assert(atomic_load(&g_remote_heads[owner].head[zone][order])==0);
  }else{assert(!result&&group.free_mask==1&&!atomic_load(&group.remote_mask));}
  assert(tries<=6+2*(MAX_CPUS-1));++cases;
 }
 assert(!pool_alloc_remote(PAGE_POOL_SMALL_MAX_ORDER+1,0,0));
 assert(!pool_alloc_remote(0,0,MAX_CPUS));
 printf("Remote borrow: %u real slow-path/borrow/free transactions; partial pools, original ownership, atomic inbox return, all small orders, DMA32 limits, zeroing/canaries, exact accounting and IRQ/preemption restoration PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='pool-remote-borrow-') as directory:
    path = Path(directory)
    for negative in (False, True):
        candidate = program
        if negative:
            candidate = candidate.replace('page = pool_alloc_remote(order, flags, cpu);',
                                          'page = 0; /* old policy: no remote borrow */')
        (path / 'test.c').write_text(candidate)
        subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I', str(root), '-I', str(root / 'include'),
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        result = subprocess.run([str(path / 'test')], capture_output=negative)
        if negative:
            assert result.returncode != 0, 'partial remote pools must remain usable'
        else:
            result.check_returncode()
print('Negative control rejected: whole-group reclaim alone cannot use free slots in partial remote pools')
