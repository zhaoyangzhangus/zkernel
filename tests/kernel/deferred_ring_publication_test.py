"""Actual normal deferred producers: all ring positions, traces and publication."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/irq/deferred.c').read_text()
enqueue=source[source.index('static bool deferred_enqueue_locked('):source.index('bool deferred_schedule_critical(')]
enqueue=enqueue.replace('static bool deferred_enqueue_locked(deferred_work_fn_t function, void *argument) {',
                        'static bool deferred_enqueue_locked(deferred_work_fn_t function, void *argument) { assert(lock_depth==1);')
run=source[source.index('uint32_t deferred_run('):source.index('static void deferred_self_test_work(')]
previous=r'''bool deferred_schedule(deferred_work_fn_t function, void *argument) {
    uint64_t flags;
    deferred_debug_set(1U);
    if (function == 0 || !deferred_init()) return false;
    deferred_debug_set(2U);
    flags = deferred_lock();
    deferred_debug_set(4U);
    if (g_deferred.count == DEFERRED_QUEUE_CAPACITY) {
        deferred_unlock(flags);
        return false;
    }
    g_deferred.items[g_deferred.tail].function = function;
    g_deferred.items[g_deferred.tail].argument = argument;
    g_deferred.tail = (g_deferred.tail + 1U) % DEFERRED_QUEUE_CAPACITY;
    ++g_deferred.count;
    deferred_debug_set(5U);
    deferred_unlock(flags);
    deferred_debug_set(6U);
    deferred_wake_worker();
    deferred_debug_set(11U);
    return true;
}

bool deferred_try_schedule(deferred_work_fn_t function, void *argument) {
    uint64_t flags;
    if (function == 0 || !deferred_init()) return false;
    flags = deferred_lock();
    if (g_deferred.count == DEFERRED_QUEUE_CAPACITY) {
        deferred_unlock(flags);
        return false;
    }
    g_deferred.items[g_deferred.tail].function = function;
    g_deferred.items[g_deferred.tail].argument = argument;
    g_deferred.tail = (g_deferred.tail + 1U) % DEFERRED_QUEUE_CAPACITY;
    ++g_deferred.count;
    deferred_unlock(flags);
    deferred_wake_worker();
    return true;
}

'''
previous=previous.replace('deferred_schedule(', 'old_schedule(').replace('deferred_try_schedule(', 'old_try(')
program=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#define DEFERRED_QUEUE_CAPACITY 128U
#define DEFERRED_CRITICAL_BURST_LIMIT 4U
typedef void (*deferred_work_fn_t)(void *);
typedef struct {deferred_work_fn_t function;void *argument;} deferred_item_t;
typedef struct {
    deferred_item_t items[128];unsigned head,tail,count,critical_burst;
    deferred_item_t critical_item;bool critical_ready;
} queue_t;
static queue_t g_deferred;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned lock_depth,trace[16],trace_count;
static bool initialized=true,concurrent;
static atomic_uint wakes,executed,producers_done;
static void record(unsigned event) {
    if(concurrent)return;assert(trace_count<16);trace[trace_count++]=event;
}
static bool deferred_init(void) {assert(!lock_depth);record(50);return initialized;}
static void deferred_debug_set(unsigned event) {
    assert(lock_depth==((event==4||event==5)?1U:0U));record(event);
}
static uint64_t deferred_lock(void) {
    assert(!lock_depth);assert(!pthread_mutex_lock(&mutex));++lock_depth;record(51);return 0;
}
static void deferred_unlock(uint64_t flags) {
    assert(!flags&&lock_depth==1);record(52);--lock_depth;assert(!pthread_mutex_unlock(&mutex));
}
static void deferred_wake_worker(void) {
    assert(!lock_depth);record(60);atomic_fetch_add(&wakes,1);
}
static bool deferred_take_critical(deferred_item_t *item) {
    (void)item;assert(lock_depth==1);assert(!g_deferred.critical_ready);return false;
}
static unsigned rcu_poll(unsigned budget) {(void)budget;assert(!lock_depth);return 0;}
static uint64_t x86_read_tsc(void) {return 0;}
static void deferred_record_work(deferred_work_fn_t fn,uint64_t start) {
    assert(fn&&!start&&!lock_depth);
}
''' + enqueue + previous + run + r'''
#define TOTAL 8000U
static atomic_uint seen[TOTAL];
static bool fifo;
static unsigned fifo_next;
static void even(void *argument) {
    assert(!lock_depth);unsigned id=(unsigned)(uintptr_t)argument-1;
    assert(id<TOTAL&&!(id&1));assert(atomic_fetch_add(&seen[id],1)==0);
    if(fifo)assert(id==fifo_next++);
    atomic_fetch_add(&executed,1);
}
static void odd(void *argument) {
    assert(!lock_depth);unsigned id=(unsigned)(uintptr_t)argument-1;
    assert(id<TOTAL&&(id&1));assert(atomic_fetch_add(&seen[id],1)==0);
    if(fifo)assert(id==fifo_next++);
    atomic_fetch_add(&executed,1);
}
static queue_t initial;
typedef struct {queue_t queue;unsigned events[16],event_count,wakes;} snapshot_t;
static void reset(void) {
    memcpy(&g_deferred,&initial,sizeof(initial));
    trace_count=0;memset(trace,0,sizeof(trace));atomic_store(&wakes,0);
}
static void snapshot(snapshot_t *out) {
    memset(out,0,sizeof(*out));memcpy(&out->queue,&g_deferred,sizeof(g_deferred));
    memcpy(out->events,trace,sizeof(trace));out->event_count=trace_count;
    out->wakes=atomic_load(&wakes);
}
static void *producer(void *argument) {
    unsigned base=(unsigned)(uintptr_t)argument*2000;
    for(unsigned n=0;n<2000;++n) {
        unsigned id=base+n;deferred_work_fn_t fn=id&1?odd:even;
        bool ok;
        do {ok=n&1?deferred_schedule(fn,(void *)(uintptr_t)(id+1)):
                      deferred_try_schedule(fn,(void *)(uintptr_t)(id+1));
            if(!ok)sched_yield();
        } while(!ok);
    }
    atomic_fetch_add(&producers_done,1);return 0;
}
static void *consumer(void *unused) {
    (void)unused;
    while(atomic_load(&producers_done)!=4||atomic_load(&executed)!=TOTAL) {
        assert(deferred_run(1)<=1);sched_yield();
    }
    return 0;
}
int main(void) {
    unsigned cases=0;
    for(unsigned count=0;count<=128;++count)
    for(unsigned tail=0;tail<128;++tail)
    for(unsigned init=0;init<2;++init)
    for(unsigned null=0;null<2;++null)
    for(unsigned diagnostic=0;diagnostic<2;++diagnostic) {
        memset(&initial,0,sizeof(initial));
        for(unsigned n=0;n<128;++n)initial.items[n]=(deferred_item_t){odd,(void *)(uintptr_t)(n+1)};
        initial.count=count;initial.tail=tail;initial.head=(tail+128-count)%128;
        initial.critical_burst=3;initial.critical_ready=true;
        initial.critical_item=(deferred_item_t){even,(void *)77};initialized=init;
        deferred_work_fn_t fn=null?0:even;reset();
        bool expected=diagnostic?old_schedule(fn,(void *)91):old_try(fn,(void *)91);
        snapshot_t old,actual;snapshot(&old);reset();
        bool ok=diagnostic?deferred_schedule(fn,(void *)91):deferred_try_schedule(fn,(void *)91);
        snapshot(&actual);assert(ok==expected&&!memcmp(&old,&actual,sizeof(old)));
        assert(ok==(bool)(init&&!null&&count<128));
        assert(actual.wakes==(unsigned)ok&&!lock_depth);
        ++cases;
    }
    /* A wrapped full queue, alternating producer APIs, drains in exact FIFO
     * order through the production consumer; failed extra enqueue is inert. */
    initialized=true;concurrent=true;memset(&g_deferred,0,sizeof(g_deferred));
    g_deferred.head=g_deferred.tail=119;atomic_store(&wakes,0);fifo=true;
    for(unsigned id=0;id<128;++id) {
        deferred_work_fn_t fn=id&1?odd:even;
        assert(id&1?deferred_schedule(fn,(void *)(uintptr_t)(id+1)):
                    deferred_try_schedule(fn,(void *)(uintptr_t)(id+1)));
    }
    assert(!deferred_schedule(even,(void *)999)&&!deferred_try_schedule(even,(void *)999));
    assert(atomic_load(&wakes)==128&&deferred_run(128)==128&&fifo_next==128);
    assert(g_deferred.head==119&&!g_deferred.count&&!g_deferred.critical_burst);
    fifo=false;memset(&g_deferred,0,sizeof(g_deferred));
    for(unsigned n=0;n<TOTAL;++n)atomic_store(&seen[n],0);
    atomic_store(&wakes,0);atomic_store(&executed,0);
    pthread_t producers[4],consumers[2];
    for(unsigned n=0;n<2;++n)assert(!pthread_create(&consumers[n],0,consumer,0));
    for(unsigned n=0;n<4;++n)assert(!pthread_create(&producers[n],0,producer,(void *)(uintptr_t)n));
    for(unsigned n=0;n<4;++n)assert(!pthread_join(producers[n],0));
    for(unsigned n=0;n<2;++n)assert(!pthread_join(consumers[n],0));
    assert(atomic_load(&executed)==TOTAL&&atomic_load(&wakes)==TOTAL&&!g_deferred.count);
    for(unsigned n=0;n<TOTAL;++n)assert(atomic_load(&seen[n])==1);
    printf("Normal deferred ring: %u exact old/new capacity/wrap/error/diagnostic-trace/wake cases; 128-item wrapped FIFO drain; 8000 concurrent mixed-entry callbacks exactly once with untorn pairs\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-deferred-ring-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)

