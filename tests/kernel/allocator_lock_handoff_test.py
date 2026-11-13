"""Production slab lock wrappers: diagnostics cannot erase a successor owner."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/kmalloc.c').read_text()
wrappers = source[source.index('static void cache_lock('):
                  source.index('uint32_t kmem_debug_cache_waiting(')]
page_source = (root / 'kernel/mm/phys/internal.h').read_text()
page_wrappers = page_source[page_source.index('static inline void lock_pages('):
                            page_source.index('/* page_db.c:')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#define KMEM_CLASS_COUNT 3U
#define MAX_CPUS 4U
typedef struct { atomic_uint state; } spinlock_t;
typedef struct { spinlock_t lock; } kmem_cache_t;
static kmem_cache_t g_caches[KMEM_CLASS_COUNT];
static uint32_t g_cache_owner[KMEM_CLASS_COUNT];
static uint32_t g_cache_class[MAX_CPUS], g_cache_waiting[MAX_CPUS];
static unsigned cpu, preempt, selected, successor;
static bool handoff;
static bool page_mode;
static spinlock_t g_page_lock;
static uint32_t x86_current_cpu_index(void) { return cpu; }
static void sched_preempt_disable(void) { ++preempt; }
static void sched_preempt_enable(void) {
    assert(preempt);
    /* 释放锁后，后继者可以立即获取该锁。 */
    if (page_mode) assert(!atomic_load(&g_page_lock.state));
    else if (atomic_load(&g_caches[selected].lock.state))
        assert(g_cache_owner[selected] == successor);
    else assert(g_cache_owner[selected] == UINT32_MAX);
    --preempt;
}
static void spinlock_lock(spinlock_t *lock) {
    assert(preempt);
    unsigned expected=0;
    assert(atomic_compare_exchange_strong(&lock->state, &expected, 1));
}
static void test_release(atomic_uint *state, unsigned value, memory_order order) {
    assert(preempt && state == (page_mode ? &g_page_lock.state :
                               &g_caches[selected].lock.state));
    assert(value == 0 && order == memory_order_release);
    atomic_store_explicit(state, value, order);
    if (handoff) {
        /* 确定性交错：CPU A 尚未继续执行 cache_unlock 的其余部分时，CPU B 已发布其所有者状态。
         * 不依赖宿主机时序。 */
        atomic_store(state, 1);
        g_cache_owner[selected] = successor;
        handoff=false;
    }
}
static inline void spinlock_unlock(spinlock_t *lock) {
    test_release(&lock->state, 0, memory_order_release);
}
#undef atomic_store_explicit
#define atomic_store_explicit test_release
''' + wrappers + page_wrappers + r'''
int main(void) {
    for (unsigned i=0; i<KMEM_CLASS_COUNT; ++i) g_cache_owner[i]=UINT32_MAX;
    for (selected=0; selected<KMEM_CLASS_COUNT; ++selected) {
        cpu=0; preempt=1;
        cache_lock(&g_caches[selected]);
        assert(preempt==2 && g_cache_owner[selected]==cpu);
        assert(g_cache_class[cpu]==selected && !g_cache_waiting[cpu]);
        successor=1; handoff=true;
        cache_unlock(&g_caches[selected]);
        assert(preempt==1 && g_cache_owner[selected]==successor);
        cpu=successor; cache_unlock(&g_caches[selected]);
        assert(!preempt && !atomic_load(&g_caches[selected].lock.state));
        assert(g_cache_owner[selected]==UINT32_MAX);
        cpu=MAX_CPUS; cache_lock(&g_caches[selected]);
        assert(preempt==1 && g_cache_owner[selected]==cpu);
        cache_unlock(&g_caches[selected]);
        assert(!preempt);
    }
    page_mode=true;
    for (unsigned nested=0; nested<3; ++nested) {
        preempt=nested;
        lock_pages(); assert(preempt==nested+1 && atomic_load(&g_page_lock.state)==1);
        unlock_pages(); assert(preempt==nested && !atomic_load(&g_page_lock.state));
    }
    puts("Allocator locks: successor handoff, diagnostic publication, CPU bounds and slab/Buddy nested preemption passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-allocator-handoff-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=10)
