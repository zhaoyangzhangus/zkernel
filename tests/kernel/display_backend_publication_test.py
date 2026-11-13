"""真实显示后端的一次发布、并发注册、只读快照与提交中拒绝切换。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/display/core.c').read_text()

def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

state = source[source.index('typedef struct display_state {'):source.index('typedef struct display_pending_commit {')]
registration = function('kstatus_t display_core_register_scanout_backend(')
copy_statement = '        g_display.scanout_backend =\n            *backend;'
assert copy_statement in registration
registration = registration.replace(copy_statement, copy_statement+'\n        before_publish();')
program = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <kernel/display.h>
#include <kernel/spinlock.h>
''' + state + r'''
static display_state_t g_display;
static atomic_uint g_display_init_state,lock_entries,successes;
static atomic_bool start,query_phase;
static _Thread_local unsigned preempt_depth;
static atomic_uint publication_checks;
static void before_publish(void);
static void sched_preempt_disable(void){
 assert(!atomic_load(&query_phase));assert(!preempt_depth);++preempt_depth;atomic_fetch_add(&lock_entries,1);
}
static void sched_preempt_enable(void){assert(preempt_depth==1);--preempt_depth;}
''' + '\n'.join(registration if s.startswith('kstatus_t display_core_register') else function(s) for s in (
    'static bool display_initialize(', 'static void display_lock(',
    'static void display_unlock(', 'static bool display_scanout_backend_snapshot(',
    'kstatus_t display_core_register_scanout_backend(', 'bool display_core_has_native_scanout(')) + r'''
static void before_publish(void){
 assert(!display_core_has_native_scanout(0));
 display_scanout_backend_t b={.context=(void *)1};
 assert(!display_scanout_backend_snapshot(&b)&&b.context==(void *)1);
 atomic_fetch_add(&publication_checks,1);
}
static unsigned contexts[8];
static void poll_a(void *context){assert(*(unsigned *)context<8);}
static void poll_b(void *context){assert(*(unsigned *)context<8);}
static kstatus_t commit(void *context,const display_scanout_request_t *request,
 display_scanout_complete_fn complete,void *completion_context){
 (void)request;(void)complete;(void)completion_context;return *(unsigned *)context<8?K_OK:K_EINVAL;
}
static display_scanout_backend_t backend(unsigned i){
 return (display_scanout_backend_t){.context=&contexts[i],.poll=i%2?poll_a:poll_b,.atomic_commit=commit};
}
static void verify(const display_scanout_backend_t *b){
 assert(b->context>= (void *)contexts&&b->context< (void *)(contexts+8));
 unsigned i=*(unsigned *)b->context;
 assert(i<8&&b->context==&contexts[i]&&b->poll==(i%2?poll_a:poll_b)&&b->atomic_commit==commit);
 b->poll(b->context);assert(b->atomic_commit(b->context,0,0,0)==K_OK);
}
static void *registrar(void *argument){
 unsigned i=(unsigned)(uintptr_t)argument;display_scanout_backend_t b=backend(i);
 while(!atomic_load(&start))sched_yield();
 kstatus_t status=display_core_register_scanout_backend(0,&b);
 assert(status==K_OK||status==K_EEXIST);
 if(status==K_OK)atomic_fetch_add(&successes,1);
 return 0;
}
static void *reader(void *unused){
 (void)unused;while(!atomic_load(&start))sched_yield();
 for(unsigned i=0;i<10000;++i){
  display_scanout_backend_t b={.context=(void *)1};
  bool ready=display_core_has_native_scanout(0);
  if(display_scanout_backend_snapshot(&b))verify(&b);
  else {assert(!ready);assert(b.context==(void *)1);}
  assert(!preempt_depth);
 }
 return 0;
}
static void query_with_lock_held(void){
 pthread_t readers[8];
 unsigned before=atomic_load(&lock_entries);
 display_lock();atomic_store(&query_phase,true);
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&readers[i],0,reader,0));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(readers[i],0));
 atomic_store(&query_phase,false);display_unlock();
 assert(atomic_load(&lock_entries)==before+1);
}
int main(void){
 for(unsigned i=0;i<8;++i)contexts[i]=i;
 assert(!display_core_has_native_scanout(1)&&!display_core_has_native_scanout(0));
 display_scanout_backend_t b=backend(0),empty={0},copy={.context=(void *)1};
 assert(!display_scanout_backend_snapshot(0));
 assert(!display_scanout_backend_snapshot(&copy)&&copy.context==(void *)1);
 assert(display_core_register_scanout_backend(1,&b)==K_EINVAL);
 assert(display_core_register_scanout_backend(0,0)==K_EINVAL);
 assert(display_core_register_scanout_backend(0,&empty)==K_EINVAL);
 g_display.pending_commit=true;
 assert(display_core_register_scanout_backend(0,&b)==K_EBUSY);
 assert(!display_core_has_native_scanout(0));g_display.pending_commit=false;
 atomic_store(&start,true);query_with_lock_held();
 atomic_store(&start,false);
 pthread_t writers[8],readers[8];
 for(unsigned i=0;i<8;++i){
  assert(!pthread_create(&writers[i],0,registrar,(void *)(uintptr_t)i));
  assert(!pthread_create(&readers[i],0,reader,0));
 }
 atomic_store(&start,true);
 for(unsigned i=0;i<8;++i){assert(!pthread_join(writers[i],0));assert(!pthread_join(readers[i],0));}
 assert(atomic_load(&successes)==1&&atomic_load(&publication_checks)==1&&display_core_has_native_scanout(0));
 assert(display_scanout_backend_snapshot(&copy));verify(&copy);
 for(unsigned i=0;i<8;++i){
  b=backend(i);assert(display_core_register_scanout_backend(0,&b)==K_EEXIST);
  display_scanout_backend_t after;assert(display_scanout_backend_snapshot(&after));
  assert(after.context==copy.context&&after.poll==copy.poll&&after.atomic_commit==copy.atomic_commit);
 }
 query_with_lock_held();
 assert(!display_core_has_native_scanout(1));
 puts("显示后端：8 路竞争仅注册一次，24 万次查询/快照组合，提交中拒绝切换，描述一致且持锁时查询不禁抢占、不取锁，通过");
}
'''
with tempfile.TemporaryDirectory(prefix='display-backend-publication-') as directory:
    binary = str(Path(directory) / 'test')
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-I'+str(root/'include'), '-x', 'c', '-', '-o', binary]
    subprocess.run(command, input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=20)
    # 恢复原来的查询锁，会在尝试禁抢占时被拒绝，不等待自旋锁超时。
    signature = 'bool display_core_has_native_scanout(uint32_t output) {'
    broken = program.replace(signature, signature+'\n    display_lock();display_unlock();',1)
    assert broken != program
    subprocess.run(command, input=broken, text=True, check=True)
    result = subprocess.run([binary], text=True, capture_output=True, timeout=20,
        preexec_fn=lambda: __import__('resource').setrlimit(__import__('resource').RLIMIT_CORE,(0,0)))
    assert result.returncode != 0 and 'query_phase' in result.stderr,result.stderr
    print('负面对照通过：重新引入显示查询锁会触发持锁查询断言')

    # 在完整描述之前发布可用标记，必须在发布边界被确定性拒绝。
    publication = 'atomic_store_explicit(&g_display.scanout_backend_registered, true, memory_order_release);'
    assert publication in program
    broken = program.replace(publication, '').replace(copy_statement, publication+'\n'+copy_statement)
    subprocess.run(command, input=broken, text=True, check=True)
    result = subprocess.run([binary], text=True, capture_output=True, timeout=20,
        preexec_fn=lambda: __import__('resource').setrlimit(__import__('resource').RLIMIT_CORE,(0,0)))
    assert result.returncode != 0 and 'display_core_has_native_scanout' in result.stderr,result.stderr
    print('负面对照通过：后端描述写完整之前发布可用标记被拒绝')
