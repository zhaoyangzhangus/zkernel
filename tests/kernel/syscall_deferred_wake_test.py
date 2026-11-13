"""Actual C return boundary: service deferred wake after committing frames."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/handlers.c').read_text()
start = source.index('static int64_t syscall_complete_dispatch(')
end = source.index('\n}', start) + 2
dispatch = source[source.index('int64_t syscall_dispatch('):
                  source.index('bool x86_validate_user_frame(')]
assert dispatch.count('return syscall_complete_dispatch(executing_thread,') == 3
linux = dispatch[:dispatch.index('bool measure_native_transition')]
assert linux.index('executing_thread->exec_pending = false') < linux.index('return syscall_complete_dispatch')
native = dispatch[dispatch.index('bool measure_native_transition'):]
assert native.index('executing_thread->exec_pending = false') < native.rindex('return syscall_complete_dispatch')
assert native.index('status = syscall_deliver_pending_signal') < native.rindex('return syscall_complete_dispatch')
program = r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <setjmp.h>
#define OS_SIGKILL 9U
#define THREAD_FLAG_TERMINATE_REQUEST 4U
#define SCHED_CLASS_IDLE 2
typedef struct { unsigned sched_class,flags; bool exec_pending; } thread_t;
static jmp_buf terminated;
static unsigned exits;
static void thread_exit(int64_t status){assert(status==-9);++exits;longjmp(terminated,1);}
static bool disabled, shootdown, pending, local_request;
static unsigned takes, drains, local_checks;
static bool sched_preempt_disabled(void) { return disabled; }
static bool x86_tlb_shootdown_active(void) { return shootdown; }
static bool x86_smp_take_reschedule_request(void) {
    ++takes;bool result=pending;pending=false;return result;
}
static bool sched_tick_should_preempt(void) { ++local_checks;return local_request; }
static void schedule(void) {
    assert(!disabled && !shootdown);++drains;local_request=false;
}
''' + source[start:end] + r'''
int main(void) {
    thread_t t={0};pending=true;
    assert(syscall_complete_dispatch(0,-22)==-22 && pending && !takes);
    t.sched_class=SCHED_CLASS_IDLE;
    assert(syscall_complete_dispatch(&t,0)==0 && pending && !takes);
    t.sched_class=0;t.exec_pending=true;
    assert(syscall_complete_dispatch(&t,7)==7 && pending && !takes);
    t.exec_pending=false;disabled=true;
    assert(syscall_complete_dispatch(&t,-1)==-1 && pending && !takes);
    disabled=false;shootdown=true;
    assert(syscall_complete_dispatch(&t,-38)==-38 && pending && !takes);
    shootdown=false;
    assert(syscall_complete_dispatch(&t,123456789)==123456789);
    assert(!pending && takes==1 && drains==1);
    assert(syscall_complete_dispatch(&t,-110)==-110 && takes==2 && drains==1);
    pending=true;
    assert(syscall_complete_dispatch(&t,0)==0 && !pending && drains==2);
    /* 本地唤醒没有待处理的 IPI 位：旧的返回辅助函数会丢失该唤醒。 */
    local_request=true;
    assert(syscall_complete_dispatch(&t,-5)==-5 && drains==3 && !local_request);
    assert(syscall_complete_dispatch(&t,9)==9 && drains==3);
    local_request=true;pending=true;
    assert(syscall_complete_dispatch(&t,1)==1 && drains==4 && !pending && !local_request);
    unsigned before=local_checks, before_takes=takes;
    disabled=true;local_request=true;pending=true;
    assert(syscall_complete_dispatch(&t,1)==1 && drains==4 && pending && local_request);
    assert(local_checks==before && takes==before_takes);
    disabled=false;shootdown=true;
    assert(syscall_complete_dispatch(&t,1)==1 && drains==4 && pending && local_request);
    assert(local_checks==before && takes==before_takes);
    shootdown=false;t.exec_pending=true;
    assert(syscall_complete_dispatch(&t,1)==1 && drains==4 && pending && local_request);
    t.exec_pending=false;
    assert(syscall_complete_dispatch(&t,1)==1 && drains==5 && !pending && !local_request);
    t.flags=THREAD_FLAG_TERMINATE_REQUEST;
    if(!setjmp(terminated)){syscall_complete_dispatch(&t,1);assert(false);}
    assert(exits==1);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-syscall-wake-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Syscall deferred wake: C return, exec/signal ordering, status preservation, idle/lock/TLB guards and pending consumption passed')
