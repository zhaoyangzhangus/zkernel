"""Measure production deferred wait accounting without per-packet logging."""
from pathlib import Path
import subprocess
import tempfile
import resource

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/core.c').read_text()
end = source.index('} g_e1000_poll_timing;') + len('} g_e1000_poll_timing;')
state = source[source.rfind('static struct {', 0, end):end]
start = source.index('static void e1000_record_poll_wait(void)')
end = source.index('void e1000_deferred_poll(void *argument)', start)
functions = source[start:end]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
''' + state + r'''
static bool g_e1000_runtime_ready=true, queue_full, immediate;
static atomic_bool g_e1000_poll_queued, g_e1000_poll_retry;
static uint64_t now=1, expected_queued;
static unsigned attempts;
static uint64_t telemetry_timestamp(void) {return now;}
static uint64_t x86_tsc_to_ns(uint64_t value) {return value;}
static void e1000_record_poll_wait(void);
void e1000_deferred_poll(void *argument) {(void)argument;}
static bool deferred_try_schedule(void (*fn)(void *),void *arg) {
    assert(fn==e1000_deferred_poll && !arg);++attempts;
    if(queue_full)return false;
    assert(atomic_load(&g_e1000_poll_timing.queued_tsc)==expected_queued);
    if(immediate){e1000_record_poll_wait();atomic_store(&g_e1000_poll_queued,false);}
    return true;
}
'''
main = r'''
int main(void) {
    e1000_record_poll_wait();assert(!atomic_load(&g_e1000_poll_timing.samples));
    queue_full=true;assert(!e1000_schedule_deferred_poll());
    assert(!atomic_load(&g_e1000_poll_queued) && atomic_load(&g_e1000_poll_retry));
    assert(!atomic_load(&g_e1000_poll_timing.samples));
    queue_full=false;expected_queued=now=100;
    e1000_retry_deferred_poll();assert(attempts==2);
    now=200;assert(!e1000_schedule_deferred_poll());
    assert(atomic_load(&g_e1000_poll_timing.queued_tsc)==100);
    now=500;e1000_record_poll_wait();
    assert(atomic_load(&g_e1000_poll_timing.samples)==1);
    assert(atomic_load(&g_e1000_poll_timing.total_ns)==400);
    assert(atomic_load(&g_e1000_poll_timing.max_ns)==400);
    const uint64_t delays[]={999999,1000000,9999999,10000000,99999999,100000000};
    uint64_t total=400;
    for(unsigned i=0;i<6;++i){
        atomic_store(&g_e1000_poll_queued,false);expected_queued=now=1000;
        assert(e1000_schedule_deferred_poll());now+=delays[i];e1000_record_poll_wait();
        total+=delays[i];
        assert(atomic_load(&g_e1000_poll_timing.samples)==i+2);
        assert(atomic_load(&g_e1000_poll_timing.total_ns)==total);
        assert(atomic_load(&g_e1000_poll_timing.max_ns)==delays[i]);
    }
    assert(atomic_load(&g_e1000_poll_timing.over_1ms)==5);
    assert(atomic_load(&g_e1000_poll_timing.over_10ms)==3);
    assert(atomic_load(&g_e1000_poll_timing.over_100ms)==1);
    atomic_store(&g_e1000_poll_queued,false);
    expected_queued=now=UINT64_MAX-9;
    assert(e1000_schedule_deferred_poll());now=10;e1000_record_poll_wait();
    assert(atomic_load(&g_e1000_poll_timing.total_ns)==total+20);
    atomic_store(&g_e1000_poll_queued,false);immediate=true;expected_queued=now=2000;
    assert(e1000_schedule_deferred_poll());
    assert(atomic_load(&g_e1000_poll_timing.samples)==9);
    assert(!atomic_load(&g_e1000_poll_queued));
    g_e1000_runtime_ready=false;unsigned before=attempts;
    assert(!e1000_schedule_deferred_poll() && attempts==before);
}
'''
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with tempfile.TemporaryDirectory(prefix='net-poll-wait-') as directory:
    binary = str(Path(directory) / 'test')
    def compile_case(body):
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                            '-x', 'c', '-', '-o', binary],
                            input=prefix+body+main, text=True, check=True)
    compile_case(functions)
    subprocess.run([binary], check=True)
    publication = '''    atomic_store_explicit(&g_e1000_poll_timing.queued_tsc, telemetry_timestamp(),
                           memory_order_release);
'''
    assert functions.count(publication) == 1
    controls = {
        'timestamp published after consumer': functions.replace(publication, '').replace(
            '    return true;', publication + '    return true;'),
        'exclusive threshold loses exact boundary': functions.replace(
            'delay >= 1000000ULL', 'delay > 1000000ULL'),
    }
    for name, mutant in controls.items():
        assert mutant != functions
        compile_case(mutant)
        result = subprocess.run([binary], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert result.returncode != 0, name
        print('Negative control rejected:', name)
print('RX queue wait: publication, coalescing, queue failure/retry, immediate consumer, thresholds and TSC wrap PASS')
