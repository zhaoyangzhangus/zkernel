"""Actual late GOP cache promotion: PCI ownership, aliases and rollback."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/init/memory.c').read_text()
body = source[source.index('static bool framebuffer_wc_owner('):
              source.index('static BOOLEAN direct_map_self_test(')]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/bootinfo.h>
#include <kernel/pci.h>
#include <arch/x86_64/paging.h>
static pci_host_t host;
static unsigned calls, fail_at;
static unsigned modes[32];
static uint64_t addresses[32];
const pci_host_t *pci_current_host(void) { return &host; }
paddr_t x86_current_root_table(void) { return paddr_make(0x1000); }
kstatus_t x86_protect_page(paddr_t root, vaddr_t address, uint32_t flags,
                           enum x86_cache_mode mode) {
    assert(root.value==0x1000 && flags==(X86_PAGE_WRITE|X86_PAGE_GLOBAL));
    assert(calls<32); modes[calls]=mode; addresses[calls++]=address;
    if (calls==fail_at) return K_EIO;
    return K_OK;
}
''' + body + r'''
static void setup(unsigned vendor, unsigned device) {
    memset(&host,0,sizeof(host)); calls=fail_at=0;
    host.initialized=true; host.device_count=1;
    host.devices[0].vendor_id=vendor; host.devices[0].device_id=device;
    host.devices[0].class_code=3;
    host.devices[0].bars[0]=(pci_bar_t){.address=0xe0000000,.length=0x1000000,
        .flags=PCI_RESOURCE_MEMORY|PCI_RESOURCE_PREFETCH};
}
int main(void) {
    OS_BOOT_INFO info={.FrameBufferBase=0xe0000123,.FrameBufferSize=0x2000};
    const uint64_t va=0xffff900000000123ULL;
    unsigned vendors[]={0x1002,0x1234};
    for(unsigned v=0;v<2;++v) {
        setup(vendors[v],v?0x1111:0x164e);
        assert(framebuffer_wc_owner(&info,&host));
        assert(framebuffer_enable_wc(&info,va) && calls==3);
        for(unsigned i=0;i<3;++i)
            assert(modes[i]==X86_CACHE_WC && addresses[i]==va-0x123+4096*i);
        for(unsigned f=1;f<=3;++f) {
            calls=0;fail_at=f;assert(!framebuffer_enable_wc(&info,va));
            assert(calls==2*f-1);
            for(unsigned i=f;i<calls;++i)assert(modes[i]==X86_CACHE_UC);
        }
    }
    setup(0x10de,0x2f04);assert(!framebuffer_enable_wc(&info,va) && !calls);
    setup(0x1234,0x9999);assert(!framebuffer_enable_wc(&info,va));
    setup(0x1002,0x164e);
    assert(!framebuffer_enable_wc(&info,va+1) && !calls);
    assert(!framebuffer_enable_wc(0,va) && !calls);
    assert(!framebuffer_wc_owner(&info,0));
    host.initialized=false;assert(!framebuffer_enable_wc(&info,va));
    setup(0x1002,0x164e);host.devices[0].bars[0].flags=PCI_RESOURCE_MEMORY;
    assert(!framebuffer_enable_wc(&info,va));
    setup(0x1002,0x164e);host.devices[0].class_code=2;
    assert(!framebuffer_enable_wc(&info,va));
    setup(0x1002,0x164e);host.devices[0].bars[0].length=0x2000;
    assert(!framebuffer_enable_wc(&info,va));
    setup(0x1002,0x164e);host.device_count=2;host.devices[1]=host.devices[0];
    host.devices[1].vendor_id=0x10de;assert(!framebuffer_enable_wc(&info,va));
    setup(0x1002,0x164e);host.device_count=2;host.devices[1]=host.devices[0];
    assert(!framebuffer_enable_wc(&info,va));
    setup(0x1002,0x164e);info.FrameBufferSize=UINT64_MAX;
    assert(!framebuffer_enable_wc(&info,va));
    assert(!calls);
    puts("GOP WC promotion: AMD/QEMU ownership, NVIDIA/unknown/overlapping BAR exclusion, page alignment, overflow and rollback PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='framebuffer-cache-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'include'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=10)
