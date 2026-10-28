"""Charge real CPU bursts even when a thread blocks before a timer tick."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/sched/clock.c').read_text()
begin = source.index('static uint64_t scheduler_fair_vruntime_delta')
end = source.index('void sched_tick(')
program = '#include "' + str(root/'kernel/sched/internal.h') + '"\n'
program += r'''
#include <assert.h>
void scheduler_update_min_vruntime(run_queue_t *queue) { (void)queue; }
''' + source[begin:end] + r'''
int main(void) {
    uint64_t seed=41;
    for(unsigned i=0;i<1000000;++i) {
        seed=seed*UINT64_C(6364136223846793005)+1;
        uint64_t runtime=seed;
        uint32_t weight=i%11==0?1024:(uint32_t)(seed>>32);
        uint32_t effective=weight?weight:1;
        __uint128_t expected=(__uint128_t)runtime*1024/effective;
        uint64_t bounded=expected>UINT64_MAX?UINT64_MAX:(uint64_t)expected;
        assert(scheduler_fair_vruntime_delta(runtime,weight)==bounded);
    }
    run_queue_t queue={0}; thread_t idle={0}, thread={0};
    queue.current=&thread; queue.idle=&idle; queue.clock_ns=100;
    thread.sched_class=SCHED_CLASS_FAIR; thread.sched.weight=1024;
    thread.sched.exec_start_ns=100;
    for (unsigned i=1;i<=1000;++i) {
        assert(scheduler_account_current(&queue,100+i*100000)==100000);
        /* Context-switch anchors: a burst shorter than the timer period. */
        thread.sched.exec_start_ns=0;
        thread.sched.exec_start_ns=queue.clock_ns;
    }
    assert(thread.sched.runtime_ns==100000000);
    assert(thread.sched.vruntime==100000000);
    assert(scheduler_account_current(&queue,queue.clock_ns)==0);
    assert(scheduler_account_current(&queue,queue.clock_ns-1)==0);
    thread.sched.weight=2048;
    scheduler_account_current(&queue,queue.clock_ns+100000);
    assert(thread.sched.vruntime==100050000);
    thread.sched_class=SCHED_CLASS_RT;
    scheduler_account_current(&queue,queue.clock_ns+100000);
    assert(thread.sched.slice_runtime_ns==100000);
    assert(thread.sched.vruntime==100050000);
    queue.current=&idle;
    scheduler_account_current(&queue,queue.clock_ns+100000);
    assert(!idle.sched.runtime_ns);
    queue.current=&thread; thread.sched.exec_start_ns=0;
    assert(!scheduler_account_current(&queue,queue.clock_ns+100000));
    thread.sched.runtime_ns=UINT64_MAX-1; thread.sched.exec_start_ns=queue.clock_ns;
    scheduler_account_current(&queue,queue.clock_ns+100000);
    assert(thread.sched.runtime_ns==UINT64_MAX);
    assert(scheduler_fair_vruntime_delta(UINT64_MAX,1)==UINT64_MAX);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-runtime-tail-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),
                    '-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('runtime tail: 1000 sub-tick bursts charged, no double charge, clock floor, weights, RT, idle and overflow passed')
