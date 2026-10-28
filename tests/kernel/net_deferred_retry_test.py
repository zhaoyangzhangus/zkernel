"""Compile production queue/retry functions against a bounded-queue stub."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/core.c').read_text()
start = source.index('bool e1000_schedule_deferred_poll(void)')
end = source.index('void e1000_deferred_poll(void *argument)', start)
timing_end = source.index('} g_e1000_poll_timing;') + len('} g_e1000_poll_timing;')
timing_start = source.rfind('static struct {', 0, timing_end)
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
''' + source[timing_start:timing_end] + r'''
static uint64_t telemetry_timestamp(void) { return 1000U; }
static bool g_e1000_runtime_ready = true;
static atomic_bool g_e1000_poll_queued, g_e1000_poll_retry;
static bool queue_full;
static unsigned attempts, enqueued;
void e1000_deferred_poll(void *argument) { (void)argument; }
bool deferred_try_schedule(void (*fn)(void *), void *argument) {
    (void)fn; (void)argument; ++attempts;
    if (queue_full) return false;
    ++enqueued; return true;
}
''' + source[start:end] + r'''
int main(void) {
    queue_full = true;
    assert(!e1000_schedule_deferred_poll());
    assert(!atomic_load(&g_e1000_poll_queued));
    assert(atomic_load(&g_e1000_poll_retry));
    e1000_retry_deferred_poll();
    assert(attempts == 2 && atomic_load(&g_e1000_poll_retry));
    queue_full = false;
    e1000_retry_deferred_poll();
    assert(enqueued == 1 && atomic_load(&g_e1000_poll_queued));
    assert(!atomic_load(&g_e1000_poll_retry));
    e1000_retry_deferred_poll();
    assert(attempts == 3); /* idle ticks do not queue polling work */
    atomic_store(&g_e1000_poll_retry, true);
    e1000_retry_deferred_poll();
    assert(enqueued == 1); /* IRQ already queued the same work */
    atomic_store(&g_e1000_poll_queued, false);
    g_e1000_runtime_ready = false;
    atomic_store(&g_e1000_poll_retry, true);
    e1000_retry_deferred_poll();
    assert(attempts == 3 && !atomic_load(&g_e1000_poll_queued));
    g_e1000_runtime_ready = true;
    assert(e1000_schedule_deferred_poll());
    assert(enqueued == 2);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-net-retry-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Full queue recovery, repeated failure, coalescing, idle and shutdown passed')
