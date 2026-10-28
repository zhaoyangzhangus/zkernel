"""MAC enable/readback and real TX submission with a fake MMIO controller."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/rtl8126.c').read_text()
model = (root / 'kernel/drivers/net/core_internal.h').read_text()
defines = '\n'.join(re.findall(r'^#define RTL8126_\w+[^\n\\]*$', source + '\n' + model, re.M))
helper = source[source.index('static bool rtl8126_ensure_mac_enabled('):
                source.index('static bool rtl8126_start_controller(')]
tx = source[source.index('static uint16_t rtl8126_reclaim_tx_locked('):
            source.index('bool rtl8126_link_up_state(')]
core = (root / 'kernel/drivers/net/core.c').read_text()
dispatch = core[core.index('kstatus_t e1000_transmit_status('):
                core.index('bool e1000_transmit(')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <kernel/base.h>
typedef struct {uint32_t options1,options2;uint64_t address;} rtl8126_descriptor_t;
typedef struct {uint8_t status;} intel_descriptor_t;
typedef struct {
    bool initialized;
    rtl8126_descriptor_t *rtl_tx_ring;
    uint8_t *rtl_tx_buffers;
    uint16_t rtl_tx_next,rtl_tx_clean,rtl_tx_pending;
    uint8_t rtl_cmd_last_before_enable,rtl_cmd_last_after_enable;
    uint32_t rtl_enable_attempts,rtl_enable_failures,rtl_tx_count;
    unsigned rtl_tx_ring_dma,rtl_tx_buffer_dma;
    unsigned backend,tx_lock,tx_ring_dma,tx_tail;
    intel_descriptor_t *tx_ring;
} e1000_state_t;
static unsigned depth,locked;
static void sched_preempt_disable(void){assert(!depth);depth=1;}
static void sched_preempt_enable(void){assert(depth && !locked);depth=0;}
static void spinlock_lock(unsigned *l){(void)l;assert(depth && !locked);locked=1;}
static void spinlock_unlock(unsigned *l){(void)l;assert(locked);locked=0;}
static bool e1000_transmit_locked(e1000_state_t *s,const uint8_t *p,size_t n){
    (void)s;(void)p;(void)n;assert(!"unexpected Intel dispatch");return false;
}
#define E1000_BUFFER_SIZE 2048U
#define E1000_TX_STATUS_DD 1U
#define NET_BACKEND_E1000 1U
#define NET_BACKEND_RTL8126 2U
static uint8_t command;
static unsigned writes, delays, doorbells, syncs, mode, reads;
static bool tracing;
static unsigned phase;
static e1000_state_t *probe_state;
static const uint8_t *probe_input;
static size_t probe_length;
static uint8_t rtl8126_read8(const e1000_state_t *s,unsigned r) {
    (void)s;assert(r==0x37);++reads;return command;
}
static void rtl8126_write8(const e1000_state_t *s,unsigned r,uint8_t value) {
    (void)s;assert(r==0x37);++writes;
    if(mode==0 || (mode==2 && writes==2))command=value;
}
static void rtl8126_write16(const e1000_state_t *s,unsigned r,uint16_t value) {
    (void)s;assert(r==0x90 && value==1);++doorbells;
    if(tracing){assert(phase==5);++phase;}
}
static void rtl8126_delay_ms(uint16_t value) {assert(value==1);++delays;}
static void dma_sync_for_cpu(unsigned *dma) {(void)dma;++syncs;if(tracing){assert(!phase);++phase;}}
static void dma_sync_for_device(unsigned *dma) {
    (void)dma;++syncs;
    if(tracing){
        assert(phase==1 || phase==3);
        if(phase==1){
            uint8_t *out=probe_state->rtl_tx_buffers+probe_state->rtl_tx_next*2048U;
            assert(!memcmp(out,probe_input,probe_length));
            for(size_t i=probe_length;i<60U;++i)assert(out[i]==0);
            assert(!(probe_state->rtl_tx_ring[probe_state->rtl_tx_next].options1&(1U<<31)));
        }
        ++phase;
    }
}
static void dma_wmb(void) {if(tracing){assert(phase==2 || phase==4);++phase;}}
void *net_kernel_memcpy(void *,const void *,size_t);
void *net_kernel_memset(void *,int,size_t);
#define memcpy net_kernel_memcpy
#define memset net_kernel_memset
''' + defines + '\n' + helper + tx + dispatch + r'''
int main(void) {
    e1000_state_t s={0};
    assert(!rtl8126_ensure_mac_enabled(0));
    for(unsigned c=0;c<256;++c) {
        command=c;writes=delays=0;mode=0;s=(e1000_state_t){0};
        bool valid=c!=255 && !(c&16);
        assert(rtl8126_ensure_mac_enabled(&s)==valid);
        if(!valid)assert(!writes && !delays);
        else {
            assert((command&0x8c)==12);
            assert((command&~0x8c)==(c&~0x8c));
            assert(writes==(((c&0x8c)==12)?0:1));
        }
    }
    command=0;writes=delays=0;mode=2;s=(e1000_state_t){0};
    assert(rtl8126_ensure_mac_enabled(&s) && writes==2 && delays==1);
    assert(s.rtl_enable_attempts==1 && !s.rtl_enable_failures);
    static rtl8126_descriptor_t ring[RTL8126_RING_COUNT];
    static uint8_t buffers[RTL8126_RING_COUNT*RTL8126_TX_BUFFER_SIZE];
    const uint8_t packet[60]={0x12,0x34};
    s=(e1000_state_t){.initialized=true,.backend=NET_BACKEND_RTL8126,
        .rtl_tx_ring=ring,.rtl_tx_buffers=buffers};
    assert(rtl8126_transmit_status(0,packet,60)==K_EINVAL);
    assert(rtl8126_transmit_status(&s,0,60)==K_EINVAL);
    s.initialized=false;assert(rtl8126_transmit_status(&s,packet,60)==K_EIO);
    s.initialized=true;s.rtl_tx_ring=0;
    assert(rtl8126_transmit_status(&s,packet,60)==K_EIO);
    s.rtl_tx_ring=ring;s.rtl_tx_buffers=0;
    assert(rtl8126_transmit_status(&s,packet,60)==K_EIO);
    s.rtl_tx_buffers=buffers;assert(!syncs && !doorbells);
    command=0;writes=delays=0;mode=1;
    assert(e1000_transmit_status(&s,packet,sizeof(packet))==K_EIO);
    assert(writes==2 && delays==1 && syncs==1 && !doorbells);
    assert(!s.rtl_tx_next && !s.rtl_tx_count && !ring[0].options1);
    assert(s.rtl_enable_failures==1);
    mode=0;writes=0;
    assert(e1000_transmit_status(&s,packet,sizeof(packet))==K_OK);
    assert(writes==1 && doorbells==1 && s.rtl_tx_next==1 && s.rtl_tx_count==1);
    assert(ring[0].options1==(0xb0000000|sizeof(packet)));
    assert(!memcmp(buffers,packet,sizeof(packet)));
    ring[1].options1=1U<<31;
    command=255;unsigned before_reads=reads;
    assert(e1000_transmit_status(&s,packet,sizeof(packet))==K_EAGAIN && doorbells==1);
    assert(reads==before_reads && !depth && !locked);
    uint8_t source[RTL8126_TX_BUFFER_SIZE+17];
    for(unsigned i=0;i<sizeof(source);++i)source[i]=(uint8_t)(i*17+3);
    const unsigned lengths[]={0,1,6,42,59,60,61,63,64,65,127,1023,1440,2047,2048,2049};
    unsigned cases=0;tracing=true;
    for(unsigned slot=0;slot<RTL8126_RING_COUNT;++slot)
    for(unsigned offset=0;offset<16;++offset)
    for(unsigned l=0;l<sizeof(lengths)/sizeof(lengths[0]);++l){
        unsigned n=lengths[l];
        memset(ring,0,sizeof(ring));memset(buffers,0x75,sizeof(buffers));
        s.rtl_tx_next=s.rtl_tx_clean=slot;s.rtl_tx_pending=0;
        s.rtl_tx_count=0;phase=syncs=doorbells=0;
        command=12;
        probe_state=&s;probe_input=source+offset;probe_length=n;
        bool valid=n && n<=RTL8126_TX_BUFFER_SIZE;
        assert(e1000_transmit_status(&s,source+offset,n)==(valid?K_OK:K_EINVAL));
        assert(!depth && !locked);
        assert(phase==(valid?6U:0U) && doorbells==(valid?1U:0U));
        assert(syncs==(valid?3U:0U));
        assert(s.rtl_tx_next==(valid?(slot+1)%RTL8126_RING_COUNT:slot));
        assert(s.rtl_tx_count==(valid?1U:0U));
        assert(s.rtl_tx_pending==(valid?1U:0U) && s.rtl_tx_clean==slot);
        uint8_t *out=buffers+slot*RTL8126_TX_BUFFER_SIZE;
        unsigned wire_n=valid && n<60U?60U:n;
        if(valid){
            assert(!memcmp(out,source+offset,n));
            for(unsigned i=n;i<wire_n;++i)assert(out[i]==0);
            assert(ring[slot].options1==(0xb0000000U|wire_n|
                (slot+1==RTL8126_RING_COUNT?RTL8126_DESC_EOR:0U)));
        }
        for(unsigned i=valid?wire_n:0;i<RTL8126_TX_BUFFER_SIZE;++i)assert(out[i]==0x75);
        if(slot)assert(out[-1]==0x75);
        if(slot+1<RTL8126_RING_COUNT)assert(out[RTL8126_TX_BUFFER_SIZE]==0x75);
        ++cases;
    }
    printf("RTL8126 TX: %u ring-slot/alignment/length cases, zero padding, canaries, wrap/EOR and DMA publication order PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='rtl-mac-enable-') as directory:
    binary = str(Path(directory) / 'test')
    bulk = str(Path(directory) / 'string.o')
    subprocess.run(['cc', '-std=c11', '-O2', '-fno-builtin', '-mno-sse', '-mno-mmx',
                    '-fsanitize=address,undefined', '-fno-pie',
                    '-I'+str(root/'include'),
                    '-Dmemcpy=net_kernel_memcpy', '-Dmemset=net_kernel_memset',
                    '-Dmemmove=net_kernel_memmove', '-Dmemcmp=net_kernel_memcmp',
                    '-c', str(root/'kernel/string.c'), '-o', bulk], check=True)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-I'+str(root/'include'),
                    '-x', 'c', '-', '-x', 'none', bulk, '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
    for old,new in [('0, wire_length - length);','0, 0);'),
                    ('options = (uint32_t)wire_length |','options = (uint32_t)length |'),
                    ('status = rtl8126_transmit_status(state, frame, length);',
                     'dma_sync_for_cpu(&state->rtl_tx_ring_dma); status = rtl8126_transmit_status(state, frame, length);')]:
        assert old in code
        mutant=code.replace(old,new)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-I'+str(root/'include'),
                        '-x', 'c', '-', '-x', 'none', bulk, '-o', binary],
                       input=mutant,text=True,check=True)
        assert subprocess.run([binary],capture_output=True).returncode != 0
        print('Negative control rejected: '+old)
print('RTL8126 MAC: 256 command states, bounded settling retry, no DMA/false success on failure, real TX enqueue PASS')
