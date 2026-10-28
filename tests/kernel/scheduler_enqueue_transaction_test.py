"""Exercise the production enqueue transaction, including failed publication."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sched/core.c').read_text()
start = source.index('static bool sched_enqueue_internal(')
function = source[start:source.index('bool sched_enqueue(thread_t', start)]
program = '#include "' + str(root / 'kernel/sched/internal.h') + '"\n'
program += r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
scheduler_cpu_t g_cpus[MAX_CPUS];
uint32_t g_cpu_count=3;
static bool interrupts=true, accepted, consume;
static unsigned notifications,pushes,chosen=1;
bool scheduler_current_cpu(uint32_t *cpu) {*cpu=0;return true;}
bool scheduler_cpu_available(uint32_t cpu) {return cpu<g_cpu_count;}
uint32_t scheduler_choose_cpu(thread_t *t,uint32_t cpu) {
    assert(t&&cpu==0);return chosen;
}
static uint64_t scheduler_irq_save(void) {assert(interrupts);interrupts=false;return 512;}
static void scheduler_irq_restore(uint64_t flags) {
    assert(!interrupts&&flags==512);interrupts=true;
}
static bool scheduler_remote_command_try_push(uint32_t from,uint32_t to,
        thread_t *t,uint32_t op,uint32_t gen) {
    assert(!interrupts&&from==0&&to==1&&op==SCHED_CMD_ENQUEUE&&!gen);
    assert(t->owner_cpu==1&&!(t->sched.flags&SCHED_ENTITY_INITIAL_PLACEMENT));
    ++pushes;
    if(accepted&&consume) {
        t->sched.flags|=SCHED_ENTITY_INITIAL_PLACEMENT|SCHED_ENTITY_ENQUEUED;
        t->owner_cpu=2; /* Simulate the receiver immediately taking ownership. */
    }
    return accepted;
}
void enqueue_local(scheduler_cpu_t *cpu,thread_t *t) {
    assert(!interrupts&&cpu==&g_cpus[0]);
    assert(!(t->sched.flags&SCHED_ENTITY_INITIAL_PLACEMENT));
    ++cpu->queue.nr_running;t->sched.flags|=SCHED_ENTITY_ENQUEUED;
}
bool x86_smp_request_reschedule(uint32_t cpu) {
    assert(interrupts&&cpu==1);++notifications;return true;
}
''' + function + r'''
static sched_cmd_channel_t inbox;
static void reset(thread_t *t) {
    memset(g_cpus,0,sizeof(g_cpus));memset(t,0,sizeof(*t));
    t->owner_cpu=2;t->current_cpu=2;t->sched_class=SCHED_CLASS_FAIR;
    t->sched.flags=SCHED_ENTITY_INITIAL_PLACEMENT|SCHED_ENTITY_REAP_QUEUED;
    g_cpus[1].command_inbox=&inbox;
    accepted=consume=false;interrupts=true;chosen=1;pushes=notifications=0;
}
int main(void) {
    thread_t t;
    for(unsigned i=0;i<20000;++i) {
        reset(&t);uint16_t flags=t.sched.flags;
        if(i%2)g_cpus[1].command_inbox=0;
        assert(!sched_enqueue_internal(&t,true));
        assert(t.owner_cpu==2&&t.current_cpu==2&&t.sched.flags==flags);
        assert(interrupts&&!notifications&&pushes==(i%2?0:1));
        g_cpus[1].command_inbox=&inbox;accepted=true;consume=i%2;
        assert(sched_enqueue_internal(&t,false));
        assert(t.owner_cpu==(consume?2:1)&&notifications==1&&interrupts);
        assert(((t.sched.flags&SCHED_ENTITY_INITIAL_PLACEMENT)!=0)==consume);
    }
    reset(&t);chosen=0;
    assert(sched_enqueue_internal(&t,false)&&g_cpus[0].queue.nr_running==1);
    assert(t.owner_cpu==0&&t.current_cpu==0&&!notifications);
    assert(!sched_enqueue_internal(0,true));
    puts("Enqueue transaction: 20000 failed/retry/early-consumer cases; missing inbox, original owner/flags and local placement passed");
}
'''
with tempfile.TemporaryDirectory(prefix='scheduler-enqueue-transaction-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True)
