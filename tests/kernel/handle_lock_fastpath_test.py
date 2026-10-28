"""真实句柄锁：无竞争时不写全局诊断，竞争时保留等待目标和嵌套保护。"""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/object/handle.c').read_text()
locks = source[source.index('static void handle_lock('):source.index('static void chunk_lock(')]
program = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <kernel/spinlock.h>
static atomic_uintptr_t g_handle_waiting_lock;
static atomic_uint writes;
static _Thread_local unsigned depth;
static void sched_preempt_disable(void){++depth;}
static void sched_preempt_enable(void){assert(depth);--depth;}
static void record(atomic_uintptr_t *p,uintptr_t v,memory_order order){
 assert(p==&g_handle_waiting_lock&&depth);atomic_fetch_add(&writes,1);
 atomic_store_explicit(p,v,order);
}
#undef atomic_store_explicit
#define atomic_store_explicit record
''' + locks + r'''
#undef atomic_store_explicit
#define atomic_store_explicit(p,v,o) __atomic_store_n((p),(v),(o))
static spinlock_t lock;
static atomic_bool ready;
static void *waiter(void *arg){
 unsigned nested=(uintptr_t)arg;depth=nested;atomic_store(&ready,true);
 handle_lock(&lock);assert(depth==nested+1);
 assert(!atomic_load(&g_handle_waiting_lock));handle_unlock(&lock);
 assert(depth==nested);return NULL;
}
int main(void){
 spinlock_init(&lock);
 for(unsigned nesting=0;nesting<4;++nesting){
  depth=nesting;unsigned before=writes;
  /* 快路径不得擦除另一等待者留下的诊断。 */
  atomic_store(&g_handle_waiting_lock,0x1234);
  for(unsigned i=0;i<10000;++i){
   handle_lock(&lock);assert(depth==nesting+1);
   assert(atomic_load(&g_handle_waiting_lock)==0x1234);
   handle_unlock(&lock);assert(depth==nesting);
  }
  assert(writes==before);atomic_store(&g_handle_waiting_lock,0);
  for(unsigned round=0;round<32;++round){
   spinlock_lock(&lock);atomic_store(&ready,false);
   pthread_t thread;assert(!pthread_create(&thread,NULL,waiter,(void *)(uintptr_t)nesting));
   while(!atomic_load(&ready)||atomic_load(&g_handle_waiting_lock)!=(uintptr_t)&lock){}
   spinlock_unlock(&lock);assert(!pthread_join(thread,NULL));
   assert(!atomic_load(&g_handle_waiting_lock)&&depth==nesting);
  }
  assert(writes==before+64);
 }
 puts("Handle locks: 40000 uncontended acquisitions -> zero diagnostic writes; 128 contended handoffs retain wait target and nested preemption PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='handle-lock-fastpath-') as directory:
    command = ['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-I' + str(root / 'include'), '-x', 'c', '-', '-o', directory + '/test']
    subprocess.run(command, input=program, text=True, check=True)
    subprocess.run([directory + '/test'], check=True, timeout=30)
    broken = program.replace('if (spinlock_try_lock(lock)) return;', '')
    assert broken != program
    subprocess.run(command, input=broken, text=True, check=True)
    result = subprocess.run([directory + '/test'], capture_output=True, timeout=30,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode == -signal.SIGABRT
    print('Negative control rejected: unconditional global diagnostic writes')
