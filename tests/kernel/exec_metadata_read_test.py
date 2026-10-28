"""真实 ELF 元数据读取：每个前缀字节只读一次，解释器范围统一扩容并原子发布。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/process/exec.c').read_text()
header = (root / 'kernel/process/exec_internal.h').read_text()
types = header[header.index('typedef struct __attribute__((packed))'):
               header.index('typedef struct elf_validation')]
start = source.index('static const elf64_program_header_t *program_header(')
end = source.index('\n}', start)+2
body = source[source.index('static kstatus_t read_vfs_metadata('):
              source.index('/* VFS exec 会立即读取解释器映像')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum {K_OK=0,K_EINVAL=-22,K_EIO=-5,K_ENOMEM=-12,ELF_PT_INTERP=3,
 ELF_INTERP_MAX=4096,ELF_MAX_PROGRAMS=128};
#define ELF_EXEC_METADATA_SIZE (16ULL*1024ULL*1024ULL)
typedef int kstatus_t;
typedef struct {uint64_t size;} vnode_t;
typedef struct {vnode_t *vnode;} file_t;
''' + types + source[start:end] + r'''
static unsigned char disk[65536],seen[sizeof(disk)];
static unsigned reads,allocations,live,fail_read,fail_alloc;
static size_t transferred;
static bool mutate_header;
static void *kmalloc(size_t n,unsigned flags){
 assert(!flags&&n<=sizeof(disk));if(++allocations==fail_alloc)return NULL;
 void *p=malloc(n);assert(p);++live;return p;
}
static void kfree(void *p){assert(p&&live);--live;free(p);}
static int read_vfs_range(file_t *f,uint64_t offset,void *out,size_t n){
 assert(n&&offset+n<=f->vnode->size&&offset+n<=sizeof(disk));
 if(++reads==fail_read)return K_EIO;
 if(mutate_header&&reads>1)((elf64_header_t *)disk)->program_header_count=UINT16_MAX;
 for(size_t i=0;i<n;++i)assert(!seen[offset+i]++);
 memcpy(out,disk+offset,n);transferred+=n;return K_OK;
}
''' + body + r'''
int main(void){
 unsigned cases=0;
 for(unsigned mutation=0;mutation<2;++mutation)for(unsigned padding=0;padding<2;++padding)
 for(unsigned kind=0;kind<15;++kind)for(unsigned fault=0;fault<6;++fault){
  assert(!live);memset(disk,0x75,sizeof(disk));memset(seen,0,sizeof(seen));
  reads=allocations=0;transferred=0;
  mutate_header=mutation!=0;
  fail_read=fault<4?fault:0;fail_alloc=fault>=4?fault-3:0;
  vnode_t vnode={sizeof(disk)};file_t file={&vnode};
  unsigned stride=sizeof(elf64_program_header_t)+padding*8;
  elf64_header_t h={.program_header_offset=64,.program_header_size=stride,
   .program_header_count=3};
  elf64_program_header_t records[3]={{0}};
  size_t prefix=64+3*stride,required=prefix;
  if(kind){
   records[0]=(elf64_program_header_t){.type=ELF_PT_INTERP,.offset=16,.file_size=4};
   if(kind>=2){records[0].offset=4000;records[0].file_size=27;required=4027;}
   if(kind==3||kind==4){
    records[1]=(elf64_program_header_t){.type=ELF_PT_INTERP,.offset=8000,.file_size=24};required=8024;
    if(kind==4){elf64_program_header_t swap=records[0];records[0]=records[1];records[1]=swap;}
   }
  }
  if(kind==5)records[0].file_size=0;
  if(kind==6)records[0].file_size=ELF_INTERP_MAX+1;
  if(kind==7)records[0].offset=vnode.size+1;
  if(kind==8){records[0].offset=vnode.size-1;records[0].file_size=2;}
  if(kind==9){vnode.size=ELF_EXEC_METADATA_SIZE+32;records[0].offset=ELF_EXEC_METADATA_SIZE;}
  if(kind==10)h.program_header_size=1;
  if(kind==11)h.program_header_count=0;
  if(kind==12)h.program_header_count=ELF_MAX_PROGRAMS+1;
  if(kind==13)h.program_header_offset=vnode.size-1;
  if(kind==14){vnode.size=ELF_EXEC_METADATA_SIZE+4096;h.program_header_offset=ELF_EXEC_METADATA_SIZE;}
  memcpy(disk,&h,sizeof(h));
  for(unsigned i=0;i<3;++i)memcpy(disk+64+i*stride,&records[i],sizeof(records[i]));
  uint8_t *out=(void *)1;size_t size=123;
  int status=read_vfs_metadata(&file,&out,&size);
  if(status==K_OK){
   assert(kind<5&&out&&size==required&&live==1);
   assert(!memcmp(out,&h,sizeof(h))&&transferred==required);
   assert(!memcmp(out+sizeof(h),disk+sizeof(h),size-sizeof(h)));
   assert(allocations==(required>prefix?2U:1U));
   assert(reads==(required>prefix?3U:2U));
   for(size_t i=0;i<required;++i)assert(seen[i]==1);
   kfree(out);
  }else{
   assert(status==K_EIO||status==K_ENOMEM||status==K_EINVAL);
   assert(!out&&!size&&!live);
   if(!fault)assert(kind>=5&&status==K_EINVAL);
  }
  ++cases;
 }
 printf("ELF metadata: %u layouts/read/allocation failures; exact prefix bytes, one extension, retained header, invalid bounds and unpublished-output cleanup PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='exec-metadata-read-') as directory:
    binary = str(Path(directory) / 'test')
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-x', 'c', '-', '-o', binary]
    subprocess.run(command, input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
    old = 'read_vfs_range(file, metadata_size, extended_image + metadata_size,\n                                (size_t)(required_size - metadata_size))'
    assert old in program
    subprocess.run(command, input=program.replace(old,
        'read_vfs_range(file, 0U, extended_image, (size_t)required_size)'), text=True, check=True)
    result = subprocess.run([binary], capture_output=True, text=True, timeout=30,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode != 0 and 'Assertion' in result.stderr, result.stderr
    print('Negative control rejected: rereading the full ELF prefix')
