"""Production event consumer must only clear RW1C EHB at batch boundaries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/usb/xhci/event_runtime.c').read_text()
body = source[source.index('#define XHCI_RING_TRB_COUNT'):
              source.index('bool xhci_defer_event(')]
msc = (root / 'kernel/drivers/usb/xhci/msc.c').read_text()
end = msc[msc.index('void xhci_usb_bulk_session_end('):
          msc.index('kstatus_t xhci_usb_bulk_transfer_locked(')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef unsigned dma_mapping_t;
typedef struct {uint64_t parameter;uint32_t status,control;} xhci_trb_t;
typedef struct {
    bool initialized;
    unsigned deferred_event_count,event_index,event_cycle,runtime_offset;
    struct {void *cpu;dma_mapping_t mapping;} event_ring;
} xhci_state_t;
static xhci_state_t controller;
static unsigned advances, clears, acks, unlocks, scheduled;
static bool locked=true,ehb=true;
static uint64_t last_dequeue;
static void dma_sync_for_cpu(dma_mapping_t *map) {(void)map;}
static uint64_t xhci_dma_address(const dma_mapping_t *map) {(void)map;return 0x100000;}
static bool xhci_controller_write64(xhci_state_t *s,unsigned offset,uint64_t value) {
    assert(locked && offset==s->runtime_offset+0x38);
    last_dequeue=value&~15ULL;
    if(value&8){++clears;ehb=false;}else {++advances;assert(ehb);}
    return true;
}
static bool xhci_controller_write32(xhci_state_t *s,unsigned offset,uint32_t value) {
    assert(locked && offset==s->runtime_offset+0x20 && value==3);++acks;return true;
}
static xhci_state_t *xhci_controller_state(void) {return &controller;}
static void xhci_event_unlock(xhci_state_t *s) {(void)s;assert(!ehb);locked=false;++unlocks;}
static bool xhci_schedule_deferred_work(void) {assert(!locked);++scheduled;return true;}
''' + body + end + r'''
int main(void) {
    xhci_trb_t ring[256],before[256],out;
    controller=(xhci_state_t){.initialized=true,.event_cycle=1,.runtime_offset=0x2000};
    controller.event_ring.cpu=ring;
    for(unsigned cycle=0;cycle<4;++cycle) {
        for(unsigned i=0;i<256;++i)ring[i]=(xhci_trb_t){i,0x1234,controller.event_cycle};
        memcpy(before,ring,sizeof(ring));locked=true;ehb=true;
        unsigned old_clears=clears;
        for(unsigned i=0;i<256;++i) {
            assert(xhci_next_ring_event(&controller,&out));
            assert(out.parameter==i && out.status==0x1234);
            assert(clears==old_clears && ehb);
            assert(last_dequeue==0x100000+controller.event_index*16);
        }
        assert(!memcmp(ring,before,sizeof(ring)));
        assert(!xhci_next_ring_event(&controller,&out));
        xhci_usb_bulk_session_end(1);
        assert(clears==old_clears+1 && acks==clears && !locked);
    }
    assert(advances==1024 && unlocks==4 && !scheduled);
    controller.deferred_event_count=1;locked=true;ehb=true;
    xhci_usb_bulk_session_end(1);assert(scheduled==1 && clears==5);
    /* Empty/error batch still acknowledges and releases exactly once. */
    controller.deferred_event_count=0;locked=true;ehb=true;
    xhci_usb_bulk_session_end(1);assert(clears==6 && unlocks==6);
}
'''
with tempfile.TemporaryDirectory(prefix='xhci-batch-ack-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
print('xHCI batching: 1024 immutable TRBs, cycle wrap, one EHB/IP ack per session, deferred HID and error completion PASS')
