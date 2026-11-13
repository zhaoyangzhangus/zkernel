"""Actual wake paths: delayed cleanup, linear scans and multi-queue ownership."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sync/wait.c').read_text()
def function(name):
    start = source.index(name)
    return source[start:source.index('\n}', start)+2]
helpers = '\n'.join(function(name) for name in [
    'static void wait_insert_tail(', 'static void wait_remove(',
    'static void waiter_unlink_locked(',
    'static wait_subscription_t *subscription_from_node(',
    'static thread_t *waiter_claim_locked(',
    'static thread_t *waiter_claim_pinned_locked(', 'static void wait_wake_pinned('])
helpers = next(line for line in source.splitlines() if line.startswith('enum { WAIT_WAKE_BATCH_COUNT')) + '\n' + helpers
wake = source[source.index('uint32_t wake_one('):source.index('bool wait_cancel_thread(')]
wake = wake.replace('wait_subscription_t *subscription = subscription_from_node(node);',
                    '++visits; wait_subscription_t *subscription = subscription_from_node(node);')
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/wait.h>
typedef struct thread { unsigned index, refs; } thread_t;
static spinlock_t g_timeout_lock;
static unsigned lock_depth,visits,wakes,last_index,lock_acquires,pinned,peak_pinned;
static uint64_t ticks;
static unsigned wake_counts[2048];
static bool drop_owner;
static void wait_global_init(void) {}
static uint64_t x86_read_tsc(void) { return ++ticks; }
static uint64_t wait_lock(spinlock_t *lock) { spinlock_lock(lock);++lock_depth;++lock_acquires;return 0; }
static void wait_unlock(spinlock_t *lock,uint64_t flags) {
 assert(!flags && lock_depth);--lock_depth;spinlock_unlock(lock);
}
static void sched_wake(thread_t *thread) {
 assert(!lock_depth && thread->index>=last_index);
 last_index=thread->index;++wakes;assert(++wake_counts[thread->index]==1);
 if(drop_owner) { assert(thread->refs==2);--thread->refs; }
 // 有意不恢复等待者：旧实现会保留已接管的前缀，直到恢复的线程自行清理。
}
static void object_get(void *object) {
 thread_t *thread=object;assert(lock_depth==2 && thread->refs);
 ++thread->refs;++pinned;if(pinned>peak_pinned)peak_pinned=pinned;
}
static void object_put(void *object) {
 thread_t *thread=object;assert(!lock_depth && thread->refs==(drop_owner?1U:2U) && pinned);
 --thread->refs;--pinned;
}
''' + helpers + wake + r'''
static waiter_t waiters[2048];
static thread_t threads[2048];
static struct { uint64_t before;wait_subscription_t entries[2048][2];uint64_t after; } storage;
static wait_queue_t first,second;
static list_head_t timeouts;
static unsigned list_count(list_head_t *head) {
 unsigned count=0;for(list_head_t *n=head->next;n!=head;n=n->next)++count;return count;
}
int main(void) {
 const unsigned sizes[]={0,1,2,4,8,15,16,17,31,32,33,256,1024,2048};
 for(unsigned si=0;si<sizeof(sizes)/sizeof(*sizes);++si)for(unsigned pattern=0;pattern<4;++pattern) {
  unsigned count=sizes[si],expected=0;
  memset(waiters,0,sizeof(waiters));memset(&storage,0,sizeof(storage));memset(wake_counts,0,sizeof(wake_counts));
  storage.before=storage.after=0x123456789abcdef0ULL;
  spinlock_init(&g_timeout_lock);spinlock_init(&first.lock);spinlock_init(&second.lock);
  list_init(&first.waiters);list_init(&second.waiters);list_init(&timeouts);
  atomic_init(&first.sequence,37);atomic_init(&second.sequence,91);
  for(unsigned i=0;i<count;++i) {
   threads[i].index=i;threads[i].refs=1;waiters[i].thread=&threads[i];waiters[i].subscriptions=storage.entries[i];
   waiters[i].subscription_count=2;
   unsigned state=pattern==0?WAITER_WAITING:pattern==1?WAITER_WOKEN:
    pattern==2?i%4:(i<count/2?WAITER_TIMED_OUT:WAITER_WAITING);
   atomic_init(&waiters[i].state,state);if(state==WAITER_WAITING)++expected;
   list_init(&waiters[i].timeout_node);wait_insert_tail(&timeouts,&waiters[i].timeout_node);
   for(unsigned q=0;q<2;++q) {
    wait_subscription_t *subscription=&storage.entries[i][q];
    subscription->waiter=&waiters[i];subscription->queue=q?&second:&first;
    list_init(&subscription->queue_node);
    wait_insert_tail(&subscription->queue->waiters,&subscription->queue_node);
   }
  }
  visits=wakes=last_index=lock_acquires=pinned=peak_pinned=0;
  assert(wake_all(&first)==expected && wakes==expected && visits==count);
  assert(lock_acquires==2*(expected/16+1) && !pinned && peak_pinned<=16);
  assert(list_empty(&first.waiters) && list_count(&second.waiters)==count);
  assert(list_count(&timeouts)==count && !lock_depth);
  assert(atomic_load(&first.sequence)==37+expected+1);
  for(unsigned i=0;i<count;++i) {
   assert(threads[i].refs==1);
   assert(atomic_load(&waiters[i].state)!=WAITER_WAITING);
   assert(storage.entries[i][0].queue_node.next==&storage.entries[i][0].queue_node);
   assert(storage.entries[i][1].queue_node.next!=&storage.entries[i][1].queue_node);
  }
  // 其他队列的通知会清除终态节点，但不得重复唤醒。
  visits=0;assert(wake_all(&second)==0 && visits==count && wakes==expected);
  assert(list_empty(&second.waiters) && list_count(&timeouts)==count);
  assert(atomic_load(&second.sequence)==92);
  for(unsigned i=0;i<count;++i)waiter_unlink_locked(&waiters[i]);
  assert(list_empty(&timeouts));
  assert(storage.before==0x123456789abcdef0ULL && storage.after==storage.before);
 }

 // 数量上限覆盖空操作、批次边界、终态前缀和大队列。
 const unsigned limits[]={0,1,2,15,16,17,31,32,33,1024,UINT32_MAX};
 for(unsigned si=0;si<sizeof(sizes)/sizeof(*sizes);++si)
 for(unsigned pattern=0;pattern<4;++pattern)
 for(unsigned li=0;li<sizeof(limits)/sizeof(*limits);++li) {
  unsigned count=sizes[si],limit=limits[li],available=0;
  memset(waiters,0,sizeof(waiters));memset(&storage,0,sizeof(storage));
  memset(wake_counts,0,sizeof(wake_counts));
  list_init(&first.waiters);list_init(&second.waiters);list_init(&timeouts);
  atomic_store(&first.sequence,37);atomic_store(&second.sequence,91);
  for(unsigned i=0;i<count;++i) {
   threads[i]=(thread_t){i,1};waiters[i].thread=&threads[i];
   waiters[i].subscriptions=storage.entries[i];waiters[i].subscription_count=2;
   unsigned state=pattern==0?WAITER_WAITING:pattern==1?WAITER_WOKEN:
    pattern==2?i%4:(i<count/2?WAITER_TIMED_OUT:WAITER_WAITING);
   atomic_init(&waiters[i].state,state);if(state==WAITER_WAITING)++available;
   list_init(&waiters[i].timeout_node);wait_insert_tail(&timeouts,&waiters[i].timeout_node);
   for(unsigned q=0;q<2;++q) {
    wait_subscription_t *sub=&storage.entries[i][q];
    sub->waiter=&waiters[i];sub->queue=q?&second:&first;
    list_init(&sub->queue_node);wait_insert_tail(&sub->queue->waiters,&sub->queue_node);
   }
  }
  unsigned expected=available<limit?available:limit,scanned=0,claimed=0;
  while(scanned<count && claimed<limit) {
   if(atomic_load(&waiters[scanned].state)==WAITER_WAITING)++claimed;
   ++scanned;
  }
  visits=wakes=last_index=lock_acquires=pinned=peak_pinned=0;
  assert(wake_n(&first,limit)==expected && wakes==expected && visits==scanned);
  unsigned batches=(expected+15)/16;
  if(limit && expected<limit && expected%16==0)++batches;
  assert(lock_acquires==2*batches && !lock_depth && !pinned && peak_pinned<=16);
  assert(atomic_load(&first.sequence)==37+expected+(expected<limit?1U:0U));
  assert(list_count(&first.waiters)==count-scanned && list_count(&second.waiters)==count);
  for(unsigned i=0;i<count;++i) {
   assert(threads[i].refs==1);
   assert((storage.entries[i][0].queue_node.next==&storage.entries[i][0].queue_node)==(i<scanned));
  }
  assert(wake_all(&second)==available-expected && wakes==available);
  for(unsigned i=0;i<count;++i)waiter_unlink_locked(&waiters[i]);
  assert(list_empty(&first.waiters) && list_empty(&second.waiters) && list_empty(&timeouts));
 }
 assert(wake_n(0,17)==0);
 puts("PASS: bounded wake FIFO/exact-count/sequence/remaining subscriptions at 0/1/15/16/17 through 2048; 1024 bounded wakes require 128 lock acquisitions instead of 2048");
 // 所有者在 sched_wake 期间退出：批次固定引用必须保留到唤醒返回。
 list_init(&first.waiters);visits=wakes=last_index=pinned=peak_pinned=0;
 memset(wake_counts,0,sizeof(wake_counts));drop_owner=true;
 for(unsigned i=0;i<17;++i) {
  threads[i].index=i;threads[i].refs=1;waiters[i].thread=&threads[i];
  atomic_store(&waiters[i].state,WAITER_WAITING);
  storage.entries[i][0].waiter=&waiters[i];
  list_init(&storage.entries[i][0].queue_node);
  wait_insert_tail(&first.waiters,&storage.entries[i][0].queue_node);
 }
 assert(wake_all(&first)==17 && !pinned && peak_pinned==16);
 for(unsigned i=0;i<17;++i)assert(!threads[i].refs);
 drop_owner=false;
 assert(wake_one(0)==0 && wake_all(0)==0);
 puts("PASS: broadcast FIFO/exact-once/multi-queue, 15/16/17 boundaries, delayed cleanup and owner exit; 1024-thread lock acquisitions 2050 -> 130");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-wake-scan-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'include'), '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', str(path / 'test.c'), '-o', str(path / 'test')],
                   check=True)
    subprocess.run([str(path / 'test')], check=True)
    (path / 'test.c').write_text(program.replace('object_get(thread);',
                                                'if (false) object_get(thread);'))
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'include'), '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    result = subprocess.run([str(path / 'test')], capture_output=True)
    assert result.returncode != 0, 'Unpinned batch thread accepted'
    print('Negative control rejected: batch threads without lifetime pins')
