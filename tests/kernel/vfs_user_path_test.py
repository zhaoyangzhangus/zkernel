"""Page-batched paths with production uaccess assembly and fault fixup table."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/user_api.c').read_text()
start = source.index('static bool vfs_copy_path_from_user(')
helper = source[start:source.index('\n}', start) + 2]
uaccess = (root / 'kernel/arch/x86_64/mm/uaccess.c').read_text()
uaccess = uaccess.replace(
    'kstatus_t copy_from_user(void *destination, const void __user *source, size_t size) {',
    'kstatus_t copy_from_user(void *destination, const void __user *source, size_t size) {\n'
    '    ++copy_calls;')
program = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include <arch/x86_64/uaccess.h>
#define VFS_PATH_LIMIT 256U
static unsigned copy_calls;
static volatile sig_atomic_t faults;
''' + uaccess + '\n' + helper + r'''
static void fault_handler(int signal, siginfo_t *info, void *context) {
 (void)info;
 ucontext_t *uc=context;
 arch_trap_frame_t frame={.vector=14,.rip=(uint64_t)uc->uc_mcontext.gregs[REG_RIP]};
 if(signal!=SIGSEGV || !x86_uaccess_fault_site(&frame) || !x86_uaccess_fixup(&frame))_exit(99);
 uc->uc_mcontext.gregs[REG_RIP]=(greg_t)frame.rip;
 ++faults;
}
static unsigned char output[288];
static bool check(const char *source) {
 memset(output,0x69,sizeof(output));copy_calls=0;
 bool result=vfs_copy_path_from_user(source,(char*)output+16);
 for(unsigned i=0;i<16;++i)assert(output[i]==0x69);
 for(unsigned i=272;i<sizeof(output);++i)assert(output[i]==0x69);
 return result;
}
int main(void) {
 assert(sysconf(_SC_PAGESIZE)==PAGE_SIZE);
 struct sigaction action={.sa_sigaction=fault_handler,.sa_flags=SA_SIGINFO};
 sigemptyset(&action.sa_mask);assert(sigaction(SIGSEGV,&action,0)==0);
 x86_uaccess_init(false); // Host user-mode cannot execute STAC/CLAC.
 char *memory=mmap(0,4*PAGE_SIZE,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 assert(memory!=MAP_FAILED);
 assert(mprotect(memory+PAGE_SIZE,2*PAGE_SIZE,PROT_READ|PROT_WRITE)==0);
 unsigned cases=0;
 for(unsigned distance=0;distance<256;++distance)for(unsigned length=0;length<256;++length) {
  char *path=memory+2*PAGE_SIZE-1-distance;
  memset(path,'a',256);path[0]='/';path[length]=0;
  bool accepted=check(path);
  assert(accepted==(length>0 && length<255));
  assert(copy_calls>=1 && copy_calls<=2 && !faults);
  if(accepted)assert(!memcmp(output+16,path,length+1));
  ++cases;
 }
 for(unsigned offset=0;offset<PAGE_SIZE;++offset) {
  char *path=memory+PAGE_SIZE+offset;
  memset(path,'a',256);path[0]='/';path[200]=0;
  assert(check(path));assert(!memcmp(output+16,path,201));
  assert(copy_calls==(offset>PAGE_SIZE-201?2U:1U) && !faults);
  ++cases;
 }
 // NUL immediately before an unmapped page must never fault or access it.
 for(unsigned length=1;length<255;++length) {
  char *path=memory+3*PAGE_SIZE-1-length;
  memset(path,'a',length);path[0]='/';path[length]=0;
  sig_atomic_t before=faults;
  assert(check(path) && copy_calls==1 && faults==before);
  assert(!memcmp(output+16,path,length+1));++cases;
 }
 for(unsigned tail=1;tail<=255;++tail) {
  char *path=memory+3*PAGE_SIZE-tail;
  memset(path,'a',tail);path[0]='/';
  sig_atomic_t before=faults;
  assert(!check(path));
  assert(faults==before+(tail<255));
  assert(copy_calls==(tail<255?2U:1U));++cases;
 }
 char *path=memory+PAGE_SIZE+16;
 strcpy(path,"relative/path");assert(check(path));
 assert(!memcmp(output+16,path,strlen(path)+1));++cases;
 path[0]=0;assert(!check(path));++cases;
 uintptr_t invalid[]={0,1,65535,0x0000800000000000ULL,UINTPTR_MAX};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
  sig_atomic_t before=faults;assert(!check((const char*)invalid[i]));
  assert(faults==before);++cases;
 }
 sig_atomic_t before=faults;
 assert(!check(memory) && faults==before+1 && copy_calls==1);++cases;
 assert(munmap(memory,4*PAGE_SIZE)==0);
 printf("PASS: %u path cases; production copy/fixup, page guards, limits, canaries; 201 calls -> 1 or 2\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-user-path-') as directory:
    path = Path(directory)
    # The kernel linker handles .rdata explicitly; the host linker requires
    # an allocated ELF rodata section. Instructions/table entries stay exact.
    assembly = (root / 'kernel/arch/x86_64/mm/uaccess.S').read_text()
    (path / 'uaccess.S').write_text(assembly.replace('    .section .rdata\n',
                                                  '    .section .rodata\n'))
    def compile_and_run(name, text, negative=False):
        (path / (name + '.c')).write_text(text)
        subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-pie', '-no-pie',
                        '-I' + str(root / 'include'), str(path / (name + '.c')),
                        str(path / 'uaccess.S'),
                        '-Wl,-z,noexecstack', '-o', str(path / name)], check=True)
        if not negative:
            subprocess.run([str(path / name)], check=True)
        else:
            result = subprocess.run([str(path / name)], stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL,
                                    preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
            assert result.returncode != 0, 'Cross-page copy unexpectedly passed'
    compile_and_run('test', program)
    unsafe = program.replace('if (chunk > remaining) chunk = remaining;', 'chunk = remaining;')
    assert unsafe != program
    compile_and_run('negative', unsafe, True)
    print('PASS: negative control rejects copying beyond the NUL into a protected page')
