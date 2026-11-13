"""Actual stack registration/reaper: retained validation and locked ownership."""
import ast
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
# 复用回收器测试夹具中的锁/引用/析构断言，但不执行回收器本身。
tree = ast.parse((root / 'tests/kernel/thread_stack_vm_pin_test.py').read_text())
fixture = next(ast.literal_eval(node.value) for node in tree.body
               if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'fixture' for t in node.targets))
fixture = fixture.replace('bool user_stack_owned;', 'atomic_uint state; bool user_stack_owned;')
thread_source = (root / 'kernel/process/thread.c').read_text()
register = thread_source[thread_source.index('kstatus_t thread_register_user_stack('):
                         thread_source.index('kstatus_t thread_start(')]
register = register.replace('            thread->user_stack_base = base;',
                            '            race_after_registration_check(thread);\n            thread->user_stack_base = base;')
exit_source = (root / 'kernel/process/exit.c').read_text()
reap = exit_source[exit_source.index('void thread_release_execution_ref('):
                   exit_source.index('static __noreturn void process_exit_internal(')]
process_source = (root / 'kernel/process/process.c').read_text()
release = process_source[process_source.index('void process_release_runtime_resources('):
                         process_source.index('static bool process_destruction_self_test_round(')]
extra = r'''
#include <sched.h>
#define __user
#define K_OK 0
#define K_EINVAL -22
#define K_EBUSY -16
#define PROCESS_RUNNING 1U
#define THREAD_NEW 0U
#define THREAD_DEAD 3U
typedef int kstatus_t;
static bool validation_pause,mapping_valid=true,publication_race;
static pthread_t reaper_job;
static unsigned validation_calls;
static bool x86_user_range_valid(const void *base,size_t size) { return base && size; }
static bool vm_range_is_mapped(vm_space_t *space,vaddr_t base,size_t size) {
 assert(!held && base==0x10000 && size==8192);++validation_calls;
 if(validation_pause) {
  pthread_mutex_lock(&gate);entered=true;pthread_cond_broadcast(&condition);
  while(!allow)pthread_cond_wait(&condition,&gate);
  pthread_mutex_unlock(&gate);
 }
 assert(atomic_load(&space->refs));return mapping_valid;
}
static void race_after_registration_check(thread_t *thread);
'''
checks = r'''
static void *reaper(void *p) { thread_release_execution_ref(p);return NULL; }
static void race_after_registration_check(thread_t *t) {
 if(!publication_race)return;
 assert(held);atomic_store(&t->state,THREAD_DEAD);
 assert(!pthread_create(&reaper_job,NULL,reaper,t));
 // 真实回收器会先声明其标志，再等待注册操作的所有者锁。
 while(!(__atomic_load_n(&t->flags,__ATOMIC_ACQUIRE)&THREAD_FLAG_EXECUTION_REAPING) && !atomic_load(&thread_puts))sched_yield();
}
typedef struct { thread_t *thread;int status; } registration_t;
static void *registration(void *p) {
 registration_t *job=p;job->status=thread_register_user_stack(job->thread,0x10000,8192);return NULL;
}
static thread_t setup(process_t *p) {
 *p=(process_t){0};pthread_mutex_init(&p->lock,NULL);atomic_init(&p->state,PROCESS_RUNNING);
 p->vm=malloc(sizeof(*p->vm));assert(p->vm);atomic_init(&p->vm->refs,1);
 thread_t t={.flags=THREAD_FLAG_EXECUTION_REF,.process=p};atomic_init(&t.state,THREAD_NEW);atomic_init(&t.refs,2);
 vm_destroyed=handle_closes=fd_closes=unmaps=thread_puts=0;
 validation_pause=publication_race=entered=false;allow=mapping_valid=true;validation_calls=0;return t;
}
static void finish(process_t *p,thread_t *t) {
 atomic_store(&t->state,THREAD_DEAD);thread_release_execution_ref(t);
 process_release_runtime_resources(p);assert(!p->vm && atomic_load(&vm_destroyed)==1);
 assert(atomic_load(&t->refs)==1);pthread_mutex_destroy(&p->lock);
}
int main(void) {
 for(unsigned round=0;round<100;++round) {
  process_t p;thread_t t=setup(&p);validation_pause=true;allow=false;mapping_valid=round%2;
  registration_t job={.thread=&t};pthread_t worker;assert(!pthread_create(&worker,NULL,registration,&job));
  pthread_mutex_lock(&gate);while(!entered)pthread_cond_wait(&condition,&gate);pthread_mutex_unlock(&gate);
  atomic_store(&t.state,THREAD_DEAD);atomic_store(&p.state,PROCESS_DEAD);
  thread_release_execution_ref(&t);
  assert(!p.vm && !atomic_load(&vm_destroyed)); // validator owns the remaining VM reference
  pthread_mutex_lock(&gate);allow=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&gate);
  assert(!pthread_join(worker,NULL));
  assert(job.status==(mapping_valid?K_EBUSY:K_EINVAL));
  assert(!t.user_stack_owned && !t.user_stack_base && !t.user_stack_size && !t.flags);
  assert(atomic_load(&vm_destroyed)==1 && !atomic_load(&unmaps) && atomic_load(&t.refs)==1);
  pthread_mutex_destroy(&p.lock);
 }
 for(unsigned round=0;round<100;++round) {
  process_t p;thread_t t=setup(&p);publication_race=true;
  assert(thread_register_user_stack(&t,0x10000,8192)==K_OK);
  assert(!pthread_join(reaper_job,NULL));
  assert(!t.user_stack_owned && !t.user_stack_base && !t.user_stack_size && !t.flags);
  assert(atomic_load(&unmaps)==1 && atomic_load(&t.refs)==1 && atomic_load(&p.vm->refs)==1);
  process_release_runtime_resources(&p);assert(atomic_load(&vm_destroyed)==1);pthread_mutex_destroy(&p.lock);
 }
 process_t p;thread_t t=setup(&p);
 assert(thread_register_user_stack(&t,0x10000,8192)==K_OK && t.user_stack_owned);
 assert(thread_register_user_stack(&t,0x10000,8192)==K_EBUSY);finish(&p,&t);
 for(unsigned mode=0;mode<5;++mode) {
  t=setup(&p);
  if(mode==0)atomic_store(&t.state,THREAD_DEAD);
  if(mode==1)atomic_store(&t.state,1); // already started
  if(mode==2)t.flags=0;
  if(mode==3)t.flags|=THREAD_FLAG_EXECUTION_REAPING;
  if(mode==4)atomic_store(&p.state,PROCESS_DEAD);
  assert(thread_register_user_stack(&t,0x10000,8192)==K_EBUSY && !t.user_stack_owned);
  t.flags=THREAD_FLAG_EXECUTION_REF;finish(&p,&t);
 }
 t=setup(&p);mapping_valid=false;
 assert(thread_register_user_stack(&t,0x10000,8192)==K_EINVAL && !t.user_stack_owned);
 assert(thread_register_user_stack(NULL,0x10000,8192)==K_EINVAL);
 assert(thread_register_user_stack(&t,0,8192)==K_EINVAL);
 assert(thread_register_user_stack(&t,0x10000,0)==K_EINVAL);
 finish(&p,&t);
 puts("100 validation/exit and 100 publication/reaper races, duplicate/terminal/started/unmapped guards: pinned VM and exactly-once stack reclamation passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-stack-registration-') as directory:
    path = Path(directory)
    def run(registration_body, reaper_body):
        (path / 'test.c').write_text(fixture + extra + release + reaper_body + registration_body + checks)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        return subprocess.run([str(path / 'test')], capture_output=True, text=True, timeout=15)
    result = run(register, reap)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout.strip())
    negative = register.replace('if (space != 0) vm_space_get(space);', 'if (false) vm_space_get(space);')
    negative = negative.replace('    vm_space_put(space);', '    (void)space;')
    result = run(negative, reap)
    assert result.returncode != 0 and 'Assertion' in result.stderr
    print('Negative control rejected: validator borrows a VM destroyed by exit')
    negative = reap.replace('if (process != 0) process_thread_lock(process);', 'if (false) process_thread_lock(process);')
    negative = negative.replace('if (process != 0) process_thread_unlock(process);', 'if (false) process_thread_unlock(process);')
    result = run(register, negative)
    assert negative != reap and result.returncode != 0 and 'Assertion' in result.stderr
    print('Negative control rejected: reaper inspects stack metadata without its owner lock')
