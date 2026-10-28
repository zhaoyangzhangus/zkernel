"""Execute production TD construction for scatter/gather and ring wrap."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/usb/xhci/msc.c').read_text()
body = source[source.index('static bool xhci_msc_ring_valid('):
              source.index('static bool xhci_msc_direct_map(')]
defines = '\n'.join(re.findall(r'^#define XHCI_\w+[^\n\\]*$', source, re.M))
defines += '\n#define XHCI_RING_TOTAL_TRB_COUNT 512U\n#define XHCI_MSC_MAX_TRANSFER (1024U*1024U)\n'
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define PAGE_SIZE 4096U
typedef struct {uint64_t value;} iova_t;
typedef struct {iova_t addr;uint64_t length;} dma_segment_t;
typedef struct {dma_segment_t *segments;unsigned segment_count;} dma_mapping_t;
typedef struct {void *cpu;unsigned page_count;dma_mapping_t mapping;} xhci_dma_region_t;
typedef struct {uint64_t parameter;uint32_t status,control;} xhci_trb_t;
static xhci_trb_t trbs[512],snapshot[512];
static unsigned first_index,first_cycle,barriers;
static void dma_wmb(void) {
    /* First TRB must remain unavailable until all descriptors are ready. */
    assert((trbs[first_index].control&1)!=first_cycle);++barriers;
}
static uint64_t xhci_dma_address(const dma_mapping_t *m) {return m->segments[0].addr.value;}
static void dma_sync_for_device(dma_mapping_t *m) {(void)m;}
''' + defines + body + r'''
static void check(dma_mapping_t *data,unsigned offset,unsigned length,unsigned start) {
    dma_segment_t ring_dma={.addr.value=0x200000,.length=8192};
    xhci_dma_region_t ring={trbs,2,{&ring_dma,1}};
    assert(xhci_msc_init_ring(&ring));
    unsigned enqueue=start;uint8_t cycle=1;uint64_t last=0;
    first_index=start;first_cycle=1;
    unsigned old_barriers=barriers;
    assert(xhci_msc_queue_data(&ring,&enqueue,&cycle,data,offset,length,512,true,&last));
    assert(barriers==old_barriers+1);
    unsigned cursor=start,expected_cycle=1,total=0,segment=0;
    uint64_t skip=offset;
    while(skip>=data->segments[segment].length)skip-=data->segments[segment++].length;
    while(total<length) {
        xhci_trb_t t=trbs[cursor];unsigned bytes=t.status&0x1ffff;
        assert(bytes && bytes<=65536 && total+bytes<=length);
        assert(t.parameter==data->segments[segment].addr.value+skip);
        assert((t.parameter&65535)+bytes<=65536);
        assert((t.control&1)==expected_cycle);
        unsigned packets=(length-total-bytes+511)/512;if(packets>31)packets=31;
        assert(((t.status>>17)&31)==packets);
        assert(t.control&4); /* Short packets must be observable. */
        bool more=total+bytes<length;
        assert(!!(t.control&16)==more && !!(t.control&32)==!more);
        if(!more)assert(last==0x200000+cursor*16);
        total+=bytes;skip+=bytes;
        if(skip==data->segments[segment].length){++segment;skip=0;}
        if(cursor%256==254) {
            assert(!!(trbs[cursor+1].control&16)==more);
            if(cursor==510){cursor=0;expected_cycle^=1;}else cursor=256;
        } else ++cursor;
    }
    assert(cursor==enqueue && expected_cycle==cycle);
    memcpy(snapshot,trbs,sizeof(trbs));unsigned old=enqueue;
    assert(!xhci_msc_queue_data(&ring,&enqueue,&cycle,data,0,1048577,512,true,&last));
    assert(enqueue==old && !memcmp(snapshot,trbs,sizeof(trbs)));
}
int main(void) {
    dma_segment_t contiguous={.addr.value=0x301000,.length=1048576+4096};
    dma_mapping_t data={&contiguous,1};
    unsigned lengths[]={1,31,512,4096,65535,65536,65537,131072,1048576};
    unsigned starts[]={0,253,254,256,509,510};
    for(unsigned offset=0;offset<4096;offset+=511)
      for(unsigned n=0;n<sizeof(lengths)/sizeof(lengths[0]);++n)
        for(unsigned i=0;i<sizeof(starts)/sizeof(starts[0]);++i)
          check(&data,offset,lengths[n],starts[i]);
    dma_segment_t segments[257];
    for(unsigned i=0;i<257;++i)segments[i]=(dma_segment_t){.addr.value=0x500000+i*0x3000,.length=4096};
    data=(dma_mapping_t){segments,257};
    for(unsigned i=0;i<6;++i)check(&data,123,1048576,starts[i]);
    dma_segment_t small={.addr.value=0x100000,.length=512};data=(dma_mapping_t){&small,1};
    dma_segment_t ring_dma={.addr.value=0x200000,.length=8192};
    xhci_dma_region_t ring={trbs,2,{&ring_dma,1}};
    unsigned enqueue=10;uint8_t cycle=1;
    memcpy(snapshot,trbs,sizeof(trbs));
    assert(!xhci_msc_queue_data(&ring,&enqueue,&cycle,&data,0,513,512,false,0));
    assert(enqueue==10 && !memcmp(snapshot,trbs,sizeof(trbs)));
}
'''
with tempfile.TemporaryDirectory(prefix='xhci-dma-chain-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
print('xHCI DMA chain: 492 boundary/size/wrap cases, 257 scattered pages, delayed publication, TD size, single final IOC and preflight failure PASS')
