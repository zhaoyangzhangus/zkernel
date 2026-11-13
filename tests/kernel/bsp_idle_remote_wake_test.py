"""Run the actual BSP post-HLT dispatch against a pending, not-yet-queued wake."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/init/runtime.c').read_text()
halt = source.index('"sti; hlt"', source.index('uint32_t reported_ipv4'))
start = source.index('        if (x86_smp_take_reschedule_request()', halt)
end = source.index('        /* Persist post-desktop', start)
program = r'''
#include <stdbool.h>
#include <assert.h>
static bool pending;
static unsigned runnable, inbox, dispatches, runs;
static bool x86_smp_take_reschedule_request(void) {
    bool value=pending; pending=false; return value;
}
static unsigned sched_runnable_count(void) { return runnable; }
static bool sched_try_run_ready(void) {
    ++dispatches;
    runnable+=inbox; inbox=0;
    if (runnable) { ++runs; --runnable; return true; }
    return false;
}
static void idle_dispatch(void) {
''' + source[start:end] + r'''
}
int main(void) {
    pending=true; inbox=1; runnable=0;
    idle_dispatch();
    assert(dispatches==1 && runs==1 && inbox==0 && !pending);
    idle_dispatch(); assert(dispatches==1);
    runnable=1; idle_dispatch(); assert(dispatches==2 && runs==2);
    pending=true; idle_dispatch(); assert(dispatches==3 && runs==2);
    /* 每次远端唤醒都必须立即分发，不能等待定时器或键盘事件。 */
    for (unsigned i=0;i<1000;++i) {
        pending=true; inbox=1; idle_dispatch();
        assert(!pending && !inbox && !runnable);
    }
    assert(runs==1002 && dispatches==1003);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-bsp-wake-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('BSP idle: empty rq with remote wake, local ready, empty request, quiet idle and 1000 wake cycles passed')

# 同时测试实际的休眠边界，而不只是 HLT 之后的分发。设备抢占保护区延迟 IPI 时会保留一个电平请求；
# 其他生产者可与该请求合并，无需再发送第二个中断。
start = source.index('        __asm__ volatile ("sti; hlt"', source.index('uint32_t reported_ipv4'))
end = source.index('        /* Persist post-desktop', start)
body = source[start:end]
# 生产修复完成后，将睡眠前检查纳入测试。
boundary = source.rfind('        __asm__ volatile ("cli"', source.index('uint32_t reported_ipv4'), start)
if boundary >= 0:
    body = source[boundary:end]
body = body.replace('__asm__ volatile ("cli" : : : "memory")', 'disable_irq()')
body = body.replace('__asm__ volatile ("sti" : : : "memory")', 'enable_irq()')
body = body.replace('__asm__ volatile ("sti; hlt" : : : "memory")', 'sleep_irq()')
program = r'''
#include <assert.h>
#include <stdbool.h>
static bool pending, irq_enabled=true, inject_after_check, hardware_ipi;
static unsigned runnable, inbox, runs, sleeps;
static void disable_irq(void) { irq_enabled=false; }
static void enable_irq(void) { irq_enabled=true; }
static bool x86_smp_take_reschedule_request(void) {
    bool value=pending; pending=false;
    if (inject_after_check && !irq_enabled) {
        inject_after_check=false; pending=true; inbox=1; hardware_ipi=true;
    }
    return value;
}
static unsigned sched_runnable_count(void) { return runnable; }
static bool sched_try_run_ready(void) {
    runnable+=inbox; inbox=0;
    if (runnable) { ++runs; --runnable; return true; }
    return false;
}
static void sleep_irq(void) {
    /* STI 的中断屏蔽窗口使 STI;HLT 在此竞争场景中不可分割。 */
    assert((!pending && !runnable) || hardware_ipi);
    ++sleeps; irq_enabled=true; hardware_ipi=false;
}
static void idle_boundary(void) {
''' + body + r'''
}
int main(void) {
    pending=true; inbox=1; idle_boundary();
    assert(runs==1 && !sleeps && !pending && irq_enabled);
    runnable=1; idle_boundary(); assert(runs==2 && !sleeps);
    idle_boundary(); assert(sleeps==1 && irq_enabled);
    inject_after_check=true; idle_boundary();
    assert(runs==3 && sleeps==2 && !pending && !inbox && irq_enabled);
    for (unsigned i=0;i<1000;++i) {
        pending=true; inbox=1; idle_boundary();
        assert(runs==i+4 && sleeps==2 && irq_enabled);
    }
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-bsp-sleep-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-Wno-unused-function','-fsanitize=address,undefined',
                    '-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('BSP sleep: coalesced wake never halts, quiet sleep and check-to-HLT IPI race passed')
