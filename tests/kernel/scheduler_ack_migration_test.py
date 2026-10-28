"""An acknowledgement waiter drains only its current CPU after migration."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sched/core.c').read_text()


def definition(name):
    match = re.search(r'static (?:bool|void) ' + name + r'\([^;]*?\) \{', source)
    assert match, name
    return source[match.start():source.index('\n}', match.end()) + 2]


production = '\n'.join(definition(name) for name in (
    'scheduler_command_ack_drain_locked', 'scheduler_command_ack_step'))
code = '#include "' + str(root / 'kernel/sched/internal.h') + '"\n' + r'''
#include <assert.h>
#include <stdio.h>
scheduler_cpu_t g_cpus[MAX_CPUS];
uint32_t g_cpu_count=4;
static uint32_t actual_cpu, cpu_on_lock;
static bool irq=true, valid=true;
static unsigned drains,releases;
static thread_t target;
bool scheduler_current_cpu(uint32_t *cpu){*cpu=actual_cpu;return valid;}
bool scheduler_cpu_available(uint32_t cpu){return cpu<g_cpu_count;}
static uint64_t scheduler_irq_save(void){bool old=irq;actual_cpu=cpu_on_lock;irq=false;return old;}
static void scheduler_irq_restore(uint64_t flags){irq=flags!=0;}
static bool scheduler_command_drain_local_limit(uint32_t cpu,scheduler_cpu_t *queue,
                                                uint32_t limit,bool snapshot){
 assert(!irq && cpu==actual_cpu && queue==&g_cpus[actual_cpu] && limit==4 && !snapshot);
 ++drains;queue->queue_snapshot_dirty=true;atomic_store(&target.command_ack,6U);return true;
}
static void scheduler_command_release_drain(uint32_t cpu){assert(irq && cpu==actual_cpu);++releases;}
''' + production + r'''
int main(void){
 for(unsigned sampled=0;sampled<4;++sampled)
 for(unsigned destination=0;destination<4;++destination)
 for(unsigned release=0;release<2;++release){
  drains=releases=0;actual_cpu=sampled;cpu_on_lock=destination;
  atomic_store(&target.command_ack,0U);
  assert(scheduler_command_ack_step(&target,4,2,2,release));
  assert(drains==1 && releases==release && irq);
  assert(!g_cpus[destination].queue_snapshot_dirty);
 }
 valid=false;drains=releases=0;atomic_store(&target.command_ack,0U);
 assert(!scheduler_command_ack_step(&target,4,2,2,true));
 assert(!drains && !releases && irq);
 atomic_store(&target.command_ack,6U);
 assert(scheduler_command_ack_step(&target,4,2,2,true));
 assert(!drains && !releases && irq);
 puts("ACK migration: all 32 sampled/current CPU and release combinations; IRQ-time migration, invalid CPU and already-published acknowledgement passed");
}
'''
with tempfile.TemporaryDirectory(prefix='scheduler-ack-migration-') as directory:
    binary = str(Path(directory) / 'test')

    def run(program):
        subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                       input=program, text=True, check=True)
        return subprocess.run([binary], capture_output=True, text=True, timeout=30)

    result = run(code)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout.strip())
    negative = code.replace('uint64_t irq_flags = scheduler_irq_save();',
                            'uint32_t stale_cpu=actual_cpu;\n'
                            '    uint64_t irq_flags = scheduler_irq_save();')
    negative = negative.replace('scheduler_current_cpu(&cpu_id) &&',
                                '(cpu_id=stale_cpu, scheduler_current_cpu(&actual_cpu)) &&')
    assert negative != code and run(negative).returncode != 0
    print('Negative control rejected: draining the sampled CPU after sender migration')
