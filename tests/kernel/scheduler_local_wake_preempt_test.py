"""Production local wake decision: FAIR reevaluation without RT inversion."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/sched/core.c').read_text()
start = source.index('static bool scheduler_local_wake_requires_preempt(')
end = source.index('\n}', start)+2
program = '#include "' + str(root/'kernel/sched/internal.h') + '"\n'
program += '#include <assert.h>\n' + source[start:end] + r'''
int main(void) {
 scheduler_cpu_t cpu={0}; thread_t current={0}, waking={0}, idle={0};
 cpu.queue.current=&current;cpu.queue.idle=&idle;
 atomic_init(&current.state,THREAD_RUNNING);
 current.sched_class=SCHED_CLASS_FAIR;waking.sched_class=SCHED_CLASS_FAIR;
 // 本地系统调用运行时 UI 已处于 READY 状态：不存在待处理的 IPI 位。
 assert(scheduler_local_wake_requires_preempt(&cpu,&waking));
 assert(!scheduler_local_wake_requires_preempt(0,&waking));
 assert(!scheduler_local_wake_requires_preempt(&cpu,0));
 current.sched_class=SCHED_CLASS_RT;current.rt_priority=4;
 assert(!scheduler_local_wake_requires_preempt(&cpu,&waking));
 waking.sched_class=SCHED_CLASS_RT;waking.rt_priority=3;
 assert(scheduler_local_wake_requires_preempt(&cpu,&waking));
 waking.rt_priority=5;
 assert(!scheduler_local_wake_requires_preempt(&cpu,&waking));
 waking.rt_priority=4;current.sched.slice_runtime_ns=SCHED_RT_TIMESLICE_NS-1;
 assert(!scheduler_local_wake_requires_preempt(&cpu,&waking));
 ++current.sched.slice_runtime_ns;
 assert(scheduler_local_wake_requires_preempt(&cpu,&waking));
 current.sched_class=SCHED_CLASS_FAIR;
 assert(scheduler_local_wake_requires_preempt(&cpu,&waking));
 waking.sched_class=SCHED_CLASS_IDLE;
 assert(!scheduler_local_wake_requires_preempt(&cpu,&waking));
 cpu.queue.current=&idle;
 assert(scheduler_local_wake_requires_preempt(&cpu,&waking));
 cpu.queue.current=0;
 assert(scheduler_local_wake_requires_preempt(&cpu,&waking));
 cpu.queue.current=&current;atomic_store(&current.state,THREAD_BLOCKED);
 assert(scheduler_local_wake_requires_preempt(&cpu,&waking));
 atomic_store(&current.state,THREAD_RUNNING);
 assert(scheduler_local_wake_requires_preempt(&cpu,&current));
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-local-wake-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(root/'include'), str(path/'test.c'),
                    '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
print('Local wake: FAIR reevaluation, RT priorities/timeslice, idle and state transitions passed')
