"""Actual thread creation/reaper: execution ownership precedes list publication."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/process/thread.c').read_text()
create = source[source.index('static kstatus_t thread_create_user_frame_internal('):
                source.index('static kstatus_t thread_create_user_internal(')]
exit_source = (root / 'kernel/process/exit.c').read_text()
reap = exit_source[exit_source.index('void thread_release_execution_ref('):
                   exit_source.index('static __noreturn void process_exit_internal(')]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define K_EINVAL -22
#define K_ENOMEM -12
#define K_EBUSY -16
#define K_OK 0
#define PROCESS_RUNNING 1U
#define PROCESS_DEAD 3U
#define PROCESS_FLAG_INIT_CPU_PINNED 4U
#define PROCESS_FLAG_EVER_HAD_THREAD 8U
#define THREAD_FLAG_EXECUTION_REF 1U
#define THREAD_FLAG_EXECUTION_REAPING 2U
#define KOBJECT_TYPE_THREAD 2U
#define X86_64_USER_TOP 0x7fffffffffffULL
#define THREAD_KERNEL_STACK_SIZE 65536U
#define OS_DEBUG_PHASE_USER_RUNTIME 1U
#define OS_DEBUG_STEP_CREATE 1U
typedef int kstatus_t;
typedef uint64_t vaddr_t;
typedef struct { unsigned refs,type; } object_header_t;
typedef struct { uint64_t words[20]; } arch_trap_frame_t;
typedef struct { uint64_t fs_base; struct { uintptr_t rsp,r12,r13,r14; } switch_ctx;bool fp; } arch_t;
typedef struct list_head { struct list_head *next,*prev; } list_head_t;
#define list_entry(n,t,m) ((t *)((char *)(n)-offsetof(t,m)))
typedef struct { unsigned refs; } vm_space_t;
typedef struct { object_header_t object;atomic_uint state;uint32_t flags,thread_count;
 uint64_t linux_thread_group_leader_tid;list_head_t threads,*enumeration_thread_cursor;vm_space_t *vm; } process_t;
typedef struct { object_header_t object;process_t *process;arch_t arch;uint64_t tid;
 uint32_t flags;atomic_uint state;size_t kernel_stack_size;void *kernel_stack_base;
 vaddr_t kernel_stack_top;list_head_t process_node,owned_mutexes;void *pi_blocked_on;
 bool user_stack_owned;vaddr_t user_stack_base;size_t user_stack_size; } thread_t;
static const unsigned g_thread_ops;
static unsigned g_process_thread_create_stage;
static unsigned held,allocation_calls,thread_frees,stack_allocations,stack_frees,fp_creates,fp_destroys,starts,reaps,resources;
static unsigned fail_stage;
static bool kill_at_unlock,valid_frame=true,start_fails;
static uint64_t next_tid=100;
static void list_init(list_head_t *h) { h->next=h->prev=h; }
static void list_add_tail(list_head_t *h,list_head_t *n) { assert(held);n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n; }
static void list_del(list_head_t *n) { n->prev->next=n->next;n->next->prev=n->prev;list_init(n); }
static void process_thread_lock(process_t *p) { assert(!held);held=1;if(fail_stage==4)atomic_store(&p->state,PROCESS_DEAD); }
void thread_release_execution_ref(thread_t *thread);
static void terminate_published(process_t *p) {
 assert(!held && p->thread_count==1);thread_t *t=list_entry(p->threads.next,thread_t,process_node);
 list_del(&t->process_node);p->thread_count=0;atomic_store(&p->state,PROCESS_DEAD);atomic_store(&t->state,3);
 thread_release_execution_ref(t);++reaps;
}
static void process_thread_unlock(process_t *p) { assert(held);held=0;if(kill_at_unlock && p->thread_count)terminate_published(p); }
static void *kzalloc(size_t size,unsigned flags) { assert(!held && !flags);++allocation_calls;return fail_stage==1?NULL:calloc(1,size); }
static void kfree(void *p) { assert(!held && p);++thread_frees;free(p); }
static void *vmalloc(size_t size) { assert(!held && size==65536);++stack_allocations;return fail_stage==3?NULL:malloc(size); }
static void vfree(void *p) { assert(!held && p);++stack_frees;free(p); }
static bool x86_fp_state_create(arch_t *a) { assert(!held);++fp_creates;if(fail_stage==2)return false;a->fp=true;return true; }
static void x86_fp_state_destroy(arch_t *a) { assert(!held && a->fp);a->fp=false;++fp_destroys; }
static void process_initialize_object(object_header_t *o,unsigned type,const void *ops) { assert(ops==&g_thread_ops);o->refs=1;o->type=type; }
static uint64_t process_allocate_task_id(void) { return next_tid++; }
static void object_get(void *p) { object_header_t *o=p;assert(o && o->refs);++o->refs; }
static void object_put(void *p) {
 object_header_t *o=p;assert(!held && o->refs);
 if(!--o->refs) {
  assert(o->type==KOBJECT_TYPE_THREAD);thread_t *t=p;
  vfree(t->kernel_stack_base);x86_fp_state_destroy(&t->arch);object_put(t->process);kfree(t);
 }
}
static bool x86_validate_user_frame(const arch_trap_frame_t *frame) { return frame && valid_frame; }
static bool x86_is_canonical(uint64_t value) { return value<=X86_64_USER_TOP; }
static unsigned x86_current_cpu_index(void) { return 2; }
static void sched_initialize_new_thread(thread_t *t,unsigned cpu,bool pinned) { assert(cpu==2 && pinned==((t->process->flags&4)!=0));atomic_init(&t->state,0); }
static void x86_user_thread_start(void) {}
static int sched_start_thread(thread_t *t) { assert(!held && t->object.refs);++starts;if(start_fails||atomic_load(&t->state)==3)return K_EBUSY;atomic_store(&t->state,1);return 0; }
static int thread_terminate(thread_t *t,int status) {
 assert(!held&&status==K_EBUSY&&t->object.refs);
 if(atomic_load(&t->state)!=3)terminate_published(t->process);
 return K_OK;
}
static void debug_trace_stage(unsigned phase,unsigned step,unsigned tid) { assert(phase==1 && step==1 && tid); }
static void vm_space_get(vm_space_t *v) { assert(held && v);++v->refs; }
static void vm_space_put(vm_space_t *v) { assert(!held && v && v->refs);--v->refs; }
static int vm_unmap(vm_space_t *v,vaddr_t base,size_t size) { assert(v && base && size);return 0; }
static void process_release_runtime_resources(process_t *p) { assert(!held && atomic_load(&p->state)==PROCESS_DEAD);++resources; }
'''
checks = r'''
static void reset(process_t *p) {
 memset(p,0,sizeof(*p));p->object.refs=1;p->object.type=1;atomic_init(&p->state,PROCESS_RUNNING);list_init(&p->threads);
 held=allocation_calls=thread_frees=stack_allocations=stack_frees=fp_creates=fp_destroys=starts=reaps=resources=0;
 fail_stage=0;kill_at_unlock=false;valid_frame=true;start_fails=false;
}
int main(void) {
 arch_trap_frame_t frame={{0}};for(unsigned i=0;i<20;++i)frame.words[i]=i*17;
 process_t p;
 for(unsigned round=0;round<100;++round)for(unsigned start=0;start<2;++start)for(unsigned kill=0;kill<2;++kill) {
  reset(&p);p.flags=round%2?PROCESS_FLAG_INIT_CPU_PINNED:0;kill_at_unlock=kill;thread_t *t=NULL;
  p.enumeration_thread_cursor=(list_head_t *)(uintptr_t)1;
  int created=thread_create_user_frame_internal(&p,&frame,0x8000,start,&t);
  assert(!p.enumeration_thread_cursor);
  if(start&&kill) {
   assert(created==K_EBUSY&&!t&&!p.thread_count&&p.object.refs==1);
   assert(thread_frees==1&&stack_frees==1&&fp_destroys==1&&reaps==1);
   continue;
  }
  assert(created==K_OK && t);
  assert(starts==start && !held && p.object.refs==2 && g_process_thread_create_stage==3);
  assert(!memcmp((void*)t->arch.switch_ctx.r12,&frame,sizeof(frame)) && t->arch.switch_ctx.r13==0x8000);
  if(kill) {
   assert(!p.thread_count && reaps==1 && resources==1 && t->object.refs==1);
   assert(!(t->flags&(THREAD_FLAG_EXECUTION_REF|THREAD_FLAG_EXECUTION_REAPING)));
  } else {
   assert(p.thread_count==1 && t->object.refs==2 && (t->flags&THREAD_FLAG_EXECUTION_REF));
   assert(p.linux_thread_group_leader_tid==t->tid && (p.flags&PROCESS_FLAG_EVER_HAD_THREAD));
   terminate_published(&p);assert(t->object.refs==1 && !(t->flags&THREAD_FLAG_EXECUTION_REF));
  }
  object_put(t);assert(p.object.refs==1 && thread_frees==1 && stack_frees==1 && fp_destroys==1);
 }
 for(unsigned failure=1;failure<=4;++failure)for(unsigned start=0;start<2;++start) {
  reset(&p);fail_stage=failure;thread_t *out=(void*)1;
  assert(thread_create_user_frame_internal(&p,&frame,0x8000,start,&out)==(failure==4?K_EBUSY:K_ENOMEM));
  assert(out==(void*)1 && p.object.refs==1 && !p.thread_count && !starts && !reaps && !held);
  assert(thread_frees==(failure!=1) && fp_destroys==(failure>=3) && stack_frees==(failure==4));
 }
 reset(&p);thread_t *out=NULL;
 start_fails=true;out=(void*)1;
 assert(thread_create_user_frame_internal(&p,&frame,0x8000,true,&out)==K_EBUSY);
 assert(out==(void*)1&&!p.thread_count&&p.object.refs==1&&starts==1&&reaps==1);
 assert(thread_frees==1&&stack_frees==1&&fp_destroys==1);
 reset(&p);out=NULL;
 assert(thread_create_user_frame_internal(NULL,&frame,0,false,&out)==K_EINVAL);
 assert(thread_create_user_frame_internal(&p,NULL,0,false,&out)==K_EINVAL);
 assert(thread_create_user_frame_internal(&p,&frame,0,false,NULL)==K_EINVAL);
 assert(thread_create_user_frame_internal(&p,&frame,UINT64_MAX,false,&out)==K_EINVAL);
 valid_frame=false;assert(thread_create_user_frame_internal(&p,&frame,0,false,&out)==K_EINVAL);
 valid_frame=true;atomic_store(&p.state,PROCESS_DEAD);
 assert(thread_create_user_frame_internal(&p,&frame,0,false,&out)==K_EBUSY && !allocation_calls);
 puts("400 real create/reap schedules, started/suspended paths, four rollback stages and input guards: no late execution ref, exact cleanup passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-thread-publication-') as directory:
    path = Path(directory)
    def run(actual):
        (path / 'test.c').write_text(fixture + reap + actual + checks)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        return subprocess.run([str(path / 'test')], capture_output=True, text=True)
    result = run(create)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout.strip())
    ownership = '''    object_get(thread);
    __atomic_fetch_or(&thread->flags, THREAD_FLAG_EXECUTION_REF,
                      __ATOMIC_RELEASE);
'''
    assert create.count(ownership) == 1
    negative = create.replace(ownership, '').replace('    if (start) {', ownership + '    if (start) {')
    result = run(negative)
    assert result.returncode != 0 and 'Assertion' in result.stderr
    print('Negative control rejected: terminator reaps before the creator adds execution ownership')
