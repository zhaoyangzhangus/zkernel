"""Production local/remote EXIT transactions preserve live I/O continuations."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sched/core.c').read_text()


def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


production = '\n'.join(function(signature) for signature in (
    'static bool scheduler_exit_can_publish(',
    'bool sched_publish_dead(',
    'static void scheduler_command_consume_local('))
fixture = '#include "' + str(root / 'kernel/sched/internal.h') + '"\n' + r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
scheduler_cpu_t g_cpus[MAX_CPUS];
uint32_t g_cpu_count=2;
static uint32_t local_cpu;
static bool irq=true, queued, guard_before_consume, migrate_caller_on_irq;
static bool migrate_before_consume;
static thread_t *migrate_on_irq;
static unsigned dequeues, releases;
static sched_cmd_t pending;
bool scheduler_current_cpu(uint32_t *id) {*id=local_cpu;return true;}
bool scheduler_cpu_available(uint32_t id) {return id<2;}
static uint64_t scheduler_irq_save(void) {
 bool saved=irq;
 if(migrate_on_irq){migrate_on_irq->owner_cpu=1;migrate_on_irq=0;}
 if(migrate_caller_on_irq){local_cpu=1;migrate_caller_on_irq=false;}
 irq=false;return saved;
}
static void scheduler_irq_restore(uint64_t flags) {irq=flags!=0;}
static bool scheduler_remote_command_try_push(uint32_t from,uint32_t to,
                                              thread_t *t,uint32_t op,uint32_t gen) {
 assert(!irq && from==0 && to==1 && !queued);
 pending=(sched_cmd_t){.object=(uintptr_t)t,.op=op,.generation=gen};queued=true;
 return true;
}
bool x86_smp_request_reschedule(uint32_t cpu) {assert(irq && cpu==1);return true;}
void dequeue_local(scheduler_cpu_t *cpu,thread_t *t) {
 (void)cpu;assert(!irq && (t->sched.flags&SCHED_ENTITY_ENQUEUED));
 ++dequeues;t->sched.flags&=~SCHED_ENTITY_ENQUEUED;
}
void dequeue_local_raw(scheduler_cpu_t *cpu,thread_t *t) {dequeue_local(cpu,t);}
void enqueue_local_raw(scheduler_cpu_t *cpu,thread_t *t) {(void)cpu;(void)t;assert(0);}
static bool scheduler_complete_wake_local(scheduler_cpu_t *cpu,thread_t *t,
                                          uint32_t generation,bool epoch) {
 (void)cpu;(void)t;(void)generation;(void)epoch;assert(0);return false;
}
static void scheduler_apply_priority_local(scheduler_cpu_t *cpu,thread_t *t,
                                            uint8_t cls,uint8_t priority) {
 (void)cpu;(void)t;(void)cls;(void)priority;assert(0);
}
static void scheduler_migrate_local(uint32_t id,scheduler_cpu_t *cpu,
                                     thread_t *t,uint32_t generation) {
 (void)id;(void)cpu;(void)t;(void)generation;assert(0);
}
static void scheduler_command_release_enqueue_local(scheduler_cpu_t *cpu,thread_t *t) {
 (void)cpu;(void)t;assert(!irq);++releases;
}
static bool scheduler_command_ack_step(thread_t *t,uint32_t count,uint32_t mask,
                                        uint32_t value,bool drain);
''' + production + r'''
static bool scheduler_command_ack_step(thread_t *t,uint32_t count,uint32_t mask,
                                        uint32_t value,bool drain) {
 assert(local_cpu==0 && count==2 && drain && irq && queued);
 /* Reproduce a target entering an I/O guard after the request was sent. */
 if(guard_before_consume)sched_exit_defer_begin(t);
 if(migrate_before_consume)t->owner_cpu=0;
 uint64_t saved=scheduler_irq_save();
 scheduler_command_consume_local(1,&g_cpus[1],pending,true);
 scheduler_irq_restore(saved);queued=false;
 return (atomic_load(&t->command_ack)&mask)==value;
}
static void init(thread_t *t,unsigned state,unsigned owner,bool current,
                  unsigned nesting,bool waiter) {
 memset(t,0,sizeof(*t));memset(g_cpus,0,sizeof(g_cpus));
 atomic_init(&t->state,state);atomic_init(&t->exit_defer_count,nesting);
 atomic_init(&t->blocked_waiter,waiter ? (struct waiter *)t : 0);
 atomic_init(&t->command_ack,0U);
 t->owner_cpu=owner;t->sched.flags=SCHED_ENTITY_ENQUEUED;
 t->signal_pending=1234;t->flags=5678;
 if(current)g_cpus[owner].queue.current=t;
 static sched_cmd_channel_t inbox;g_cpus[owner].command_inbox=&inbox;
 local_cpu=0;irq=true;queued=guard_before_consume=false;dequeues=releases=0;
 migrate_on_irq=0;migrate_caller_on_irq=migrate_before_consume=false;
}
int main(void) {
 thread_t t;unsigned cases=0;
 for(unsigned remote=0;remote<2;++remote)
 for(unsigned state=THREAD_NEW;state<=THREAD_DEAD;++state)
 for(unsigned current=0;current<2;++current)
 for(unsigned nesting=0;nesting<4;++nesting)
 for(unsigned waiter=0;waiter<2;++waiter) {
  init(&t,state,remote,current,nesting,waiter);
  bool allowed=remote ? state==THREAD_NEW || state==THREAD_READY :
      state!=THREAD_DEAD && state!=THREAD_BLOCKED &&
      (state!=THREAD_RUNNING || current);
  allowed=allowed && !nesting && !waiter;
  assert(sched_publish_dead(&t)==allowed);
  assert(atomic_load(&t.state)==(allowed?THREAD_DEAD:state));
  unsigned removed=allowed && !current && (remote || state==THREAD_READY);
  assert(dequeues==removed && irq && !queued);
  assert(!!(t.sched.flags&SCHED_ENTITY_ENQUEUED)==!removed);
  assert(t.signal_pending==1234 && t.flags==5678);
  assert(atomic_load(&t.exit_defer_count)==nesting);
  assert(releases==remote);
  if(remote)assert(atomic_load(&t.command_ack)==
      (SCHED_CMD_STATE_ACK|(allowed?SCHED_CMD_STATE_CHANGED:0U)));
  ++cases;
 }
 for(unsigned repeat=0;repeat<10000;++repeat) {
  init(&t,THREAD_READY,1,false,0,false);guard_before_consume=true;
  assert(!sched_publish_dead(&t) && !dequeues &&
         atomic_load(&t.state)==THREAD_READY &&
         (t.sched.flags&SCHED_ENTITY_ENQUEUED));
  guard_before_consume=false;sched_exit_defer_end(&t);
  assert(sched_publish_dead(&t) && dequeues==1 &&
         atomic_load(&t.state)==THREAD_DEAD);
 }
 init(&t,THREAD_READY,0,false,0,false);migrate_on_irq=&t;
 assert(!sched_publish_dead(&t) && !dequeues && t.owner_cpu==1 && irq);
 assert(atomic_load(&t.state)==THREAD_READY && (t.sched.flags&SCHED_ENTITY_ENQUEUED));
 init(&t,THREAD_READY,1,false,0,false);migrate_caller_on_irq=true;
 assert(!sched_publish_dead(&t) && !queued && !dequeues && irq);
 assert(atomic_load(&t.state)==THREAD_READY && (t.sched.flags&SCHED_ENTITY_ENQUEUED));
 init(&t,THREAD_READY,1,false,0,false);migrate_before_consume=true;
 assert(!sched_publish_dead(&t) && !dequeues && !queued && releases==1 && irq);
 assert(atomic_load(&t.state)==THREAD_READY && (t.sched.flags&SCHED_ENTITY_ENQUEUED));
 assert(atomic_load(&t.command_ack)==SCHED_CMD_STATE_ACK);
 /* STOP/REMOVE retain their old queue-removal and acknowledgement policy. */
 for(unsigned op=SCHED_CMD_REMOVE;op<=SCHED_CMD_STOP;++op) {
  init(&t,THREAD_READY,1,false,1,false);
  sched_cmd_t msg={.object=(uintptr_t)&t,.op=op,.generation=SCHED_CMD_REMOVE_ACK};
  irq=false;scheduler_command_consume_local(1,&g_cpus[1],msg,true);irq=true;
  assert(dequeues==1 && releases==1 && atomic_load(&t.state)==THREAD_READY);
  assert(atomic_load(&t.command_ack)==(op==SCHED_CMD_REMOVE?SCHED_CMD_REMOVE_ACK:0U));
 }
 printf("EXIT guard: %u local/remote state/current/nesting/waiter cases, "
        "10000 request-to-consume races/retries, owner/caller migration, "
        "stale EXIT acknowledgement and REMOVE/STOP compatibility passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='scheduler-exit-guard-') as directory:
    binary = str(Path(directory) / 'test')

    def run(code):
        subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                       input=code, text=True, check=True)
        return subprocess.run([binary], capture_output=True, text=True, timeout=30)

    result = run(fixture)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout.strip())
    # Prove the matrix rejects both the missing owner-side guard and the
    # old dequeue-before-deciding bug, rather than merely covering success.
    guard = 'scheduler_exit_can_publish(thread)'
    negative = fixture.replace(guard, '(scheduler_exit_can_publish(thread), true)')
    assert run(negative).returncode != 0
    negative = fixture.replace('if (message.op == SCHED_CMD_EXIT) {',
                               'if (cpu->queue.current != thread) dequeue_local_raw(cpu, thread);\n'
                               '            if (message.op == SCHED_CMD_EXIT) {')
    assert negative != fixture and run(negative).returncode != 0
    print('Negative controls rejected: missing owner-side guard and dequeue before rejected EXIT')
