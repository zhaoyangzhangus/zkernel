"""Actual queue and transfer code: move relative debt between per-CPU clocks."""
from pathlib import Path
import subprocess
import tempfile
import resource
import signal

root = Path(__file__).resolve().parents[2]
queue = (root / 'kernel/sched/runqueue.c').read_text()
core = (root / 'kernel/sched/core.c').read_text()
helpers = queue[queue.index('static thread_t *rt_thread_from_node'):
                queue.index('void initialize_thread(')]
operations = queue[queue.index('void enqueue_local_raw('):
                   queue.index('bool sched_rt_policy_self_test(')]
transfer = core[core.index('static bool scheduler_transfer_local('):
                core.index('static inline void scheduler_thread_mailbox_lock(')]
program = '#include "' + str(root / 'kernel/sched/internal.h') + '"\n'
program += r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
scheduler_cpu_t g_cpus[MAX_CPUS];
uint32_t g_cpu_count=3;
static bool accepted=true;
static unsigned pushes,ipis;
static thread_t *published;
bool scheduler_current_cpu(uint32_t *cpu) {*cpu=0;return true;}
static bool scheduler_remote_command_try_push_once(uint32_t source,uint32_t target,
                                              thread_t *thread,uint32_t op,uint32_t generation) {
    assert(source<3&&target<3&&source!=target);
    assert(thread->owner_cpu==target&&op==SCHED_CMD_ENQUEUE&&!generation);
    ++pushes;if(accepted)published=thread;return accepted;
}
bool x86_smp_request_reschedule(uint32_t target) {assert(target<3);++ipis;return true;}
bool scheduler_cpu_available(uint32_t cpu) {return cpu<3;}
''' + helpers + operations + transfer + r'''
static sched_cmd_channel_t inbox;
static void reset(uint64_t source_floor,uint64_t target_floor,thread_t *thread) {
    memset(g_cpus,0,sizeof(g_cpus));memset(thread,0,sizeof(*thread));
    for(unsigned c=0;c<3;++c) {
        g_cpus[c].command_inbox=&inbox;
        for(unsigned p=0;p<RT_PRIORITY_LEVELS;++p)list_init(&g_cpus[c].queue.rt_queues[p]);
    }
    g_cpus[0].queue.min_vruntime=source_floor;
    g_cpus[1].queue.min_vruntime=target_floor;
    thread->sched_class=SCHED_CLASS_FAIR;
    atomic_init(&thread->state,THREAD_READY);
    for(unsigned w=0;w<MAX_CPUS/64;++w)thread->affinity.bits[w]=UINT64_MAX;
    pushes=ipis=0;published=0;accepted=true;
}
int main(void) {
    thread_t thread;
    reset(90000000000ULL,1000000,&thread);
    thread.sched.vruntime=90000004000ULL;
    assert(scheduler_transfer_local(0,&g_cpus[0],&thread,1));
    assert(published==&thread&&pushes==1&&ipis==1&&thread.sched.vruntime==4000);
    enqueue_local_raw(&g_cpus[1],&thread);
    assert(thread.sched.vruntime==1004000);
    assert(!(thread.sched.flags&SCHED_ENTITY_VRUNTIME_RELATIVE));
    /* Source clock must not impose an 89-second artificial debt on CPU1. */
    assert(thread.sched.vruntime<2000000);
    dequeue_local_raw(&g_cpus[1],&thread);
    /* Exercise the real dequeue/transfer/rollback path with another running
     * FAIR entity keeping the source clock at its independent baseline. */
    thread_t runner={0};runner.sched_class=SCHED_CLASS_FAIR;
    atomic_init(&runner.state,THREAD_RUNNING);runner.sched.vruntime=9000;
    reset(9000,800000,&thread);g_cpus[0].queue.current=&runner;
    thread.sched.vruntime=9400;enqueue_local_raw(&g_cpus[0],&thread);
    scheduler_migrate_local(0,&g_cpus[0],&thread,1);
    assert(!g_cpus[0].queue.nr_running&&thread.owner_cpu==1&&thread.sched.vruntime==400);
    enqueue_local_raw(&g_cpus[1],&thread);
    assert(thread.sched.vruntime==800400);
    dequeue_local_raw(&g_cpus[1],&thread);
    reset(9000,800000,&thread);g_cpus[0].queue.current=&runner;
    thread.sched.vruntime=9400;enqueue_local_raw(&g_cpus[0],&thread);
    accepted=false;scheduler_migrate_local(0,&g_cpus[0],&thread,1);
    assert(thread.owner_cpu==0&&thread.sched.vruntime==9400);
    assert(thread.migration_pending&&thread.migration_target_cpu==1);
    assert(g_cpus[0].queue.nr_running==1&&pick_next_local(&g_cpus[0])==&thread);
    assert(!(thread.sched.flags&SCHED_ENTITY_VRUNTIME_RELATIVE));
    dequeue_local_raw(&g_cpus[0],&thread);
    uint64_t seed=7;unsigned cases=0;
    for(unsigned trial=0;trial<50000;++trial) {
        seed=seed*UINT64_C(6364136223846793005)+1;
        uint64_t source_floor=seed;
        seed=seed*UINT64_C(6364136223846793005)+1;
        uint64_t target_floor=seed;
        reset(source_floor,target_floor,&thread);
        seed=seed*UINT64_C(6364136223846793005)+1;
        thread.sched.vruntime=seed;
        uint64_t original=seed,debt=seed>source_floor?seed-source_floor:0;
        /* Failed publication rolls back before the caller re-enqueues. */
        accepted=false;
        assert(!scheduler_transfer_local(0,&g_cpus[0],&thread,1));
        assert(thread.owner_cpu==0&&thread.sched.vruntime==original);
        assert(!thread.sched.flags&&!ipis&&!published);
        accepted=true;
        assert(scheduler_transfer_local(0,&g_cpus[0],&thread,1));
        assert(thread.sched.vruntime==debt);
        enqueue_local_raw(&g_cpus[1],&thread);
        uint64_t expected=debt>UINT64_MAX-target_floor?UINT64_MAX:target_floor+debt;
        assert(thread.sched.vruntime==expected);
        /* Duplicate enqueue must not apply the target clock twice. */
        enqueue_local_raw(&g_cpus[1],&thread);
        assert(thread.sched.vruntime==expected&&g_cpus[1].queue.nr_running==1);
        dequeue_local_raw(&g_cpus[1],&thread);++cases;
    }
    reset(9000,800000,&thread);thread.sched.vruntime=9100;
    atomic_store(&thread.state,THREAD_BLOCKED);
    scheduler_migrate_local(0,&g_cpus[0],&thread,1);
    assert(thread.owner_cpu==1&&thread.sched.vruntime==100&&!pushes&&!ipis);
    g_cpus[2].queue.min_vruntime=50000;
    scheduler_migrate_local(1,&g_cpus[1],&thread,2);
    assert(thread.owner_cpu==2&&thread.sched.vruntime==100);
    atomic_store(&thread.state,THREAD_READY);
    enqueue_local_raw(&g_cpus[2],&thread);
    assert(thread.sched.vruntime==50100);
    dequeue_local_raw(&g_cpus[2],&thread);
    /* A class change while blocked must consume the relative marker too. */
    reset(9000,800000,&thread);thread.sched.vruntime=9100;
    scheduler_prepare_fair_migration(&g_cpus[0].queue,&thread);
    thread.sched_class=SCHED_CLASS_RT;thread.rt_priority=3;
    enqueue_local_raw(&g_cpus[1],&thread);
    assert(thread.sched.vruntime==800100&&!(thread.sched.flags&SCHED_ENTITY_VRUNTIME_RELATIVE));
    dequeue_local_raw(&g_cpus[1],&thread);
    reset(9000,800000,&thread);thread.sched.vruntime=9100;
    atomic_store(&thread.state,THREAD_RUNNING);g_cpus[0].queue.current=&thread;
    scheduler_migrate_local(0,&g_cpus[0],&thread,1);
    assert(thread.migration_pending&&thread.owner_cpu==0&&thread.sched.vruntime==9100&&!pushes);
    /* Running migrations stage privately until the assembly switch finishes.
     * Invalid early-test contexts must not strand an unpublishable thread. */
    thread_t idle={0};g_cpus[0].queue.idle=&idle;
    assert(!scheduler_stage_handoff(&g_cpus[0],&thread));
    thread.arch.switch_ctx.rsp=1;
    assert(!scheduler_stage_handoff(&g_cpus[0],&thread));
    idle.arch.switch_ctx.rsp=1;
    assert(scheduler_stage_handoff(&g_cpus[0],&thread));
    assert(g_cpus[0].switch_handoff==&thread&&!pushes);
    assert(thread.owner_cpu==0&&thread.sched.vruntime==100);
    assert(atomic_load(&thread.state)==THREAD_READY&&!thread.migration_pending);
    g_cpus[0].queue.current=&idle;
    scheduler_finish_handoff();
    assert(!g_cpus[0].switch_handoff&&pushes==1&&thread.owner_cpu==1);
    enqueue_local_raw(&g_cpus[1],&thread);
    assert(thread.sched.vruntime==800100);
    printf("Fair migration: %u clock/debt/saturation/rollback cases, duplicate insertion, repeated blocked moves, class change and running-thread deferral passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='scheduler-fair-migration-') as directory:
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-I' + str(root / 'include'),
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True)
    negative = program.replace('scheduler_prepare_fair_migration(&source->queue, thread);',
                               '(void)thread;')
    assert negative != program
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=negative, text=True, check=True)
    result = subprocess.run([binary], capture_output=True)
    assert result.returncode == -signal.SIGABRT, result.stderr
    print('Negative control rejected: carrying the source absolute clock creates artificial destination debt')
