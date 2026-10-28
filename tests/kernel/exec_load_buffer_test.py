"""执行真实 ELF 段加载器，验证按需中转、低内存缩批和错误释放。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/process/exec.c').read_text()
body = source[source.index('static kstatus_t map_load_segment_from_vfs('):
              source.index('static kstatus_t map_load_segment(')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define PAGE_SIZE 4096ULL
#define ELF_EXEC_LOAD_CHUNK_SIZE (1024ULL*1024ULL)
#define OS_DEBUG_SERIAL 0
enum {K_OK=0,K_EINVAL=-22,K_ENOMEM=-12,K_EIO=-5,K_EOVERFLOW=-75,ELF_PT_LOAD=1,ELF_PF_WRITE=2,OS_FILE_SEEK_SET=0};
typedef int kstatus_t;
typedef uint64_t vaddr_t;
typedef struct {uint64_t root_table;} vm_space_t;
typedef struct {uint64_t value;} paddr_t;
typedef struct {uint64_t address;unsigned access,flags;} vm_fault_info_t;
typedef struct {void *vnode;uint64_t position;} file_t;
typedef struct {unsigned type,flags;uint64_t virtual_address,memory_size,file_size,offset;} elf64_program_header_t;
static unsigned allocations,live,reads,writes,mode,stop;
static size_t allowed,capacity,peak;
static unsigned char image[3*1024*1024],output[sizeof(image)+4096];
static uint64_t start,completed;
static void *kmalloc(size_t n,unsigned flags){
 assert(!flags&&n&&n<=ELF_EXEC_LOAD_CHUNK_SIZE&&!live);++allocations;
 if(n>allowed)return NULL;
 void *p=malloc(n);assert(p);++live;capacity=n;if(peak<n)peak=n;return p;
}
static void kfree(void *p){assert(p&&live);--live;free(p);}
static bool align_up_page(uint64_t n,uint64_t *out){
 if(n>UINT64_MAX-(PAGE_SIZE-1))return false;
 *out=(n+PAGE_SIZE-1)&~(PAGE_SIZE-1);return true;
}
static unsigned vm_protection(unsigned flags){return flags;}
static unsigned fault_access(unsigned flags){return flags;}
static uint64_t exec_load_start(void){return 0;}
static void exec_load_finish(uint64_t t,unsigned phase,uint64_t n,int status){(void)t;(void)phase;(void)n;(void)status;}
static int map_exec_range(vm_space_t *s,void *v,uint64_t off,vaddr_t address,size_t n,unsigned prot,bool backed){
 assert(s&&!v&&!off&&n&&(prot&ELF_PF_WRITE)&&!backed&&address<=start);return K_OK;
}
static int vfs_seek(file_t *f,int64_t off,unsigned whence,uint64_t *out){
 assert(off>=0&&whence==OS_FILE_SEEK_SET);if(mode==1)return K_EIO;
 f->position=(uint64_t)off;*out=f->position;return K_OK;
}
static int vfs_read_cached_kernel(file_t *f,void *buf,size_t n,uint64_t *bytes){
 assert(live&&n&&n<=capacity&&f->position+n<=sizeof(image));
 unsigned call=reads++;*bytes=n;
 if(call==stop){
  if(mode==2)return K_EIO;
  if(mode==3)*bytes=0;
  if(mode==4)*bytes=n/2;
  if(mode==5){*bytes=n+1;return K_OK;}
 }
 memcpy(buf,image+f->position,(size_t)*bytes);f->position+=*bytes;return K_OK;
}
static int write_address_space(vm_space_t *s,uint64_t address,const void *buf,size_t n){
 assert(s&&address>=start&&address-start+n<=sizeof(output));
 unsigned call=writes++;if(mode==6&&call==stop)return K_EIO;
 memcpy(output+address-start,buf,n);completed+=n;return K_OK;
}
static int read_vfs_range(file_t *f,uint64_t p,void *buf,size_t n){
 (void)f;(void)p;(void)buf;(void)n;assert(false);return K_EIO;
}
static int x86_translate_page(uint64_t root,uint64_t address,paddr_t *p,void *flags){
 (void)root;(void)address;(void)p;(void)flags;assert(false);return K_EIO;
}
static int vm_handle_fault(vm_space_t *s,const vm_fault_info_t *f){(void)s;(void)f;assert(false);return K_EIO;}
'''
main = r'''
int main(void){
 for(unsigned i=0;i<sizeof(image);++i)image[i]=(unsigned char)(i*17+3);
 const size_t sizes[]={0,1,4095,4096,4097,65536,1048575,1048576,1048577,2*1048576+19};
 const size_t limits[]={0,4096,16384,65536,1048576};unsigned cases=0;
 for(unsigned n=0;n<sizeof(sizes)/sizeof(*sizes);++n)
 for(unsigned limit=0;limit<sizeof(limits)/sizeof(*limits);++limit)
 for(unsigned failure=0;failure<7;++failure)for(unsigned point=0;point<3;++point){
  allowed=limits[limit];mode=failure;stop=point;
  allocations=live=reads=writes=0;capacity=peak=0;completed=0;
  memset(output,0x75,sizeof(output));start=0x400000;
  vm_space_t space={0};file_t file={&space,0};
  elf64_program_header_t p={ELF_PT_LOAD,ELF_PF_WRITE,start,sizes[n]+4096,sizes[n],13};
  int status=map_load_segment_from_vfs(&space,&p,0,&file);
  assert(!live&&peak<=allowed&&peak<=sizes[n]&&allocations<=9);
  if(sizes[n]&&!allowed){assert(status==K_ENOMEM&&!reads&&!writes);}
  else if(sizes[n]&&failure==1){assert(status==K_EIO&&!reads&&!writes);}
  else if(status==K_OK){
   assert(!memcmp(output,image+13,sizes[n]));
   size_t zero=(sizes[n]+4095)/4096*4096;
   for(size_t i=sizes[n];i<zero;++i)assert(output[i]==0);
   for(size_t i=zero;i<sizeof(output);++i)assert(output[i]==0x75);
   assert(completed==zero);
   if(sizes[n])assert(reads==(sizes[n]+capacity-1)/capacity);
  }else assert(status==K_EIO);
  if(!sizes[n])assert(!allocations&&!reads&&!writes&&status==K_OK);
  ++cases;
 }
 printf("ELF buffer: %u sizes/OOM/seek/read/short/invalid-count/write cases; bounded retries, exact content, BSS tail and cleanup PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='exec-load-buffer-') as directory:
    binary = str(Path(directory) / 'test')
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-x', 'c', '-', '-o', binary]
    subprocess.run(command, input=prefix+body+main, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
    mutated = body.replace('kmalloc(capacity, 0U)', 'kmalloc(ELF_EXEC_LOAD_CHUNK_SIZE, 0U)')
    assert mutated != body
    subprocess.run(command, input=prefix+mutated+main, text=True, check=True)
    result = subprocess.run([binary], capture_output=True, text=True, timeout=30,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode != 0 and 'Assertion' in result.stderr, result.stderr
    print('Negative control rejected: fixed 1MiB segment buffer')
