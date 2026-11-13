"""Actual notify/cancel/timeout: owner exits at unlock, scheduler pointer stays live."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/sync/wait.c').read_text()
def function(name):
    start=source.index(name)
    return source[start:source.index('\n}',start)+2]
names=['static void wait_insert_tail(', 'static void wait_remove(',
       'static void waiter_unlink_locked(',
       'static wait_subscription_t *subscription_from_node(',
       'static waiter_t *waiter_from_timeout_node(', 'static bool deadline_reached(',
       'static thread_t *waiter_claim_locked(', 'static thread_t *waiter_claim_pinned_locked(',
       'static void wait_wake_pinned(', 'uint32_t wake_one(', 'bool wait_cancel_thread(',
       'void wait_poll_timeouts(']
actual='\n'.join(function(name) for name in names)
actual=next(line for line in source.splitlines() if line.startswith('enum { WAIT_WAKE_BATCH_COUNT'))+'\n'+actual
fixture=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/wait.h>
typedef struct thread { unsigned refs; _Atomic(waiter_t *) blocked_waiter; } thread_t;
static spinlock_t g_timeout_lock;
static list_head_t g_timeout_waiters;
static atomic_uint_fast64_t g_timeout_next_deadline;
static atomic_uint g_wait_init_state;
static unsigned depth,gets,puts_count,wakes,destroys,lock_calls;
static bool exit_at_unlock;
static thread_t *owner;
static uint64_t x86_read_tsc(void) { return 1; }
static void wait_global_init(void) {}
static uint64_t wait_lock(spinlock_t *lock) { spinlock_lock(lock);++depth;++lock_calls;return 0; }
static void wait_unlock(spinlock_t *lock,uint64_t flags) {
 assert(!flags && depth);--depth;spinlock_unlock(lock);
 if(!depth && exit_at_unlock) {
  assert(owner->refs==2);--owner->refs;exit_at_unlock=false;
 }
}
static void object_get(void *object) {
 thread_t *thread=object;assert(depth && thread->refs==1);++thread->refs;++gets;
}
static void object_put(void *object) {
 thread_t *thread=object;assert(!depth && thread->refs);++puts_count;
 if(!--thread->refs)++destroys;
}
static void sched_wake(thread_t *thread) { assert(!depth && thread->refs);++wakes; }
'''
checks=r'''
int main(void) {
 const unsigned sizes[]={1,2,8,16,32};
 for(unsigned action=0;action<3;++action)
 for(unsigned si=0;si<5;++si)
 for(unsigned state=WAITER_WAITING;state<=WAITER_CANCELLED;++state)
 for(unsigned exiting=0;exiting<2;++exiting) {
  thread_t thread={.refs=1};waiter_t waiter={.thread=&thread};
  atomic_init(&thread.blocked_waiter,&waiter);
  wait_queue_t queues[32];wait_subscription_t subscriptions[32];
  spinlock_init(&g_timeout_lock);list_init(&g_timeout_waiters);
  atomic_store(&g_wait_init_state,2);atomic_store(&g_timeout_next_deadline,10);atomic_init(&waiter.state,state);
  unsigned count=sizes[si];waiter.subscriptions=subscriptions;waiter.subscription_count=count;
  waiter.deadline_tsc=10;list_init(&waiter.timeout_node);
  wait_insert_tail(&g_timeout_waiters,&waiter.timeout_node);
  for(unsigned i=0;i<count;++i) {
   spinlock_init(&queues[i].lock);list_init(&queues[i].waiters);atomic_init(&queues[i].sequence,0);
   subscriptions[i].queue=&queues[i];subscriptions[i].waiter=&waiter;
   list_init(&subscriptions[i].queue_node);
   wait_insert_tail(&queues[i].waiters,&subscriptions[i].queue_node);
  }
  bool claimed=state==WAITER_WAITING;
  depth=gets=puts_count=wakes=destroys=lock_calls=0;owner=&thread;exit_at_unlock=exiting && claimed;
  if(action==0) assert(wake_one(&queues[0])==(claimed?1U:0U));
  else if(action==1) assert(wait_cancel_thread(&thread)==claimed);
  else wait_poll_timeouts(10);
  if(action==1)assert(lock_calls==(claimed?count+1U:1U));
  assert(!depth && !exit_at_unlock && gets==(claimed?1U:0U) && puts_count==gets && wakes==gets);
  assert(thread.refs==(exiting && claimed ? 0U : 1U));
  assert(destroys==(exiting && claimed ? 1U : 0U));
  if(claimed) {
   assert(atomic_load(&waiter.state)==(action==0?WAITER_WOKEN:action==1?WAITER_CANCELLED:WAITER_TIMED_OUT));
   if(action==1) {
    assert(list_empty(&g_timeout_waiters));
    for(unsigned i=0;i<count;++i)assert(list_empty(&queues[i].waiters));
   } else if(action==2) assert(list_empty(&g_timeout_waiters));
   else assert(list_empty(&queues[0].waiters));
  }
  unsigned before_locks=lock_calls;
  assert(!wait_cancel_thread(&thread));assert(gets==(claimed?1U:0U));
  assert(lock_calls==before_locks+1);
  waiter_unlink_locked(&waiter);
 }
 assert(!wake_one(NULL) && !wait_cancel_thread(NULL));
 thread_t invalid={.refs=1};atomic_init(&invalid.blocked_waiter,NULL);
 assert(!wait_cancel_thread(&invalid));
 puts("120 notify/cancel/timeout layouts: exact-once pin, owner exit at unlock, unlocked final destruction and no failed-claim refs passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-wait-thread-') as directory:
    path=Path(directory)
    def run(body):
        (path/'test.c').write_text(fixture+body+checks)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                        '-I'+str(root/'include'),'-fsanitize=address,undefined',
                        '-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
        return subprocess.run([str(path/'test')],capture_output=True,text=True)
    result=run(actual)
    assert result.returncode==0,result.stdout+result.stderr
    print(result.stdout.strip())
    negative=actual.replace('if (thread != 0) object_get(thread);',
                            'if (false) object_get(thread);')
    assert run(negative).returncode != 0,'Missing thread pin survived owner exit'
    print('Negative control rejected: claimed thread not pinned before unlock')
    negative=actual.replace(
        '        atomic_load_explicit(&waiter->state, memory_order_acquire) != WAITER_WAITING ||\n','')
    assert negative != actual
    assert run(negative).returncode != 0,'Terminal cancellation still took every queue lock'
    print('Negative control rejected: terminal cancellation locks all subscription queues')
