"""验证实际初始栈构造：空参数、满容量向量、auxv、对齐和写入失败。"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/process/exec.c').read_text()
body = source[source.index('static kstatus_t build_initial_stack('):
              source.index('static kstatus_t read_vfs_range(', source.index('static kstatus_t build_initial_stack('))]
type_start = source.index('typedef struct elf_exec_arguments {')
argument_type = source[type_start:source.index('} elf_exec_arguments_t;', type_start) + len('} elf_exec_arguments_t;')]
tags = sorted(set(re.findall(r'LINUX_AUX_PUSH\((\w+),', body)) - {'tag'})
constants = '\n'.join(f'#define {tag} {i}' for i, tag in enumerate(['AUX_NULL'] + [t for t in tags if t != 'AUX_NULL']))
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define ELF_EXEC_ARG_MAX 32U
#define ELF_EXEC_ENV_MAX 64U
#define ELF_EXEC_ARG_LENGTH 256U
#define ELF_EXEC_AUX_MAX 36U
#define OS_DEBUG_SERIAL 0
#define PAGE_SIZE 4096U
#define OS_LINUX_USER_STACK_BASE 0x1000U
#define OS_LINUX_USER_STACK_SIZE 65536U
#define OS_LINUX_USER_STACK_TOP (OS_LINUX_USER_STACK_BASE+OS_LINUX_USER_STACK_SIZE)
#define LINUX_ARCH_ELF_PLATFORM "x86_64"
#define VM_MAP_PRIVATE 1
#define VM_MAP_FIXED 2
#define VM_MAP_GUARD 4
#define VM_MAP_STACK 8
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_USER 4
#define K_OK 0
#define K_EINVAL -22
#define K_EIO -5
typedef uint64_t vaddr_t;
typedef int kstatus_t;
typedef struct { int unused; } vm_space_t;
typedef struct { uint64_t address,protection,flags; } vm_fault_info_t;
typedef struct { uint64_t program_headers,program_header_size,program_header_count,interpreter_base,main_entry; bool linux_abi; } user_elf_image_info_t;
typedef struct { uint32_t count,entries[16]; } os_exec_fd_map_t;
static unsigned maps,writes,fail_write;
static uint8_t memory[OS_LINUX_USER_STACK_SIZE];
static int vm_map_object(vm_space_t *s,void *o,vaddr_t *a,uint64_t offset,size_t len,unsigned prot,unsigned flags) {
 assert(s && !o && a && !offset && len && flags);(void)prot;++maps;return 0;
}
static int vm_handle_fault(vm_space_t *s,const vm_fault_info_t *f) { assert(s && f->address==OS_LINUX_USER_STACK_TOP-1);return 0; }
static int write_address_space(vm_space_t *s,vaddr_t address,const void *data,size_t len) {
 assert(s && address>=OS_LINUX_USER_STACK_BASE && address+len<=OS_LINUX_USER_STACK_TOP);
 if(++writes==fail_write)return K_EIO;
 memcpy(memory+address-OS_LINUX_USER_STACK_BASE,data,len);return 0;
}
static uint64_t word(vaddr_t a) { uint64_t n;memcpy(&n,memory+a-OS_LINUX_USER_STACK_BASE,8);return n; }
static const char *string(vaddr_t a) { assert(a>=OS_LINUX_USER_STACK_BASE && a<OS_LINUX_USER_STACK_TOP);return (const char*)memory+a-OS_LINUX_USER_STACK_BASE; }
'''
checks = r'''
int main(void) {
 vm_space_t space={0};elf_exec_arguments_t arguments={0};
 for(unsigned linux=0;linux<2;++linux)for(unsigned mode=0;mode<4;++mode)for(unsigned mapped=0;mapped<2;++mapped) {
  memset(&arguments,0,sizeof(arguments));
  if(mode>=2) {
   arguments.count=mode==2?2:ELF_EXEC_ARG_MAX;
   arguments.environment_count=mode==2?1:ELF_EXEC_ENV_MAX;
   for(unsigned i=0;i<arguments.count;++i)snprintf(arguments.values[i],256,"arg%u",i);
   for(unsigned i=0;i<arguments.environment_count;++i)snprintf(arguments.environment[i],256,"KEY%u=value",i);
  }
  user_elf_image_info_t info={.linux_abi=linux,.program_headers=0x1234,.program_header_size=56,.program_header_count=9,.main_entry=0x5678};
  os_exec_fd_map_t map={.count=1,.entries={42}};vaddr_t sp=0;
  maps=writes=fail_write=0;memset(memory,0xcc,sizeof(memory));
  assert(build_initial_stack(&space,&info,mode?&arguments:NULL,mapped?&map:NULL,&sp)==0);
  assert(maps==2 && !(sp&15));
  assert(word(sp)==arguments.count);sp+=8;
  for(unsigned i=0;i<arguments.count;++i,sp+=8)assert(!strcmp(string(word(sp)),arguments.values[i]));
  assert(!word(sp));sp+=8;
  for(unsigned i=0;i<arguments.environment_count;++i,sp+=8)assert(!strcmp(string(word(sp)),arguments.environment[i]));
  assert(!word(sp));sp+=8;
  unsigned pairs=0,random=0,execfn=0,platform=0,fdmap=0;
  for(;;sp+=16) {
   uint64_t tag=word(sp),value=word(sp+8);++pairs;
   assert(pairs<=ELF_EXEC_AUX_MAX/2);
   if(tag==AUX_NULL){assert(value==0);break;}
   if(tag==AUX_RANDOM){assert(value==OS_LINUX_USER_STACK_TOP-16);++random;}
   if(tag==AUX_PHDR)assert(value==0x1234);
   if(tag==AUX_ENTRY)assert(value==0x5678);
   if(tag==AUX_EXECFN){assert(!strcmp(string(value),arguments.count?arguments.values[0]:""));++execfn;}
   if(tag==AUX_PLATFORM){assert(!strcmp(string(value),"x86_64"));++platform;}
   if(tag==OS_AUX_FD_MAP){assert(!memcmp(string(value),&map,sizeof(map)));++fdmap;}
  }
  assert(pairs==8+linux*9+mapped && random==1 && execfn==linux && platform==linux && fdmap==mapped);
 }
 user_elf_image_info_t info={0};vaddr_t sp=0xdead;
 arguments.count=ELF_EXEC_ARG_MAX+1;maps=0;
 assert(build_initial_stack(&space,&info,&arguments,NULL,&sp)==K_EINVAL && maps==0 && sp==0xdead);
 arguments.count=0;arguments.environment_count=ELF_EXEC_ENV_MAX+1;
 assert(build_initial_stack(&space,&info,&arguments,NULL,&sp)==K_EINVAL && maps==0);
 memset(&arguments,0,sizeof(arguments));arguments.count=1;memset(arguments.values[0],'x',256);
 assert(build_initial_stack(&space,&info,&arguments,NULL,&sp)==K_EINVAL && sp==0xdead);
 for(unsigned failure=1;failure<=5;++failure) {
  writes=0;fail_write=failure;
  assert(build_initial_stack(&space,&info,NULL,NULL,&sp)==K_EIO && sp==0xdead);
 }
 puts("16 native/Linux stack layouts, full vectors, descriptor maps, bounds and write failures passed");
}
'''
with tempfile.TemporaryDirectory(prefix='exec-stack-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(fixture + constants + '\n' + argument_type + '\n' + body + checks)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
