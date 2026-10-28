"""Execute the kernel-side thread start/destroy lifetime gates, not host stubs."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/drivers/gpu/nvidia/port/nv_port_thread.c').read_text()
deferred=(root/'kernel/irq/deferred.c').read_text()
assert 'worker->tid = process_allocate_task_id();' in deferred
def function(name):
    start=source.index('kstatus_t '+name+'(')
    return source[start:source.index('\n}',start)+2]
program=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nv_port.h"
#include <kernel/kmem.h>
#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#define NV_PORT_THREAD_STACK_SIZE (64U*1024U)
static nv_port_thread_t allocated[2];
static unsigned allocations, stacks;
static unsigned char stack_memory[2][NV_PORT_THREAD_STACK_SIZE];
static uint64_t next_task_id=100;
uint64_t process_allocate_task_id(void) { return next_task_id++; }
void *kzalloc(size_t size,uint32_t flags) {
 assert(size==sizeof(nv_port_thread_t) && !flags && allocations<2);
 return &allocated[allocations++];
}
void *vmalloc(size_t size) {
 assert(size==NV_PORT_THREAD_STACK_SIZE && stacks<2);
 return stack_memory[stacks++];
}
uint32_t x86_current_cpu_index(void) { return 0; }
void sched_initialize_new_thread(thread_t *t,uint32_t cpu,bool pin) {
 assert(cpu==0 && pin); atomic_init(&t->state,THREAD_NEW);
}
void nv_port_completion_init(nv_port_completion_t *c) { memset(c,0,sizeof(*c)); }
void x86_kernel_thread_start(void) { __builtin_trap(); }
static void nv_port_kernel_thread_entry(void *argument) { (void)argument; }
static unsigned gets, drops, frees;
static unsigned monitor_removes;
void sched_monitor_register(thread_t *t,const char *name) {
 assert(!t->process&&name&&!t->monitor_name);t->monitor_name=name;
}
void sched_monitor_unregister(thread_t *t) {++monitor_removes;t->monitor_name=0;}
static kstatus_t start_status;
void object_get(void *p) { thread_t *t=p; ++gets; atomic_fetch_add(&t->object.refs.value,1); }
void object_put(void *p) { thread_t *t=p; ++drops; assert(atomic_fetch_sub(&t->object.refs.value,1)>1); }
kstatus_t sched_start_thread(thread_t *t) {
 assert(atomic_load(&t->object.refs.value)==2 && (t->flags&THREAD_FLAG_EXECUTION_REF));
 if(start_status==K_OK) atomic_store(&t->state,THREAD_READY);
 return start_status;
}
thread_t *sched_current_thread(void) { return 0; }
void vfree(void *p) { (void)p; assert(monitor_removes==1);++frees; }
void kfree(void *p) { assert(!((nv_port_thread_t*)p)->thread.monitor_name);++frees; }
''' + function('nv_port_thread_create') + '\n' + function('nv_port_thread_start') + '\n' + function('nv_port_thread_destroy') + r'''
int main(void) {
 nv_port_thread_t *first,*second;
 assert(nv_port_thread_create(nv_port_kernel_thread_entry,0,&first)==K_OK);
 assert(nv_port_thread_create(nv_port_kernel_thread_entry,0,&second)==K_OK);
 assert(first->thread.tid==100 && second->thread.tid==101);
 assert(first->thread.tid!=second->thread.tid && !first->thread.process);
 nv_port_thread_t t;
 for(unsigned failed=0; failed<2; ++failed) {
  memset(&t,0,sizeof(t)); atomic_init(&t.thread.state,THREAD_NEW);
  atomic_init(&t.thread.object.refs.value,1); start_status=failed?K_EIO:K_OK;
  gets=drops=frees=monitor_removes=0;
  assert(nv_port_thread_start(&t)==start_status && gets==1);
  if(failed) {
   assert(drops==1 && !atomic_load(&t.started) && !t.thread.flags);
   assert(nv_port_thread_destroy(&t)==K_OK && frees==2); continue;
  }
  atomic_store(&t.finished,true);
  assert(nv_port_thread_destroy(&t)==K_EBUSY && !frees);
  atomic_store(&t.thread.state,THREAD_DEAD);
  assert(nv_port_thread_destroy(&t)==K_EBUSY && !frees);
  /* DEAD and finished alone do not prove that another CPU left the stack. */
  t.thread.flags=THREAD_FLAG_EXECUTION_REAPING;
  atomic_store(&t.thread.object.refs.value,1);
  assert(!monitor_removes);
  assert(nv_port_thread_destroy(&t)==K_EBUSY && !frees);
  t.thread.flags=0;
  atomic_store(&t.thread.object.refs.value,2); /* Outstanding scheduler command. */
  assert(nv_port_thread_destroy(&t)==K_EBUSY && !frees);
  atomic_store(&t.thread.object.refs.value,1);
  assert(nv_port_thread_destroy(&t)==K_OK && frees==2);
 }
 puts("NVIDIA kernel thread lifetime: start rollback, DEAD-before-switch, reaper and command references PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='nv-thread-lifetime-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    compile_args=['cc','-std=c11','-Wall','-Wextra','-Werror','-DOS_KERNEL_BUILD',
                    '-I',str(root/'include'),'-I',str(root/'kernel/drivers/gpu/nvidia/port/include'),
                    '-fsanitize=address,undefined','-no-pie',str(path/'test.c'),
                    '-o',str(path/'test')]
    subprocess.run(compile_args,check=True)
    subprocess.run([str(path/'test')],check=True,timeout=15)
    negative=program.replace('thread->thread.tid = process_allocate_task_id();',
                             'thread->thread.tid = 0U;')
    assert negative != program
    negative=negative.replace('assert(first->thread.tid==100 && second->thread.tid==101);',
                             'if(first->thread.tid!=100 || second->thread.tid!=101) return 77;')
    (path/'test.c').write_text(negative)
    subprocess.run(compile_args,check=True)
    assert subprocess.run([str(path/'test')],timeout=15).returncode==77
    print('Negative control rejected: shared zero TID for kernel GPU workers')
