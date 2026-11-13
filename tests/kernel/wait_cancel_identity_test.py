"""Actual cancellation reads current stack-waiter identity only after locking."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/sync/wait.c').read_text()
def function(name):
    start=source.index(name)
    return source[start:source.index('\n}',start)+2]
body='\n'.join(function(name) for name in [
    'static void wait_remove(', 'static void waiter_unlink_locked(',
    'static thread_t *waiter_claim_locked(', 'static thread_t *waiter_claim_pinned_locked(',
    'static void wait_wake_pinned(', 'bool wait_cancel_thread('])
fixture=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <kernel/wait.h>
typedef struct thread { unsigned refs; _Atomic(waiter_t *) blocked_waiter; } thread_t;
static spinlock_t g_timeout_lock;
static wait_queue_t queues[16];
static wait_subscription_t subscriptions[16];
static thread_t target;
static unsigned depth,gets,puts_count,wakes,mode,queue_count;
static bool race_armed;
static waiter_t *old_waiter;
static uint64_t x86_read_tsc(void) { return 1; }
static void wait_global_init(void) {}
static waiter_t *new_waiter(void) {
 waiter_t *w=calloc(1,sizeof(*w));assert(w);
 w->thread=&target;w->subscriptions=subscriptions;w->subscription_count=queue_count;
 atomic_init(&w->state,WAITER_WAITING);list_init(&w->timeout_node);
 for(unsigned i=0;i<queue_count;++i) {
  subscriptions[i].waiter=w;subscriptions[i].queue=&queues[i];
  list_init(&subscriptions[i].queue_node);
  queues[i].waiters.next=queues[i].waiters.prev=&subscriptions[i].queue_node;
  subscriptions[i].queue_node.next=subscriptions[i].queue_node.prev=&queues[i].waiters;
 }
 return w;
}
static uint64_t wait_lock(spinlock_t *lock) {
 if(lock==&g_timeout_lock && race_armed) {
  assert(!depth);race_armed=false;
  /* 取消方获取公共超时锁之前，所有者已经完成操作。 */
  atomic_store(&target.blocked_waiter,NULL);
  for(unsigned i=0;i<queue_count;++i)list_init(&queues[i].waiters);
  free(old_waiter);
  if(mode==2)atomic_store(&target.blocked_waiter,new_waiter());
 }
 spinlock_lock(lock);++depth;return 0;
}
static void wait_unlock(spinlock_t *lock,uint64_t flags) {
 assert(!flags && depth);--depth;spinlock_unlock(lock);
}
static void object_get(void *p) {
 thread_t *t=p;assert(depth && t==&target && t->refs==1);++t->refs;++gets;
}
static void object_put(void *p) {
 thread_t *t=p;assert(!depth && t->refs==2);--t->refs;++puts_count;
}
static void sched_wake(thread_t *t) { assert(!depth && t->refs==2);++wakes; }
'''
checks=r'''
int main(void) {
 const unsigned sizes[]={1,4,16};
 for(unsigned repeat=0;repeat<100;++repeat)for(mode=0;mode<3;++mode)
 for(unsigned s=0;s<3;++s) {
  queue_count=sizes[s];spinlock_init(&g_timeout_lock);
  for(unsigned i=0;i<queue_count;++i) { spinlock_init(&queues[i].lock);list_init(&queues[i].waiters); }
  target.refs=1;old_waiter=new_waiter();atomic_init(&target.blocked_waiter,old_waiter);
  gets=puts_count=wakes=depth=0;race_armed=mode!=0;
  bool cancelled=wait_cancel_thread(&target);
  assert(cancelled==(mode!=1) && !race_armed && !depth && target.refs==1);
  assert(gets==(mode!=1?1U:0U) && puts_count==gets && wakes==gets);
  waiter_t *current=atomic_load(&target.blocked_waiter);
  if(current) {
   assert(atomic_load(&current->state)==WAITER_CANCELLED);
   for(unsigned i=0;i<queue_count;++i)assert(list_empty(&queues[i].waiters));
   assert(!wait_cancel_thread(&target) && gets==1);
   atomic_store(&target.blocked_waiter,NULL);free(current);
  }
  assert(!wait_cancel_thread(&target));
 }
 puts("900 cancellation identity cases: live waiter, completed/freed waiter and newly published waiter; pins balanced");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-wait-cancel-identity-') as directory:
    path=Path(directory)
    def run(actual):
        (path/'test.c').write_text(fixture+actual+checks)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                        '-I'+str(root/'include'),'-fsanitize=address,undefined',
                        '-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
        return subprocess.run([str(path/'test')],capture_output=True,text=True)
    result=run(body)
    assert result.returncode==0,result.stdout+result.stderr
    print(result.stdout.strip())
    load='''    waiter_t *waiter = atomic_load_explicit(&target->blocked_waiter,
                                           memory_order_acquire);'''
    assert load in body
    negative=body.replace(load,'').replace(
        '    uint64_t timeout_flags = wait_lock(&g_timeout_lock);',
        load+'\n    uint64_t timeout_flags = wait_lock(&g_timeout_lock);')
    result=run(negative)
    assert result.returncode!=0 and 'heap-use-after-free' in result.stderr, result.stderr
    print('Negative control reproduced use-after-free: waiter loaded before the timeout lock')
