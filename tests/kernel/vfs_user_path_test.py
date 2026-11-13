"""共用用户字符串及路径/exec 适配：实际访存汇编、异常修复和页边界。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/user_api.c').read_text()
start = source.index('static bool vfs_copy_path_from_user(')
helper = source[start:source.index('\n}', start) + 2]
exec_source = (root / 'kernel/process/exec.c').read_text()
vector_start = exec_source.index('static kstatus_t copy_exec_vector(')
vector = exec_source[vector_start:exec_source.index('static kstatus_t copy_exec_arguments(', vector_start)]
path_start = exec_source.index('static kstatus_t exec_copy_user_path(')
exec_path = exec_source[path_start:exec_source.index('static bool exec_path_is_self_exe(', path_start)]
linux_source = (root / 'kernel/syscall/linux.c').read_text()
linux_start = linux_source.index('static kstatus_t linux_copy_string(')
linux_string = linux_source[linux_start:linux_source.index('static kstatus_t linux_system_config_path(', linux_start)]
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
''' + uaccess + '\n' + helper + '\n#define ELF_EXEC_ARG_LENGTH 256U\n' + vector + '\n#define EXEC_PATH_CAPACITY 256U\n' + exec_path + linux_string + r'''
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
 // 未映射页前的 NUL 不能触发故障，也不能访问该页面。
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
 // 执行路径和 Linux 字符串适配保留不同的空串、长度边界。
 for(unsigned distance=0;distance<256;++distance)for(unsigned length=0;length<=256;++length) {
  char *text=memory+2*PAGE_SIZE-1-distance;
  memset(text,'p',257);text[length]=0;
  char out[258];memset(out,0x69,sizeof(out));copy_calls=0;
  kstatus_t result=exec_copy_user_path(text,out+1);
  assert(result==(length==0?K_ENOENT:length<256?K_OK:K_ENAMETOOLONG));
  assert(copy_calls>=1 && copy_calls<=2 && out[0]==0x69 && out[257]==0x69);
  if(length<256)assert(!memcmp(out+1,text,length+1));
  else assert(out[256]==0);
  copy_calls=0;memset(out,0x69,sizeof(out));
  result=linux_copy_string((uintptr_t)text,out+1,256);
  assert(result==(length<255?K_OK:K_ENAMETOOLONG));
  assert(copy_calls>=1 && copy_calls<=2 && out[0]==0x69 && out[257]==0x69);
  if(length<255)assert(!memcmp(out+1,text,length+1));
  else assert(out[256]==0);
 }
 // 共用函数允许空字符串和容量末尾的终止符，exec 保留自己的错误码。
 char values[3][256];uint32_t count=99;
 for(unsigned distance=0;distance<256;++distance)for(unsigned length=0;length<256;++length) {
  char *text=memory+2*PAGE_SIZE-1-distance;
  memset(text,'v',256);text[length]=0;
  const char *vector[]={text,"",NULL};
  copy_calls=0;count=99;
  assert(copy_exec_vector(vector,3,values,&count)==K_OK && count==2);
  assert(!memcmp(values[0],text,length+1) && !values[1][0]);
  assert(copy_calls>=5 && copy_calls<=6);
 }
 char adapter_output[256];
 assert(exec_copy_user_path(NULL,adapter_output)==K_EINVAL);
 assert(exec_copy_user_path("a",NULL)==K_EINVAL);
 assert(linux_copy_string(0,adapter_output,sizeof(adapter_output))==K_EINVAL);
 assert(linux_copy_string((uintptr_t)"a",NULL,256)==K_EINVAL);
 assert(linux_copy_string((uintptr_t)"a",adapter_output,1)==K_EINVAL);
 for(unsigned tail=1;tail<=255;++tail) {
  char *text=memory+3*PAGE_SIZE-tail;memset(text,'f',tail);
  sig_atomic_t previous=faults;
  assert(exec_copy_user_path(text,adapter_output)==K_EACCES && faults==previous+1);
  previous=faults;
  assert(linux_copy_string((uintptr_t)text,adapter_output,256)==(tail<255?K_EACCES:K_ENAMETOOLONG));
  assert(faults==previous+(tail<255));
  text[tail-1]=0;previous=faults;
  assert(exec_copy_user_path(text,adapter_output)==(tail==1?K_ENOENT:K_OK));
  assert(linux_copy_string((uintptr_t)text,adapter_output,256)==K_OK && faults==previous);
 }
 char *last=memory+3*PAGE_SIZE-1;*last=0;
 assert(copy_string_from_user(values[0],last,256)==K_OK && !values[0][0]);
 *last='x';sig_atomic_t earlier=faults;
 assert(copy_string_from_user(values[0],last,256)==K_EACCES && faults==earlier+1);
 char *full=memory+PAGE_SIZE;memset(full,'v',256);
 const char *invalid_vector[]={full,NULL};
 assert(copy_exec_vector(invalid_vector,3,values,&count)==K_EINVAL && count==0);
 assert(copy_exec_vector(NULL,3,values,&count)==K_OK && count==0);
 assert(copy_exec_vector(invalid_vector,3,values,NULL)==K_EINVAL);
 assert(copy_string_from_user(values[0],full,0)==K_EINVAL);
 assert(copy_string_from_user(NULL,full,256)==K_EINVAL);
 const char *unterminated_vector[]={"a","b","c"};
 assert(copy_exec_vector(unterminated_vector,3,values,&count)==K_EINVAL && count==0);
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
 printf("PASS: %u path cases; production copy/fixup, page guards, limits, canaries; 201 calls -> 1 or 2; 65536 exec vector and 131584 path adapter cases\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-user-path-') as directory:
    path = Path(directory)
    # 内核链接器会显式处理 .rdata；宿主链接器则要求存在已分配的 ELF 只读数据节。
    # 指令和表项必须保持不变。
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
