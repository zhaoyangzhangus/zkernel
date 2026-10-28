"""Actual bounded local/global recovery against an explicit unrolled model."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/phys/page_alloc.c').read_text()
function = source[source.index('PAGE_NOINLINE page_t *page_alloc_slow('):source.index('PAGE_NOINLINE void page_free_non_o0(')]
program = '#include "' + str(root / 'kernel/mm/phys/internal.h') + '"\n' + r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static page_t pages[2];
static unsigned availability,phase,cpu,order_now,initial_depth,depth,locked,pool_held;
static bool initial_irq,irq_enabled,mapping;
static char trace[128];static unsigned trace_count;
static size_t zero_bytes;
static void event(char c) {assert(trace_count+1<sizeof(trace));trace[trace_count++]=c;trace[trace_count]=0;}
static uint64_t test_irq_save(void) {
    event('S');uint64_t flags=irq_enabled?1ULL<<9:0;irq_enabled=false;return flags;
}
static void test_irq_restore(uint64_t flags) {
    assert(!irq_enabled&&depth==initial_depth);event('T');irq_enabled=(flags&(1ULL<<9))!=0;
}
static void test_disable(void) {event('D');++depth;}
static void test_enable(void) {assert(depth==initial_depth+1);event('E');--depth;}
static uint32_t test_cpu(void) {assert(depth==initial_depth+1);event('C');return cpu;}
static void test_lock(void) {assert(!locked&&depth==initial_depth);event('L');locked=1;}
static void test_unlock(void) {assert(locked);event('U');locked=0;}
static page_t *result(unsigned zone) {
    assert(zone<PAGE_ZONE_COUNT&&phase<3);
    return availability&(1U<<(phase*2+zone))?&pages[zone]:0;
}
page_t *pool_alloc_small(uint8_t zone,uint8_t order,uint32_t owner) {
    assert(!irq_enabled&&depth==initial_depth+1&&!locked&&pool_held);
    assert(order==order_now&&owner==cpu);event(zone==PAGE_ZONE_NORMAL?'N':'n');return result(zone);
}
page_t *take_from_zone_locked(uint8_t zone,uint8_t order) {
    assert(locked&&depth==initial_depth&&order==order_now);
    event(zone==PAGE_ZONE_NORMAL?'B':'b');return result(zone);
}
void pool_drain_cpu(uint32_t owner) {
    assert(owner==cpu&&!locked&&!pool_held&&depth==initial_depth+1&&phase==0);
    if(order_now<=PAGE_POOL_SMALL_MAX_ORDER)assert(!irq_enabled);
    event('R');phase=1;
}
void pool_reclaim_all(void) {
    assert(!locked&&!pool_held&&phase==1);
    assert(depth==initial_depth+(order_now<=PAGE_POOL_SMALL_MAX_ORDER));
    if(order_now<=PAGE_POOL_SMALL_MAX_ORDER)assert(!irq_enabled);
    event('X');phase=2;
}
void pool_cpu_lock(uint32_t owner){assert(owner==cpu&&!pool_held&&!locked&&!irq_enabled);pool_held=1;event('q');}
void pool_cpu_unlock(uint32_t owner){assert(owner==cpu&&pool_held&&!locked);pool_held=0;event('Q');}
page_t *pool_alloc_remote(uint8_t order,page_alloc_flags_t flags,uint32_t requester){
    (void)flags;assert(order==order_now&&requester==cpu&&(phase==1||phase==2));
    assert(!locked&&!pool_held&&!irq_enabled&&depth==initial_depth+1);event('Y');return 0;
}
void page_stats_note_alloc(uint8_t order) {
    assert(order==order_now&&!locked&&!pool_held&&depth==initial_depth&&irq_enabled==initial_irq);event('A');
}
paddr_t page_to_phys(const page_t *page) {assert(page==&pages[0]||page==&pages[1]);event('P');return (paddr_t){4096};}
void *phys_to_direct(paddr_t physical) {assert(physical.value==4096);event('M');return mapping?pages:0;}
static void test_zero(void *memory,size_t size) {assert(memory==pages);event('Z');zero_bytes=size;}
void page_free(page_t *page) {
    assert(page==&pages[0]||page==&pages[1]);
    assert(!locked&&depth==initial_depth&&irq_enabled==initial_irq);event('F');
}
#define page_pool_irq_save test_irq_save
#define page_pool_irq_restore test_irq_restore
#define x86_preempt_disable_fast test_disable
#define x86_preempt_enable_fast test_enable
#define x86_current_cpu_index_fast test_cpu
#define lock_pages test_lock
#define unlock_pages test_unlock
#define page_memory_zero test_zero
''' + function + r'''
/* Independent unrolled control: first try, local recovery, then global recovery. */
static page_t *reference_pool_try(uint8_t order,page_alloc_flags_t flags,uint32_t owner){
    pool_cpu_lock(owner);
    page_t *page=0;
    if(flags&PAGE_ALLOC_DMA32)page=pool_alloc_small(PAGE_ZONE_DMA32,order,owner);
    else{page=pool_alloc_small(PAGE_ZONE_NORMAL,order,owner);
         if(!page)page=pool_alloc_small(PAGE_ZONE_DMA32,order,owner);}
    pool_cpu_unlock(owner);return page;
}
static page_t *reference_buddy_try(uint8_t order,page_alloc_flags_t flags){
    lock_pages();page_t *page=0;
    if(flags&PAGE_ALLOC_DMA32)page=take_from_zone_locked(PAGE_ZONE_DMA32,order);
    else{page=take_from_zone_locked(PAGE_ZONE_NORMAL,order);
         if(!page)page=take_from_zone_locked(PAGE_ZONE_DMA32,order);}
    unlock_pages();return page;
}
static page_t *reference_alloc(uint8_t order,page_alloc_flags_t flags) {
    if(order>BUDDY_MAX_ORDER)return 0;
    page_t *page=0;
    if(order<=PAGE_POOL_SMALL_MAX_ORDER) {
        uint64_t irq_flags=page_pool_irq_save();x86_preempt_disable_fast();
        uint32_t owner=x86_current_cpu_index_fast();
        page=reference_pool_try(order,flags,owner);
        if(!page) {
            pool_drain_cpu(owner);
            page=reference_pool_try(order,flags,owner);
        }
        if(!page)page=pool_alloc_remote(order,flags,owner);
        if(!page){pool_reclaim_all();page=reference_pool_try(order,flags,owner);}
        if(!page)page=pool_alloc_remote(order,flags,owner);
        x86_preempt_enable_fast();page_pool_irq_restore(irq_flags);
    } else {
        page=reference_buddy_try(order,flags);
        if(!page) {
            x86_preempt_disable_fast();uint32_t owner=x86_current_cpu_index_fast();
            pool_drain_cpu(owner);x86_preempt_enable_fast();
            page=reference_buddy_try(order,flags);
        }
        if(!page){pool_reclaim_all();page=reference_buddy_try(order,flags);}
    }
    if(page)page_stats_note_alloc(order);
    if(page&&(flags&PAGE_ALLOC_ZERO)) {
        void *memory=phys_to_direct(page_to_phys(page));
        if(!memory) {page_free(page);return 0;}
        page_memory_zero(memory,(size_t)(1ULL<<order)*PAGE_SIZE);
    }
    return page;
}
static void reset(void) {
    phase=locked=pool_held=trace_count=0;trace[0]=0;zero_bytes=0;
    depth=initial_depth;irq_enabled=initial_irq;
}
int main(void) {
    unsigned cases=0;
    for(unsigned order=0;order<=BUDDY_MAX_ORDER+2;++order)
    for(unsigned f=0;f<9;++f)
    for(availability=0;availability<64;++availability)
    for(unsigned map=0;map<2;++map)
    for(unsigned irq=0;irq<2;++irq)
    for(initial_depth=0;initial_depth<4;++initial_depth)
    for(unsigned owner=0;owner<2;++owner) {
        order_now=order==BUDDY_MAX_ORDER+2?255:order;
        unsigned flags=f==8?0x80000000U:f;
        mapping=map;initial_irq=irq;cpu=owner?MAX_CPUS-1:0;
        reset();page_t *expected=reference_alloc(order_now,flags);
        char expected_trace[128];strcpy(expected_trace,trace);
        size_t expected_zero=zero_bytes;unsigned expected_phase=phase;
        reset();page_t *actual=page_alloc_slow(order_now,flags);
        assert(actual==expected&&!strcmp(trace,expected_trace));
        assert(zero_bytes==expected_zero&&phase==expected_phase);
        assert(!locked&&depth==initial_depth&&irq_enabled==initial_irq);
        if(zero_bytes)assert(zero_bytes==(size_t)PAGE_SIZE<<order_now);
        ++cases;
    }
    printf("Allocation retries: %u production/unrolled-model traces across orders/flags/three-stage zone availability/CPUs/IRQ/nested preemption; bounded local/global recovery, pool/Buddy lock separation, DMA32 restriction, zeroing and mapping-failure rollback PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-reclaim-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
