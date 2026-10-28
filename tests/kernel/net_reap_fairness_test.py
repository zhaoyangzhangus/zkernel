"""Production reaper routing, bounded cleanup, bootstrap and full-queue retry."""
from pathlib import Path
import resource
import subprocess
import tempfile

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sched/core.c').read_text()
start = source.index('static thread_t *scheduler_reap_pop_cpu(')
production = source[start:source.index('\nvoid sched_finish_switch(', start)]
finish = source[source.index('void sched_finish_switch('):source.index('\nvoid sched_block_current(')]
asm_start = finish.index('    __asm__ volatile (')
asm_end = finish.index('\n\n    uint32_t cpu_id;', asm_start)
finish = finish[:asm_start] + '    flags = test_irq_flags;' + finish[asm_end:]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <string.h>
#define SCHED_ENTITY_REAP_QUEUED 1U
typedef struct thread {
    struct thread *reap_next;
    struct {uint16_t flags;} sched;
    unsigned released;
} thread_t;
typedef struct {
    pthread_mutex_t reap_lock;
    thread_t *reap_head,*reap_tail;
    atomic_bool reap_deferred_queued;
} scheduler_cpu_t;
static scheduler_cpu_t g_cpus[2];
static uint32_t g_cpu_count=2;
static _Thread_local unsigned depth;
typedef void (*work_fn)(void *);
typedef struct {work_fn fn;void *arg;} item_t;
static item_t work[128];
static unsigned head,tail,count,releases,network_calls;
static bool full,enqueue_on_release;
static bool background_ready=true;
static unsigned irq_posts;
static uint64_t test_irq_flags=512;
static bool test_preempt,test_tlb,test_cpu_valid=true;
static uint32_t test_cpu;
static unsigned command_drains;
static bool scheduler_current_cpu(uint32_t *cpu){*cpu=test_cpu;return test_cpu_valid;}
static bool sched_preempt_disabled(void){return test_preempt;}
static bool x86_tlb_shootdown_active(void){return test_tlb;}
static void scheduler_command_release_drain(uint32_t cpu){assert(cpu<2);++command_drains;}
static thread_t extra;
static uint64_t scheduler_lock(pthread_mutex_t *lock) {
    assert(!depth);assert(!pthread_mutex_lock(lock));++depth;return 9;
}
static void scheduler_unlock(pthread_mutex_t *lock,uint64_t flags) {
    assert(flags==9&&depth==1);--depth;assert(!pthread_mutex_unlock(lock));
}
static bool queue_work(work_fn fn,void *arg) {
    assert(!depth);if(full||count==128)return false;
    work[tail]=(item_t){fn,arg};tail=(tail+1)%128;++count;return true;
}
static bool deferred_background_worker_started(void) {return background_ready;}
static bool deferred_background_schedule(work_fn fn,void *arg) {
 return background_ready&&queue_work(fn,arg);
}
static bool deferred_try_schedule(work_fn fn,void *arg) {
 ++irq_posts;return queue_work(fn,arg);
}
static void enqueue(unsigned cpu,thread_t *thread) {
    scheduler_cpu_t *queue=&g_cpus[cpu];
    uint64_t flags=scheduler_lock(&queue->reap_lock);
    thread->reap_next=0;thread->sched.flags|=SCHED_ENTITY_REAP_QUEUED;
    if(queue->reap_tail)queue->reap_tail->reap_next=thread;
    else queue->reap_head=thread;
    queue->reap_tail=thread;scheduler_unlock(&queue->reap_lock,flags);
}
static void thread_release_execution_ref(thread_t *thread) {
    assert(!depth&&!(thread->sched.flags&SCHED_ENTITY_REAP_QUEUED));
    assert(!thread->reap_next&&!thread->released);++thread->released;++releases;
    if(enqueue_on_release){enqueue_on_release=false;enqueue(0,&extra);}
}
''' + production + finish + r'''
static void network(void *arg) {(void)arg;assert(releases==1);++network_calls;}
static void run_one(void) {
    assert(count);item_t item=work[head];head=(head+1)%128;--count;item.fn(item.arg);
}
int main(void) {
    for(unsigned i=0;i<2;++i)assert(!pthread_mutex_init(&g_cpus[i].reap_lock,0));
    thread_t threads[64]={0};
    for(unsigned i=0;i<64;++i)enqueue(0,&threads[i]);
    scheduler_reap_schedule_deferred(0);
    for(unsigned i=0;i<8;++i)scheduler_reap_schedule_deferred(0);
    assert(count==1&&g_cpus[0].reap_deferred_queued);
    assert(deferred_try_schedule(network,0));
    run_one();assert(releases==1&&count==2&&threads[0].released);
    run_one();assert(network_calls==1&&releases==1);
    for(unsigned i=1;i<64;++i){run_one();assert(releases==i+1&&threads[i].released);}
    assert(!count&&!g_cpus[0].reap_head&&!g_cpus[0].reap_tail);
    assert(!g_cpus[0].reap_deferred_queued);
    scheduler_reap_deferred_work(0);assert(releases==64&&!count);
    scheduler_reap_deferred_work((void *)99);scheduler_reap_schedule_deferred(99);
    assert(!count&&releases==64);
    thread_t a={0},b={0};enqueue(0,&a);enqueue_on_release=true;
    scheduler_reap_schedule_deferred(0);run_one();
    assert(releases==65&&count==1&&!extra.released);
    run_one();assert(extra.released&&releases==66&&!count);
    enqueue(1,&b);full=true;scheduler_reap_schedule_deferred(1);
    assert(!count&&!g_cpus[1].reap_deferred_queued&&g_cpus[1].reap_head==&b);
    assert(irq_posts==1); /* A full background queue never spills into input. */
    full=false;scheduler_reap_schedule_deferred(1);run_one();
    assert(b.released&&releases==67&&!count);
    thread_t sync[16]={0};
    for(unsigned i=0;i<16;++i)enqueue(1,&sync[i]);
    scheduler_reap_drain_cpu_budget(1,0);assert(releases==67);
    scheduler_reap_drain_cpu(1);assert(releases==83&&!g_cpus[1].reap_head);
    for(unsigned i=0;i<16;++i)assert(sync[i].released==1);
    scheduler_reap_drain_cpu(99);assert(releases==83);
    background_ready=false;
    thread_t early={0};enqueue(1,&early);scheduler_reap_schedule_deferred(1);
    assert(count==1&&irq_posts==2);run_one();assert(early.released&&releases==84);
    for(unsigned ready=0;ready<2;++ready)for(unsigned mode=0;mode<4;++mode){
        background_ready=ready;test_irq_flags=mode==1?0:512;
        test_preempt=mode==2;test_tlb=mode==3;test_cpu=1;
        thread_t finished={0};enqueue(1,&finished);unsigned before=releases;
        sched_finish_switch();
        if(ready||mode!=0){assert(!finished.released&&count==1);run_one();}
        assert(finished.released&&releases==before+1&&!count);
    }
    assert(command_drains==2);
    test_cpu_valid=false;sched_finish_switch();assert(command_drains==2&&!count);
}
'''
with tempfile.TemporaryDirectory(prefix='network-reap-fairness-') as directory:
    path = Path(directory)
    for name, unit, fails in [('current', program, False), ('unbounded', program.replace(
            'scheduler_reap_drain_cpu_budget(cpu_id, 1U);',
            'scheduler_reap_drain_cpu(cpu_id);'), True),
            ('inline-switch-reap', program.replace(
                'if (deferred_background_worker_started())\n        scheduler_reap_schedule_deferred(cpu_id);',
                'if (deferred_background_worker_started())\n        scheduler_reap_drain_cpu(cpu_id);'), True)]:
        if fails: assert unit != program
        file = path / (name + '.c')
        file.write_text(unit)
        binary = path / name
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(file), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True)
        assert (result.returncode != 0) == fails, result.stderr.decode()
        if fails:
            print('Negative control rejected:', name)
print('Reaper: background routing, no input spill on full queue, eight switch-context cases, bootstrap fallback, one teardown per item and ownership/requeue passed')
