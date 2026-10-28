"""Actual coalesced release ownership under cross-CPU consumers and reuse."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sched/core.c').read_text()
start = source.index('static inline void scheduler_thread_mailbox_lock(')
end = source.index('static bool scheduler_command_ring_try_pop(', start)
production = source[start:end]
production = production.replace('if (thread->command_release_count != 0U)',
                                'if (snapshot_count(thread) != 0U)')
prefix = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#define SCHED_UNLIKELY(x) (x)
typedef struct thread {
    struct thread *command_release_next;
    uint32_t command_release_count;
    atomic_uint remote_wake_lock,refs,puts;
} thread_t;
typedef struct {
    thread_t *command_release_head,*command_release_tail;
    atomic_bool command_release_pending;
} scheduler_cpu_t;
static scheduler_cpu_t g_cpus[2];
static uint32_t g_cpu_count=2;
static _Thread_local uint32_t current_cpu;
static _Thread_local bool irq=true;
static _Thread_local unsigned held_locks;
static thread_t shared;
static pthread_barrier_t start_barrier,snapshot_barrier;
static pthread_mutex_t negative_gate=PTHREAD_MUTEX_INITIALIZER;
static uint32_t snapshot_count(thread_t *thread) {
#ifdef NEGATIVE_CONTROL
    assert(!pthread_mutex_lock(&negative_gate));
    uint32_t count=thread->command_release_count;
    assert(!pthread_mutex_unlock(&negative_gate));
    pthread_barrier_wait(&snapshot_barrier);
    return count;
#else
    return thread->command_release_count;
#endif
}
static uint64_t scheduler_irq_save(void) { bool before=irq;irq=false;return before?1ULL<<9:0; }
static void scheduler_irq_restore(uint64_t flags) { irq=(flags&(1ULL<<9))!=0; }
static bool scheduler_current_cpu(uint32_t *cpu) { *cpu=current_cpu;return true; }
static bool sched_preempt_disabled(void) { return false; }
static bool x86_tlb_shootdown_active(void) { return false; }
static bool deferred_background_worker_started(void) { return false; }
static bool deferred_background_schedule(void (*work)(void *),void *arg) {
    (void)work;(void)arg;assert(false);return false;
}
static uint32_t x86_current_cpu_index(void) { return current_cpu; }
static void object_put(void *object) {
    thread_t *thread=object;
    assert(irq&&!held_locks);
    assert(atomic_fetch_sub_explicit(&thread->refs,1,memory_order_relaxed)>1);
    atomic_fetch_add_explicit(&thread->puts,1,memory_order_relaxed);
}
'''
# Negative control keeps writes mutex-serialized to avoid test-side C data races,
# but forces both old unsynchronized admission checks to observe count zero.
negative = production.replace('scheduler_thread_mailbox_lock(thread);', '(void)thread;')
negative = negative.replace('scheduler_thread_mailbox_unlock(thread);', '(void)thread;')
negative = negative.replace('bool was_empty =',
                            'assert(!pthread_mutex_lock(&negative_gate)); bool was_empty =')
negative = negative.replace('}\n\nstatic void scheduler_command_release_enqueue_local(',
                            '    assert(!pthread_mutex_unlock(&negative_gate));\n}\n\nstatic void scheduler_command_release_enqueue_local(')
assert 'assert(!pthread_mutex_unlock(&negative_gate));\n}' in negative
production = production.replace('scheduler_thread_mailbox_lock(thread);',
    'scheduler_thread_mailbox_lock(thread); assert(!irq&&!held_locks); ++held_locks;')
production = production.replace('scheduler_thread_mailbox_unlock(thread);',
    'assert(held_locks==1); --held_locks; scheduler_thread_mailbox_unlock(thread);')
body = r'''
static void *first_enqueue(void *argument) {
    current_cpu=(uint32_t)(uintptr_t)argument;
    pthread_barrier_wait(&start_barrier);
    uint64_t flags=scheduler_irq_save();
    scheduler_command_release_enqueue_local(&g_cpus[current_cpu],&shared);
    scheduler_irq_restore(flags);
    return 0;
}
#ifndef NEGATIVE_CONTROL
static atomic_uint finished;
static thread_t batch[64];
static bool wide;
static void *stress(void *argument) {
    current_cpu=(uint32_t)(uintptr_t)argument;
    for(unsigned n=0;n<50000;++n) {
        thread_t *thread=wide?&batch[(n*17U+current_cpu)%64U]:&shared;
        atomic_fetch_add_explicit(&thread->refs,1,memory_order_relaxed);
        uint64_t flags=scheduler_irq_save();
        scheduler_command_release_enqueue_local(&g_cpus[current_cpu],thread);
        scheduler_irq_restore(flags);
        scheduler_command_release_drain(current_cpu);
    }
    atomic_fetch_add(&finished,1);
    do {
        scheduler_command_release_drain(current_cpu);
    } while(atomic_load(&finished)!=2 || atomic_load(&g_cpus[current_cpu].command_release_pending));
    return 0;
}
#endif
int main(void) {
    atomic_init(&shared.refs,3);
    assert(!pthread_barrier_init(&start_barrier,0,2));
    assert(!pthread_barrier_init(&snapshot_barrier,0,2));
    pthread_t workers[2];
    for(unsigned cpu=0;cpu<2;++cpu)
        assert(!pthread_create(&workers[cpu],0,first_enqueue,(void *)(uintptr_t)cpu));
    for(unsigned cpu=0;cpu<2;++cpu)assert(!pthread_join(workers[cpu],0));
    unsigned nodes=(g_cpus[0].command_release_head==&shared)+(g_cpus[1].command_release_head==&shared);
#ifdef NEGATIVE_CONTROL
    assert(nodes==2&&shared.command_release_count==1);
    puts("Negative control: two old admission checks queue one shared node on two CPUs and lose a reference count");
#else
    assert(nodes==1&&shared.command_release_count==2);
    for(unsigned cpu=0;cpu<2;++cpu) { current_cpu=cpu;scheduler_command_release_drain(cpu); }
    assert(atomic_load(&shared.refs)==1&&atomic_load(&shared.puts)==2);
    assert(!shared.command_release_count&&!shared.command_release_next);
    for(unsigned n=0;n<64;++n)atomic_init(&batch[n].refs,1);
    for(unsigned round=0;round<8;++round) {
        wide=(round&1U)!=0;
        atomic_store(&finished,0);
        for(unsigned cpu=0;cpu<2;++cpu)
            assert(!pthread_create(&workers[cpu],0,stress,(void *)(uintptr_t)cpu));
        for(unsigned cpu=0;cpu<2;++cpu)assert(!pthread_join(workers[cpu],0));
        assert(atomic_load(&shared.refs)==1&&!shared.command_release_count);
        assert(!shared.command_release_next&&!atomic_load(&shared.remote_wake_lock));
        for(unsigned n=0;n<64;++n)
            assert(atomic_load(&batch[n].refs)==1&&!batch[n].command_release_count&&
                   !batch[n].command_release_next&&!atomic_load(&batch[n].remote_wake_lock));
        for(unsigned cpu=0;cpu<2;++cpu)
            assert(!g_cpus[cpu].command_release_head&&!g_cpus[cpu].command_release_tail&&
                   !atomic_load(&g_cpus[cpu].command_release_pending));
    }
    unsigned total_puts=atomic_load(&shared.puts);
    for(unsigned n=0;n<64;++n)total_puts+=atomic_load(&batch[n].puts);
    assert(total_puts==800002);
    puts("Cross-CPU release: one-node and 64-node ownership, 800000 concurrent enqueue/pop/reuse operations and exact reference balance passed");
#endif
    assert(!pthread_barrier_destroy(&start_barrier));
    assert(!pthread_barrier_destroy(&snapshot_barrier));
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-release-cross-cpu-') as directory:
    for label, implementation in [('negative', negative), ('production', production)]:
        binary = str(Path(directory) / label)
        flags = ['-DNEGATIVE_CONTROL'] if label == 'negative' else []
        subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-Wno-unused-variable', '-pthread',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie', *flags,
                        '-x', 'c', '-', '-o', binary], input=prefix+implementation+body,
                       text=True, check=True)
        subprocess.run([binary], check=True, timeout=60)
