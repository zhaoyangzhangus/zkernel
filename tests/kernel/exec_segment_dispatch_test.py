"""Exercise the real ELF segment dispatcher after removing its dead fallback."""
from pathlib import Path
import subprocess
import resource
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/process/exec.c').read_text()
body = source[source.index('static kstatus_t map_load_segment('):
              source.index('static kstatus_t map_elf_segments(')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
enum {K_OK=0,K_EINVAL=-22,K_EIO=-5,K_ENOMEM=-12,ELF_PT_LOAD=1,
 VM_MAP_PRIVATE=1,VM_MAP_FIXED=2,K_ENOENT=-2};
#define PAGE_SIZE 4096ULL
#define PAGE_SHIFT 12
typedef int kstatus_t;
typedef uint64_t vaddr_t;
typedef struct {uint64_t root_table;} vm_space_t;
typedef struct {uint64_t value;} paddr_t;
typedef struct {unsigned unused;} vm_object_t;
typedef struct {unsigned unused;} file_t;
typedef struct {vaddr_t address;unsigned access;unsigned flags;} vm_fault_info_t;
typedef struct {uint32_t type,flags;uint64_t virtual_address,memory_size,file_size,offset;} elf64_program_header_t;
static unsigned failure,created,mapped,faults,copies,delegated,live;
static uint64_t expected_start,expected_end;
static const uint8_t *expected_source;
static size_t expected_size;
static vm_object_t object;
static vm_space_t space;
static unsigned char output[4*PAGE_SIZE];
static bool resident[4];
static file_t file;
static const elf64_program_header_t *active_program;
static uint64_t active_bias;
static void exec_debug_progress(unsigned stage){(void)stage;}
static void exec_debug_result(int status){assert(status!=K_OK);}
static bool align_up_page(uint64_t value,uint64_t *out){
 if(value>UINT64_MAX-(PAGE_SIZE-1))return false;
 *out=(value+PAGE_SIZE-1)&~(PAGE_SIZE-1);return true;
}
static uint32_t vm_protection(unsigned flags){return flags+16;}
static uint32_t fault_access(unsigned protection){return protection+32;}
static kstatus_t vm_object_create_anon(size_t size,vm_object_t **out){
 assert(size==expected_end-expected_start && !live);++created;
 if(failure==1)return K_ENOMEM;
 ++live;*out=&object;return K_OK;
}
static void vm_object_put(vm_object_t *obj){assert(obj==&object && live==1);--live;}
static kstatus_t vm_map_object(vm_space_t *s,vm_object_t *obj,vaddr_t *address,
 unsigned offset,size_t size,unsigned protection,unsigned flags){
 assert(s==&space && obj==&object && live==1 && *address==expected_start);
 assert(!offset && size==expected_end-expected_start && flags==(VM_MAP_PRIVATE|VM_MAP_FIXED));
 assert(protection==vm_protection(active_program->flags));++mapped;
 return failure==2?K_EIO:K_OK;
}
static kstatus_t vm_handle_fault(vm_space_t *s,const vm_fault_info_t *fault){
 assert(s==&space && !live && fault->access==fault_access(vm_protection(active_program->flags)));
 unsigned page=(fault->address-expected_start)/PAGE_SIZE;assert(page<4);
 ++faults;if(failure==3)return K_EIO;
 resident[page]=true;return K_OK;
}
static kstatus_t x86_translate_page(uint64_t root,uint64_t address,paddr_t *out,void *flags){
 (void)root;assert(!flags && address>=expected_start);
 unsigned page=(address-expected_start)/PAGE_SIZE;assert(page<4);
 if(!resident[page])return K_ENOENT;
 out->value=(uintptr_t)output+page*PAGE_SIZE;return K_OK;
}
static void *phys_to_direct(paddr_t p){return failure==4?NULL:(void*)(uintptr_t)p.value;}
static void copy_segment(void *destination,const void *data,size_t size){
 assert(data==expected_source && size==expected_size && destination==output+3);++copies;
 memcpy(destination,data,size);
}
static kstatus_t map_load_segment_from_vfs(vm_space_t *s,const elf64_program_header_t *p,
 uint64_t bias,file_t *f){
 assert(s==&space && p==active_program && bias==active_bias && f==&file);
 assert(!created && !faults && !copies);++delegated;return K_EIO;
}
''' + source[source.index('static kstatus_t write_address_space_access('):source.index('/* 栈和可写段')] .replace('__builtin_memcpy', 'copy_segment') + body + r'''
int main(void){
 static const uint8_t image[128]={0};unsigned cases=0;
 for(unsigned backed=0;backed<2;++backed)for(unsigned fail=0;fail<5;++fail)
 for(unsigned bss=0;bss<2;++bss)for(unsigned skip=0;skip<3;++skip){
  elf64_program_header_t p={ELF_PT_LOAD,5,0x400003,3*PAGE_SIZE+17,bss?0:67,13};
  if(skip==1)p.type=0;if(skip==2)p.memory_size=0;
  active_program=&p;active_bias=0x200000;failure=fail;
  expected_start=(active_bias+p.virtual_address)&~(PAGE_SIZE-1);
  assert(align_up_page(active_bias+p.virtual_address+p.memory_size,&expected_end));
  expected_source=image+p.offset;expected_size=p.file_size;
  created=mapped=faults=copies=delegated=live=0;
  memset(resident,0,sizeof(resident));memset(output,0,sizeof(output));
  int status=map_load_segment(&space,image,&p,active_bias,backed?&file:NULL);
  if(skip){assert(status==K_OK && !created && !delegated);}
  else if(backed){assert(status==K_EIO && delegated==1 && !created);}
  else {
   int expected=fail==1?K_ENOMEM:fail==2 || ((fail==3 || fail==4) && !bss)?K_EIO:K_OK;
   assert(status==expected && created==1 && !delegated);
   assert(mapped==(unsigned)(fail!=1));
   assert(copies==(unsigned)(fail==0 && !bss));
   assert(faults==(unsigned)(fail!=1 && fail!=2 && !bss));
   for(unsigned page=1;page<4;++page)assert(!resident[page]);
   if(copies)assert(!memcmp(output+3,expected_source,expected_size));
   for(unsigned i=3+67;i<sizeof(output);++i)assert(!output[i]);
  }
  assert(!live);++cases;
 }
 printf("ELF segment dispatch: %u file-backed/in-memory/BSS/skipped and lazy BSS and allocation/map/fault/direct-map failure cases passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='exec-segment-dispatch-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)

    # 故意恢复整段预分配，测试必须发现 BSS 页面被提前物化。
    eager = body.replace('    exec_debug_progress(0x123U);', r'''    for (uint64_t page=mapping_start;page<mapping_end;page+=PAGE_SIZE) {
        status=vm_handle_fault(space,&(vm_fault_info_t){page,fault_access(protection),0});
        if(status!=K_OK)return status;
    }
    exec_debug_progress(0x123U);''')
    assert eager != body
    negative = program.replace(body, eager)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=negative,text=True,check=True)
    failed = subprocess.run([binary], capture_output=True, text=True, timeout=30,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert failed.returncode != 0 and 'Assertion' in failed.stderr
    print('Negative control rejected: eager materialization of file and BSS pages')
