"""Whole production timer implementation: lifecycle and initialized hot path."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/time/hrtimer.c').read_text()
code = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#define timer_create host_posix_timer_create
#include <pthread.h>
#include <sched.h>
#undef timer_create
static atomic_uint init_attempts;
static atomic_uint expiry_calls;
static atomic_uint poll_slot_checks;
static _Bool parallel_init;
static _Bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
 atomic_fetch_add(&init_attempts,1);
 if(parallel_init)while(atomic_load(&init_attempts)<8)sched_yield();
 return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + source.replace('atomic_compare_exchange_strong_explicit(&g_timer_init_state',
                     'counted_cas(&g_timer_init_state').replace(
                         'atomic_store_explicit(&timer->fired, true, memory_order_release);',
                         'atomic_fetch_add(&expiry_calls,1); atomic_store_explicit(&timer->fired, true, memory_order_release);').replace(
                             'timer_object_t *timer = g_timers[i];',
                             'atomic_fetch_add(&poll_slot_checks,1); timer_object_t *timer = g_timers[i];') + r'''
static _Thread_local unsigned depth;
static atomic_uint allocations,frees;
static bool fail_next_allocation;
static atomic_uint notifications;
static uint64_t now=100000;
void sched_preempt_disable(void) {++depth;}
void sched_preempt_enable(void) {assert(depth);--depth;}
uint64_t x86_read_tsc(void) {return now;}
uint64_t x86_timeout_ns_to_tsc(uint64_t ns) {return ns;}
void *kzalloc(size_t n,uint32_t flags) {
 (void)flags;if(fail_next_allocation) {fail_next_allocation=false;return 0;}
 void *p=calloc(1,n);if(p)++allocations;return p;
}
void kfree(void *p) {if(p) {assert(((timer_object_t*)p)->registry_slot==TIMER_OBJECT_LIMIT);++frees;}free(p);}
void object_header_init(object_header_t *h,object_type_id_t type,const object_ops_t *ops) {
 refcount_init(&h->refs,1);h->type=type;h->ops=ops;
 spinlock_init(&h->signal_waiters.lock);list_init(&h->signal_waiters.waiters);
 atomic_init(&h->signal_waiters.sequence,0);
}
void object_notify_signaled(void *p) {
 assert(p);++notifications;atomic_fetch_add(&((object_header_t*)p)->signal_waiters.sequence,1);
}
bool object_is_signaled(const void *p) {return ((const object_header_t*)p)->ops->is_signaled(p);}
int64_t object_wait_value(const void *p) {return ((const object_header_t*)p)->ops->wait_value(p);}
void object_put(void *p) {
 object_header_t *h=p;unsigned refs=atomic_fetch_sub(&h->refs.value,1);assert(refs);
 if(refs==1)h->ops->destroy(p);
}
static void *initialize_worker(void *unused) {
 (void)unused;timer_global_initialize();
 assert(atomic_load(&g_timer_init_state)==2 && !atomic_load(&g_timer_lock.state));
 assert(g_timer_scan_limit==0);
 for(unsigned i=0;i<TIMER_SLOT_WORDS;++i)assert(!g_timer_occupied[i]);
 for(unsigned i=0;i<TIMER_OBJECT_LIMIT;++i)assert(!g_timers[i]);
 return 0;
}
static void *poll_worker(void *opaque) {
 uint64_t deadline=*(uint64_t*)opaque;
 for(unsigned i=0;i<10000;++i)timer_poll(deadline);
 assert(!depth);return 0;
}
static void *cancel_worker(void *opaque) {
 timer_object_t *timer=opaque;
 for(unsigned i=0;i<10000;++i)assert(timer_cancel(timer)==K_OK);
 assert(object_wait_value(timer)==K_ECANCELED && !depth);return 0;
}
static void *lifecycle_worker(void *unused) {
 (void)unused;
 for(unsigned i=0;i<1000;++i) {
  timer_object_t *timer=0;assert(timer_create(5000,0,&timer)==K_OK);
  timer_poll(now);if(i&1)assert(timer_cancel(timer)==K_OK);
  object_put(timer);assert(!depth);
 }
 return 0;
}
int main(void) {
 for(unsigned i=0;i<TIMER_OBJECT_LIMIT;++i)g_timers[i]=(void*)1;
 for(unsigned i=0;i<TIMER_SLOT_WORDS;++i)g_timer_occupied[i]=UINT64_MAX;
 parallel_init=true;pthread_t threads[8];
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initialize_worker,0));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 parallel_init=false;
 assert(timer_self_test() && !depth && allocations==frees && atomic_load(&init_attempts)==8);
 unsigned initialized=atomic_load(&init_attempts);
 timer_object_t *timer=(void*)1;
 assert(timer_create(0,999,&timer)==K_EINVAL && timer==(void*)1);
 assert(timer_create(0,0,0)==K_EINVAL && timer_cancel(0)==K_EINVAL);
 timer_object_t invalid={0};assert(timer_cancel(&invalid)==K_EINVAL);
 fail_next_allocation=true;
 assert(timer_create(25,0,&timer)==K_ENOMEM && timer==(void*)1 && !g_timer_scan_limit);
 assert(timer_create(25,0,&timer)==K_OK);
 assert(timer->deadline_tsc==now+25 && !object_is_signaled(timer) && object_wait_value(timer)==0);
 unsigned before=notifications,locks=expiry_calls;timer_poll(now+24);
 assert(!object_is_signaled(timer) && notifications==before && !depth && expiry_calls==locks);
 timer_poll(now+25);assert(object_is_signaled(timer) && object_wait_value(timer)==1 && notifications==before+1);
 before=notifications;locks=expiry_calls;
 for(unsigned i=0;i<10000;++i)timer_poll(now+25+i);
 assert(notifications==before && object_wait_value(timer)==1 && !depth && expiry_calls==locks);
 assert(timer_cancel(timer)==K_OK && object_wait_value(timer)==K_ECANCELED);
 before=notifications;
 for(unsigned i=0;i<10000;++i)assert(timer_cancel(timer)==K_OK);
 timer_poll(now+26);assert(notifications==before);object_put(timer);
 assert(notifications==before); // destructor must not notify cancellation again
 assert(timer_create(5000,0,&timer)==K_OK);before=notifications;
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,cancel_worker,timer));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 assert(notifications==before+1 && object_wait_value(timer)==K_ECANCELED);
 object_put(timer);assert(notifications==before+1);
 assert(timer_create(5000,0,&timer)==K_OK);before=notifications;
 object_put(timer);assert(notifications==before+1); // uncanceled destruction still cancels
 assert(timer_create(100,1000,&timer)==K_OK);
 uint64_t deadline=timer->deadline_tsc;
 timer_poll(deadline-1);assert(!object_is_signaled(timer));
 timer_poll(deadline);assert(timer->deadline_tsc==deadline+1000 && object_wait_value(timer)==1);
 before=notifications;
 timer_poll(deadline+1000);assert(timer->deadline_tsc==deadline+2000);
 assert(notifications==before+1);
 timer_poll(deadline+1999);assert(notifications==before+1);
 timer->deadline_tsc=UINT64_MAX-500;before=notifications;
 timer_poll(UINT64_MAX-501);assert(notifications==before);
 timer_poll(UINT64_MAX-500);assert(timer->deadline_tsc==UINT64_MAX && notifications==before+1);
 timer_poll(UINT64_MAX-1);assert(notifications==before+1);
 timer_poll(UINT64_MAX);assert(timer->deadline_tsc==UINT64_MAX && notifications==before+2);
 object_put(timer);
 // 同时到期时只能发布/通知一次；即使后续又轮询 80000 次，就绪状态仍须保持可见，
 // 直到显式取消。
 assert(timer_create(25,0,&timer)==K_OK);deadline=timer->deadline_tsc;before=notifications;locks=expiry_calls;
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,poll_worker,&deadline));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 assert(notifications==before+1 && expiry_calls==locks+1 && object_is_signaled(timer) && object_wait_value(timer)==1);
 assert(timer_cancel(timer)==K_OK && notifications==before+2 && object_wait_value(timer)==K_ECANCELED);
 object_put(timer);
 timer_object_t *full[TIMER_OBJECT_LIMIT];
 for(unsigned i=0;i<TIMER_OBJECT_LIMIT;++i)assert(timer_create(5000,0,&full[i])==K_OK);
 locks=expiry_calls;before=notifications;
 for(unsigned i=0;i<1000;++i)timer_poll(now);
 assert(expiry_calls==locks && notifications==before);
 timer=(void*)1;assert(timer_create(0,0,&timer)==K_ENOMEM && timer==(void*)1);
 object_put(full[63]);assert(timer_create(5000,0,&full[63])==K_OK);
 for(unsigned i=0;i<TIMER_OBJECT_LIMIT;++i)object_put(full[i]);
 for(unsigned i=0;i<TIMER_OBJECT_LIMIT;++i)assert(!g_timers[i]);
 assert(!g_timer_scan_limit);
 // 真实的创建/回收序列只留下最后一个槽位；轮询时必须保持其生命周期，同时跳过全部
 // 127 个已回收指针槽位。
 for(unsigned i=0;i<TIMER_OBJECT_LIMIT;++i)assert(timer_create(5000,0,&full[i])==K_OK);
 for(unsigned i=0;i<TIMER_OBJECT_LIMIT-1;++i)object_put(full[i]);
 unsigned sparse_scans=poll_slot_checks;
 timer_poll(now);assert(poll_slot_checks==sparse_scans+1 && g_timer_scan_limit==TIMER_OBJECT_LIMIT);
 object_put(full[TIMER_OBJECT_LIMIT-1]);assert(!g_timer_scan_limit);
 unsigned scans=poll_slot_checks;
 for(unsigned i=0;i<10000;++i)timer_poll(now);
 assert(poll_slot_checks==scans);
 // 独立建模最低空闲槽位和精确的最高占用边界。
 for(unsigned i=0;i<TIMER_OBJECT_LIMIT;++i)full[i]=0;
 unsigned seed=19;
 for(unsigned step=0;step<20000;++step) {
  seed=seed*1664525U+1013904223U;unsigned selected=(seed>>16)%TIMER_OBJECT_LIMIT;
  if(full[selected]) {
   if(seed&1) {object_put(full[selected]);full[selected]=0;}
   else assert(timer_cancel(full[selected])==K_OK);
  } else {
   unsigned lowest=0;while(lowest<TIMER_OBJECT_LIMIT && full[lowest])++lowest;
   timer_object_t *created=(void*)1;
   assert(timer_create(5000,0,&created)==K_OK && lowest<TIMER_OBJECT_LIMIT);
   full[lowest]=created;assert(created->registry_slot==lowest);
  }
  unsigned bound=0,registered=0;
  for(unsigned i=0;i<TIMER_OBJECT_LIMIT;++i) {
   assert(g_timers[i]==full[i]);assert(bitmap_test_bit(g_timer_occupied,i)==(full[i]!=0));
   if(full[i]) {assert(full[i]->registry_slot==i);bound=i+1;++registered;}
  }
  assert(g_timer_scan_limit==bound);
  if(step%16==0) {
   scans=poll_slot_checks;timer_poll(now);assert(poll_slot_checks==scans+registered);
  }
 }
 for(unsigned i=0;i<TIMER_OBJECT_LIMIT;++i)if(full[i])object_put(full[i]);
 assert(!g_timer_scan_limit);
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,lifecycle_worker,0));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 assert(!g_timer_scan_limit);
 assert(atomic_load(&init_attempts)==initialized && allocations==frees && !depth);
 sched_preempt_disable();sched_preempt_disable();timer_poll(now);
 assert(depth==2);sched_preempt_enable();sched_preempt_enable();
 puts("Timers: eight initializers, 80000 one-shot polls, 90000 serial/concurrent cancels with single notification, destructor notification policy, 20000 slot/bound oracle operations, 8000 concurrent lifecycles, OOM/periods and 10000 empty polls passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-hrtimer-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
