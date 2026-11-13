"""Actual execution reaper and resource teardown: sibling detaches VM mid-unmap."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
exit_source=(root/'kernel/process/exit.c').read_text()
process_source=(root/'kernel/process/process.c').read_text()
reap=exit_source[exit_source.index('void thread_release_execution_ref('):
                 exit_source.index('static __noreturn void process_exit_internal(')]
release=process_source[process_source.index('void process_release_runtime_resources('):
                       process_source.index('static bool process_destruction_self_test_round(')]
fixture=r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define THREAD_FLAG_EXECUTION_REF 1U
#define THREAD_FLAG_EXECUTION_REAPING 2U
#define PROCESS_FLAG_RESOURCES_RELEASED 2U
#define PROCESS_DEAD 2U
typedef uint64_t vaddr_t;
typedef struct { atomic_uint refs; } vm_space_t;
typedef struct { uint32_t flags; uint64_t pid; atomic_uint state; pthread_mutex_t lock;
                 vm_space_t *vm; unsigned handles,linux_fds; } process_t;
typedef struct { uint32_t flags; process_t *process; bool user_stack_owned;
                 vaddr_t user_stack_base; size_t user_stack_size; atomic_uint refs; } thread_t;
static _Thread_local unsigned held;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static bool entered,allow;
static atomic_uint vm_destroyed,handle_closes,fd_closes,unmaps,thread_puts;
static void process_thread_lock(process_t *p) { assert(!held);pthread_mutex_lock(&p->lock);++held; }
static void process_thread_unlock(process_t *p) { assert(held==1);--held;pthread_mutex_unlock(&p->lock); }
static void vm_space_get(vm_space_t *space) { assert(held && atomic_load(&space->refs));atomic_fetch_add(&space->refs,1); }
static void vm_space_put(vm_space_t *space) {
 assert(!held && atomic_load(&space->refs));
 if(atomic_fetch_sub(&space->refs,1)==1) { ++vm_destroyed;free(space); }
}
static int vm_unmap(vm_space_t *space,vaddr_t base,size_t size) {
 assert(!held && base==0x10000 && size==8192);++unmaps;
 pthread_mutex_lock(&gate);entered=true;pthread_cond_broadcast(&condition);
 while(!allow)pthread_cond_wait(&condition,&gate);
 pthread_mutex_unlock(&gate);
 assert(atomic_load(&space->refs)); /* ASan catches the old unpinned snapshot. */
 return -22; /* Failure must release the VM pin too. */
}
static void handle_table_destroy(unsigned *table) { assert(!held && table);++handle_closes; }
static void linux_fd_table_destroy(unsigned *table) { assert(!held && table);++fd_closes; }
static void file_lock_release_owner(uint64_t owner) { assert(!held && owner == 0 && !fd_closes); }
static void object_put(void *object) { thread_t *t=object;assert(!held && atomic_load(&t->refs)==2);--t->refs;++thread_puts; }
void process_release_runtime_resources(process_t *process);
'''
checks=r'''
static void *reaper(void *p) { thread_release_execution_ref(p);return NULL; }
static thread_t new_thread(process_t *process,bool owned) {
 thread_t t={.flags=THREAD_FLAG_EXECUTION_REF,.process=process,.user_stack_owned=owned,
             .user_stack_base=owned?0x10000:0,.user_stack_size=owned?8192:0};
 atomic_init(&t.refs,2);return t;
}
int main(void) {
 for(unsigned round=0;round<100;++round) {
  process_t process={0};pthread_mutex_init(&process.lock,NULL);atomic_init(&process.state,PROCESS_DEAD);
  process.vm=malloc(sizeof(*process.vm));assert(process.vm);atomic_init(&process.vm->refs,1);
  thread_t first=new_thread(&process,true),second=new_thread(&process,false);
  vm_destroyed=handle_closes=fd_closes=unmaps=thread_puts=0;entered=allow=false;
  pthread_t job;assert(!pthread_create(&job,NULL,reaper,&first));
  pthread_mutex_lock(&gate);while(!entered)pthread_cond_wait(&condition,&gate);pthread_mutex_unlock(&gate);
  thread_release_execution_ref(&second); /* final sibling drops the process VM owner */
  assert(!process.vm && !atomic_load(&vm_destroyed));
  assert(atomic_load(&handle_closes)==1 && atomic_load(&fd_closes)==1);
  pthread_mutex_lock(&gate);allow=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&gate);
  assert(!pthread_join(job,NULL));
  assert(atomic_load(&vm_destroyed)==1 && atomic_load(&unmaps)==1 && atomic_load(&thread_puts)==2);
  assert(!first.flags && !second.flags && !first.user_stack_owned && !first.user_stack_base && !first.user_stack_size);
  assert(atomic_load(&first.refs)==1 && atomic_load(&second.refs)==1);
  thread_release_execution_ref(&first);thread_release_execution_ref(&second);
  process_release_runtime_resources(&process);
  assert(atomic_load(&handle_closes)==1 && atomic_load(&fd_closes)==1 && atomic_load(&thread_puts)==2);
  pthread_mutex_destroy(&process.lock);
 }
 thread_release_execution_ref(NULL);process_release_runtime_resources(NULL);
 thread_t detached=new_thread(NULL,true);thread_release_execution_ref(&detached);
 assert(!detached.user_stack_owned && !detached.flags && atomic_load(&detached.refs)==1);
 process_t empty={0};pthread_mutex_init(&empty.lock,NULL);atomic_init(&empty.state,0);
 thread_t no_vm=new_thread(&empty,true);thread_release_execution_ref(&no_vm);
 assert(!no_vm.user_stack_owned && !no_vm.flags);pthread_mutex_destroy(&empty.lock);
 puts("100 sibling VM-detach/unmap interleavings, unmap error, null VM/process and repeated reaping: lifetime pins and once-only teardown passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-stack-vm-pin-') as directory:
    path=Path(directory)
    def run(body):
        (path/'test.c').write_text(fixture+release+body+checks)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-pthread',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        str(path/'test.c'),'-o',str(path/'test')],check=True)
        return subprocess.run([str(path/'test')],capture_output=True,text=True)
    result=run(reap)
    assert result.returncode==0,result.stdout+result.stderr
    print(result.stdout.strip())
    # 同样省略所有权递减，以复现借用快照的情况。
    negative=reap.replace('if (space != 0) vm_space_get(space);','if (false) vm_space_get(space);')
    negative=negative.replace('            vm_space_put(space);','')
    result=run(negative)
    assert result.returncode!=0, 'Unpinned VM survived concurrent process teardown'
    print('Negative control rejected: sibling teardown destroys VM before unmap returns')
