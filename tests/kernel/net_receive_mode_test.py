"""Real receive-mode queries and lifecycle lock: busy must not imply polling."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/drivers/net/core.c').read_text()
locks=source[source.index('static bool e1000_lifecycle_try_lock(void)'):
             source.index('void e1000_configure_link_local(')]
helper=source[source.index('static bool e1000_receive_mode_matches('):
              source.index('bool e1000_poll(void)')]
queries=source[source.index('bool e1000_interrupt_ready(void)'):
               source.index('static void e1000_record_poll_wait(')]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
enum {NET_BACKEND_NONE,NET_BACKEND_E1000,NET_BACKEND_RTL8126};
static struct {
 bool initialized,rtl_irq_bound;
 unsigned backend;
 struct {bool irq_bound;} recovery;
} g_e1000;
static bool g_e1000_runtime_ready;
static atomic_uint g_e1000_lifecycle_lock;
static unsigned preempt;
static void sched_preempt_disable(void){++preempt;}
static void sched_preempt_enable(void){assert(preempt);--preempt;}
''' + locks + helper + queries + r'''
int main(void){
 unsigned cases=0;
 for(unsigned backend=0;backend<4;++backend)
 for(unsigned init=0;init<2;++init)
 for(unsigned ready=0;ready<2;++ready)
 for(unsigned rtl_irq=0;rtl_irq<2;++rtl_irq)
 for(unsigned intel_irq=0;intel_irq<2;++intel_irq){
  g_e1000.backend=backend;g_e1000.initialized=init;
  g_e1000_runtime_ready=ready;g_e1000.rtl_irq_bound=rtl_irq;
  g_e1000.recovery.irq_bound=intel_irq;
  bool active=init && ready && (backend==1 || backend==2);
  bool bound=backend==2?rtl_irq:intel_irq;
  assert(e1000_interrupt_ready()==(active && bound));
  assert(e1000_polling_required()==(active && !bound));
  assert(!preempt && !atomic_load(&g_e1000_lifecycle_lock));
  assert(e1000_lifecycle_try_lock() && preempt==1);
  for(unsigned i=0;i<1000;++i){
   assert(!e1000_interrupt_ready() && !e1000_polling_required());
   assert(preempt==1 && atomic_load(&g_e1000_lifecycle_lock)==1);
  }
  e1000_lifecycle_unlock();assert(!preempt);++cases;
 }
 printf("Receive mode: %u backend/runtime/IRQ combinations, contended queries preserve ownership and never request polling PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='net-receive-mode-') as temp:
    binary=str(Path(temp)/'test')
    def run(program,capture=False):
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-x','c','-','-o',binary],input=program,text=True,check=True)
        return subprocess.run([binary],capture_output=capture,timeout=30)
    run(code).check_returncode()
    broken=code.replace('return e1000_receive_mode_matches(false);',
                        'return !e1000_interrupt_ready();')
    assert broken != code and run(broken,True).returncode != 0
    print('Negative control rejected: busy IRQ query inverted into polling')
