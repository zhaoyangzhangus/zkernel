"""Diagnostic preemption callers: actual outermost-entry/exit hooks."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/sched/core.c').read_text()
start = source.index('void sched_preempt_disable(void)')
end = source.index('\n/*', start)
program = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define MAX_CPUS 256U
static uint64_t g_sched_wake_trace_tid;
static volatile uint64_t g_sched_preempt_callers[MAX_CPUS];
static unsigned cpu,depth;
static uint32_t x86_current_cpu_index(void) { return cpu; }
static bool x86_preempt_disabled_fast(void) { return depth!=0; }
static void x86_preempt_disable_fast(void) { ++depth; }
static void x86_preempt_enable_fast(void) { assert(depth);--depth; }
''' + source[start:end] + r'''
int main(void) {
 cpu=3;sched_preempt_disable();assert(depth==1&&!g_sched_preempt_callers[3]);
 sched_preempt_enable();assert(!depth);
 g_sched_wake_trace_tid=126;
 sched_preempt_disable();uint64_t caller=g_sched_preempt_callers[3];
 assert(caller&&depth==1);
 sched_preempt_disable();assert(depth==2&&g_sched_preempt_callers[3]==caller);
 sched_preempt_enable();assert(depth==1&&g_sched_preempt_callers[3]==caller);
 sched_preempt_enable();assert(!depth&&!g_sched_preempt_callers[3]);
 sched_preempt_enable();assert(!depth&&!g_sched_preempt_callers[3]);
 cpu=MAX_CPUS;sched_preempt_disable();assert(depth==1);
 sched_preempt_enable();assert(!depth);
 // 在已有临界区中启用探测不能凭空推断调用者。
 cpu=0;g_sched_wake_trace_tid=0;sched_preempt_disable();
 g_sched_wake_trace_tid=126;sched_preempt_disable();
 assert(!g_sched_preempt_callers[0]);sched_preempt_enable();sched_preempt_enable();
 assert(!depth);
 assert(!sched_preempt_disabled());
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-preempt-caller-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
print('Preempt caller: disabled probe, outermost entry, nesting, exit, invalid CPU and mid-section activation passed')
