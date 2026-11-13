"""Actual late GOP cache promotion: PCI ownership, aliases and rollback."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/init/memory.c').read_text()
body = source[source.index('static bool framebuffer_page_range('):
              source.index('static BOOLEAN direct_map_self_test(')]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/bootinfo.h>
#include <kernel/pci.h>
#include <arch/x86_64/paging.h>
#define FRAMEBUFFER_KERNEL_VIRTUAL_BASE (X86_64_MMIO_BASE + 0x20000000ULL)
static pci_host_t host;
static unsigned map_calls,unmap_calls,map_fail;
static uint64_t physical_base;
kstatus_t x86_map_page(paddr_t root,vaddr_t va,paddr_t pa,uint32_t flags,enum x86_cache_mode mode){
 assert(root.value==0x1000&&flags==(X86_PAGE_WRITE|X86_PAGE_GLOBAL)&&mode==X86_CACHE_UC);
 assert(va==FRAMEBUFFER_KERNEL_VIRTUAL_BASE+(uint64_t)map_calls*PAGE_SIZE);
 assert(pa.value==physical_base+(uint64_t)map_calls*PAGE_SIZE);
 ++map_calls;return map_calls==map_fail?K_EIO:K_OK;
}
kstatus_t x86_unmap_page(paddr_t root,vaddr_t va,paddr_t *old){
 assert(root.value==0x1000&&!old);
 ++unmap_calls;assert(va==FRAMEBUFFER_KERNEL_VIRTUAL_BASE+(uint64_t)(map_fail-1-unmap_calls)*PAGE_SIZE);
 return K_OK;
}
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
    /* 对全部页内偏移验证向外取整，初始映射保留原始偏移。 */
    for(unsigned offset=0;offset<PAGE_SIZE;++offset) {
        const uint64_t sizes[]={1,4095,4096,4097};
        for(unsigned n=0;n<4;++n) {
            OS_BOOT_INFO sample={.FrameBufferBase=0xe0000000+offset,
                                 .FrameBufferSize=sizes[n]};
            uint64_t base=7,span=9,out=0xdead;
            assert(framebuffer_page_range(&sample,&base,&span));
            uint64_t pages=(offset+sizes[n]-1)/PAGE_SIZE+1;
            assert(base==0xe0000000&&span==pages*PAGE_SIZE);
            map_calls=unmap_calls=map_fail=0;physical_base=base;
            assert(map_framebuffer_wc(&sample,&out));
            assert(out==FRAMEBUFFER_KERNEL_VIRTUAL_BASE+offset);
            assert(map_calls==pages&&!unmap_calls);
        }
    }
    /* 每个失败位置只撤回已成功的页，输出参数不可提前发布。 */
    OS_BOOT_INFO mapped={.FrameBufferBase=0xe0000123,.FrameBufferSize=0x2000};
    for(unsigned failure=1;failure<=3;++failure) {
        uint64_t out=0xdead;
        map_calls=unmap_calls=0;map_fail=failure;physical_base=0xe0000000;
        assert(!map_framebuffer_wc(&mapped,&out));
        assert(map_calls==failure&&unmap_calls==failure-1&&out==0xdead);
    }
    /* 溢出与空范围必须在任何页表写入前拒绝，范围输出也保持不变。 */
    const uint64_t invalid[][2]={{0,1},{1,0},{UINT64_MAX,1},
        {UINT64_MAX-4095,1},{UINT64_MAX-4096,4096},{1,UINT64_MAX}};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        OS_BOOT_INFO sample={.FrameBufferBase=invalid[i][0],.FrameBufferSize=invalid[i][1]};
        uint64_t base=7,span=9,out=0xdead;
        map_calls=unmap_calls=map_fail=0;
        assert(!framebuffer_page_range(&sample,&base,&span)&&base==7&&span==9);
        assert(!map_framebuffer_wc(&sample,&out)&&out==0xdead);
        assert(!map_calls&&!unmap_calls);
    }
    uint64_t base=7,span=9,out=0xdead;
    assert(!framebuffer_page_range(0,&base,&span)&&base==7&&span==9);
    assert(!map_framebuffer_wc(0,&out)&&out==0xdead);
    assert(!map_framebuffer_wc(&mapped,0));
    OS_BOOT_INFO too_large={.FrameBufferBase=0x1000,
        .FrameBufferSize=X86_64_MMIO_END-FRAMEBUFFER_KERNEL_VIRTUAL_BASE+2ULL};
    assert(!map_framebuffer_wc(&too_large,&out)&&out==0xdead&&!map_calls);
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
    command = ['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-I' + str(root / 'include'), str(path / 'test.c'),
               '-o', str(path / 'test')]
    subprocess.run(command, check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=10)
    for old,new in [
        ('info->FrameBufferSize > UINT64_MAX - info->FrameBufferBase', 'false'),
        ('if (end > UINT64_MAX - (PAGE_SIZE - 1ULL)) return false;', ''),
        ('*span = end - start;', '*span = info->FrameBufferSize;'),
        ('while (mapped != 0) {', 'while (mapped > PAGE_SIZE) {'),
    ]:
        assert old in program
        (path/'test.c').write_text(program.replace(old,new))
        subprocess.run(command, check=True)
        result = subprocess.run([str(path/'test')], capture_output=True, timeout=10,
            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        assert result.returncode != 0 and b'Assertion' in result.stderr
    print('页范围：16384 组偏移/长度、空值/溢出/容量边界和逐页回滚通过；四组错误实现均被拒绝')
