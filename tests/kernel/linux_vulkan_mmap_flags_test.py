"""Execute production mmap flag translation used by NVIDIA Vulkan allocators."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/linux.c').read_text()
start = source.index('static int64_t linux_mmap(')
end = source.index('    if ((flags & LINUX_MAP_ANONYMOUS) == 0U)', start)
function = source[start:end]
function += '\n(void)object; (void)entry; (void)status; (void)vm_prot;\nlast_address=address; return vm_flags;\n}\n'
code = r'''
#include <assert.h>
#include <kernel/vm.h>
#include <uapi/linux_abi.h>
#define X86_64_USER_TOP 0x7fffffffffffULL
#define LINUX_MAP_32BIT 0x40U
#define LINUX_MAP_NORESERVE 0x4000U
typedef int process_t;
typedef struct { int dummy; } linux_fd_entry_t;
static bool x86_is_canonical(uint64_t a) { return a <= X86_64_USER_TOP; }
static uint64_t last_address;
''' + function + r'''
int main(void) {
    uint64_t anon=LINUX_MAP_PRIVATE|LINUX_MAP_ANONYMOUS;
    assert(linux_mmap(0,0,0xabf40,3,anon|LINUX_MAP_32BIT,-1,0)&VM_MAP_LOW32);
    assert(last_address==0);
    assert(linux_mmap(0,0,0x8000000,0,anon|LINUX_MAP_NORESERVE,-1,0)==
           (VM_MAP_PRIVATE|VM_MAP_ALLOW_RWX));
    assert(last_address==0x100000000ULL);
    assert(linux_mmap(0,0x180000,4096,3,anon,-1,0)>0 && last_address==0x180000);
    assert(linux_mmap(0,0,4096,1,LINUX_MAP_PRIVATE,0,0)>0 && last_address==0x100000000ULL);
    assert(!(linux_mmap(0,0x90000000,4096,3,anon|LINUX_MAP_FIXED|LINUX_MAP_32BIT,-1,0)&VM_MAP_LOW32));
    assert(last_address==0x90000000);
    assert(linux_mmap(0,0,4096,3,anon|(1ULL<<63),-1,0)==K_EINVAL);
    assert(linux_mmap(0,0,0,3,anon,-1,0)==K_EINVAL);
    assert(linux_mmap(0,0,UINT64_MAX,3,anon,-1,0)==K_EINVAL);
}
'''
with tempfile.TemporaryDirectory(prefix='vulkan-mmap-flags-') as directory:
    base = Path(directory)
    (base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                    '-I'+str(root/'include'),str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('Vulkan mmap: real flag translation, NORESERVE, low-32-bit bounds policy, FIXED override and invalid requests passed')
