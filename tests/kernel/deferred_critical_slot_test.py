"""Actual critical slot: old-state oracle, callback requeue and concurrent publication."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/irq/deferred.c').read_text()
critical = source[source.index('bool deferred_schedule_critical('):source.index('static void deferred_record_work(')]
run = source[source.index('uint32_t deferred_run('):source.index('static void deferred_self_test_work(')]
has_work = source[source.index('static bool deferred_worker_has_work_locked('):source.index('static bool deferred_worker_prepare_sleep(')]
sleep = source[source.index('static bool deferred_worker_prepare_sleep('):source.index('static void __attribute__((noreturn))')]
assert 'critical_state' not in source
previous_critical = r'''bool deferred_schedule_critical(deferred_work_fn_t function, void *argument) {
    unsigned expected = DEFERRED_CRITICAL_EMPTY;
    uint64_t flags;

    /*
     * This single slot is deliberately reserved for the xHCI MSI-X worker.
     * Its caller owns a coalescing queued bit, so a READY/RUNNING item is
     * already sufficient to cover every later interrupt.  Do not turn this
     * into a shared fallback queue without adding per-source ownership.
     *
     * Publication now shares deferred_lock with the worker's sleep handshake.
     * That is what closes the EMPTY->BLOCKED lost-wakeup window for the
     * emergency slot as well as the normal ring.
     */
    if (function == 0 || !deferred_init()) return false;

    flags = deferred_lock();

    if (!atomic_compare_exchange_strong_explicit(&g_deferred.critical_state,
                                                 &expected,
                                                 DEFERRED_CRITICAL_WRITING,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        deferred_unlock(flags);
        return false;
    }

    g_deferred.critical_item.function = function;
    g_deferred.critical_item.argument = argument;

    atomic_store_explicit(&g_deferred.critical_state,
                          DEFERRED_CRITICAL_READY,
                          memory_order_release);

    deferred_unlock(flags);
    deferred_wake_worker();
    return true;
}

static bool deferred_take_critical(deferred_item_t *item) {
    unsigned expected = DEFERRED_CRITICAL_READY;
    if (item == 0 ||
        !atomic_compare_exchange_strong_explicit(&g_deferred.critical_state,
                                                 &expected,
                                                 DEFERRED_CRITICAL_RUNNING,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return false;
    }
    *item = g_deferred.critical_item;
    atomic_store_explicit(&g_deferred.critical_state, DEFERRED_CRITICAL_EMPTY,
                          memory_order_release);
    return item->function != 0;
}

'''
previous_critical = previous_critical.replace('g_deferred', 'old_queue').replace(
    'deferred_schedule_critical(', 'old_schedule(').replace('deferred_take_critical(', 'old_take(')
previous_run = run.replace('g_deferred', 'old_queue').replace(
    'deferred_run(', 'old_run(').replace('deferred_take_critical(', 'old_take(')
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#define DEFERRED_QUEUE_CAPACITY 128U
#define DEFERRED_CRITICAL_BURST_LIMIT 4U
enum {DEFERRED_CRITICAL_EMPTY=0,DEFERRED_CRITICAL_WRITING=1,
      DEFERRED_CRITICAL_READY=2,DEFERRED_CRITICAL_RUNNING=3};
typedef void (*deferred_work_fn_t)(void *);
typedef struct {deferred_work_fn_t function;void *argument;} deferred_item_t;
typedef struct {atomic_uint state;} thread_t;
enum {THREAD_RUNNING=1,THREAD_BLOCKED=2,THREAD_READY=3};
typedef struct {
    deferred_item_t items[128];uint32_t head,tail,count,critical_burst;
    deferred_item_t critical_item;bool critical_ready;thread_t worker;
} queue_t;
static queue_t g_deferred;
static struct {
    deferred_item_t items[128];uint32_t head,tail,count,critical_burst;
    deferred_item_t critical_item;atomic_uint critical_state;
} old_queue;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned lock_depth;
static bool initialized=true,legacy,requeue;
static unsigned followups,log_count;
static uintptr_t log_items[256];
static atomic_uint wakes,rcu_budget,old_cas;
static thread_t *current_thread;
static unsigned sleep_calls;
static void (*after_unlock)(void);
static thread_t *sched_current_thread(void) {return current_thread;}
static bool sched_publish_blocked(thread_t *thread) {
    assert(lock_depth==1&&thread==&g_deferred.worker);
    assert(!g_deferred.count&&!g_deferred.critical_ready);
    ++sleep_calls;atomic_store(&thread->state,THREAD_BLOCKED);return true;
}
static bool deferred_init(void) {return initialized;}
static uint64_t deferred_lock(void) {
    assert(!lock_depth);assert(!pthread_mutex_lock(&mutex));++lock_depth;return 0;
}
static void deferred_unlock(uint64_t flags) {
    assert(!flags&&lock_depth==1);--lock_depth;assert(!pthread_mutex_unlock(&mutex));
    if(after_unlock) {void (*hook)(void)=after_unlock;after_unlock=0;hook();}
}
static void deferred_wake_worker(void) {
    assert(!lock_depth);atomic_fetch_add(&wakes,1);
    unsigned expected=THREAD_BLOCKED;
    (void)atomic_compare_exchange_strong(&g_deferred.worker.state,&expected,THREAD_READY);
}
static unsigned rcu_poll(unsigned budget) {
    assert(!lock_depth);atomic_fetch_add(&rcu_budget,budget);return 0;
}
static uint64_t x86_read_tsc(void) {return 0;}
static void deferred_record_work(deferred_work_fn_t fn,uint64_t start) {
    assert(fn&&!start&&!lock_depth);
}
static bool counted_cas(atomic_uint *state,unsigned *expected,unsigned desired,
                        memory_order success,memory_order failure) {
    assert(lock_depth==1);atomic_fetch_add(&old_cas,1);
    return atomic_compare_exchange_strong_explicit(state,expected,desired,success,failure);
}
''' + critical + has_work + sleep + r'''
#undef atomic_compare_exchange_strong_explicit
#define atomic_compare_exchange_strong_explicit counted_cas
''' + previous_critical + run + previous_run + r'''
static void normal(void *argument) {
    assert(!lock_depth&&log_count<256);log_items[log_count++]=(uintptr_t)argument;
}
static void priority(void *argument) {
    assert(!lock_depth&&log_count<256);log_items[log_count++]=1000+(uintptr_t)argument;
    if(requeue&&followups<3) {
        ++followups;
        bool ok=legacy?old_schedule(priority,(void *)(uintptr_t)followups):
                       deferred_schedule_critical(priority,(void *)(uintptr_t)followups);
        assert(ok);
    }
}
static void enqueue_at_sleep_boundary(void) {
    assert(!lock_depth&&atomic_load(&g_deferred.worker.state)==THREAD_BLOCKED);
    assert(deferred_schedule_critical(priority,(void *)71));
}
static void reset(bool occupied,unsigned normals,unsigned burst,bool previous) {
    memset(&g_deferred,0,sizeof(g_deferred));memset(&old_queue,0,sizeof(old_queue));
    for(unsigned n=0;n<128;++n) {
        g_deferred.items[n]=(deferred_item_t){normal,(void *)(uintptr_t)n};
        old_queue.items[n]=g_deferred.items[n];
    }
    g_deferred.count=old_queue.count=normals;
    g_deferred.tail=old_queue.tail=normals%128;
    g_deferred.critical_burst=old_queue.critical_burst=burst;
    g_deferred.critical_item=old_queue.critical_item=(deferred_item_t){priority,(void *)7};
    g_deferred.critical_ready=occupied;
    atomic_store(&old_queue.critical_state,occupied?DEFERRED_CRITICAL_READY:DEFERRED_CRITICAL_EMPTY);
    legacy=previous;followups=log_count=0;memset(log_items,0,sizeof(log_items));
    atomic_store(&wakes,0);atomic_store(&rcu_budget,0);
}
typedef struct {
    queue_t queue;uintptr_t log[256];unsigned calls,followups,wakes,rcu;
} snapshot_t;
static void snapshot(snapshot_t *out,bool previous) {
    memset(out,0,sizeof(*out));
    if(previous) {
        memcpy(out->queue.items,old_queue.items,sizeof(old_queue.items));
        out->queue.head=old_queue.head;out->queue.tail=old_queue.tail;
        out->queue.count=old_queue.count;out->queue.critical_burst=old_queue.critical_burst;
        out->queue.critical_item=old_queue.critical_item;
        unsigned state=atomic_load(&old_queue.critical_state);
        assert(state==DEFERRED_CRITICAL_EMPTY||state==DEFERRED_CRITICAL_READY);
        out->queue.critical_ready=state==DEFERRED_CRITICAL_READY;
    } else {
        memcpy(out->queue.items,g_deferred.items,sizeof(g_deferred.items));
        out->queue.head=g_deferred.head;out->queue.tail=g_deferred.tail;
        out->queue.count=g_deferred.count;out->queue.critical_burst=g_deferred.critical_burst;
        out->queue.critical_item=g_deferred.critical_item;
        out->queue.critical_ready=g_deferred.critical_ready;
    }
    memcpy(out->log,log_items,sizeof(log_items));
    out->calls=log_count;out->followups=followups;
    out->wakes=atomic_load(&wakes);out->rcu=atomic_load(&rcu_budget);
}
#define TOTAL 8000U
static atomic_uint seen[TOTAL],executed,producers_done;
static void even(void *argument) {
    assert(!lock_depth);unsigned id=(unsigned)(uintptr_t)argument-1;
    assert(id<TOTAL&&!(id&1));assert(atomic_fetch_add(&seen[id],1)==0);
    atomic_fetch_add(&executed,1);
}
static void odd(void *argument) {
    assert(!lock_depth);unsigned id=(unsigned)(uintptr_t)argument-1;
    assert(id<TOTAL&&(id&1));assert(atomic_fetch_add(&seen[id],1)==0);
    atomic_fetch_add(&executed,1);
}
static void *producer(void *argument) {
    unsigned base=(unsigned)(uintptr_t)argument*2000;
    for(unsigned n=0;n<2000;++n) {
        unsigned id=base+n;
        while(!deferred_schedule_critical(id&1?odd:even,(void *)(uintptr_t)(id+1)))sched_yield();
    }
    atomic_fetch_add(&producers_done,1);return 0;
}
static void *consumer(void *argument) {
    (void)argument;
    while(atomic_load(&producers_done)!=4||atomic_load(&executed)!=TOTAL) {
        assert(deferred_run(1)<=1);sched_yield();
    }
    return 0;
}
int main(void) {
    unsigned cases=0;
    const unsigned counts[]={0,1,3,4,5,16,127,128},budgets[]={0,1,4,5,64,129};
    for(unsigned occupied=0;occupied<2;++occupied)
    for(unsigned c=0;c<8;++c)
    for(unsigned burst=0;burst<=4;++burst)
    for(unsigned b=0;b<6;++b)
    for(unsigned repeat=0;repeat<2;++repeat) {
        requeue=repeat;reset(occupied,counts[c],burst,true);
        unsigned expected=old_run(budgets[b]);snapshot_t old,actual;snapshot(&old,true);
        reset(occupied,counts[c],burst,false);
        assert(deferred_run(budgets[b])==expected);snapshot(&actual,false);
        assert(!memcmp(&old,&actual,sizeof(old)));
        uint64_t flags=deferred_lock();
        assert(deferred_worker_has_work_locked()==(g_deferred.count!=0||g_deferred.critical_ready));
        deferred_unlock(flags);++cases;
    }
    requeue=false;
    for(unsigned init=0;init<2;++init)
    for(unsigned occupied=0;occupied<2;++occupied)
    for(unsigned null=0;null<2;++null) {
        initialized=init;reset(occupied,128,4,true);
        bool expected=old_schedule(null?0:priority,(void *)31);
        snapshot_t old,actual;snapshot(&old,true);
        reset(occupied,128,4,false);
        assert(deferred_schedule_critical(null?0:priority,(void *)31)==expected);
        snapshot(&actual,false);assert(!memcmp(&old,&actual,sizeof(old)));++cases;
    }
    initialized=true;
    for(unsigned occupied=0;occupied<2;++occupied)
    for(unsigned null=0;null<2;++null)
    for(unsigned bad_function=0;bad_function<2;++bad_function) {
        reset(occupied,0,0,true);
        if(bad_function)old_queue.critical_item.function=0;
        deferred_item_t expected={normal,(void *)99},actual=expected;
        uint64_t flags=deferred_lock();bool ok=old_take(null?0:&expected);deferred_unlock(flags);
        snapshot_t old,current;snapshot(&old,true);
        reset(occupied,0,0,false);
        if(bad_function)g_deferred.critical_item.function=0;
        flags=deferred_lock();assert(deferred_take_critical(null?0:&actual)==ok);deferred_unlock(flags);
        snapshot(&current,false);
        assert(!memcmp(&expected,&actual,sizeof(actual))&&!memcmp(&old,&current,sizeof(old)));++cases;
    }
    assert(atomic_load(&old_cas)>0);
    unsigned previous_attempts=atomic_load(&old_cas);
    unsigned sleep_cases=0;
    for(unsigned count=0;count<2;++count)
    for(unsigned occupied=0;occupied<2;++occupied)
    for(unsigned target=0;target<2;++target) {
        reset(occupied,count,0,false);sleep_calls=0;
        atomic_store(&g_deferred.worker.state,THREAD_RUNNING);
        current_thread=target?&g_deferred.worker:0;
        bool should_sleep=target&&!count&&!occupied;
        assert(deferred_worker_prepare_sleep()==should_sleep);
        assert(sleep_calls==(unsigned)should_sleep);
        assert(atomic_load(&g_deferred.worker.state)==(should_sleep?THREAD_BLOCKED:THREAD_RUNNING));
        ++sleep_cases;
    }
    reset(false,0,0,false);current_thread=&g_deferred.worker;sleep_calls=0;
    atomic_store(&g_deferred.worker.state,THREAD_RUNNING);
    after_unlock=enqueue_at_sleep_boundary;
    assert(deferred_worker_prepare_sleep()&&sleep_calls==1&&!after_unlock);
    assert(g_deferred.critical_ready&&atomic_load(&g_deferred.worker.state)==THREAD_READY);
    assert(deferred_run(1)==1&&log_count==1&&log_items[0]==1071);
    assert(!g_deferred.critical_ready);++sleep_cases;
    reset(false,0,0,false);requeue=false;
    pthread_t producers[4],consumers[2];
    for(unsigned n=0;n<2;++n)assert(!pthread_create(&consumers[n],0,consumer,0));
    for(unsigned n=0;n<4;++n)assert(!pthread_create(&producers[n],0,producer,(void *)(uintptr_t)n));
    for(unsigned n=0;n<4;++n)assert(!pthread_join(producers[n],0));
    for(unsigned n=0;n<2;++n)assert(!pthread_join(consumers[n],0));
    assert(atomic_load(&executed)==TOTAL&&atomic_load(&wakes)==TOTAL);
    for(unsigned n=0;n<TOTAL;++n)assert(atomic_load(&seen[n])==1);
    uint64_t flags=deferred_lock();assert(!deferred_worker_has_work_locked());deferred_unlock(flags);
    assert(!g_deferred.critical_ready&&atomic_load(&old_cas)==previous_attempts);
    printf("Critical slot: %u complete old-state/queue/order/wake/RCU oracle cases; %u concurrent callbacks exactly once with untorn function/argument pairs; %u actual sleep handoff cases including enqueue after BLOCKED publication; callback requeue preserved; %u baseline critical CAS operations, none in new slot path\n",cases,TOTAL,sleep_cases,previous_attempts);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-deferred-critical-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
