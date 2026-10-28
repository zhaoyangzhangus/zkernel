"""Production RTL TX producer/completion: progress-driven doorbell, no timer."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/net/rtl8126.c').read_text()
model = (root/'kernel/drivers/net/core_internal.h').read_text()
defines = '\n'.join(re.findall(r'^#define RTL8126_\w+[^\n\\]*$', source+'\n'+model, re.M))
producer = source[source.index('static uint16_t rtl8126_reclaim_tx_locked('):
                  source.index('bool rtl8126_link_up_state(')]
completion = source[source.index('static void rtl8126_complete_tx('):
                    source.index('void rtl8126_rearm_interrupts(')]
assert 'rtl8126_complete_tx(state);' in source[source.index('bool rtl8126_poll_receive_state('):]
assert 'rtl_tx_rekick_tsc' not in source+model
assert 'x86_read_tsc' not in completion and 'timeout' not in completion
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <kernel/base.h>
typedef struct {uint32_t options1,options2;uint64_t address;} rtl8126_descriptor_t;
typedef struct {
 bool initialized; rtl8126_descriptor_t *rtl_tx_ring; uint8_t *rtl_tx_buffers;
 unsigned tx_lock,rtl_tx_ring_dma,rtl_tx_buffer_dma,rtl_tx_count;
 uint16_t rtl_tx_next,rtl_tx_clean,rtl_tx_pending;
} e1000_state_t;
static unsigned preempt,locked,bells,syncs;
static void sched_preempt_disable(void){assert(!preempt);preempt=1;}
static void sched_preempt_enable(void){assert(preempt && !locked);preempt=0;}
static void spinlock_lock(unsigned *p){(void)p;assert(preempt && !locked);locked=1;}
static void spinlock_unlock(unsigned *p){(void)p;assert(locked);locked=0;}
static void dma_sync_for_cpu(unsigned *p){(void)p;assert(locked);++syncs;}
static void dma_sync_for_device(unsigned *p){(void)p;assert(locked);}
static void dma_wmb(void){assert(locked && syncs);}
static bool rtl8126_ensure_mac_enabled(e1000_state_t *s){assert(s && locked);return true;}
static void rtl8126_write16(e1000_state_t *s,unsigned reg,unsigned value){
 assert(s && locked && reg==0x90 && value==1);++bells;
}
''' + defines+'\n'+producer+completion+r'''
static kstatus_t send(e1000_state_t *s){
 const uint8_t packet[60]={0x12,0x34};
 sched_preempt_disable();spinlock_lock(&s->tx_lock);
 kstatus_t status=rtl8126_transmit_status(s,packet,sizeof(packet));
 spinlock_unlock(&s->tx_lock);sched_preempt_enable();return status;
}
int main(void){
 static rtl8126_descriptor_t ring[RTL8126_RING_COUNT],saved[RTL8126_RING_COUNT];
 static uint8_t buffers[RTL8126_RING_COUNT*RTL8126_TX_BUFFER_SIZE];
 e1000_state_t s={.initialized=true,.rtl_tx_ring=ring,.rtl_tx_buffers=buffers};
 rtl8126_complete_tx(NULL);s.initialized=false;rtl8126_complete_tx(&s);
 s.initialized=true;s.rtl_tx_ring=NULL;rtl8126_complete_tx(&s);
 assert(!bells && !syncs && !preempt && !locked);s.rtl_tx_ring=ring;
 const unsigned counts[]={0,1,2,31,32,255,256};unsigned cases=0;
 for(unsigned origin=0;origin<RTL8126_RING_COUNT;++origin)
 for(unsigned c=0;c<sizeof(counts)/sizeof(counts[0]);++c)
 for(unsigned done=0;done<=counts[c];++done){
  unsigned count=counts[c];
  /* Unsubmitted slots carry stale OWN/data; neither is TX progress. */
  memset(ring,0xa5,sizeof(ring));
  s.rtl_tx_clean=origin;s.rtl_tx_pending=count;
  s.rtl_tx_next=(origin+count)%RTL8126_RING_COUNT;
  for(unsigned n=0;n<count;++n)
   ring[(origin+n)%RTL8126_RING_COUNT].options1=n<done?60U:RTL8126_DESC_OWN|60U;
  memcpy(saved,ring,sizeof(ring));bells=syncs=0;
  rtl8126_complete_tx(&s);
  assert(s.rtl_tx_pending==count-done);
  assert(s.rtl_tx_clean==(origin+done)%RTL8126_RING_COUNT);
  assert(s.rtl_tx_next==(origin+count)%RTL8126_RING_COUNT);
  assert(bells==(done && done<count?1U:0U));
  assert(syncs==(count?1U:0U));
  assert(!memcmp(saved,ring,sizeof(ring)) && !preempt && !locked);
  unsigned before=bells;
  for(unsigned repeat=0;repeat<3;++repeat)rtl8126_complete_tx(&s);
  assert(bells==before);++cases;
 }
 /* A clear OWN after an owned head is not contiguous completion. */
 memset(ring,0,sizeof(ring));s.rtl_tx_clean=255;s.rtl_tx_next=2;s.rtl_tx_pending=3;
 ring[255].options1=RTL8126_DESC_OWN;ring[1].options1=RTL8126_DESC_OWN;
 bells=0;rtl8126_complete_tx(&s);assert(!bells && s.rtl_tx_pending==3 && s.rtl_tx_clean==255);
 ring[255].options1=0;rtl8126_complete_tx(&s);
 assert(bells==1 && s.rtl_tx_pending==1 && s.rtl_tx_clean==1);
 ring[1].options1=0;rtl8126_complete_tx(&s);assert(bells==1 && !s.rtl_tx_pending);
 /* A producer wins the race with the deferred completion worker. It must
  * notify the hardware about the completion it consumes, not silently
  * erase the worker's evidence that an older packet still needs a kick. */
 memset(ring,0,sizeof(ring));s.rtl_tx_clean=0;s.rtl_tx_next=2;s.rtl_tx_pending=2;
 ring[0].options1=60U;ring[1].options1=RTL8126_DESC_OWN|60U;
 bells=syncs=0;
 assert(send(&s)==K_OK);
 assert(s.rtl_tx_clean==1 && s.rtl_tx_pending==2 && s.rtl_tx_next==3);
 assert(bells==2); /* One completion notification and one new submission. */
 rtl8126_complete_tx(&s);assert(bells==2 && s.rtl_tx_pending==2);
 /* Real producer fills the ring, applies pressure without overwriting it,
  * reclaims before producer reuse, and survives repeated full wraps. */
 for(unsigned origin=0;origin<RTL8126_RING_COUNT;++origin){
  memset(ring,0,sizeof(ring));s.rtl_tx_clean=s.rtl_tx_next=origin;s.rtl_tx_pending=0;
  s.rtl_tx_count=0;bells=0;
  for(unsigned lap=0;lap<4;++lap){
   for(unsigned n=0;n<RTL8126_RING_COUNT;++n)assert(send(&s)==K_OK);
   assert(s.rtl_tx_pending==RTL8126_RING_COUNT && s.rtl_tx_next==origin);
   memcpy(saved,ring,sizeof(ring));unsigned before=bells;
   assert(send(&s)==K_EAGAIN && !memcmp(saved,ring,sizeof(ring)) && bells==before);
   for(unsigned n=0;n<RTL8126_RING_COUNT;++n)ring[n].options1 &= ~RTL8126_DESC_OWN;
   /* No completion callback: the next producer must retire the old full
    * ring before reusing a descriptor whose OWN hardware cleared. */
  }
  rtl8126_complete_tx(&s);
  assert(!s.rtl_tx_pending && s.rtl_tx_clean==origin);
  assert(s.rtl_tx_count==4*RTL8126_RING_COUNT && bells==4*RTL8126_RING_COUNT);
 }
 printf("RTL8126 TX completion: %u slot/count/progress cases, full-ring pressure, producer reuse, wrap, ordered reclaim, no idle/repeated doorbells or timer recovery PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='rtl-tx-complete-') as directory:
    binary = str(Path(directory)/'test')
    variants = [('production', code),
                ('missing completion doorbell', code.replace(
                    'rtl8126_write16(state, RTL8126_REG_TX_POLL, 1U);\n    }',
                    '(void)rtl8126_write16;\n    }')),
                ('repeated doorbell without progress', code.replace(
                    'completed != 0U && state->rtl_tx_pending != 0U',
                    'state->rtl_tx_pending != 0U')),
                ('producer steals completion notification', code.replace(
                    'if (completed != 0U && state->rtl_tx_pending != 0U)',
                    'if (completed != 0U && state->rtl_tx_pending != 0U && !suppress_kick)')
                    .replace('static unsigned preempt,locked,bells,syncs;',
                             'static unsigned preempt,locked,bells,syncs; static bool suppress_kick;')
                    .replace('(void)rtl8126_reclaim_tx_locked(state);',
                             'suppress_kick=true; (void)rtl8126_reclaim_tx_locked(state); suppress_kick=false;',1)),
                ('producer reuses slots without retiring old submission', code.replace(
                    '(void)rtl8126_reclaim_tx_locked(state);',
                    'dma_sync_for_cpu(&state->rtl_tx_ring_dma);', 1))]
    for label, program in variants:
        if label != 'production': assert program != code
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-I'+str(root/'include'),'-fsanitize=address,undefined',
                        '-fno-pie','-no-pie','-x','c','-','-o',binary],
                       input=program,text=True,check=True)
        result = subprocess.run([binary],capture_output=True,text=True,timeout=30)
        if label == 'production':
            assert result.returncode == 0, result.stderr
            print(result.stdout,end='')
        else:
            assert result.returncode != 0
            print('Negative control rejected: '+label)
