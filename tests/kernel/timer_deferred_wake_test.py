"""Exercise the actual timer boundary after a syscall-deferred wake IPI."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/arch/x86_64/irq/apic.c').read_text()
start = source.index('UINT64 lapic_timer_interrupt(')
end = source.index('\n}', start) + 2
entry = (root/'kernel/arch/x86_64/time/deadline.S').read_text()
entry = entry[entry.index('arch_lapic_timer_entry:'):entry.index('call lapic_timer_interrupt')]
assert entry.count('pushq %')==15 and 'movq %rsp, %rdi' in entry
program = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <setjmp.h>
#define OS_SIGKILL 9U
#define THREAD_FLAG_TERMINATE_REQUEST 4U
typedef struct {uint64_t signal_pending;uint32_t flags;} thread_t;
static thread_t current;
static jmp_buf killed;
static unsigned exits;
static thread_t *sched_current_thread(void) { return &current; }
static void process_exit_signal(unsigned signal) { assert(signal==9);++exits;longjmp(killed,1); }
static void thread_exit(int64_t status) { assert(status==-9);++exits;longjmp(killed,1); }
typedef uint64_t UINT64;
typedef struct { int unused; } OS_INTERRUPT_CONTEXT;
static uint64_t g_tick_count;
static uint64_t expected_rip, expected_flags;
static bool active, shootdown, disabled, pending, timeslice;
static unsigned ticks, takes, drains, switches, maintenance, eoi;
static bool nvme_schedule_deferred_poll(void) { return true; }
static void lapic_schedule_timer_maintenance(void) { ++maintenance; }
static void lapic_end_of_interrupt(void) { ++eoi; }
static uint64_t x86_read_tsc(void) { return 100; }
static void sched_trace_wake_stall(uint64_t n) { assert(n==100); }
static void sched_trace_inbox_stall(uint64_t n,uint64_t rip,uint64_t flags) {
    assert(n==100 && rip==expected_rip && flags==expected_flags);
}
static uint64_t x86_tsc_to_ns(uint64_t n) { return n; }
static bool realtest_timeout_poll(uint64_t n) { (void)n;return false; }
static void realtest_timeout_reboot(void) { assert(false); }
static bool x86_tlb_shootdown_active(void) { return shootdown; }
static bool x86_syscall_active_fast(void) { return active; }
static bool sched_preempt_disabled(void) { return disabled; }
static void sched_tick(uint64_t n) { assert(n==100);++ticks; }
static bool sched_tick_should_preempt(void) { return timeslice; }
static bool x86_smp_take_reschedule_request(void) {
    ++takes;bool result=pending;pending=false;return result;
}
static void sched_handle_reschedule_request(void) {
    assert(!active && !shootdown && !disabled);++drains;++switches;
}
static void schedule(void) { if (!disabled) ++switches; }
''' + source[start:end] + r'''
int main(void) {
    pending=true;active=true;
    lapic_timer_interrupt(0);
    assert(pending && !takes && !ticks && !switches);
    active=false;shootdown=true;
    lapic_timer_interrupt(0);
    assert(pending && !takes && !ticks && !switches);
    shootdown=false;disabled=true;
    lapic_timer_interrupt(0);
    assert(pending && !takes && ticks==1 && !switches);
    /* 尚无可运行快照：唤醒请求仍在远端收件箱中。 */
    disabled=false;timeslice=false;
    lapic_timer_interrupt(0);
    assert(!pending && takes==1 && drains==1 && switches==1 && ticks==2);
    lapic_timer_interrupt(0);
    assert(takes==2 && drains==1 && switches==1 && ticks==3);
    pending=true;timeslice=true;
    lapic_timer_interrupt(0);
    assert(!pending && drains==2 && switches==2); /* No double scheduling. */
    lapic_timer_interrupt(0);
    assert(drains==2 && switches==3); /* Normal timeslice still works. */
    assert(g_tick_count==7 && maintenance==7 && eoi==7);
    uint64_t context[18]={0};context[15]=expected_rip=0x1234;
    context[17]=expected_flags=0x202;
    lapic_timer_interrupt((OS_INTERRUPT_CONTEXT *)context);
    assert(g_tick_count==8 && maintenance==8 && eoi==8);
    current.signal_pending=256;
    lapic_timer_interrupt((OS_INTERRUPT_CONTEXT *)context);
    assert(!exits && current.signal_pending==256); // never kill while in kernel mode
    context[16]=3;
    disabled=true;
    lapic_timer_interrupt((OS_INTERRUPT_CONTEXT *)context);
    assert(!exits && current.signal_pending==256);
    disabled=false;
    if(!setjmp(killed)) {
        lapic_timer_interrupt((OS_INTERRUPT_CONTEXT *)context);
        assert(false);
    }
    assert(exits==1 && !current.signal_pending && eoi==11);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-timer-wake-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
print('Timer deferred wake: inbox-only wake, syscall/TLB/preemption guards, normal tick and no double schedule passed')
