"""Actual DMA staging growth must move registered mappings, never copy them."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/usb/xhci/msc.c').read_text()
body = source[source.index('static bool xhci_msc_reserve('):
              source.index('static bool xhci_msc_control_no_data(')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define PAGE_SIZE 4096U
#define XHCI_MSC_MAX_TRANSFER (1024U*1024U)
#define K_OK 0
typedef int xhci_state_t;
typedef struct {unsigned active;} dma_mapping_t;
typedef struct {
    void *head,*pages;unsigned page_count,allocation_order;dma_mapping_t mapping;void *cpu;
} xhci_dma_region_t;
static dma_mapping_t *registry;
static unsigned allocations,frees,moves;
static bool alloc_fail,free_fail,move_fail;
static bool xhci_alloc_dma_region(xhci_state_t *s,xhci_dma_region_t *r,unsigned pages) {
    assert(s && !registry);++allocations;if(alloc_fail)return false;
    *r=(xhci_dma_region_t){.head=(void *)0x1000,.pages=(void *)0x2000,
        .page_count=pages,.allocation_order=8,.mapping.active=1,.cpu=(void *)0x3000};
    registry=&r->mapping;return true;
}
static bool xhci_free_dma_region(xhci_dma_region_t *r) {
    ++frees;if(free_fail && r->cpu==(void *)0x4000)return false;
    if(registry==&r->mapping)registry=0;
    *r=(xhci_dma_region_t){0};return true;
}
static int dma_mapping_move(dma_mapping_t *d,dma_mapping_t *s) {
    assert(registry==s && !d->active);++moves;if(move_fail)return -1;
    *d=*s;*s=(dma_mapping_t){0};registry=d;return K_OK;
}
''' + body + r'''
int main(void) {
    xhci_state_t state=1;xhci_dma_region_t region={0};
    assert(!xhci_msc_reserve(&state,&region,0));
    assert(!xhci_msc_reserve(&state,&region,1048577) && !allocations);
    assert(xhci_msc_reserve(&state,&region,65537));
    assert(region.page_count==17 && registry==&region.mapping && moves==1);
    assert(xhci_msc_reserve(&state,&region,4096) && allocations==1); /* Reuse, no shrink. */
    assert(xhci_free_dma_region(&region));
    alloc_fail=true;assert(!xhci_msc_reserve(&state,&region,1048576));alloc_fail=false;
    region.cpu=(void *)0x4000;free_fail=true;
    assert(!xhci_msc_reserve(&state,&region,1048576));
    assert(!registry && region.cpu==(void *)0x4000);free_fail=false;
    region=(xhci_dma_region_t){0};move_fail=true;
    assert(!xhci_msc_reserve(&state,&region,1048576) && !registry && !region.cpu);
    move_fail=false;assert(xhci_msc_reserve(&state,&region,1048576));
    assert(region.page_count==256 && registry==&region.mapping);
    assert(xhci_free_dma_region(&region) && !registry);
}
'''
with tempfile.TemporaryDirectory(prefix='xhci-dynamic-buffer-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
print('Dynamic DMA staging: exact growth, reuse, mapping address identity, allocation/free/move failure and 1MiB bound PASS')
