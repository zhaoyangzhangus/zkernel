"""Actual non-order-zero release preserves IRQ/preemption at every exit."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/phys/page_alloc.c').read_text()
function = source[source.index('PAGE_NOINLINE void page_free_non_o0('):]
previous = function.replace('page_free_non_o0(', 'previous_free(')
previous = previous.replace('        uint64_t irq_flags = page_pool_irq_save();\n', '')
previous = previous.replace('        page_pool_irq_restore(irq_flags);\n', '')
program = '#include "' + str(root / 'kernel/mm/phys/internal.h') + '"\n' + r'''
#include <assert.h>
#include <stdio.h>
static page_t page;
static bool irq_enabled,initial_irq,pooled;
static unsigned preempt_depth,initial_depth,cpu,order_now;
static unsigned saves,restores,pool_calls,buddy_calls,unprotected,pool_held;
void pool_cpu_lock(uint32_t c){assert(c==cpu&&!pool_held&&preempt_depth==initial_depth+1);pool_held=1;}
void pool_cpu_unlock(uint32_t c){assert(c==cpu&&pool_held);pool_held=0;}
static uint64_t test_irq_save(void) {
    assert(preempt_depth==initial_depth);
    uint64_t flags=irq_enabled?1ULL<<9:0;
    irq_enabled=false;++saves;return flags;
}
static void test_irq_restore(uint64_t flags) {
    assert(!irq_enabled&&preempt_depth==initial_depth);
    irq_enabled=(flags&(1ULL<<9))!=0;++restores;
}
static void test_preempt_disable(void) { ++preempt_depth; }
static void test_preempt_enable(void) {
    assert(preempt_depth==initial_depth+1);--preempt_depth;
}
static uint32_t test_current_cpu(void) {
    assert(preempt_depth==initial_depth+1);return cpu;
}
bool pool_free_small(page_t *head,uint8_t order,uint32_t owner) {
    assert(head==&page&&order==order_now&&owner==cpu);
    assert(preempt_depth==initial_depth+1&&pool_held);++pool_calls;
    if(irq_enabled)++unprotected;
    return pooled;
}
void free_legacy_slow(page_t *head,uint8_t order) {
    assert(head==&page&&order==order_now);
    assert(irq_enabled==initial_irq&&preempt_depth==initial_depth&&!pool_held);
    ++buddy_calls;
}
#define page_pool_irq_save test_irq_save
#define page_pool_irq_restore test_irq_restore
#define x86_preempt_disable_fast test_preempt_disable
#define x86_preempt_enable_fast test_preempt_enable
#define x86_current_cpu_index_fast test_current_cpu
''' + function + previous + r'''
static void reset(void) {
    irq_enabled=initial_irq;preempt_depth=initial_depth;
    saves=restores=pool_calls=buddy_calls=unprotected=0;
}
int main(void) {
    unsigned cases=0,old_unprotected=0;
    for(order_now=1;order_now<=BUDDY_MAX_ORDER;++order_now)
    for(unsigned state=0;state<2;++state)
    for(initial_depth=0;initial_depth<4;++initial_depth)
    for(unsigned outcome=0;outcome<2;++outcome)
    for(unsigned owner=0;owner<MAX_CPUS;++owner) {
        initial_irq=state;pooled=outcome;cpu=owner;reset();
        previous_free(&page,order_now);
        old_unprotected+=unprotected;
        unsigned expected_pool=pool_calls,expected_buddy=buddy_calls;
        reset();page_free_non_o0(&page,order_now);
        bool small=order_now<=PAGE_POOL_SMALL_MAX_ORDER;
        assert(pool_calls==expected_pool&&buddy_calls==expected_buddy);
        assert(pool_calls==(unsigned)small);
        assert(buddy_calls==(unsigned)(!small||!pooled));
        assert(!unprotected&&saves==(unsigned)small&&restores==saves);
        assert(irq_enabled==initial_irq&&preempt_depth==initial_depth);
        ++cases;
    }
    /* All nonzero orders now bypass CPU pools and return directly to Buddy. */
    assert(old_unprotected==0&&PAGE_POOL_SMALL_MAX_ORDER==0);
    printf("Nonzero page free: %u actual orders/CPUs/nested-preempt/IRQ/fallback cases; exact old pool/Buddy routing, every state restored; %u old unmasked pool calls rejected\n",cases,old_unprotected);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-page-free-irq-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
