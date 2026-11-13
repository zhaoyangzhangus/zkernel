"""Actual occupied-slot timer poll against the original linear locked poll."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/time/hrtimer.c').read_text()
poll=source[source.index('void timer_poll('):source.index('bool timer_self_test(')]
deadline=source[source.index('static bool timer_deadline_reached('):source.index('kstatus_t timer_create(')]
previous=r'''void timer_poll(uint64_t now_tsc) {
    timer_global_initialize();
    timer_global_lock();
    for (uint32_t i = 0; i < g_timer_scan_limit; ++i) {
        timer_object_t *timer = g_timers[i];
        if (timer == 0 || atomic_load_explicit(&timer->canceled, memory_order_acquire)) {
            continue;
        }
        /* fired 表示持久就绪状态，而不是可消费令牌。所有轮询器都由全局锁串行化，且
         * 只有轮询过程会发布 fired；单次计时器完成后不再需要锁/通知。 */
        if (timer->period_tsc == 0U &&
            atomic_load_explicit(&timer->fired, memory_order_acquire)) continue;
        /* 创建和更新期限也由全局锁串行化。尚未到期的计时器无需在每个时钟节拍都获取
         * 对象锁。 */
        if (!timer_deadline_reached(now_tsc, timer->deadline_tsc)) continue;
        timer_lock(timer);
        atomic_store_explicit(&timer->fired, true, memory_order_release);
        if (timer->period_tsc != 0) {
            uint64_t next = timer->deadline_tsc + timer->period_tsc;
            timer->deadline_tsc = next < timer->deadline_tsc ? UINT64_MAX : next;
        }
        object_notify_signaled(timer);
        timer_unlock(timer);
    }
    timer_global_unlock();
}

'''.replace('void timer_poll(', 'static void previous_poll(')
program='#include "'+str(root/'include/kernel/timer.h')+'"\n#include <kernel/bitmap.h>\n'+r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static timer_object_t timers[TIMER_OBJECT_LIMIT];
static timer_object_t *g_timers[TIMER_OBJECT_LIMIT];
static uint32_t g_timer_scan_limit;
static uint64_t g_timer_occupied[(TIMER_OBJECT_LIMIT+63U)/64U];
static unsigned visits;
static unsigned depth,initial_depth,global_calls,object_calls,notifications;
static unsigned notified[TIMER_OBJECT_LIMIT],max_depth;
static bool cancel_next,legacy;
static void timer_global_initialize(void) {}
static void timer_global_lock(void) {assert(depth==initial_depth);++depth;++global_calls;}
static void timer_global_unlock(void) {assert(depth==initial_depth+1);--depth;}
static void timer_lock(timer_object_t *timer) {
    assert(legacy&&timer>=timers&&timer<timers+TIMER_OBJECT_LIMIT&&depth==initial_depth+1);
    ++depth;++object_calls;
}
static void timer_unlock(timer_object_t *timer) {(void)timer;assert(depth==initial_depth+2);--depth;}
void object_notify_signaled(void *object) {
    timer_object_t *timer=object;unsigned index=timer->object.flags;
    assert(index<TIMER_OBJECT_LIMIT&&timer==timers+index);
    assert(depth==initial_depth+(legacy?2U:1U));
    if(depth>max_depth)max_depth=depth;
    notified[notifications++]=index;
    if(cancel_next)atomic_store(&timers[(index+1)%TIMER_OBJECT_LIMIT].canceled,true);
}
''' + deadline + poll.replace('timer_object_t *timer = g_timers[i];',
    '++visits; timer_object_t *timer = g_timers[i];') + previous.replace(
    'timer_object_t *timer = g_timers[i];',
    '++visits; timer_object_t *timer = g_timers[i];') + r'''
typedef struct {
    timer_object_t timers[TIMER_OBJECT_LIMIT];unsigned notified[TIMER_OBJECT_LIMIT],notifications;
} snapshot_t;
static unsigned bound,pattern;
static bool canceled,fired;
static uint64_t expiration,period;
static void reset(bool old) {
    memset(timers,0,sizeof(timers));memset(g_timers,0,sizeof(g_timers));
    memset(notified,0,sizeof(notified));notifications=global_calls=object_calls=0;
    depth=max_depth=initial_depth;legacy=old;g_timer_scan_limit=bound;visits=0;
    bitmap_zero(g_timer_occupied,TIMER_OBJECT_LIMIT);
    for(unsigned n=0;n<TIMER_OBJECT_LIMIT;++n) {
        timers[n].object.flags=n;atomic_init(&timers[n].canceled,canceled);
        atomic_init(&timers[n].fired,fired);
        timers[n].deadline_tsc=expiration;timers[n].period_tsc=period;
        if(n<bound&&(pattern==0||(pattern==1&&n%2)||
                    (pattern==3&&n%17==0)||n==bound-1)) {
            g_timers[n]=timers+n;bitmap_set_bit(g_timer_occupied,n);
        }
    }
}
static void snapshot(snapshot_t *out) {
    memset(out,0,sizeof(*out));memcpy(out->timers,timers,sizeof(timers));
    memcpy(out->notified,notified,sizeof(notified));out->notifications=notifications;
}
int main(void) {
    const uint64_t times[]={0,1,1000,INT64_MAX-1,INT64_MAX,UINT64_MAX-1000,UINT64_MAX-1,UINT64_MAX};
    const uint64_t periods[]={0,1,1000,UINT64_MAX};
    const unsigned bounds[]={0,1,16,63,64,65,127,TIMER_OBJECT_LIMIT};
    unsigned cases=0,total_removed=0;
    uint64_t old_visits=0,new_visits=0;
    for(unsigned b=0;b<sizeof(bounds)/sizeof(bounds[0]);++b)
    for(pattern=0;pattern<4;++pattern)
    for(unsigned now=0;now<8;++now)
    for(unsigned deadline_index=0;deadline_index<8;++deadline_index)
    for(unsigned p=0;p<4;++p)
    for(unsigned state=0;state<4;++state)
    for(unsigned cancellation=0;cancellation<2;++cancellation)
    for(initial_depth=0;initial_depth<3;++initial_depth) {
        bound=bounds[b];expiration=times[deadline_index];period=periods[p];
        canceled=(state&1)!=0;fired=(state&2)!=0;cancel_next=cancellation;
        reset(true);previous_poll(times[now]);snapshot_t old,actual;snapshot(&old);
        unsigned previous_notifications=notifications,previous_calls=object_calls;
        assert(visits==bound);old_visits+=visits;
        assert(global_calls==1&&depth==initial_depth&&previous_calls==previous_notifications);
        assert(max_depth==initial_depth+(previous_notifications?2U:0U));
        reset(false);timer_poll(times[now]);snapshot(&actual);
        assert(!memcmp(&old,&actual,sizeof(old)));
        assert(global_calls==1&&!object_calls&&depth==initial_depth);
        assert(max_depth==initial_depth+(notifications?1U:0U));
        unsigned registered=0;
        for(unsigned i=0;i<TIMER_OBJECT_LIMIT;++i)registered+=(g_timers[i]!=0);
        assert(visits==registered);new_visits+=visits;
        total_removed+=previous_calls;++cases;
    }
    printf("Timer poll: %u exact old/new state/deadline/overflow/period/cancel/notify/order/hole/nested-preemption cases; %u redundant object-lock pairs removed; global lifetime lock preserved\n",cases,total_removed);
    printf("Occupied-slot reads: %llu -> %llu; sparse final-slot fixture 128 -> 1\n",
           (unsigned long long)old_visits,(unsigned long long)new_visits);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-timer-lock-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
