"""Production link query: preemptible read, pinned lifecycle, deferred retry."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/drivers/net/core.c').read_text()
def function(signature):
    start=source.index(signature);opening=source.index('{',start)
    depth=1;end=opening+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
helpers=source[source.index('static bool e1000_lifecycle_try_lock(void)'):
               source.index('void e1000_configure_link_local(')]
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
enum { NET_BACKEND_E1000=1,NET_BACKEND_RTL8126=2,
       E1000_REG_STATUS=8,E1000_STATUS_LU=2,E1000_RX_STATUS_DD=1 };
typedef struct {unsigned status,options1;} descriptor_t;
typedef struct {
 bool initialized,link_up;
 unsigned backend,rx_clean;
 descriptor_t *rx_ring;
 unsigned rx_ring_dma;
 descriptor_t *rtl_rx_ring;
 unsigned rtl_rx_next,rtl_rx_ring_dma;
 struct {bool link_up;} device;
} e1000_state_t;
static e1000_state_t g_e1000;
static atomic_uint g_e1000_lifecycle_lock;
static atomic_bool g_e1000_poll_queued,g_e1000_poll_retry;
static struct { atomic_uint_fast64_t queued_tsc; } g_e1000_poll_timing;
static uint64_t telemetry_timestamp(void) { return 1000U; }
static void e1000_record_poll_wait(void) {}
static bool g_e1000_runtime_ready=true,inject_contention,desired_link=true;
static unsigned preempt_depth,expected_outer,reads,queue_attempts;
static void sched_preempt_disable(void){++preempt_depth;}
static void sched_preempt_enable(void){assert(preempt_depth);--preempt_depth;}
void e1000_deferred_poll(void *argument);
static bool deferred_try_schedule(void (*fn)(void*),void *arg){
 (void)fn;(void)arg;++queue_attempts;return true;
}
static void dma_sync_for_cpu(unsigned *dma){(void)dma;}
static void rtl8126_rearm_interrupts(e1000_state_t *s){
 assert(s==&g_e1000 && !atomic_load(&g_e1000_poll_queued));
}
''' + helpers + r'''
static bool e1000_poll(void){
 if(!e1000_lifecycle_try_lock())return false;
 e1000_lifecycle_unlock();return true;
}
static uint32_t e1000_read(const e1000_state_t *state,unsigned reg){
 assert(state==&g_e1000&&reg==E1000_REG_STATUS);
 assert(preempt_depth==expected_outer);
 assert(atomic_load(&g_e1000_lifecycle_lock)==1);
 /* Simulate a local worker displacing the read-only owner. The reset/RX
  * contenders cannot release or unmap the owner's device. */
 assert(!e1000_lifecycle_try_lock());
 assert(preempt_depth==expected_outer);
 if(inject_contention){
  atomic_store(&g_e1000_poll_queued,true);
  e1000_deferred_poll(0);
  assert(atomic_load(&g_e1000_poll_retry));
  assert(!atomic_load(&g_e1000_poll_queued));
  assert(!queue_attempts);
 }
 ++reads;return desired_link?E1000_STATUS_LU:0;
}
static bool rtl8126_link_up_state(e1000_state_t *state){
 return (e1000_read(state,E1000_REG_STATUS)&E1000_STATUS_LU)!=0;
}
''' + '\n'.join(function(s) for s in (
    'bool e1000_schedule_deferred_poll(void)',
    'void e1000_retry_deferred_poll(void)',
    'void e1000_deferred_poll(void *argument)',
    'bool e1000_link_up(void)')) + r'''
int main(void){
 assert(!e1000_link_up()&&!preempt_depth&&!reads);
 g_e1000.initialized=true;g_e1000.backend=NET_BACKEND_E1000;
 inject_contention=true;
 assert(e1000_link_up()&&!preempt_depth);
 assert(g_e1000.link_up&&g_e1000.device.link_up&&reads==1);
 /* Acknowledged request is retried after owner release, without a new IRQ. */
 e1000_retry_deferred_poll();assert(queue_attempts==1);
 assert(atomic_load(&g_e1000_poll_queued)&&!atomic_load(&g_e1000_poll_retry));
 inject_contention=false;e1000_deferred_poll(0);
 assert(!atomic_load(&g_e1000_poll_queued));
 expected_outer=1;sched_preempt_disable();
 desired_link=false;assert(!e1000_link_up()&&preempt_depth==1);
 assert(!g_e1000.device.link_up&&!g_e1000.link_up);
 sched_preempt_enable();expected_outer=0;
 g_e1000.backend=NET_BACKEND_RTL8126;desired_link=true;
 assert(e1000_link_up()&&!preempt_depth&&reads==3);
 assert(e1000_lifecycle_try_lock());
 assert(!e1000_link_up()&&preempt_depth==1&&reads==3);
 e1000_lifecycle_unlock();
 /* Existing ring-DD retry still schedules fresh receive work. */
 descriptor_t ring={.status=E1000_RX_STATUS_DD};g_e1000.rx_ring=&ring;
 g_e1000.backend=NET_BACKEND_E1000;e1000_deferred_poll(0);
 assert(queue_attempts==2&&atomic_load(&g_e1000_poll_queued));
 /* RTL ownership is inverted, and only the next descriptor counts. */
 descriptor_t rtl[2]={{.options1=0},{.options1=1U<<31}};
 g_e1000.backend=NET_BACKEND_RTL8126;g_e1000.rtl_rx_ring=rtl;
 g_e1000.rtl_rx_next=1;e1000_deferred_poll(0);
 assert(queue_attempts==2&&!atomic_load(&g_e1000_poll_queued));
 rtl[1].options1=0;e1000_deferred_poll(0);
 assert(queue_attempts==3&&atomic_load(&g_e1000_poll_queued));
 g_e1000.rtl_rx_next=0;e1000_deferred_poll(0);
 assert(queue_attempts==4&&atomic_load(&g_e1000_poll_queued));
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-link-preempt-') as directory:
    path=Path(directory);(path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Link query: preemptible MMIO with lifecycle pinned, nested guards, contention, IRQ-free retry, both backends and ring-DD preserved')
