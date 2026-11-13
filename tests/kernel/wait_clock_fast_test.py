"""Actual wait timing helpers: optional diagnostics and infinite wait fast path."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sync/wait.c').read_text()
def function(name):
    start = source.index(name)
    return source[start:source.index('\n}', start) + 2]
helpers = '\n'.join(function(name) for name in [
    'static void wait_start_diagnostic(', 'static uint64_t wait_resume_timestamp(',
    'static bool deadline_reached(', 'static bool wait_deadline_reached(',
    'static thread_t *waiter_claim_locked('])
# 验证生产等待路径会调用已测试的辅助函数。
assert 'deadline_reached(x86_read_tsc(), deadline)' not in source[source.index('static bool wait_without_scheduler('):]
assert 'wait_start_diagnostic(&waiter);' in source
assert 'uint64_t resumed = wait_resume_timestamp(&waiter);' in source
program = r'''
#include <assert.h>
#include <stdio.h>
#include <kernel/wait.h>
typedef struct thread { uint64_t tid; } thread_t;
static volatile uint64_t g_wait_delay_tid;
static uint64_t ticks=100,reads;
static uint64_t x86_read_tsc(void) { ++reads;return ticks; }
''' + helpers + r'''
int main(void) {
 thread_t thread={126};waiter_t waiter={.thread=&thread};
 for(unsigned i=0;i<100000;++i) {
  waiter.diagnostic_started_tsc=7;waiter.diagnostic_claimed_tsc=9;
  atomic_init(&waiter.state,WAITER_WAITING);wait_start_diagnostic(&waiter);
  assert(!waiter.diagnostic_started_tsc && !waiter.diagnostic_claimed_tsc);
  assert(waiter_claim_locked(&waiter,WAITER_WOKEN)==&thread);
  assert(!wait_resume_timestamp(&waiter) && !wait_deadline_reached(UINT64_MAX));
 }
 assert(!reads);
 g_wait_delay_tid=127;wait_start_diagnostic(&waiter);assert(!reads);
 // 只启用选定的等待者，并保留全部三个诊断阶段。
 g_wait_delay_tid=126;wait_start_diagnostic(&waiter);
 assert(reads==1 && waiter.diagnostic_started_tsc==100 && !waiter.diagnostic_claimed_tsc);
 ticks=200;atomic_store(&waiter.state,WAITER_WAITING);
 assert(waiter_claim_locked(&waiter,WAITER_WOKEN)==&thread);
 assert(reads==2 && waiter.diagnostic_claimed_tsc==200);
 assert(!waiter_claim_locked(&waiter,WAITER_TIMED_OUT) && reads==2);
 ticks=300;assert(wait_resume_timestamp(&waiter)==300 && reads==3);
 // 等待期间禁用后，保留已捕获的起始时间，直到所有者清理；新等待会清除过期计时状态。
 // 等待期间启用则从下一次等待开始生效。
 g_wait_delay_tid=0;ticks=400;assert(wait_resume_timestamp(&waiter)==400);
 wait_start_diagnostic(&waiter);uint64_t before=reads;
 g_wait_delay_tid=126;atomic_store(&waiter.state,WAITER_WAITING);
 assert(waiter_claim_locked(&waiter,WAITER_TIMED_OUT)==&thread);
 assert(!wait_resume_timestamp(&waiter) && reads==before);
 for(unsigned state=WAITER_WOKEN;state<=WAITER_CANCELLED;++state) {
  wait_start_diagnostic(&waiter);atomic_store(&waiter.state,WAITER_WAITING);
  assert(waiter_claim_locked(&waiter,state)==&thread);
  assert(atomic_load(&waiter.state)==state);
 }
 ticks=999;assert(!wait_deadline_reached(1000));
 ticks=1000;assert(wait_deadline_reached(1000));
 ticks=1001;assert(wait_deadline_reached(1000));
 ticks=UINT64_MAX-4;assert(!wait_deadline_reached(3));
 ticks=3;assert(wait_deadline_reached(3));
 before=reads;assert(!wait_deadline_reached(UINT64_MAX) && reads==before);
 puts("PASS: 100000 untracked infinite wait cycles use zero diagnostic/deadline clock reads; selector changes, all terminal claims, finite deadline and wrap preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-wait-clock-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'include'), '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True)
