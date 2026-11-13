"""Production vmalloc lock: contention traffic and TLB/IRQ/preemption rules."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/vmalloc.c').read_text()
code = source[source.index('static uint64_t vmalloc_lock_irqsave('):
              source.index('static void vmalloc_initialize(')]
for before, after in {
    '__asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");':
        'flags = irq_flags;',
    '__asm__ volatile ("sti" : : : "memory");': 'irq_flags |= 512;',
    '__asm__ volatile ("cli" : : : "memory");': 'irq_flags &= ~512ULL;',
    '__asm__ volatile ("pause");': 'test_pause();',
    'atomic_compare_exchange_strong_explicit(': 'test_acquire(',
}.items():
    assert before in code, before
    code = code.replace(before, after)

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
static struct { atomic_uint state; } g_vmalloc_lock;
static _Thread_local uint64_t irq_flags;
static _Thread_local unsigned preempt, waiting, begins, ends;
static atomic_uint attempts, entered, polls;
static uint64_t value, checksum;
static bool test_acquire(atomic_uint *s, unsigned *e, unsigned v,
                         memory_order a, memory_order b) {
    atomic_fetch_add(&attempts, 1);
    return atomic_compare_exchange_strong_explicit(s, e, v, a, b);
}
static void sched_preempt_disable(void) { ++preempt; }
static void sched_preempt_enable(void) { assert(preempt); --preempt; }
static void x86_tlb_wait_begin(void) {
    assert(preempt && !waiting); waiting = 1; ++begins;
    atomic_fetch_add(&entered, 1);
}
static void x86_tlb_wait_end(void) {
    assert(preempt && waiting); waiting = 0; ++ends;
}
static void test_pause(void) {
    assert(preempt && waiting && (irq_flags & 512));
    atomic_fetch_add(&polls, 1); sched_yield();
}
''' + code + r'''
static void *worker(void *arg) {
    uint64_t initial = (uintptr_t)arg;
    irq_flags = initial; preempt = 1;
    for (unsigned i = 0; i < 10000; ++i) {
        uint64_t saved = vmalloc_lock_irqsave();
        assert(saved == initial && irq_flags == initial && preempt == 2);
        assert(!waiting && begins == ends && checksum == value * 17);
        ++value; checksum = value * 17;
        vmalloc_unlock_irqrestore(saved);
        assert(irq_flags == initial && preempt == 1 && !waiting);
    }
    return 0;
}
int main(void) {
    /* 无竞争获取时不得进入 TLB 等待，也不得更改任一 IF 状态。 */
    for (unsigned i = 0; i < 2; ++i) {
        irq_flags = i ? 512 : 0;
        uint64_t saved = vmalloc_lock_irqsave();
        assert(preempt == 1 && saved == irq_flags && !begins && !ends);
        vmalloc_unlock_irqrestore(saved);
        assert(!preempt && irq_flags == (i ? 512U : 0U));
    }
    atomic_store(&attempts, 0);
    irq_flags = 0; uint64_t saved = vmalloc_lock_irqsave();
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_create(&threads[i], 0, worker,
                               (void *)(uintptr_t)(i & 1 ? 512 : 0)));
    while (atomic_load(&entered) < 4 || atomic_load(&polls) < 20000)
        sched_yield();
    assert(atomic_load(&attempts) == 5);
    vmalloc_unlock_irqrestore(saved);
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], 0));
    assert(value == 40000 && checksum == 680000 && !preempt && !irq_flags);
    assert(!atomic_load(&g_vmalloc_lock.state));
    puts("vmalloc lock: 40000 publication updates, nested preemption, IF=0/1, balanced TLB wait; 20000+ polls with four failed acquires passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-vmalloc-lock-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize=address,undefined', '-fno-pie',
                    '-no-pie', str(path / 'test.c'), '-o', str(path / 'test')],
                   check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=20)
