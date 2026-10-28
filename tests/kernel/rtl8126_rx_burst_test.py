"""Production RTL receive batches drain a full TCP window without ring overrun."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/net/rtl8126.c').read_text()
constants = '\n'.join(line for line in source.replace('\\\n',' ').splitlines()
                      if line.startswith('#define RTL8126_'))
start = source.index('bool rtl8126_poll_receive_state(')
receive = source[start:source.index('bool rtl8126_interrupt_ready(',start)]
program = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "core_internal.h"
''' + constants + r'''
static unsigned pending,processed,dispatches;
static bool mask_pending;
static void rtl8126_write32(e1000_state_t *s,unsigned reg,unsigned value){
 assert(s && reg==RTL8126_REG_INT_MASK && !value);
 mask_pending=true;
}
static uint32_t rtl8126_read32(e1000_state_t *s,unsigned reg){
 assert(s && reg==RTL8126_REG_INT_MASK && mask_pending);
 mask_pending=false;return 0;
}
void dma_sync_for_cpu(dma_mapping_t *m){assert(m && !mask_pending);}
void dma_sync_for_device(dma_mapping_t *m){assert(m);}
void dma_wmb(void){}
bool e1000_process_rx_frame(e1000_state_t *s,const uint8_t *p,uint16_t n){
 assert(p==s->rtl_rx_buffers+(size_t)s->rtl_rx_next*RTL8126_RX_BUFFER_SIZE);
 assert(n==1460 && pending<E1000_SOFTWARE_QUEUE_DEPTH);
 ++pending;++processed;return true;
}
bool e1000_dispatch_software_queues(e1000_state_t *s){
 assert(s && pending<=E1000_RX_BUDGET);++dispatches;pending=0;return true;
}
uint64_t x86_read_tsc(void){return 1;}
void socket_tcp_poll(uint64_t now){assert(now==1 && !pending);}
/* The dedicated TxPoll fixture exercises the production TX helper. This
 * receive-only controller intentionally has no TX ring to recover. */
static void rtl8126_complete_tx(e1000_state_t *s){assert(s && !pending && !s->rtl_tx_ring);}
''' + receive + r'''
int main(void){
 assert(RTL8126_RING_COUNT*sizeof(rtl8126_descriptor_t)<=PAGE_SIZE);
 assert(RTL8126_RING_COUNT*1460U>=SOCKET_STREAM_TCP_CAPACITY_PLACEHOLDER);
 static rtl8126_descriptor_t ring[RTL8126_RING_COUNT+1];
 uint8_t *buffers=malloc((size_t)RTL8126_RING_COUNT*RTL8126_RX_BUFFER_SIZE);assert(buffers);
 e1000_state_t s={.initialized=true,.rtl_irq_bound=true,.rtl_rx_ring=ring,.rtl_rx_buffers=buffers};
 for(unsigned origin=0;origin<RTL8126_RING_COUNT;++origin){
  memset(ring,0,sizeof(ring));s.rtl_rx_next=origin;
  for(unsigned i=0;i<RTL8126_RING_COUNT;++i)
   ring[i].options1=RTL8126_DESC_FS|RTL8126_DESC_LS|1464U;
  unsigned before=processed,passes=0;
  while(processed-before<RTL8126_RING_COUNT){
   unsigned previous=processed;assert(rtl8126_poll_receive_state(&s));++passes;
   assert(processed-previous<=E1000_RX_BUDGET && !pending);
  }
  assert(s.rtl_rx_next==origin && passes==(RTL8126_RING_COUNT+E1000_RX_BUDGET-1)/E1000_RX_BUDGET);
  for(unsigned i=0;i<RTL8126_RING_COUNT;++i)
   assert(ring[i].options1==(RTL8126_DESC_OWN|RTL8126_RX_DESCRIPTOR_LENGTH|
     (i+1==RTL8126_RING_COUNT?RTL8126_DESC_EOR:0U)));
  assert(!ring[RTL8126_RING_COUNT].options1 && !ring[RTL8126_RING_COUNT].address);
 }
 assert(dispatches);free(buffers);
 printf("RTL8126 RX burst: %u start slots, full-ring bursts, 32-packet software budget, wrap/EOR, DMA canary and rearm PASS\n",RTL8126_RING_COUNT);
}
'''
# Keep the window target tied to the production socket model, not a copied size.
model = (root/'kernel/net/socket_model.h').read_text()
capacity = next(line for line in model.splitlines() if line.startswith('#define SOCKET_STREAM_TCP_CAPACITY'))
program = program.replace('SOCKET_STREAM_TCP_CAPACITY_PLACEHOLDER', 'SOCKET_STREAM_TCP_CAPACITY')
program = capacity+'\n'+program
with tempfile.TemporaryDirectory(prefix='rtl-rx-burst-') as directory:
    binary=directory+'/test'
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-I'+str(root/'kernel/drivers/net'),
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    missing_flush = receive.replace('(void)rtl8126_read32(state, RTL8126_REG_INT_MASK);', '')
    assert missing_flush != receive
    # Keep the fixture read helper referenced even in this broken variant.
    broken = program.replace(receive, missing_flush).replace(
        'int main(void){', 'int main(void){(void)rtl8126_read32;')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-I'+str(root/'kernel/drivers/net'),
                    '-x','c','-','-o',binary],input=broken,text=True,check=True)
    result = subprocess.run([binary],capture_output=True,timeout=30)
    assert result.returncode != 0
    print('Negative control rejected: RX ring touched before posted mask is drained')
