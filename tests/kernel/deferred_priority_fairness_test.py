"""Exercise production deferred batches under a sustained critical IRQ stream."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/irq/deferred.c').read_text()
start = source.index('uint32_t deferred_run(uint32_t budget) {')
end = source.index('\nstatic void deferred_self_test_work', start)
run = source[start:end]
limit = re.search(r'^#define DEFERRED_CRITICAL_BURST_LIMIT (\d+)U$', source, re.M)
assert limit, 'Production critical burst bound must be explicit'
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#define DEFERRED_QUEUE_CAPACITY 128U
#define DEFERRED_CRITICAL_BURST_LIMIT LIMIT_VALUE
typedef void (*deferred_work_fn_t)(void*);
typedef struct { deferred_work_fn_t function; void* argument; } deferred_item_t;
static struct {
    deferred_item_t items[DEFERRED_QUEUE_CAPACITY];
    uint32_t head, count, critical_burst;
} g_deferred;
static atomic_bool critical_pending, keep_critical_pending;
static atomic_uint critical_calls, normal_calls, first_normal_at;
static atomic_uint total_calls, rcu_budget;
static bool initialized = true;
static bool check_fifo = true;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned lock_depth;
static bool deferred_init(void) { return initialized; }
static uint64_t deferred_lock(void) {
    assert(lock_depth == 0);
    assert(pthread_mutex_lock(&queue_lock) == 0);
    ++lock_depth;
    return 0;
}
static void deferred_unlock(uint64_t flags) {
    assert(flags == 0 && lock_depth == 1);
    --lock_depth;
    assert(pthread_mutex_unlock(&queue_lock) == 0);
}
static unsigned rcu_poll(unsigned budget) { rcu_budget += budget; return 0; }
static uint64_t x86_read_tsc(void) { return 0; }
static void deferred_record_work(deferred_work_fn_t fn, uint64_t start) {
    assert(fn && !start && !lock_depth);
}
static void critical(void* unused) {
    (void)unused;
    assert(lock_depth == 0);
    ++critical_calls; ++total_calls;
    critical_pending = keep_critical_pending;
}
static void normal(void* unused) {
    assert(lock_depth == 0);
    unsigned previous = atomic_fetch_add(&normal_calls, 1);
    if (check_fifo) assert((uintptr_t)unused == previous);
    if (!previous) first_normal_at = total_calls;
    ++total_calls;
}
static bool deferred_take_critical(deferred_item_t* item) {
    if (!atomic_exchange(&critical_pending, false)) return false;
    item->function = critical; item->argument = 0;
    return true;
}
''' + run + r'''
static void reset(unsigned normals, bool critical_ready, bool continuous) {
    g_deferred.head = 0; g_deferred.count = normals; g_deferred.critical_burst = 0;
    for (unsigned i = 0; i < normals; ++i)
        g_deferred.items[i] = (deferred_item_t){normal, (void*)(uintptr_t)i};
    critical_pending = critical_ready; keep_critical_pending = continuous;
    critical_calls = normal_calls = total_calls = rcu_budget = 0;
    first_normal_at = UINT32_MAX;
}
static void* consume_short_batches(void* unused) {
    (void)unused;
    for (unsigned i = 0; i < 64; ++i) assert(deferred_run(1) <= 1);
    return 0;
}
int main(void) {
    reset(8, true, true);
    assert(deferred_run(64) == 64);
    assert(normal_calls == 8);
    assert(first_normal_at <= DEFERRED_CRITICAL_BURST_LIMIT);
    assert(critical_calls == 56 && total_calls == 64 && !rcu_budget);
    for (unsigned budget = 1; budget <= 4; ++budget) {
        reset(1, true, true);
        for (unsigned call = 0; call < DEFERRED_CRITICAL_BURST_LIMIT + 1; ++call)
            assert(deferred_run(budget) == budget);
        assert(normal_calls == 1);
        assert(first_normal_at <= DEFERRED_CRITICAL_BURST_LIMIT);
    }
    reset(0, true, true);
    assert(deferred_run(64) == 64 && critical_calls == 64);
    g_deferred.items[0] = (deferred_item_t){normal, 0};
    g_deferred.count = 1;
    assert(deferred_run(1) == 1 && normal_calls == 1 && critical_calls == 64);
    reset(8, false, false);
    assert(deferred_run(64) == 8 && normal_calls == 8 && rcu_budget == 56);
    reset(1, true, false);
    assert(deferred_run(2) == 2 && critical_calls == 1 && normal_calls == 1);
    assert(first_normal_at == 1);
    reset(0, false, false);
    assert(deferred_run(4) == 0 && rcu_budget == 4);
    reset(2, false, false);
    g_deferred.items[127] = g_deferred.items[0];
    g_deferred.items[0] = g_deferred.items[1];
    g_deferred.head = 127;
    assert(deferred_run(2) == 2 && normal_calls == 2);
    assert(g_deferred.head == 1 && g_deferred.count == 0);
    check_fifo = false; // Concurrent callbacks may finish in either order.
    for (unsigned round = 0; round < 64; ++round) {
        reset(8, true, true);
        pthread_t consumers[4];
        for (unsigned i = 0; i < 4; ++i)
            assert(pthread_create(&consumers[i], 0, consume_short_batches, 0) == 0);
        for (unsigned i = 0; i < 4; ++i)
            assert(pthread_join(consumers[i], 0) == 0);
        assert(normal_calls == 8 && g_deferred.count == 0);
        assert(normal_calls + critical_calls == total_calls && total_calls <= 256);
    }
    check_fifo = true;
    reset(1, true, true);
    assert(deferred_run(0) == 0 && total_calls == 0);
    initialized = false;
    assert(deferred_run(64) == 0 && total_calls == 0);
}
'''
program = program.replace('LIMIT_VALUE', limit.group(1) + 'U')
with tempfile.TemporaryDirectory(prefix='liteos-deferred-fairness-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Deferred fairness: short batches, 64 four-consumer rounds, FIFO/wrap, lock boundaries, budgets and RCU passed')
