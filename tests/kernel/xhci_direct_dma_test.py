"""Production direct mapping and lifetime cleanup with fake page tables/DMA."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/usb/xhci/msc.c').read_text()
body = source[source.index('static bool xhci_msc_direct_map('):
              source.index('static bool xhci_msc_init_stream_contexts(')]
# This last-resort hardware-failure branch is never executed on the host.
body = body.replace('for (;;) __asm__ volatile ("cli; hlt" : : : "memory");',
                    'assert(!"DMA could not be quiesced");')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#define PAGE_SIZE 4096U
#define XHCI_MSC_MAX_TRANSFER (1024U*1024U)
#define X86_64_DIRECT_MAP_BASE 0xffff800000000000ULL
#define K_OK 0
#define XHCI_STOP_ENDPOINT_TYPE 15
#define DMA_FROM_DEVICE 1
#define DMA_TO_DEVICE 0
typedef struct {uint64_t value;} paddr_t;
typedef struct {unsigned n;} page_t;
typedef struct {unsigned active;} dma_mapping_t;
typedef struct {unsigned device;} pci_t;
typedef unsigned device_t;
typedef struct {pci_t *pci;bool initialized;} xhci_state_t;
typedef struct {dma_mapping_t direct_mapping;void *direct_cpu;uint32_t direct_offset;uint8_t slot;} xhci_msc_transport_t;
static page_t pages[257];
static unsigned translations,maps,unmaps,syncs,stops,clears;
static unsigned mapped_count,mapped_direction;
static int bad_page=-1;
static bool allocation_fail,map_fail,unmap_fail,readonly,stop_fail;
static uint64_t first_virtual;
static void *kmalloc(size_t n,unsigned flags) {(void)flags;return allocation_fail?0:malloc(n);}
static void kfree(void *p) {free(p);}
static paddr_t paddr_make(uint64_t p) {return (paddr_t){p};}
static paddr_t x86_current_root_table(void) {return paddr_make(0x1000);}
static bool dma_mapping_active(dma_mapping_t *m) {return m->active!=0;}
static bool x86_page_entry_writable(uint64_t f) {return f&2;}
static int x86_translate_page(paddr_t r,uint64_t v,paddr_t *p,uint64_t *f) {
    assert(r.value==0x1000 && !(v&4095));
    unsigned i=(unsigned)((v-first_virtual)/4096);++translations;
    if((int)i==bad_page)return -1;
    *p=paddr_make(0x100000+i*4096);*f=readonly?1:3;return K_OK;
}
static page_t *phys_to_page(paddr_t p) {return &pages[(p.value-0x100000)/4096];}
static int dma_map_pages(device_t *d,page_t **p,unsigned n,unsigned dir,dma_mapping_t *out) {
    (void)d;++maps;assert(!out->active);
    for(unsigned i=0;i<n;++i)assert(p[i]==&pages[i]);
    mapped_count=n;mapped_direction=dir;
    if(map_fail)return -1;out->active=1;return K_OK;
}
static void dma_sync_for_cpu(dma_mapping_t *m) {assert(m->active);++syncs;}
static int dma_unmap_checked(dma_mapping_t *m) {
    assert(m->active);++unmaps;if(unmap_fail)return -1;m->active=0;return K_OK;
}
static bool xhci_submit_command_ex(xhci_state_t *s,unsigned cmd,unsigned slot,unsigned ep,unsigned stream,unsigned arg) {
    (void)s;assert(cmd==15 && slot==1 && ep==3 && !stream && !arg);++stops;return !stop_fail;
}
static int pci_read_config32(pci_t *p,unsigned off,uint32_t *out) {
    (void)p;assert(off==4);*out=clears?2:6;return K_OK;
}
static int pci_write_config32(pci_t *p,unsigned off,uint32_t value) {
    (void)p;assert(off==4 && value==2);++clears;return K_OK;
}
static void kernel_serial_write_serial_only(const char *s) {(void)s;}
''' + body + r'''
int main(void) {
    pci_t pci={0};xhci_state_t s={&pci,true};xhci_msc_transport_t t={.slot=1};
    first_virtual=X86_64_DIRECT_MAP_BASE+0x1000000;
    for(unsigned offset=0;offset<4096;offset+=511) {
        void *buffer=(void *)(uintptr_t)(first_virtual+offset);
        assert(xhci_msc_direct_map(&s,&t,buffer,1048576,true));
        assert(t.direct_cpu==buffer && t.direct_offset==offset);
        assert(mapped_count==256+(offset!=0) && mapped_direction==DMA_FROM_DEVICE);
        assert(!xhci_msc_direct_map(&s,&t,buffer,4096,true)); /* Don't overwrite a live map. */
        assert(xhci_msc_direct_finish(&s,&t,3,true) && !t.direct_cpu && !t.direct_mapping.active);
    }
    assert(!stops && !clears && syncs==9 && unmaps==9);
    void *buffer=(void *)(uintptr_t)first_virtual;
    assert(!xhci_msc_direct_map(&s,&t,(void *)0x1000,4096,true));
    assert(!xhci_msc_direct_map(&s,&t,buffer,31,true));
    allocation_fail=true;assert(!xhci_msc_direct_map(&s,&t,buffer,4096,true));allocation_fail=false;
    bad_page=1;assert(!xhci_msc_direct_map(&s,&t,buffer,8192,true));bad_page=-1;
    readonly=true;assert(!xhci_msc_direct_map(&s,&t,buffer,4096,true));
    assert(xhci_msc_direct_map(&s,&t,buffer,4096,false) && mapped_direction==DMA_TO_DEVICE);
    assert(xhci_msc_direct_finish(&s,&t,3,false) && stops==1);readonly=false;
    map_fail=true;assert(!xhci_msc_direct_map(&s,&t,buffer,4096,true));map_fail=false;
    assert(xhci_msc_direct_map(&s,&t,buffer,4096,true));
    unmap_fail=true;assert(!xhci_msc_direct_finish(&s,&t,3,true));
    assert(t.direct_mapping.active && t.direct_cpu==buffer);
    unmap_fail=false;assert(xhci_msc_direct_finish(&s,&t,3,true));
    assert(xhci_msc_direct_map(&s,&t,buffer,4096,true));stop_fail=true;
    assert(xhci_msc_direct_finish(&s,&t,3,false) && clears==1 && !s.initialized);
}
'''
with tempfile.TemporaryDirectory(prefix='xhci-direct-dma-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
print('Direct DMA: 256/257 pinned pages, offsets, directions, mapping/allocation/permission failures, stop-before-unpin and unmap quarantine PASS')
