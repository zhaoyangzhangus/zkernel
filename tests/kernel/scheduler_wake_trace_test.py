"""Actual wake tracing: distinguish inbox delay from ready-queue delay."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/sched/core.c').read_text()
begin=source.index('typedef struct sched_wake_trace')
end=source.index('static bool scheduler_command_drain_local', begin)
program='#include "'+str(root/'kernel/sched/internal.h')+'"\n'
program+=r'''
#include <arch/x86_64/syscall_internal.h>
#include <assert.h>
#include <kernel/process.h>
static uint64_t now;
scheduler_cpu_t g_cpus[MAX_CPUS];
uint32_t g_cpu_count=4;
static bool preempt_disabled;
static bool tlb_active;
static uint32_t current_cpu=3;
bool x86_tlb_shootdown_active(void) { return tlb_active; }
bool sched_preempt_disabled(void) { return preempt_disabled; }
uint64_t x86_read_tsc(void) { return now; }
uint64_t x86_tsc_to_ns(uint64_t value) { return value; }
uint32_t x86_current_cpu_index(void) { return current_cpu; }
static bool host_syscall_active(void) { return true; }
#define x86_syscall_active_fast host_syscall_active
''' + source[begin:end] + r'''
int main(void) {
    assert(scheduler_trace_threshold_ns()==10000000ULL);
    g_sched_wake_trace_threshold_ns=0;
    assert(scheduler_trace_threshold_ns()==10000000ULL);
    g_sched_wake_trace_threshold_ns=1;
    assert(scheduler_trace_threshold_ns()==100000ULL);
    g_sched_wake_trace_threshold_ns=UINT64_MAX;
    assert(scheduler_trace_threshold_ns()==1000000000ULL);
    g_sched_wake_trace_threshold_ns=10000000ULL;
    thread_t thread={0}, other={0}; scheduler_cpu_t cpu={0};
    arch_trap_frame_t frame={0};
    process_t process={0}; process.pid=91;
    thread.process=&process;
    /* Boot self-tests may use tid zero without any real saved user frame. */
    atomic_init(&thread.state,THREAD_BLOCKED);
    scheduler_trace_notify(&thread);
    scheduler_trace_ready(&cpu,&thread);
    scheduler_trace_run(&thread);
    assert(!g_sched_wake_trace.tid && !g_sched_wake_trace.notified);
    thread.tid=126; thread.kernel_stack_top=(uintptr_t)&frame+sizeof(frame);
    atomic_init(&thread.state,THREAD_BLOCKED); atomic_init(&thread.block_epoch,1);
    thread.sched.vruntime=300000000;
    frame.rax=202; frame.rdi=0x537ecbc0;
    other.tid=531; other.sched.vruntime=200000000;
    cpu.queue.current=&other; cpu.queue.min_vruntime=200000000;
    cpu.queue.nr_running=2;
    now=100; scheduler_trace_notify(&thread); assert(!g_sched_wake_trace.tid);
    g_sched_wake_trace_tid=126;
    scheduler_trace_notify(&thread); assert(g_sched_wake_trace.notified==100);
    now=200; scheduler_trace_notify(&thread);
    assert(g_sched_wake_trace.notified==100); /* Duplicate does not reset origin. */
    now=500; scheduler_trace_publish(&thread);
    assert(g_sched_wake_trace.published==500);
    scheduler_trace_reschedule_request(2, true);
    assert(!g_sched_wake_trace.requested);
    now=550; scheduler_trace_reschedule_request(0, false);
    assert(g_sched_wake_trace.requested==550 && g_sched_wake_trace.request_send==1);
    scheduler_trace_reschedule_request(0, true);
    assert(g_sched_wake_trace.request_send==1);
    now=600; scheduler_trace_publish(&thread);
    assert(g_sched_wake_trace.published==500);
    g_cpus[0].queue.current=&other;
    scheduler_trace_reschedule_interrupt(2); /* Wrong target, not this wake. */
    assert(!g_sched_wake_trace.interrupt);
    preempt_disabled=true;
    g_sched_preempt_callers[0]=0xffffffff80012345ULL;
    now=700;scheduler_trace_reschedule_interrupt(0);
    assert(g_sched_wake_trace.interrupt==700 && g_sched_wake_trace.interrupt_tid==531);
    now=800;scheduler_trace_reschedule_interrupt(0);
    assert(g_sched_wake_trace.interrupt==700); /* Preserve the first deferral. */
    now=1000100; scheduler_trace_ready(&cpu,&thread);
    other.process=&process; other.sched_class=SCHED_CLASS_FAIR;
    other.kernel_stack_top=(uintptr_t)&frame+sizeof(frame);
    current_cpu=2; sched_trace_wake_stall(20000100);
    assert(!g_sched_wake_stall[0]); /* Wrong target CPU. */
    current_cpu=0; tlb_active=true; sched_trace_wake_stall(20000100);
    assert(g_sched_wake_stall[0]==19000000);
    assert(g_sched_wake_stall[1]==531 && g_sched_wake_stall[2]==91);
    assert(g_sched_wake_stall[3]==1 && g_sched_wake_stall[4]==1);
    assert(g_sched_wake_stall[5]==0xffffffff80012345ULL);
    assert(g_sched_wake_stall[6]==1 && g_sched_wake_stall[7]==SCHED_CLASS_FAIR);
    assert(g_sched_wake_stall[8]==1 && g_sched_wake_stall[9]==0 && g_sched_wake_stall[10]==126);
    assert(g_sched_wake_stall[11]==202 && g_sched_wake_stall[12]==0x537ecbc0);
    sched_trace_wake_stall(100); /* Timestamp inversion cannot underflow. */
    assert(g_sched_wake_stall[0]==19000000);
    assert(!atomic_flag_test_and_set(&g_sched_wake_trace_guard));
    sched_trace_wake_stall(21000100); /* Never wait for a publisher. */
    assert(g_sched_wake_stall[0]==19000000);
    atomic_flag_clear(&g_sched_wake_trace_guard);
    g_sched_wake_trace_tid=0; sched_trace_wake_stall(21000100);
    assert(g_sched_wake_stall[0]==19000000);
    g_sched_wake_trace_tid=126;
    g_cpus[0].queue.current=&thread; sched_trace_wake_stall(21000100);
    assert(g_sched_wake_stall[0]==19000000); /* Selected thread already running. */
    g_cpus[0].queue.current=&other;
    current_cpu=3; tlb_active=false;
    now=21000100; scheduler_trace_run(&thread);
    assert(g_sched_wake_delays[0].total_ns==21000000);
    assert(g_sched_wake_delays[0].notify_to_ready_ns==1000000);
    assert(g_sched_wake_delays[0].notify_to_publish_ns==400);
    assert(g_sched_wake_delays[0].publish_to_ready_ns==999600);
    assert(g_sched_wake_delays[0].source_cpu==3);
    assert(g_sched_wake_delays[0].target_cpu==0);
    assert(g_sched_wake_delays[0].publish_to_interrupt_ns==200);
    assert(g_sched_wake_delays[0].interrupt_to_ready_ns==999400);
    assert(g_sched_wake_delays[0].interrupt_tid==531);
    assert(g_sched_wake_delays[0].interrupt_active==1);
    assert(g_sched_wake_delays[0].interrupt_preempt==1);
    assert(g_sched_wake_delays[0].interrupt_preempt_caller==0xffffffff80012345ULL);
    assert(g_sched_wake_delays[0].publish_to_request_ns==50);
    assert(g_sched_wake_delays[0].request_to_interrupt_ns==150);
    assert(g_sched_wake_delays[0].request_send==1);
    assert(g_sched_wake_delays[0].ready_to_run_ns==20000000);
    assert(g_sched_wake_delays[0].current_tid==531);
    assert(g_sched_wake_delays[0].queued==2 && g_sched_wake_delays[0].active==1);
    assert(!g_sched_wake_trace.tid);
    atomic_store(&thread.block_epoch,2);
    now=30000000; scheduler_trace_notify(&thread);
    scheduler_trace_reschedule_request(0, true); /* No publication yet. */
    assert(!g_sched_wake_trace.requested);
    now+=50;scheduler_trace_reschedule_interrupt(0); /* IPI can beat publication. */
    now+=100; scheduler_trace_ready(&cpu,&thread);
    now+=100; scheduler_trace_publish(&thread); /* Consumer can beat producer. */
    now+=100; scheduler_trace_run(&thread);
    assert(g_sched_wake_delays[0].total_ns==21000000);
    atomic_store(&thread.block_epoch,5);
    frame.rdi=9; now=35000000; scheduler_trace_notify(&thread);
    now+=10; scheduler_trace_publish(&thread);
    now+=20; scheduler_trace_reschedule_request(0,true);
    assert(g_sched_wake_trace.request_send==2);
    now+=30; scheduler_trace_reschedule_interrupt(0);
    now+=40; scheduler_trace_ready(&cpu,&thread);
    now+=21000000; scheduler_trace_run(&thread);
    assert(g_sched_wake_delays[1].request_send==2);
    assert(g_sched_wake_delays[1].publish_to_request_ns==20);
    assert(g_sched_wake_delays[1].request_to_interrupt_ns==30);
    atomic_store(&thread.block_epoch,3);
    now=40000000; scheduler_trace_notify(&thread);
    atomic_store(&thread.block_epoch,4);
    scheduler_trace_ready(&cpu,&thread); assert(!g_sched_wake_trace.ready);
    assert(!atomic_flag_test_and_set(&g_sched_wake_trace_guard));
    scheduler_trace_notify(&thread); assert(g_sched_wake_trace.epoch==3);
    assert(atomic_flag_test_and_set(&g_sched_wake_trace_guard));
    atomic_flag_clear(&g_sched_wake_trace_guard);
    /* Never read a return frame from a nonblocked thread. */
    atomic_store(&thread.state,THREAD_RUNNING);thread.kernel_stack_top=0;
    scheduler_trace_notify(&thread);assert(g_sched_wake_trace.epoch==3);
    // Disabled, wrong CPU and busy observer must never disturb a live trace.
    g_sched_wake_trace_tid=0;uint64_t observed=g_sched_wake_trace.interrupt;
    scheduler_trace_reschedule_interrupt(0);assert(g_sched_wake_trace.interrupt==observed);
    g_sched_wake_trace_tid=126;
    scheduler_trace_reschedule_interrupt(MAX_CPUS);assert(g_sched_wake_trace.interrupt==observed);
    assert(!atomic_flag_test_and_set(&g_sched_wake_trace_guard));
    scheduler_trace_reschedule_interrupt(0);assert(g_sched_wake_trace.interrupt==observed);
    assert(atomic_flag_test_and_set(&g_sched_wake_trace_guard));
    atomic_flag_clear(&g_sched_wake_trace_guard);
    /* Inbox sampling observes the pre-READY phase, not ordinary FAIR queue
       waiting, and preserves the interrupted hardware frame for symbolizing. */
    g_sched_wake_trace=(sched_wake_trace_t){.tid=126,.epoch=9,.published=1000,
                                          .source_cpu=3,.target_cpu=0};
    current_cpu=1; sched_trace_inbox_stall(20001000,0xabcdef,0x202);
    assert(!g_sched_inbox_stall[0]);
    current_cpu=0;preempt_disabled=true;tlb_active=false;
    sched_trace_inbox_stall(20001000,0xabcdef,0x202);
    assert(g_sched_inbox_stall[0]==20000000 && g_sched_inbox_stall[1]==531);
    assert(g_sched_inbox_stall[2]==91 && g_sched_inbox_stall[3]==1);
    assert(g_sched_inbox_stall[8]==9 && g_sched_inbox_stall[10]==126);
    assert(g_sched_inbox_stall[14]==0xabcdef && g_sched_inbox_stall[15]==0x202);
    assert(g_sched_inbox_stall[16]==1000);
    sched_trace_inbox_stall(999,0,0);assert(g_sched_inbox_stall[0]==20000000);
    g_sched_wake_trace.ready=2000;
    sched_trace_inbox_stall(30001000,0,0);assert(g_sched_inbox_stall[0]==20000000);
    g_sched_wake_trace.ready=0;
    assert(!atomic_flag_test_and_set(&g_sched_wake_trace_guard));
    sched_trace_inbox_stall(40001000,0,0);
    assert(g_sched_inbox_stall[0]==20000000);
    atomic_flag_clear(&g_sched_wake_trace_guard);
    g_sched_wake_trace_tid=0;
    sched_trace_inbox_stall(40001000,0,0);assert(g_sched_inbox_stall[0]==20000000);
    now=4000;scheduler_trace_reschedule_send(0,2000,true,1);
    assert(!g_sched_ipi_send[0]);
    g_sched_wake_trace_tid=126;current_cpu=3;
    scheduler_trace_reschedule_send(0,2000,true,1);
    assert(g_sched_ipi_send[0]==2000 && g_sched_ipi_send[1]==3);
    assert(g_sched_ipi_send[2]==0 && g_sched_ipi_send[5]==1 && g_sched_ipi_send[6]==1);
    scheduler_trace_reschedule_send(0,3000,false,4);
    assert(g_sched_ipi_send[0]==2000); /* Preserve worst, not latest. */
    scheduler_trace_reschedule_send(0,5000,false,4); /* Reversed clock. */
    scheduler_trace_reschedule_send(MAX_CPUS,0,false,4);
    assert(g_sched_ipi_send[0]==2000);
    assert(!atomic_flag_test_and_set(&g_sched_wake_trace_guard));
    scheduler_trace_reschedule_send(0,0,false,4);assert(g_sched_ipi_send[0]==2000);
    atomic_flag_clear(&g_sched_wake_trace_guard);
    scheduler_trace_reschedule_send(0,0,false,4);
    assert(g_sched_ipi_send[0]==2000); /* Probe enabled after this send began. */
    now=4001;scheduler_trace_reschedule_send(0,1,false,4);
    assert(g_sched_ipi_send[0]==4000 && g_sched_ipi_send[5]==0 && g_sched_ipi_send[6]==4);
    /* Finer opt-in sampling captures short waits without changing ownership,
       epoch handling, or the historical default threshold. */
    g_sched_wake_trace_threshold_ns=1000000;
    g_sched_wake_trace=(sched_wake_trace_t){0};
    thread.kernel_stack_top=(uintptr_t)&frame+sizeof(frame);
    atomic_store(&thread.state,THREAD_BLOCKED);
    atomic_store(&thread.block_epoch,12);
    frame.rdi=0x123;now=100000000;scheduler_trace_notify(&thread);
    now+=100;scheduler_trace_publish(&thread);
    now=101000000;scheduler_trace_ready(&cpu,&thread);
    g_sched_wake_stall[0]=0;current_cpu=0;
    sched_trace_wake_stall(102000000);
    assert(g_sched_wake_stall[0]==1000000);
    now=103000000;scheduler_trace_run(&thread);
    assert(g_sched_wake_delays[2].total_ns==3000000);
    assert(g_sched_wake_delays[2].notify_to_ready_ns==1000000);
    assert(g_sched_wake_delays[2].ready_to_run_ns==2000000);
    g_sched_wake_trace=(sched_wake_trace_t){.tid=126,.epoch=13,.published=1000,
                                          .source_cpu=3,.target_cpu=0};
    g_sched_inbox_stall[0]=0;
    sched_trace_inbox_stall(1001000,0xabcdef,0x202);
    assert(g_sched_inbox_stall[0]==1000000);
    /* 内核线程即使保留了非零栈地址，也不能读取用户系统调用参数。
       地址 1 在宿主测试中不可访问，能直接检出原来的无条件解引用。 */
    thread.process=0;
    g_sched_wake_delays[0]=(sched_wake_delay_t){0};
    for (unsigned invalid=0;invalid<3;++invalid) {
        thread.kernel_stack_top=invalid==0 ? 0 : invalid==1 ? 1 :
            (uintptr_t)&frame+sizeof(frame);
        atomic_store(&thread.block_epoch,20+invalid);
        now=200000000+invalid*30000000;
        scheduler_trace_notify(&thread);
        assert(g_sched_wake_trace.tid==126);
        assert(!g_sched_wake_trace.number && !g_sched_wake_trace.argument0);
        now+=100;scheduler_trace_publish(&thread);
        now+=100;scheduler_trace_ready(&cpu,&thread);
        now+=20000000;scheduler_trace_run(&thread);
        assert(g_sched_wake_delays[0].tid==126);
        assert(!g_sched_wake_delays[0].number && !g_sched_wake_delays[0].argument0);
    }
    /* 用户线程的不足长栈同样不能解引用，正常帧仍保留准确参数。 */
    thread.process=&process;thread.kernel_stack_top=1;
    atomic_store(&thread.block_epoch,30);now+=1;
    scheduler_trace_notify(&thread);
    assert(!g_sched_wake_trace.number && !g_sched_wake_trace.argument0);
    thread.kernel_stack_top=(uintptr_t)&frame+sizeof(frame);
    atomic_store(&thread.block_epoch,31);now+=1;
    scheduler_trace_notify(&thread);
    assert(g_sched_wake_trace.number==frame.rax &&
           g_sched_wake_trace.argument0==frame.rdi);
    assert(!scheduler_trace_user_frame(0));
    /* READY 和邮箱停滞采样复用同一校验，不能在当前线程是内核工作线程时
       读取伪造的用户帧；延迟和线程身份仍须保留。 */
    current_cpu=0;other.process=0;
    for (unsigned invalid=0;invalid<3;++invalid) {
        other.kernel_stack_top=invalid==0 ? 0 : invalid==1 ? 1 :
            (uintptr_t)&frame+sizeof(frame);
        g_sched_wake_trace=(sched_wake_trace_t){.tid=126,.epoch=32+invalid,
            .published=1000,.ready=2000,.source_cpu=3,.target_cpu=0};
        g_sched_wake_stall[0]=0;
        sched_trace_wake_stall(20002000);
        assert(g_sched_wake_stall[0]==20000000 && g_sched_wake_stall[1]==531);
        assert(!g_sched_wake_stall[11] && !g_sched_wake_stall[12] &&
               !g_sched_wake_stall[13]);
        g_sched_wake_trace.ready=0;g_sched_inbox_stall[0]=0;
        sched_trace_inbox_stall(20001000,0xabcdef,0x202);
        assert(g_sched_inbox_stall[0]==20000000 && g_sched_inbox_stall[1]==531);
        assert(!g_sched_inbox_stall[11] && !g_sched_inbox_stall[12] &&
               !g_sched_inbox_stall[13]);
        assert(g_sched_inbox_stall[14]==0xabcdef);
    }
    g_sched_wake_trace_threshold_ns=10000000;
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-wake-trace-') as directory:
    path=Path(directory);(path/'test.c').write_text(program)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('scheduler wake trace: selector, phases, duplicate wake, epoch race, busy guard, kernel-worker frame safety and nonblocked safety passed')
