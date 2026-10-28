"""Whole production mutex implementation with controlled scheduler/wait hooks."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/sync/mutex.c').read_text()
code = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
static atomic_uint attempts;
static _Bool parallel;
static _Bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
 atomic_fetch_add(&attempts,1);
 if(parallel)while(atomic_load(&attempts)<8)sched_yield();
 return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + source.replace('atomic_compare_exchange_strong_explicit(&g_pi_init_state',
                     'counted_cas(&g_pi_init_state') + r'''
static _Thread_local thread_t *current;
static _Thread_local unsigned preempt_depth;
void sched_preempt_disable(void){++preempt_depth;}
void sched_preempt_enable(void){assert(preempt_depth);--preempt_depth;}
static unsigned wakes,waits,internal_waits;
static bool release_owner,async_priority;
thread_t *sched_current_thread(void) {return current;}
void sched_initialize_test_thread(thread_t *thread,uint64_t tid,uint8_t cls,uint8_t priority) {
 memset(thread,0,sizeof(*thread));thread->tid=tid;
 thread->sched_class=thread->base_sched_class=cls;
 thread->rt_priority=thread->base_rt_priority=priority;
 list_init(&thread->owned_mutexes);
}
void sched_set_effective_priority(thread_t *thread,uint8_t cls,uint8_t priority) {
 assert(atomic_load(&g_pi_lock.state));
 if(async_priority){
  atomic_store(&thread->priority_request,((unsigned)cls<<8U)|priority);
  atomic_store(&thread->priority_queued,true);return;
 }
 thread->sched_class=cls;thread->rt_priority=priority;
}
void wait_queue_init(wait_queue_t *queue) {
 spinlock_init(&queue->lock);list_init(&queue->waiters);atomic_init(&queue->sequence,0);
}
uint32_t wake_all(wait_queue_t *queue) {(void)queue;assert(!atomic_load(&g_pi_lock.state));++wakes;return 0;}
kstatus_t wait_on_queue(wait_queue_t *queue,bool (*predicate)(void *),void *context,uint64_t timeout) {
 (void)queue;(void)timeout;++waits;
 assert(!preempt_depth);
 kmutex_wait_context_t *wait=context;kmutex_t *mutex=wait->mutex;
 assert(!atomic_load(&g_pi_lock.state) && current->pi_blocked_on==mutex);
 if(mutex->pi_enabled)assert(mutex->owner->sched_class==SCHED_CLASS_RT && mutex->owner->rt_priority==current->rt_priority);
 if(release_owner) {
  thread_t *saved=current;current=mutex->owner;
  assert(kmutex_unlock(mutex)==K_OK);current=saved;
  assert(predicate(context));return K_OK;
 }
 assert(!predicate(context));return K_ETIMEDOUT;
}
static void *initializer(void *unused) {(void)unused;pi_initialize();assert(!atomic_load(&g_pi_lock.state));return 0;}
kstatus_t wait_on_queue_uninterruptible(wait_queue_t *queue,bool (*predicate)(void *),void *context,uint64_t timeout) {
 assert(atomic_load(&current->exit_defer_count)==1U);
 ++internal_waits;return wait_on_queue(queue,predicate,context,timeout);
}
int main(void) {
 pthread_t threads[8];parallel=true;
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initializer,0));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 parallel=false;assert(atomic_load(&attempts)==8 && kmutex_pi_self_test());
 /* 远端捐赠尚未落到运行队列时，也要传播逻辑优先级，并允许及时撤销。 */
 thread_t donor,owner; kmutex_t pending; kmutex_waiter_t pending_waiter;
 sched_initialize_test_thread(&donor,8,SCHED_CLASS_FAIR,0);
 sched_initialize_test_thread(&owner,9,SCHED_CLASS_FAIR,0);
 atomic_store(&donor.priority_request,((unsigned)SCHED_CLASS_RT<<8U)|2U);
 atomic_store(&donor.priority_queued,true);
 assert(donation_priority(&donor)==2U);
 kmutex_init(&pending,true);list_init(&pending_waiter.node);
 pending_waiter.thread=&donor;pending_waiter.mutex=&pending;pending_waiter.linked=true;
 async_priority=true;pi_lock();assign_owner(&pending,&owner);
 list_add_tail(&pending.pi_waiters,&pending_waiter.node);
 recompute_thread_priority(&owner,0);
 assert(owner.sched_class==SCHED_CLASS_FAIR && atomic_load(&owner.priority_queued));
 assert(atomic_load(&owner.priority_request)==(((unsigned)SCHED_CLASS_RT<<8U)|2U));
 list_del(&pending_waiter.node);recompute_thread_priority(&owner,0);
 assert(atomic_load(&owner.priority_request)==((unsigned)SCHED_CLASS_FAIR<<8U));
 list_del(&pending.owner_node);pending.owner=0;pi_unlock();async_priority=false;
 thread_t low,high;
 sched_initialize_test_thread(&low,10,SCHED_CLASS_FAIR,0);
 sched_initialize_test_thread(&high,11,SCHED_CLASS_RT,2);
 kmutex_t mutex;memset(&mutex,0,sizeof(mutex));
 assert(kmutex_try_lock(&mutex)==K_EINVAL && kmutex_unlock(&mutex)==K_EINVAL);
 assert(kmutex_lock_uninterruptible(&mutex,0)==K_EINVAL);
 for(unsigned enabled=0;enabled<2;++enabled) {
  kmutex_init(&mutex,enabled);current=0;
  assert(kmutex_try_lock(&mutex)==K_EPERM && kmutex_lock(&mutex,0)==K_EPERM);
  assert(kmutex_lock_uninterruptible(&mutex,0)==K_EPERM);
  current=&low;assert(kmutex_try_lock(&mutex)==K_OK && mutex.owner==&low);
  assert(kmutex_try_lock(&mutex)==K_EBUSY && kmutex_lock(&mutex,0)==K_EBUSY);
  current=&high;assert(kmutex_try_lock(&mutex)==K_EAGAIN && kmutex_unlock(&mutex)==K_EPERM);
  release_owner=false;assert(kmutex_lock(&mutex,100)==K_ETIMEDOUT);
  assert(!high.pi_blocked_on && mutex.owner==&low && low.sched_class==SCHED_CLASS_FAIR);
  assert(mutex.pi_waiters.next==&mutex.pi_waiters);
  release_owner=true;assert(kmutex_lock(&mutex,100)==K_OK && mutex.owner==&high);
  assert(!high.pi_blocked_on && low.sched_class==SCHED_CLASS_FAIR);
  assert(kmutex_unlock(&mutex)==K_OK && !mutex.owner);
  assert(low.owned_mutexes.next==&low.owned_mutexes && high.owned_mutexes.next==&high.owned_mutexes);
  current=&low;assert(kmutex_try_lock(&mutex)==K_OK);current=&high;
  release_owner=false;assert(kmutex_lock_uninterruptible(&mutex,100)==K_ETIMEDOUT);
  assert(!atomic_load(&high.exit_defer_count) && !mutex.exit_deferred);
  assert(!high.pi_blocked_on && mutex.owner==&low && list_empty(&mutex.pi_waiters));
  release_owner=true;assert(kmutex_lock_uninterruptible(&mutex,100)==K_OK);
  assert(mutex.owner==&high && !high.pi_blocked_on && mutex.exit_deferred);
  assert(atomic_load(&high.exit_defer_count)==1U);
  assert(kmutex_lock_uninterruptible(&mutex,0)==K_EBUSY);
  assert(atomic_load(&high.exit_defer_count)==1U);
  current=&low;assert(kmutex_unlock(&mutex)==K_EPERM);
  assert(atomic_load(&high.exit_defer_count)==1U);current=&high;
  assert(kmutex_unlock(&mutex)==K_OK);
  assert(!atomic_load(&high.exit_defer_count) && !mutex.exit_deferred);
 }
 /* Real two-mutex owner cycle must fail before invoking the wait hook. */
 kmutex_t first,second;kmutex_init(&first,true);kmutex_init(&second,true);
 current=&low;assert(kmutex_try_lock(&first)==K_OK);
 current=&high;assert(kmutex_try_lock(&second)==K_OK);
 high.pi_blocked_on=&first;unsigned before=waits;current=&low;
 assert(kmutex_lock(&second,0)==K_EBUSY && waits==before);
 high.pi_blocked_on=0;assert(kmutex_unlock(&first)==K_OK);
 current=&high;assert(kmutex_unlock(&second)==K_OK);
 /* Distinct nested I/O guards retain the continuation through the last unlock. */
 assert(kmutex_lock_uninterruptible(&first,0)==K_OK);
 assert(kmutex_lock_uninterruptible(&second,0)==K_OK);
 assert(atomic_load(&high.exit_defer_count)==2U);
 high.signal_pending=123U;high.flags=456U;
 assert(kmutex_unlock(&first)==K_OK && atomic_load(&high.exit_defer_count)==1U);
 assert(kmutex_unlock(&second)==K_OK && !atomic_load(&high.exit_defer_count));
 assert(high.signal_pending==123U && high.flags==456U);
 assert(kmutex_lock_uninterruptible(&first,0)==K_OK);
 assert(kmutex_try_lock(&second)==K_OK && !second.exit_deferred);
 assert(kmutex_unlock(&second)==K_OK && atomic_load(&high.exit_defer_count)==1U);
 assert(kmutex_unlock(&first)==K_OK && !atomic_load(&high.exit_defer_count));
 high.sched_class=high.base_sched_class=SCHED_CLASS_IDLE;
 current=&low;low.owner_cpu=1;assert(kmutex_try_lock(&first)==K_OK);
 current=&high;high.owner_cpu=0;
 assert(kmutex_try_lock_bootstrap(&first)==K_EAGAIN);
 low.owner_cpu=0;assert(kmutex_try_lock_bootstrap(&first)==K_EBUSY);
 current=&low;assert(kmutex_unlock(&first)==K_OK);current=&high;
 assert(kmutex_try_lock_bootstrap(&first)==K_OK);
 assert(kmutex_try_lock_bootstrap(&first)==K_EBUSY);
 assert(kmutex_unlock(&first)==K_OK);
 assert(kmutex_lock_uninterruptible(&first,0)==K_EPERM);
 assert(!atomic_load(&high.exit_defer_count) && !first.owner);
 high.sched_class=high.base_sched_class=SCHED_CLASS_RT;
 for(unsigned i=0;i<10000;++i)kmutex_init(&mutex,true);
 assert(atomic_load(&attempts)==8 && !atomic_load(&g_pi_lock.state) && !preempt_depth && waits==8 && internal_waits==4 && wakes==16);
 puts("Mutex PI: eight concurrent initializers, donation/restoration, PI-on/off timeout/handoff, exit-deferral nesting/failure/unlock, mixed guards, deadlock cycle and 10000 initialized calls passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-mutex-pi-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
