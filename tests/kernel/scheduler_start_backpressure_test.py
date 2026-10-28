"""Actual start transaction: one publication, rollback and destination ownership."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sched/core.c').read_text()
begin = source.index('kstatus_t sched_start_thread(')
function = source[begin:source.index('bool sched_publish_blocked(', begin)]
assert 'scheduler_remote_command_try_push_once(' in function
program = '#include "' + str(root / 'kernel/sched/internal.h') + '"\n'
program += r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
scheduler_cpu_t g_cpus[MAX_CPUS];
uint32_t g_cpu_count=3;
static unsigned chosen=1,pushes,enqueues,notifications;
static bool valid_cpu=true,available=true,accepted,consume;
static bool interrupts=true;
bool scheduler_current_cpu(uint32_t *cpu) {*cpu=0;return valid_cpu;}
bool scheduler_cpu_available(uint32_t cpu) {return available&&cpu<g_cpu_count;}
uint32_t scheduler_choose_start_cpu(thread_t *t,uint32_t current) {
    assert(t&&current==0);return chosen;
}
static uint64_t scheduler_irq_save(void) {
    assert(interrupts);interrupts=false;return 1ULL<<9;
}
static void scheduler_irq_restore(uint64_t flags) {assert(!interrupts&&flags==(1ULL<<9));interrupts=true;}
static bool scheduler_remote_command_try_push_once(uint32_t from,uint32_t to,
                                                  thread_t *t,uint32_t op,uint32_t gen) {
    assert(!interrupts&&from==0&&to==1&&t->owner_cpu==1&&op==SCHED_CMD_START&&!gen);
    assert(!(t->sched.flags&SCHED_ENTITY_INITIAL_PLACEMENT));
    ++pushes;
    if(accepted&&consume) {
        atomic_store(&t->state,THREAD_RUNNING);atomic_store(&t->start_pending,false);
        /* Destination owns every scheduler flag after publication. */
        t->sched.flags|=SCHED_ENTITY_INITIAL_PLACEMENT|SCHED_ENTITY_ENQUEUED;
    }
    return accepted;
}
void enqueue_local(scheduler_cpu_t *cpu,thread_t *t) {
    assert(cpu==&g_cpus[0]&&!interrupts&&atomic_load(&t->state)==THREAD_READY);
    ++enqueues;t->sched.flags|=SCHED_ENTITY_ENQUEUED;
}
bool x86_smp_request_reschedule(uint32_t cpu) {
    assert(interrupts&&cpu==chosen);++notifications;return true;
}
''' + function + r'''
static sched_cmd_channel_t inbox;
static void reset(thread_t *t) {
    memset(t,0,sizeof(*t));memset(g_cpus,0,sizeof(g_cpus));
    atomic_init(&t->state,THREAD_NEW);atomic_init(&t->start_pending,false);
    t->owner_cpu=2;t->current_cpu=2;
    t->sched.flags=SCHED_ENTITY_INITIAL_PLACEMENT|SCHED_ENTITY_REAP_QUEUED;
    g_cpus[1].command_inbox=&inbox;
    chosen=1;available=valid_cpu=interrupts=true;accepted=consume=false;
    pushes=enqueues=notifications=0;
}
int main(void) {
    thread_t t;
    for(unsigned trial=0;trial<20000;++trial) {
        reset(&t);
        assert(sched_start_thread(&t)==K_EBUSY);
        assert(t.owner_cpu==2&&t.current_cpu==2&&atomic_load(&t.state)==THREAD_NEW);
        assert(!atomic_load(&t.start_pending)&&pushes==1&&!notifications&&interrupts);
        assert(t.sched.flags==(SCHED_ENTITY_INITIAL_PLACEMENT|SCHED_ENTITY_REAP_QUEUED));
        accepted=true;consume=trial%2;
        assert(sched_start_thread(&t)==K_OK&&pushes==2&&notifications==1);
        assert(t.owner_cpu==1&&t.current_cpu==2&&interrupts);
        assert(atomic_load(&t.start_pending)==!consume);
        assert((t.sched.flags&SCHED_ENTITY_REAP_QUEUED)!=0);
        assert(((t.sched.flags&SCHED_ENTITY_INITIAL_PLACEMENT)!=0)==consume);
    }
    reset(&t);g_cpus[1].command_inbox=0;
    assert(sched_start_thread(&t)==K_EBUSY&&t.owner_cpu==2&&!pushes&&!atomic_load(&t.start_pending));
    reset(&t);available=false;
    assert(sched_start_thread(&t)==K_EBUSY&&t.owner_cpu==2&&!pushes&&!atomic_load(&t.start_pending));
    reset(&t);valid_cpu=false;
    assert(sched_start_thread(&t)==K_EBUSY&&!pushes&&!atomic_load(&t.start_pending));
    reset(&t);atomic_store(&t.start_pending,true);
    assert(sched_start_thread(&t)==K_EBUSY&&atomic_load(&t.start_pending)&&!pushes);
    for(unsigned state=THREAD_READY;state<=THREAD_DEAD;++state) {
        reset(&t);atomic_store(&t.state,state);
        assert(sched_start_thread(&t)==(state==THREAD_READY||state==THREAD_RUNNING?K_EBUSY:K_EINVAL));
        assert(!atomic_load(&t.start_pending)&&!pushes);
    }
    reset(&t);chosen=0;
    assert(sched_start_thread(&t)==K_OK&&enqueues==1&&!pushes&&!atomic_load(&t.start_pending));
    assert(t.owner_cpu==0&&t.current_cpu==0&&atomic_load(&t.state)==THREAD_READY);
    assert(!(t.sched.flags&SCHED_ENTITY_INITIAL_PLACEMENT));
    assert(sched_start_thread(0)==K_EINVAL);
    puts("Start backpressure: 20000 full/retry/early-consumer transactions; owner/flag/gate rollback, no post-publication flag writes, local start and invalid states passed");
}
'''
with tempfile.TemporaryDirectory(prefix='scheduler-start-backpressure-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True)
