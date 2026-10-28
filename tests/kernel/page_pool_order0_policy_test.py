"""Multi-page frees must bypass CPU pools and return their entire Buddy block."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/phys/page_alloc.c').read_text()
legacy = source[source.index('PAGE_NOINLINE void free_legacy_slow('):source.index('PAGE_NOINLINE page_t *page_alloc_slow(')]
free = source[source.index('PAGE_NOINLINE void page_free_non_o0('):]
program = '#include "' + str(root / 'kernel/mm/phys/internal.h') + '"\n' + r'''
#include <assert.h>
#include <stdio.h>
static page_t page;
static unsigned held, calls, expected_order;
static void test_lock(void) { assert(!held); held=1; }
static void test_unlock(void) { assert(held); held=0; }
void free_block_locked(page_t *head,uint8_t order) {
    assert(held && head==&page && order==expected_order); ++calls;
}
static uint64_t forbidden_irq_save(void) { assert(0); return 0; }
static void forbidden_irq_restore(uint64_t flags) { (void)flags; assert(0); }
static void forbidden_preempt(void) { assert(0); }
static uint32_t forbidden_cpu(void) { assert(0); return 0; }
void pool_cpu_lock(uint32_t cpu) { (void)cpu; assert(0); }
void pool_cpu_unlock(uint32_t cpu) { (void)cpu; assert(0); }
bool pool_free_small(page_t *head,uint8_t order,uint32_t cpu) {
    (void)head;(void)order;(void)cpu;assert(0);return false;
}
#define lock_pages test_lock
#define unlock_pages test_unlock
#define page_pool_irq_save forbidden_irq_save
#define page_pool_irq_restore forbidden_irq_restore
#define x86_preempt_disable_fast forbidden_preempt
#define x86_preempt_enable_fast forbidden_preempt
#define x86_current_cpu_index_fast forbidden_cpu
''' + legacy + free + r'''
int main(void) {
    _Static_assert(PAGE_POOL_SMALL_MAX_ORDER==0,"Only single pages may be cached");
    _Static_assert(PAGE_POOL_ORDER_COUNT==1,"No hidden higher-order metadata layers");
    unsigned cases=0;
    for(unsigned zone=0;zone<PAGE_ZONE_COUNT;++zone)
        for(unsigned order=1;order<=BUDDY_MAX_ORDER;++order) {
            page.zone=zone;page.order=order;expected_order=order;calls=0;
            page_free_non_o0(&page,order);
            assert(calls==1 && !held);++cases;
        }
    printf("Order0-only policy: %u multi-page free routes return the complete block to Buddy; no CPU cache/IRQ/preemption path entered PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='page-order0-policy-') as directory:
    c = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    c.write_text(program)
    subprocess.run(['cc', '-std=gnu11', '-O2', '-I'+str(root/'include'), str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
