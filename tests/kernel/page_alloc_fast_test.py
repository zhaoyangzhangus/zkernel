#!/usr/bin/env python3
"""Execute the production page_alloc dispatcher with mocked allocator zones."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / "kernel/mm/phys/percpu_page.c").read_text()
start = source.index("page_t *page_alloc(uint8_t order, page_alloc_flags_t flags) {")
end = source.index("\nvoid page_free(", start)
dispatcher = source[start:end]
mock = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef struct { int zone; } page_t;
typedef unsigned page_alloc_flags_t;
#define PAGE_LIKELY(x) (x)
#define PAGE_UNLIKELY(x) (x)
#define MAX_CPUS 256U
#define PAGE_ZONE_DMA32 0U
static page_t normal_page, dma_page, slow_page;
static page_t *normal_result, *dma_result, *recovery_result;
static unsigned cpu, normal_calls, dma_calls, slow_calls, stats, disabled, pool_held;
static void pool_cpu_lock(unsigned c){assert(disabled&&c==cpu&&c<MAX_CPUS&&!pool_held);pool_held=1;}
static void pool_cpu_unlock(unsigned c){assert(c==cpu&&pool_held);pool_held=0;}
static uint64_t page_pool_irq_save(void) { return 42; }
static void page_pool_irq_restore(uint64_t f) { assert(f == 42 && !disabled); }
static void x86_preempt_disable_fast(void) { ++disabled; }
static void x86_preempt_enable_fast(void) { assert(disabled == 1); --disabled; }
static unsigned x86_current_cpu_index_fast(void) { return cpu; }
static page_t *pool_alloc_o0_normal(unsigned c) {
    assert(disabled && pool_held && c == cpu); ++normal_calls;
    return c < MAX_CPUS ? normal_result : 0;
}
static page_t *pool_alloc_small(unsigned z, unsigned o, unsigned c) {
    assert(disabled && pool_held && z == PAGE_ZONE_DMA32 && o == 0 && c < MAX_CPUS);
    ++dma_calls; return dma_result;
}
static void page_stats_note_alloc(unsigned o) { assert(o == 0); ++stats; }
static page_t *page_alloc_slow(unsigned o, unsigned f) {
    assert(!disabled&&!pool_held); ++slow_calls;
    if (!o && !f) {
        if (recovery_result) ++stats;
        return recovery_result;
    }
    return &slow_page;
}
'''
cases = r'''
int main(void) {
    normal_result = &normal_page; dma_result = &dma_page;
    assert(page_alloc(0, 0) == &normal_page && dma_calls == 0 && stats == 1);
    normal_result = 0;
    assert(page_alloc(0, 0) == &dma_page && dma_calls == 1 && stats == 2);
    dma_result = 0;
    assert(page_alloc(0, 0) == 0 && dma_calls == 2 && stats == 2);
    assert(slow_calls == 1);
    recovery_result = &slow_page;
    assert(page_alloc(0, 0) == &slow_page && dma_calls == 3 && stats == 3);
    assert(slow_calls == 2);
    cpu = MAX_CPUS;
    assert(page_alloc(0, 0) == 0 && dma_calls == 3 && stats == 3 && slow_calls == 2);
    assert(page_alloc(1, 0) == &slow_page);
    assert(page_alloc(0, 1) == &slow_page);
    assert(slow_calls == 4 && normal_calls == 4 && !disabled&&!pool_held);
    puts("page-alloc-fast: normal, DMA32, bounded recovery, single accounting, exhaustion, CPU bounds, slow dispatch OK");
}
'''
with tempfile.TemporaryDirectory(prefix="system-page-alloc-test-") as directory:
    binary = str(pathlib.Path(directory) / "test")
    for negative in (False, True):
        candidate = dispatcher
        if negative:
            omitted = ('        if (PAGE_UNLIKELY(page == 0) && cpu < MAX_CPUS)\n'
                       '            return page_alloc_slow(order, flags);\n')
            assert omitted in candidate
            candidate = candidate.replace(omitted, '')
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-x", "c", "-", "-o", binary],
                       input=mock + candidate + cases, text=True, check=True)
        result = subprocess.run([binary], capture_output=negative)
        if negative:
            assert result.returncode != 0, 'omitting bounded recovery must fail'
        else:
            result.check_returncode()
print('Negative control rejected: the previous fast path bypassed reclaim on exhaustion')
