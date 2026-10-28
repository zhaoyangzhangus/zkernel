"""Run production fatal-fault mapping through production process-exit cascade.

CPU gates, lazy-fault resolution and device interrupts are not emulated here;
their placement relative to fatal termination is checked in the dispatcher.
"""
from pathlib import Path
import subprocess
import tempfile
import resource

root = Path(__file__).resolve().parents[2]
exception = (root / 'kernel/arch/x86_64/irq/exception.c').read_text()
exit_source = (root / 'kernel/process/exit.c').read_text()
signals = (root / 'include/uapi/signal.h').read_text()

start = exception.index('static __noreturn void exception_exit_user_process(')
fault_exit = exception[start:exception.index('\n}', start) + 2]
start = exception.index('static void exception_stop_exiting_user(')
stop_exiting = exception[start:exception.index('\n}', start) + 2]
enable_irqs = '__asm__ volatile ("sti" : : : "memory");'
assert fault_exit.count(enable_irqs) == stop_exiting.count(enable_irqs) == 1
fault_exit = fault_exit.replace(enable_irqs, 'interrupts_enabled = true;')
stop_exiting = stop_exiting.replace(enable_irqs, 'interrupts_enabled = true;')
start = exception.index('static bool exception_is_user_process_fault(')
classify = exception[start:exception.index('\n}', start) + 2]
start = exit_source.index('static __noreturn void process_exit_internal(')
cascade = exit_source[start:exit_source.index('__noreturn void thread_exit(', start)]
start = exception.index('void x86_interrupt_dispatch(')
dispatch = exception[start:exception.index('bool x86_exception_self_test(', start)]
assert dispatch.count('exception_exit_user_process(frame->vector);') == 2
assert 'thread_exit(K_EACCES)' not in dispatch
assert dispatch.index('if (handled_fault || x86_uaccess_fixup(frame)) return;') < dispatch.index('exception_exit_user_process(frame->vector);')
assert dispatch.index('if (frame->vector == 3U) return;') < dispatch.rindex('exception_exit_user_process(frame->vector);')
assert dispatch.index('if (frame->vector == 6U && g_ud_self_test)') < dispatch.rindex('exception_exit_user_process(frame->vector);')
assert 'thread_exit(process->exit_code);' in exception  # exiting siblings stay cheap
signal_defines = '\n'.join(line for line in signals.splitlines()
                           if line.startswith('#define OS_SIG') and not line.endswith('\\'))
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdatomic.h>
#define __noreturn _Noreturn
#define PROCESS_EXITING 1U
@SIGNALS@
typedef struct { uint64_t vector,cs; } arch_trap_frame_t;
typedef struct { atomic_uint state; unsigned live_siblings,close_notifications; int64_t status,exit_code; unsigned signal; } process_t;
typedef struct { process_t *process; unsigned index; } thread_t;
static process_t process;
static thread_t workers[4];
static thread_t *running;
static jmp_buf escaped;
static unsigned cascades,diagnostics,thread_exits;
static bool interrupts_enabled;
static thread_t *sched_current_thread(void){return running;}
static bool process_begin_exit(process_t *p,int64_t status,uint32_t signal){
 assert(interrupts_enabled);
 assert(p==&process);
 if(p->state)return false;
 p->state=1;p->status=status;p->signal=signal;return true;
}
static void process_exit_diag(process_t *p,thread_t *t,int64_t status,uint32_t signal){
 assert(p==&process && t==running && p->status==status && p->signal==signal);
 ++diagnostics;
}
static void process_terminate_siblings(process_t *p,thread_t *exclude,int64_t status){
 assert(p==&process && p->state==1 && exclude==running && p->status==status);
 ++cascades;
 /* Model the existing cancellation boundary, not the scheduler's internals. */
 p->live_siblings=0;++p->close_notifications;
}
static __noreturn void thread_exit(int64_t status){
 assert(interrupts_enabled);
 assert(status==process.status);++thread_exits;longjmp(escaped,1);
}
@CASCADE@
@CLASSIFY@
@FAULT_EXIT@
@STOP_EXITING@
static void fault_on_worker(unsigned worker,unsigned vector,unsigned expected){
 process=(process_t){.live_siblings=3};running=&workers[worker];
 cascades=diagnostics=thread_exits=0;
 interrupts_enabled=false;
 if(!setjmp(escaped))exception_exit_user_process(vector);
 assert(process.state==1 && process.signal==expected && process.status==-(int64_t)expected);
 assert(!process.live_siblings && process.close_notifications==1);
 assert(cascades==1 && diagnostics==1 && thread_exits==1);
 /* A second fault cannot overwrite status or repeat sibling termination. */
 running=&workers[(worker+1)%4];
 interrupts_enabled=false;
 if(!setjmp(escaped))exception_exit_user_process(vector);
 assert(cascades==1 && diagnostics==1 && thread_exits==2);
}
int main(void){
 for(unsigned i=0;i<4;++i)workers[i]=(thread_t){&process,i};
 for(unsigned vector=0;vector<32;++vector){
  arch_trap_frame_t f={vector,3};
  assert(exception_is_user_process_fault(&f)==(vector!=2 && vector!=3));
  f.cs=0;assert(!exception_is_user_process_fault(&f));
 }
 assert(!exception_is_user_process_fault(NULL));
 for(unsigned worker=0;worker<4;++worker){
  fault_on_worker(worker,0,OS_SIGFPE);
  fault_on_worker(worker,6,OS_SIGILL);
  fault_on_worker(worker,13,OS_SIGSEGV);
  fault_on_worker(worker,14,OS_SIGSEGV);
  fault_on_worker(worker,16,OS_SIGFPE);
  fault_on_worker(worker,17,OS_SIGBUS);
  fault_on_worker(worker,19,OS_SIGFPE);
 }
 /* Already-exiting user siblings must accept IPIs before thread cleanup;
  * returning kernel and live-user paths must leave the gate state alone. */
 process=(process_t){.state=PROCESS_EXITING,.status=-11,.exit_code=-11};
 running=&workers[0];thread_exits=0;interrupts_enabled=false;
 exception_stop_exiting_user(false,&process);
 exception_stop_exiting_user(true,NULL);
 assert(!interrupts_enabled && !thread_exits);
 process.state=0;exception_stop_exiting_user(true,&process);
 assert(!interrupts_enabled && !thread_exits);
 process.state=PROCESS_EXITING;
 if(!setjmp(escaped))exception_stop_exiting_user(true,&process);
 assert(interrupts_enabled && thread_exits==1);
}
'''
for token, value in (('SIGNALS', signal_defines), ('CASCADE', cascade),
                     ('CLASSIFY', classify), ('FAULT_EXIT', fault_exit),
                     ('STOP_EXITING', stop_exiting)):
    program = program.replace('@' + token + '@', value)
with tempfile.TemporaryDirectory(prefix='user-fault-process-') as directory:
    binary = str(Path(directory) / 'test')
    def run(text):
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-x', 'c', '-', '-o', binary], input=text, text=True, check=True)
        return subprocess.run([binary], capture_output=True, text=True, timeout=10,
                              preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    result = run(program)
    assert result.returncode == 0, result.stderr
    negative = program.replace('    process_exit_signal(signal);',
                               '    process.status=-(int64_t)signal; thread_exit(process.status);')
    assert negative != program
    result = run(negative)
    assert result.returncode != 0
    negative = program.replace('interrupts_enabled = true;', '(void)0;', 1)
    assert negative != program
    assert run(negative).returncode != 0
    location = program.index('static void exception_stop_exiting_user(')
    negative = program[:location] + program[location:].replace(
        'interrupts_enabled = true;', '(void)0;', 1)
    assert negative != program
    assert run(negative).returncode != 0
print('PASS: 28 worker/vector faults, exact signal/status, one sibling cascade, repeated faults, kernel/debugger exclusions and dispatcher ordering')
print('Negative control rejected: fault exits only one thread and strands siblings')
print('PASS: fatal and already-exiting user teardown enables IPIs; returning paths preserve IF')
print('Negative controls rejected: either teardown boundary retains IF=0')
