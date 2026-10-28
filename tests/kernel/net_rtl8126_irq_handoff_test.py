"""Actual RTL ISR/rearm and common completion, with no timer recovery."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
rtl = (root/'kernel/drivers/net/rtl8126.c').read_text()
core = (root/'kernel/drivers/net/core.c').read_text()
irq = rtl[rtl.index('static void rtl8126_irq_handler('):
          rtl.index('bool rtl8126_bind_interrupt_state(')]
rearm = rtl[rtl.index('void rtl8126_rearm_interrupts('):
            rtl.index('bool rtl8126_poll_receive_state(')]
worker = core[core.index('void e1000_deferred_poll(void *argument) {'):
              core.index('uint32_t e1000_last_error(')]
poll = rtl[rtl.index('bool rtl8126_poll_receive_state('):
           rtl.index('bool rtl8126_interrupt_ready(')]
assert 'rtl8126_rearm_interrupts(state)' not in poll
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#define NET_BACKEND_E1000 1
#define NET_BACKEND_RTL8126 2
#define E1000_RX_STATUS_DD 1U
#define RTL8126_REG_INT_MASK 0x38U
#define RTL8126_REG_INT_STATUS 0x3cU
#define RTL8126_INT_MASK_VALUE 0x2fU
struct arch_trap_frame;
typedef struct {unsigned status,options1;} descriptor;
typedef struct {
 bool initialized,rtl_irq_bound;
 unsigned backend,rx_clean,rtl_rx_next,rx_ring_dma,rtl_rx_ring_dma;
 unsigned rtl_irq_count,rtl_irq_status_count;
 descriptor *rx_ring,*rtl_rx_ring;
} e1000_state_t;
static e1000_state_t g_e1000;
static atomic_bool g_e1000_poll_queued,g_e1000_poll_retry;
static bool g_e1000_runtime_ready=true;
static unsigned cause,mask,inject_cause,unmask_cause,sync_cause;
static unsigned queued,coalesced,acks,polls,rearms;
static bool locked,leave_rx;
static void rtl8126_irq_handler(uint8_t,struct arch_trap_frame*,void*);
static void deliver(void){
 if(cause & mask)rtl8126_irq_handler(0,0,&g_e1000);
}
static uint32_t rtl8126_read32(e1000_state_t *s,unsigned reg){
 assert(s==&g_e1000);
 if(reg==RTL8126_REG_INT_STATUS)return cause;
 assert(reg==RTL8126_REG_INT_MASK);return mask;
}
static void rtl8126_write32(e1000_state_t *s,unsigned reg,uint32_t value){
 assert(s==&g_e1000);
 if(reg==RTL8126_REG_INT_STATUS){cause &= ~value;++acks;return;}
 assert(reg==RTL8126_REG_INT_MASK);mask=value;
 if(value){
  ++rearms;cause|=unmask_cause;unmask_cause=0;
  /* An MSI may arrive immediately on another CPU, before this write
   * returns. It must see idle, otherwise TX/link-only work is lost. */
  deliver();
 }
}
static bool e1000_schedule_deferred_poll(void){
 bool expected=false;
 if(!atomic_compare_exchange_strong(&g_e1000_poll_queued,&expected,true)){
  ++coalesced;return false;
 }
 ++queued;return true;
}
static void e1000_record_poll_wait(void){}
static bool e1000_lifecycle_try_lock(void){assert(!locked);locked=true;return true;}
static void e1000_lifecycle_unlock(void){assert(locked);locked=false;}
static void dma_sync_for_cpu(unsigned *dma){
 assert(locked && dma==&g_e1000.rtl_rx_ring_dma);
 cause|=sync_cause;sync_cause=0;
}
static bool e1000_poll(void){
 ++polls;mask=0; /* Hardware is masked while the RX batch runs. */
 g_e1000.rtl_rx_ring[g_e1000.rtl_rx_next].options1=leave_rx?0:1U<<31;
 cause|=inject_cause;inject_cause=0;
 return true;
}
''' + irq + rearm + worker + r'''
int main(void){
 descriptor ring[256]={0};
 g_e1000=(e1000_state_t){.initialized=true,.rtl_irq_bound=true,
  .backend=NET_BACKEND_RTL8126,.rtl_rx_ring=ring};
 for(unsigned slot=0;slot<256;++slot){
  g_e1000.rtl_rx_next=slot;
  for(unsigned event=1;event<64;++event){
   if(!(event & RTL8126_INT_MASK_VALUE))continue;
   for(unsigned phase=0;phase<3;++phase){
    cause=0;mask=0;queued=coalesced=acks=polls=rearms=0;
    atomic_store(&g_e1000_poll_queued,true);
    atomic_store(&g_e1000_poll_retry,false);
    if(phase==0)inject_cause=event;
    if(phase==1)sync_cause=event;
    if(phase==2)unmask_cause=event;
    e1000_deferred_poll(0);
    assert(queued==1 && !coalesced && acks==1 && !cause && !mask);
    assert(atomic_load(&g_e1000_poll_queued));
    assert(!atomic_load(&g_e1000_poll_retry) && polls==1 && rearms==1);
    e1000_deferred_poll(0); /* Run only the actual event's successor. */
    assert(polls==2 && queued==1 && !coalesced && rearms==2);
    assert(!atomic_load(&g_e1000_poll_queued) && !cause);
    assert(mask==RTL8126_INT_MASK_VALUE);
   }
  }
  /* An exhausted receive budget keeps MSI masked and schedules RX now. */
  cause=0;queued=coalesced=rearms=0;leave_rx=true;
  atomic_store(&g_e1000_poll_queued,true);
  e1000_deferred_poll(0);
  assert(queued==1 && !rearms && !mask);
  leave_rx=false;e1000_deferred_poll(0);
  assert(rearms==1 && mask==RTL8126_INT_MASK_VALUE);
 }
 puts("RTL MSI handoff: 256 slots, all RX/TX/link cause combinations at three completion edges, budget continuation, no timer recovery PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='rtl-msi-handoff-') as temp:
    binary=str(Path(temp)/'test')
    def compile_run(program, capture=False):
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-x','c','-','-o',binary],input=program,text=True,check=True)
        return subprocess.run([binary],capture_output=capture,timeout=30)
    compile_run(code).check_returncode()
    old='    rtl8126_write32(state, RTL8126_REG_INT_MASK, RTL8126_INT_MASK_VALUE);'
    assert old in code
    broken=code.replace(old,
        '    if (status) rtl8126_write32(state, RTL8126_REG_INT_STATUS, status);\n'+old)
    assert compile_run(broken,True).returncode != 0
    print('Negative control rejected: completion ACK discards fresh events')
    old='        if (!pending) rtl8126_rearm_interrupts(&g_e1000);'
    assert old in code
    broken=code.replace(old,
        '        atomic_store(&g_e1000_poll_queued, true);\n'+old+
        '\n        atomic_store(&g_e1000_poll_queued, false);')
    assert compile_run(broken,True).returncode != 0
    print('Negative control rejected: unmask before publishing idle loses MSI')
