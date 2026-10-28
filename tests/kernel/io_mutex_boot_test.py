"""验证实际 I/O 锁的阶段策略，以及 idle 路径的 IPI 与抢占状态恢复。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
production = (root / 'kernel/sync/io_mutex.c').read_text()
production = production.replace('__asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");',
                                'flags = interrupt_flags;')
production = production.replace('__asm__ volatile ("sti" : : : "memory");', 'enable_irq();')
production = production.replace('__asm__ volatile ("cli" : : : "memory");',
                                'interrupt_flags &= ~(1ULL << 9);')
production = production.replace('__asm__ volatile ("pause");', 'pause_probe();')
assert '__asm__' not in production
fixture = r'''
#include <kernel/io_mutex.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static thread_t thread,owner;
static thread_t *current;
static unsigned depth,outer,tries,contention,pauses,begins,ends,enabled,sleeps;
static uint64_t interrupt_flags;
static bool tlb_waiting,local_owner;
static kstatus_t result;
thread_t *sched_current_thread(void){return current;}
uint32_t x86_current_cpu_index(void){return 0U;}
void sched_preempt_disable(void){++depth;}
void sched_preempt_enable(void){assert(depth);--depth;}
bool x86_tlb_shootdown_active(void){return tlb_waiting;}
void x86_tlb_wait_begin(void){assert(!tlb_waiting);tlb_waiting=true;++begins;}
void x86_tlb_wait_end(void){assert(tlb_waiting);tlb_waiting=false;++ends;}
static void enable_irq(void){assert(depth==outer+1 && tlb_waiting);interrupt_flags|=1ULL<<9;++enabled;}
static void pause_probe(void){assert(++pauses<=contention);}
void kmutex_init(kmutex_t *mutex,bool pi){memset(mutex,0,sizeof(*mutex));mutex->initialized=true;mutex->pi_enabled=pi;}
kstatus_t kmutex_try_lock_bootstrap(kmutex_t *mutex){
 ++tries;
 if(tries<=contention){owner.owner_cpu=local_owner?0:1;mutex->owner=&owner;return local_owner?K_EBUSY:K_EAGAIN;}
 if(contention)assert(enabled && tlb_waiting && (interrupt_flags&(1ULL<<9)));
 if(result==K_OK)mutex->owner=current;
 return result;
}
kstatus_t kmutex_lock_uninterruptible(kmutex_t *mutex,uint64_t timeout){
 assert(timeout==UINT64_MAX && current && current->sched_class!=SCHED_CLASS_IDLE && !depth);
 ++sleeps;if(result==K_OK){mutex->owner=current;sched_exit_defer_begin(current);mutex->exit_deferred=true;}
 return result;
}
kstatus_t kmutex_unlock(kmutex_t *mutex){
 if(mutex->owner!=current)return K_EPERM;
 if(mutex->exit_deferred){sched_exit_defer_end(current);mutex->exit_deferred=false;}
 mutex->owner=0;return K_OK;
}
''' + production + r'''
int main(void){
 io_mutex_t mutex;memset(&thread,0,sizeof(thread));io_mutex_init(&mutex,true);
 current=0;assert(io_mutex_lock(&mutex)==K_OK && atomic_load(&mutex.boot_owned));
 assert(io_mutex_lock(&mutex)==K_EBUSY && atomic_load(&mutex.boot_owned));
 current=&thread;thread.sched_class=SCHED_CLASS_FAIR;
 assert(io_mutex_lock(&mutex)==K_EBUSY && io_mutex_unlock(&mutex)==K_EPERM);
 assert(atomic_load(&mutex.boot_owned));current=0;
 assert(io_mutex_unlock(&mutex)==K_OK && !atomic_load(&mutex.boot_owned));
 assert(io_mutex_unlock(&mutex)==K_EPERM);
 current=&thread;
 for(unsigned fail=0;fail<2;++fail){
  result=fail?K_EBUSY:K_OK;sleeps=0;
  assert(io_mutex_lock(&mutex)==result && sleeps==1 && !depth);
  if(!fail){assert(atomic_load(&thread.exit_defer_count)==1U);assert(io_mutex_unlock(&mutex)==K_OK);}
  assert(!atomic_load(&thread.exit_defer_count));
 }
 unsigned cases=0;
 for(unsigned irq=0;irq<2;++irq)for(unsigned nesting=0;nesting<3;++nesting)
 for(unsigned prior=0;prior<2;++prior)for(unsigned fail=0;fail<2;++fail)
 for(unsigned busy=0;busy<8;++busy){
  io_mutex_init(&mutex,true);thread.sched_class=SCHED_CLASS_IDLE;
  depth=outer=nesting;tlb_waiting=prior;interrupt_flags=0x46|((uint64_t)irq<<9);
  contention=busy;tries=pauses=begins=ends=enabled=sleeps=0;local_owner=false;
  result=fail?K_EBUSY:K_OK;
  assert(io_mutex_lock(&mutex)==result && !sleeps);
  assert(tries==busy+1 && pauses==busy && enabled==!!busy);
  assert(begins==(!!busy && !prior) && ends==begins);
  assert(depth==outer && tlb_waiting==(bool)prior && interrupt_flags==(0x46|((uint64_t)irq<<9)));
  if(!fail)assert(io_mutex_unlock(&mutex)==K_OK);
  assert(!atomic_load(&thread.exit_defer_count));++cases;
 }
 io_mutex_init(&mutex,true);contention=7;tries=enabled=begins=ends=pauses=0;
 depth=outer=0;local_owner=true;interrupt_flags=0;tlb_waiting=false;
 assert(io_mutex_lock(&mutex)==K_EBUSY && tries==1 && !enabled && !depth && !pauses);
 assert(!tlb_waiting && !interrupt_flags);
 assert(io_mutex_lock(0)==K_EINVAL && io_mutex_unlock(0)==K_EINVAL);
 printf("I/O mutex: boot phase/recursion/ownership, scheduled exit protection, %u idle IRQ/TLB/nesting cases and local-owner refusal passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='io-mutex-boot-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(root/'include'), '-x', 'c', '-', '-o', binary],
                   input=fixture, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
