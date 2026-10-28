"""Production page-fault entry stops exiting user siblings before VM work."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/arch/x86_64/irq/exception.c').read_text()
helper=source[source.index('static void exception_stop_exiting_user('):
              source.index('static bool exception_resolve_page_fault(')]
assert 'memory_order_acquire' in helper
irq_enable='__asm__ volatile ("sti" : : : "memory");'
assert helper.count(irq_enable)==1
helper=helper.replace(irq_enable,'interrupts_enabled=true;')
start=source.index('        thread_t *fault_thread = sched_current_thread();')
end=source.index('        if (from_uaccess',start)
entry=source[start:end]
assert entry.index('exception_stop_exiting_user(')<entry.index('exception_resolve_page_fault(')
program=r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <arch/x86_64/interrupt.h>
static jmp_buf exit_target;
static unsigned exits,resolutions;
static int64_t received;
static thread_t current;
static bool interrupts_enabled;
thread_t *sched_current_thread(void) { return &current; }
__noreturn void thread_exit(int64_t status) { assert(interrupts_enabled);received=status;++exits;longjmp(exit_target,1); }
''' + helper + r'''
static bool exception_resolve_page_fault(const arch_trap_frame_t *frame,
                                         vaddr_t address,bool from_user,bool from_uaccess) {
    (void)from_user;(void)from_uaccess;assert(frame && address==0x2950d08);++resolutions;return true;
}
static bool run(bool from_user,bool from_uaccess,process_t *process) {
    current.process=process;arch_trap_frame_t frame_data={0};
    arch_trap_frame_t *frame=&frame_data;uint64_t fault_address=0x2950d08;
    bool handled_fault;
''' + entry + r'''
    return handled_fault;
}
static bool try_run(bool user,bool uaccess,process_t *process) {
    if(setjmp(exit_target)==0)return run(user,uaccess,process);
    return false;
}
int main(void) {
    static process_t process;
    const int64_t codes[]={0,-9,-13,42,INT64_MIN,INT64_MAX};
    unsigned cases=0;
    for(unsigned user=0;user<2;++user)for(unsigned uaccess=0;uaccess<2;++uaccess)
      for(unsigned state=PROCESS_NEW;state<=PROCESS_DEAD;++state)
        for(unsigned code=0;code<sizeof(codes)/sizeof(*codes);++code)
          for(unsigned present=0;present<2;++present) {
            exits=resolutions=0;received=123;interrupts_enabled=false;
            process.exit_code=codes[code];atomic_store_explicit(&process.state,state,memory_order_release);
            bool stopped=user && present && state==PROCESS_EXITING;
            assert(try_run(user,uaccess,present?&process:0)==!stopped);
            assert(exits==(unsigned)stopped && resolutions==(unsigned)!stopped);
            assert(interrupts_enabled==stopped);
            if(stopped)assert(received==codes[code]);++cases;
          }
    printf("Exiting user fault: %u production-entry user/kernel/UAccess/process/state/exit-code cases; exiting siblings terminate before VM resolution, live process faults retain original path passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-exiting-user-fault-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=15)
