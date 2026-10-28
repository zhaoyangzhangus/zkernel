"""Actual per-CPU guards and cross-CPU drain under concurrent pool owners."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/phys/percpu_page.c').read_text()
guards = source[source.index('typedef struct __attribute__((aligned(CACHELINE_SIZE)))'):
                source.index('static page_pool_group_t *pool_group_by_id(')]
guards = guards.replace('    spinlock_lock(&g_pool_cpu_guards[cpu].lock);',
    '    assert(!held&&!irq_enabled&&depth==initial_depth+1);\n'
    '    spinlock_lock(&g_pool_cpu_guards[cpu].lock); held=cpu+1;')
guards = guards.replace('    spinlock_unlock(&g_pool_cpu_guards[cpu].lock);',
    '    assert(held==cpu+1&&!irq_enabled&&depth==initial_depth+1); held=0;\n'
    '    spinlock_unlock(&g_pool_cpu_guards[cpu].lock);')
drain = source[source.index('void pool_drain_cpu('):source.index('page_t *pool_alloc_remote(')]
code = '#include "kernel/mm/phys/internal.h"\n' + r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
static _Thread_local bool irq_enabled;
static _Thread_local unsigned depth,initial_depth,held;
static unsigned values[MAX_CPUS];
static uint64_t irq_save(void){uint64_t old=irq_enabled?512:0;irq_enabled=false;return old;}
static void irq_restore(uint64_t old){assert(!irq_enabled&&!held);irq_enabled=(old&512)!=0;}
static void disable(void){++depth;}
static void enable(void){assert(depth==initial_depth+1&&!held);--depth;}
''' + guards + r'''
static void pool_drain_one(uint32_t cpu,uint8_t zone,uint8_t order){
 assert(cpu<MAX_CPUS&&held==cpu+1&&!irq_enabled&&depth==initial_depth+1);
 assert(zone<PAGE_ZONE_COUNT&&order<PAGE_POOL_ORDER_COUNT);
 ++values[cpu];
}
#define page_pool_irq_save irq_save
#define page_pool_irq_restore irq_restore
#define x86_preempt_disable_fast disable
#define x86_preempt_enable_fast enable
''' + drain + r'''
static atomic_bool start;
static void *owner(void *arg){
 unsigned cpu=(unsigned)(uintptr_t)arg;initial_depth=depth=cpu;
 irq_enabled=(cpu%2)==0;
 while(!atomic_load_explicit(&start,memory_order_acquire))sched_yield();
 for(unsigned i=0;i<2000;++i){
  bool before=irq_enabled;uint64_t saved=irq_save();disable();
  pool_cpu_lock(cpu);++values[cpu];sched_yield();pool_cpu_unlock(cpu);
  enable();irq_restore(saved);assert(irq_enabled==before&&depth==initial_depth&&!held);
 }
 return 0;
}
static void *reclaimer(void *unused){
 (void)unused;initial_depth=depth=2;irq_enabled=true;
 atomic_store_explicit(&start,true,memory_order_release);
 for(unsigned pass=0;pass<4;++pass){
  pool_reclaim_all();assert(depth==initial_depth&&irq_enabled&&!held);
 }
 return 0;
}
int main(void){
 _Static_assert(sizeof(page_pool_cpu_guard_t)==CACHELINE_SIZE,"avoid false sharing");
 pthread_t threads[5];
 for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,owner,(void*)(uintptr_t)i));
 assert(!pthread_create(&threads[4],0,reclaimer,0));
 for(unsigned i=0;i<5;++i)assert(!pthread_join(threads[i],0));
 assert(atomic_load(&g_pool_reclaim_passes)==4);
 assert(!atomic_load(&g_pool_remote_borrows));
 for(unsigned i=0;i<MAX_CPUS;++i){
  assert(values[i]==4*PAGE_ZONE_COUNT*PAGE_POOL_ORDER_COUNT+(i<4?2000:0));
  assert(!atomic_load(&g_pool_cpu_guards[i].lock.state));
 }
}
'''
with tempfile.TemporaryDirectory(prefix='pool-guard-concurrency-') as directory:
    path = Path(directory)
    for negative in (False, True):
        candidate = code
        if negative:
            candidate = candidate.replace('    pool_cpu_lock(cpu);\n', '')
        (path / 'test.c').write_text(candidate)
        subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I', str(root), '-I', str(root / 'include'),
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        result = subprocess.run([str(path / 'test')], capture_output=True, timeout=30)
        assert (result.returncode != 0) if negative else (result.returncode == 0), result.stderr
print('Pool guards: 8000 concurrent local mutations plus four all-CPU drain passes, cache-line isolation, one-guard lock order, exact counts, IRQ/preemption restoration and negative control PASS')
