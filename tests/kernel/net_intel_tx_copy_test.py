"""Production Intel TX: bounded bulk copy before DMA publication and doorbell."""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/core.c').read_text()
start = source.index('static bool e1000_transmit_locked(')
transmit = source[start:source.index('kstatus_t e1000_transmit_status(', start)]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "core_internal.h"
#define E1000_BUFFER_SIZE 2048U
#define E1000_TX_STATUS_DD 1U
#define E1000_TX_CMD_EOP 1U
#define E1000_TX_CMD_IFCS 2U
#define E1000_TX_CMD_RS 8U
#define E1000_REG_TDT 0x3818U
#define phys_to_direct(p) ((void *)(uintptr_t)(p))
#define page_to_phys(p) ((uintptr_t)(p))
void *net_kernel_memcpy(void *,const void *,size_t);
static e1000_state_t state;
static unsigned phase,slot;
static size_t bytes;
static const uint8_t *input;
static uint8_t buffers[E1000_RING_COUNT][2048+32];
static void dma_sync_for_device_probe(dma_mapping_t *mapping){
 assert(mapping==&state.tx_dma[slot] && phase==0);
 e1000_descriptor_t *d=&state.tx_ring[slot];
 assert(d->length==bytes && d->command==11 && d->status==0);
 assert(!memcmp(buffers[slot]+16,input,bytes));
 for(unsigned i=0;i<16;++i)assert(buffers[slot][i]==0xa5);
 for(unsigned i=16+bytes;i<sizeof(buffers[slot]);++i)assert(buffers[slot][i]==0xa5);
 phase=1;
}
static void dma_wmb_probe(void){assert(phase==1);phase=2;}
#define dma_sync_for_device dma_sync_for_device_probe
#define dma_wmb dma_wmb_probe
static void e1000_write_probe(const e1000_state_t *s,uint32_t reg,uint32_t value){
 assert(s==&state && reg==E1000_REG_TDT && phase==2);
 assert(value==(slot+1)%E1000_RING_COUNT && s->tx_tail==value);
 phase=3;
}
#define e1000_write e1000_write_probe
#define memcpy net_kernel_memcpy
''' + transmit + r'''
#undef memcpy
int main(void){
 static e1000_descriptor_t ring[E1000_RING_COUNT];
 static uint8_t payload[2048+16];
 const size_t lengths[]={0,1,63,64,65,127,1500,2047,2048,2049};
 for(unsigned i=0;i<sizeof(payload);++i)payload[i]=(uint8_t)(i*37+1);
 state.initialized=true;state.backend=NET_BACKEND_E1000;state.tx_ring=ring;
 unsigned cases=0;
 for(slot=0;slot<E1000_RING_COUNT;++slot)for(unsigned offset=0;offset<16;++offset)
 for(unsigned n=0;n<sizeof(lengths)/sizeof(*lengths);++n){
  bytes=lengths[n];input=payload+offset;phase=0;
  memset(ring,0x5a,sizeof(ring));ring[slot].status=E1000_TX_STATUS_DD;
  memset(buffers,0xa5,sizeof(buffers));state.tx_tail=slot;
  state.tx_pages[slot]=(page_t *)(buffers[slot]+16);
  bool ok=e1000_transmit_locked(&state,input,bytes);
  assert(ok==(bytes>0 && bytes<=2048));
  if(ok)assert(phase==3);
  else{
   assert(!phase && state.tx_tail==slot && ring[slot].status==1);
   for(unsigned i=0;i<sizeof(buffers[slot]);++i)assert(buffers[slot][i]==0xa5);
  }
  for(unsigned i=0;i<E1000_RING_COUNT;++i)if(i!=slot){
   for(unsigned j=0;j<sizeof(ring[i]);++j)assert(((uint8_t *)&ring[i])[j]==0x5a);
   for(unsigned j=0;j<sizeof(buffers[i]);++j)assert(buffers[i][j]==0xa5);
  }
  ++cases;
 }
 slot=0;state.tx_tail=0;bytes=1;input=payload;phase=0;
 assert(!e1000_transmit_locked(0,input,1));
 assert(!e1000_transmit_locked(&state,0,1));
 state.initialized=false;assert(!e1000_transmit_locked(&state,input,1));
 state.initialized=true;ring[0].status=0;
 assert(!e1000_transmit_locked(&state,input,1) && !phase && !state.tx_tail);
 printf("Intel TX: %u ring/alignment/length cases; exact payload, guards, DMA/doorbell order, wrap and busy PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='net-intel-tx-') as directory:
    binary = str(Path(directory) / 'test')
    bulk = str(Path(directory) / 'string.o')
    common = ['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
              '-fno-builtin', '-mno-sse', '-mno-mmx',
              '-fsanitize=address,undefined', '-fno-pie',
              '-I'+str(root/'include'), '-I'+str(root/'kernel/drivers/net')]
    subprocess.run([*common, '-Dmemcpy=net_kernel_memcpy',
                    '-Dmemset=net_kernel_memset', '-Dmemmove=net_kernel_memmove',
                    '-Dmemcmp=net_kernel_memcmp', '-c',
                    str(root/'kernel/string.c'), '-o', bulk], check=True)
    command = [*common, '-no-pie', '-x', 'c', '-', '-x', 'none', bulk, '-o', binary]
    subprocess.run(command, input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
    for name, old, new, diagnostic in (
        ('short copy', 'memcpy(buffer, frame, length);',
         'memcpy(buffer, frame, length-1);', '!memcmp'),
        ('early doorbell', 'dma_sync_for_device(&state->tx_dma[state->tx_tail]);',
         'e1000_write(state, E1000_REG_TDT, state->tx_tail);\n'
         '    dma_sync_for_device(&state->tx_dma[state->tx_tail]);', 'phase==2'),
    ):
        broken = program.replace(old, new)
        assert broken != program, name
        subprocess.run(command, input=broken, text=True, check=True)
        result = subprocess.run([binary], capture_output=True, text=True, timeout=30,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        assert result.returncode == -signal.SIGABRT and diagnostic in result.stderr, (name, result)
        print('Negative control rejected:', name)
