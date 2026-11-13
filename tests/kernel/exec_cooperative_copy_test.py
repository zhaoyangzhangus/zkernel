"""Actual ELF copying: exact bytes and no scheduling of an uncommitted image."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/process/exec.c').read_text()
begin=source.index('static kstatus_t write_address_space')
end=source.index('static kstatus_t read_vfs_range(',begin)
program=r'''
#include <kernel/process.h>
#include <kernel/vm.h>
#include <kernel/sched.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/smp.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static thread_t current;
static bool preempt, shootdown, pending, tick;
static unsigned yields, fault_calls;
static unsigned expected_access=VM_PROT_WRITE;
static unsigned char *destination;
static bool resident[70];
static int fail_page=-1;
void *liteos_memcpy(void *, const void *, size_t);
thread_t *sched_current_thread(void) { return &current; }
bool sched_preempt_disabled(void) { return preempt; }
bool x86_tlb_shootdown_active(void) { return shootdown; }
bool x86_smp_reschedule_pending(uint32_t cpu) { (void)cpu;return pending; }
uint32_t x86_current_cpu_index(void) { return 0; }
bool sched_tick_should_preempt(void) { return tick; }
bool x86_smp_take_reschedule_request(void) { bool p=pending;pending=false;return p; }
void schedule(void) { assert(!preempt && !shootdown);++yields; }
kstatus_t x86_translate_page(paddr_t root,vaddr_t address,paddr_t *out,uint64_t *flags) {
    (void)root;(void)flags;
    unsigned page=(address-0x400000)/PAGE_SIZE;
    assert(page<70);
    if (!resident[page]) return K_ENOENT;
    *out=paddr_make((uintptr_t)destination+page*PAGE_SIZE);return K_OK;
}
kstatus_t vm_handle_fault(vm_space_t *space,const vm_fault_info_t *fault) {
    (void)space;++fault_calls;assert(fault->access==expected_access);
    unsigned page=(fault->address-0x400000)/PAGE_SIZE;
    if ((int)page==fail_page) return K_EIO;
    resident[page]=true;return K_OK;
}
void *phys_to_direct(paddr_t physical) { return (void*)(uintptr_t)physical.value; }
''' + source[begin:end].replace('__builtin_memcpy', 'liteos_memcpy') + r'''
int main(void) {
    size_t size=65*PAGE_SIZE+19;
    destination=calloc(70,PAGE_SIZE);unsigned char *input=malloc(size);
    assert(destination && input);
    for (size_t i=0;i<size;++i)input[i]=(unsigned char)(i*13+7);
    vm_space_t space={0};tick=true;
    assert(write_address_space(&space,0x400003,input,size)==K_OK);
    assert(!memcmp(destination+3,input,size));
    assert(destination[0]==0 && destination[size+3]==0);
    assert(fault_calls==66 && yields==0);
    tick=false;pending=false;yields=0;
    assert(write_address_space(&space,0x400003,input,size)==K_OK && !yields);
    pending=true;
    assert(write_address_space(&space,0x400003,input,size)==K_OK && yields==0 && pending);
    memset(resident,0,sizeof(resident));memset(destination,0,70*PAGE_SIZE);
    fail_page=10;tick=true;yields=0;
    assert(write_address_space(&space,0x400003,input,size)==K_EIO && !yields);
    assert(!memcmp(destination+3,input,10*PAGE_SIZE-3));
    assert(destination[10*PAGE_SIZE]==0);
    assert(write_address_space(&space,0x400003,0,0)==K_OK && !yields);
    // 只读和可执行段按各自权限建立页面，只触碰文件内容所在的页。
    fail_page=-1;
    const unsigned accesses[]={VM_PROT_READ,VM_PROT_WRITE,VM_PROT_EXEC};
    for(unsigned i=0;i<3;++i) {
        expected_access=accesses[i];fault_calls=0;
        memset(resident,0,sizeof(resident));memset(destination,0,70*PAGE_SIZE);
        assert(write_address_space_access(&space,0x400003,input,size,expected_access)==K_OK);
        assert(!memcmp(destination+3,input,size) && fault_calls==66 && !yields);
        for(unsigned p=66;p<70;++p)assert(!resident[p]);
        assert(destination[size+3]==0);
    }
    free(input);free(destination);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-exec-copy-') as directory:
    path=Path(directory);(path/'test.c').write_text(program)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fno-builtin',
                    '-mno-sse','-mno-mmx','-fno-tree-vectorize',
                    '-DOS_KERNEL_BUILD','-Dmemcpy=liteos_memcpy',
                    '-Dmemset=liteos_memset','-Dmemmove=liteos_memmove',
                    '-Dmemcmp=liteos_memcmp','-I'+str(root/'include'),
                    '-c',str(root/'kernel/string.c'),'-o',str(path/'string.o')],check=True)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),str(path/'string.o'),
                    '-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('ELF copy: exact bytes, RX/RW/X fault access, untouched BSS pages, failure and no uncommitted-image scheduling passed')
