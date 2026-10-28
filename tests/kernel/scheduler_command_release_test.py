"""Production command-reference FIFO: destruction only at safe local boundaries."""
from pathlib import Path
import subprocess
import tempfile
import re

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sched/core.c').read_text()
start = source.index('static inline void scheduler_thread_mailbox_lock(')
end = source.index('static bool scheduler_command_ring_try_pop(', start)
production = source[start:end]
publication = 'atomic_store_explicit(&cpu->command_release_pending, true,'
assert production.count(publication) == 1
production = production.replace(publication, '++pending_publications; ' + publication)
finish_start = source.index('void sched_finish_switch(void) {')
finish_end = source.index('void sched_block_current(void)', finish_start)
finish = re.sub(r'__asm__ volatile \(\s*"pushfq; popq %0"\s*'
                r': "=r"\(flags\)\s*:\s*: "memory"\);',
                'flags=irq ? 1ULL<<9 : 0;', source[finish_start:finish_end])
assert '__asm__' not in finish
# All consumers must have an interruptible retry, including BSP logical idle.
runtime = (root / 'kernel/init/runtime.c').read_text()
idle_loop = runtime[runtime.index('    for (;;) {\n        net_manager_status_t net_status;'):]
assert idle_loop.index('sched_finish_switch();') < idle_loop.index('"cli"')
assert source.count('scheduler_command_release_drain(cpu_id);') == 2
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#define SCHED_UNLIKELY(x) (x)
typedef struct thread {
    struct thread *command_release_next;
    uint32_t command_release_count;
    atomic_uint remote_wake_lock;
    unsigned refs, puts;
} thread_t;
typedef struct {
    thread_t *command_release_head, *command_release_tail;
    atomic_bool command_release_pending;
} scheduler_cpu_t;
static scheduler_cpu_t g_cpus[2];
static uint32_t g_cpu_count=2, current_cpu;
static bool irq=true, locked, tlb, migrate_on_put, enqueue_on_put;
static thread_t extra;
static unsigned releases;
static unsigned pending_publications;
static bool requeue_self_on_put;
static thread_t *fifo_base;
static unsigned fifo_index;
static unsigned deferred, reaped;
static bool background_ready;
static bool deferred_background_worker_started(void) {return background_ready;}
static bool background_full;
static void (*background_work)(void *);
static void *background_argument;
static bool deferred_background_schedule(void (*work)(void *),void *arg) {
    if(background_full)return false;
    assert(!background_work);background_work=work;background_argument=arg;return true;
}
static uint32_t x86_current_cpu_index(void){return current_cpu;}
static void scheduler_reap_schedule_deferred(uint32_t cpu) { assert(cpu==current_cpu); ++deferred; }
static void scheduler_reap_drain_cpu(uint32_t cpu) {
    (void)cpu; assert(irq && !locked && !tlb); ++reaped;
}
static uint64_t scheduler_irq_save(void) {
    uint64_t flags=irq ? 1ULL<<9 : 0; irq=false; return flags;
}
static void scheduler_irq_restore(uint64_t flags) { irq=(flags & (1ULL<<9))!=0; }
static bool scheduler_current_cpu(uint32_t *cpu) { *cpu=current_cpu; return current_cpu<2; }
static bool sched_preempt_disabled(void) { return locked; }
static bool x86_tlb_shootdown_active(void) { return tlb; }
static void scheduler_command_release_enqueue_local(scheduler_cpu_t *,thread_t *);
static void object_put(void *object) {
    thread_t *thread=object;
    assert(irq && !locked && !tlb && thread->refs);
    --thread->refs; ++thread->puts; ++releases;
    if (fifo_base) {
        assert(thread==&fifo_base[fifo_index]);
        if(!thread->refs)++fifo_index;
    }
    if (requeue_self_on_put) {
        requeue_self_on_put=false;
        ++thread->refs;
        uint64_t flags=scheduler_irq_save();
        scheduler_command_release_enqueue_local(&g_cpus[current_cpu],thread);
        scheduler_irq_restore(flags);
    }
    if (enqueue_on_put) {
        enqueue_on_put=false;
        uint64_t flags=scheduler_irq_save();
        scheduler_command_release_enqueue_local(&g_cpus[current_cpu], &extra);
        scheduler_irq_restore(flags);
    }
    if (migrate_on_put) { migrate_on_put=false; current_cpu=1; }
}
''' + production + finish + r'''
static void queue(unsigned cpu, thread_t *t, unsigned count) {
    uint64_t flags=scheduler_irq_save();
    while(count--) scheduler_command_release_enqueue_local(&g_cpus[cpu],t);
    scheduler_irq_restore(flags);
}
int main(void) {
    thread_t a={.refs=3}, b={.refs=1}, c={.refs=2}; extra.refs=1;
    queue(0,&a,3);queue(0,&b,1);
    assert(pending_publications==1);
    irq=false; sched_finish_switch();
    assert(!irq && !releases && a.command_release_count==3);
    assert(g_cpus[0].command_release_pending && g_cpus[0].command_release_head==&a);
    irq=true;locked=true;sched_finish_switch();
    assert(!releases && g_cpus[0].command_release_pending);
    locked=false;tlb=true;sched_finish_switch();
    assert(!releases && g_cpus[0].command_release_pending);
    assert(deferred==3 && reaped==0);
    tlb=false;current_cpu=1;scheduler_command_release_drain(0);
    assert(!releases && g_cpus[0].command_release_head==&a);
    current_cpu=0;migrate_on_put=true;scheduler_command_release_drain(0);
    /* Detached counts stay owned by this continuation across migration;
       another CPU must not pop the remaining source CPU FIFO. */
    assert(a.puts==3 && !a.refs && b.puts==0 && current_cpu==1);
    assert(g_cpus[0].command_release_pending && g_cpus[0].command_release_head==&b);
    current_cpu=0;enqueue_on_put=true;scheduler_command_release_drain(0);
    assert(b.puts==1 && extra.puts==1 && releases==5);
    assert(!g_cpus[0].command_release_pending && !g_cpus[0].command_release_head);
    assert(!g_cpus[0].command_release_tail);
    scheduler_command_release_drain(0);scheduler_command_release_drain(99);
    assert(releases==5 && irq);
    queue(0,&c,2);sched_finish_switch();
    assert(c.puts==2 && !c.refs && releases==7 && irq);
    assert(reaped==1);
    assert(!g_cpus[0].command_release_pending && !g_cpus[0].command_release_head);
    thread_t again={.refs=2};
    unsigned publications_before=pending_publications;
    queue(0,&again,2);requeue_self_on_put=true;
    scheduler_command_release_drain(0);
    assert(!again.refs&&again.puts==3&&again.command_release_count==0);
    assert(pending_publications==publications_before+2);
    assert(!g_cpus[0].command_release_pending&&!g_cpus[0].command_release_tail);
    thread_t batch[64]={0};fifo_base=batch;fifo_index=0;
    publications_before=pending_publications;
    for(unsigned round=0;round<256;++round)
    for(unsigned n=0;n<64;++n) {
        ++batch[n].refs;queue(0,&batch[n],1);
    }
    assert(pending_publications==publications_before+1);
    scheduler_command_release_drain(0);
    assert(fifo_index==64&&!g_cpus[0].command_release_pending);
    for(unsigned n=0;n<64;++n)
        assert(!batch[n].refs&&batch[n].puts==256&&!batch[n].command_release_count);
    fifo_base=0;
    /* Once background cleanup is available, the last owned reference must
     * leave the switching thread without running its destructor there. */
    background_ready=true;
    thread_t background_case={.refs=1};queue(0,&background_case,1);
    unsigned before_deferred=deferred,before_reaped=reaped;
    sched_finish_switch();
    assert(!background_case.puts&&background_case.refs==1);
    assert(deferred==before_deferred+1&&reaped==before_reaped);
    assert(background_argument==&background_case&&background_work);
    background_work(background_argument);background_work=0;
    assert(background_case.puts==1&&!background_case.refs);
    // A saturated cleanup queue must retain the last reference for retry.
    thread_t saturated={.refs=256};queue(0,&saturated,256);
    background_full=true;sched_finish_switch();
    assert(saturated.refs==1&&saturated.puts==255&&!background_work);
    assert(g_cpus[0].command_release_pending&&saturated.command_release_count==1);
    background_full=false;sched_finish_switch();
    assert(saturated.refs==1&&!g_cpus[0].command_release_pending);
    background_work(background_argument);background_work=0;
    assert(!saturated.refs&&saturated.puts==256);
    /* An interruptible release can migrate after a non-final put. Restore
     * the retained reference on the new CPU, not the stale source FIFO. */
    thread_t migrated={.refs=3};queue(0,&migrated,3);
    background_full=true;migrate_on_put=true;
    scheduler_command_release_drain(0);
    assert(current_cpu==1&&migrated.refs==1&&migrated.puts==2);
    assert(!g_cpus[0].command_release_pending);
    assert(g_cpus[1].command_release_head==&migrated);
    assert(g_cpus[1].command_release_pending&&migrated.command_release_count==1);
    background_full=false;scheduler_command_release_drain(0);
    assert(!background_work&&migrated.refs==1);
    scheduler_command_release_drain(1);
    assert(background_argument==&migrated&&background_work);
    assert(!g_cpus[1].command_release_pending);
    background_work(background_argument);background_work=0;
    assert(!migrated.refs&&migrated.puts==3);
    puts("Coalesced release publications: 16384 references/64 FIFO nodes -> 1 pending publication; FIFO and same-node reentry passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-command-release-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-Wno-unused-function','-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=program,text=True,check=True)
    subprocess.run([binary],check=True)
print('Command release: IRQ/lock/TLB guards, local ownership, migration, coalesced refs and enqueue race passed')
