"""Actual native pipe syscall staging: inline/heap boundaries and cleanup."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/pipe.c').read_text()
source = source[source.index('static int64_t syscall_pipe_transfer('):]
program = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/mm.h>
#include <kernel/pipe.h>
#include <kernel/process.h>
static process_t process;
static process_t *active=&process;
static pipe_endpoint_t endpoint;
static unsigned allocations,frees,puts_count,transfers,lookups;
static bool fail_alloc,invalid_endpoint,from_fault,to_fault,result_fault;
static kstatus_t lookup_status,io_status;
static unsigned char user_data[OS_PIPE_DEFAULT_SIZE+32];
static uint64_t result_bytes;
static size_t expected_length,completed_length;
static uint64_t expected_timeout=123;
static bool nested_read;
static int64_t call(bool write,size_t length);
process_t *current_process(void) {return active;}
void *kmalloc(size_t n,uint32_t flags) {
 (void)flags;if(fail_alloc)return 0;++allocations;return malloc(n);
}
void kfree(void *p) {if(p){++frees;free(p);}}
void object_put(void *p) {assert(p==&endpoint);++puts_count;}
kstatus_t handle_lookup(handle_table_t *table,handle_t handle,uint32_t rights,void **out) {
 assert(table==&process.handles && handle==7);
 assert(rights==(endpoint.read_end?PIPE_RIGHT_READ:PIPE_RIGHT_WRITE));
 ++lookups;if(lookup_status)return lookup_status;*out=&endpoint;return K_OK;
}
bool pipe_is_endpoint(const void *p) {assert(p==&endpoint);return !invalid_endpoint;}
bool pipe_endpoint_is_read(const pipe_endpoint_t *p) {assert(p==&endpoint);return p->read_end;}
kstatus_t copy_from_user(void *to,const void __user *from,size_t n) {
 assert(from==user_data && n==expected_length);
 if(from_fault)return K_EACCES;memcpy(to,from,n);return K_OK;
}
kstatus_t copy_to_user(void __user *to,const void *from,size_t n) {
 if(to==&result_bytes) {
  assert(n==sizeof(result_bytes));if(result_fault)return K_EACCES;
 } else {assert(to==user_data && n==completed_length);if(to_fault)return K_EACCES;}
 memcpy(to,from,n);return K_OK;
}
static kstatus_t transfer(pipe_endpoint_t *p,void *data,size_t length,uint64_t timeout,uint64_t *bytes,bool write) {
 assert(p==&endpoint && length==expected_length && timeout==expected_timeout);
 assert((length==0)==(data==0));++transfers;
 if(io_status)return io_status;
 assert(completed_length<=length);
 if(write && nested_read) {
  unsigned char saved[256];assert(length==sizeof(saved));nested_read=false;
  memcpy(saved,user_data,sizeof(saved));
  assert(call(false,sizeof(saved))==K_OK);
  memcpy(user_data,saved,sizeof(saved));
 }
 if(write && length)assert(!memcmp(data,user_data,length));
 else for(size_t i=0;i<completed_length;++i)((unsigned char*)data)[i]=(unsigned char)(i*17+9);
 *bytes=completed_length;return K_OK;
}
kstatus_t pipe_read(pipe_endpoint_t *p,void *data,size_t n,uint64_t t,uint64_t *bytes) {
 return transfer(p,data,n,t,bytes,false);
}
kstatus_t pipe_write(pipe_endpoint_t *p,const void *data,size_t n,uint64_t t,uint64_t *bytes) {
 return transfer(p,(void*)data,n,t,bytes,true);
}
''' + source + r'''
static int64_t call(bool write,size_t length) {
 endpoint.read_end=!write;
 expected_length=length>OS_PIPE_DEFAULT_SIZE?OS_PIPE_DEFAULT_SIZE:length;
 return syscall_pipe_transfer(write,7,(uint64_t)(uintptr_t)user_data,length,
                              expected_timeout,(uint64_t)(uintptr_t)&result_bytes);
}
int main(void) {
 for(unsigned write=0;write<2;++write) {
  for(size_t length=0;length<=OS_PIPE_DEFAULT_SIZE+1;++length) {
   expected_length=length>OS_PIPE_DEFAULT_SIZE?OS_PIPE_DEFAULT_SIZE:length;
   completed_length=expected_length;result_bytes=UINT64_MAX;
   for(size_t i=0;i<sizeof(user_data);++i)user_data[i]=(unsigned char)(i*11+3);
   unsigned a=allocations,f=frees,p=puts_count,t=transfers;
   assert(call(write,length)==K_OK && result_bytes==completed_length);
   assert(allocations==a+(length>256) && frees==f+(length>256));
   assert(puts_count==p+1 && transfers==t+1);
   if(!write)for(size_t i=0;i<completed_length;++i)assert(user_data[i]==(unsigned char)(i*17+9));
   for(size_t i=completed_length;i<sizeof(user_data);++i)assert(user_data[i]==(unsigned char)(i*11+3));
  }
 }
 // 嵌套或交错休眠的系统调用不得覆盖另一调用的暂存区。
 completed_length=256;nested_read=true;
 unsigned a=allocations,t=transfers;
 assert(call(true,256)==K_OK && !nested_read && allocations==a && transfers==t+2);
 // 分配器失败不得影响内嵌读取/写入或零字节调用。
 fail_alloc=true;
 for(unsigned write=0;write<2;++write) {
  for(size_t length=0;length<=256;++length) {
   completed_length=length;assert(call(write,length)==K_OK);
  }
  completed_length=257;unsigned t=transfers,p=puts_count;
  assert(call(write,257)==K_ENOMEM && transfers==t && puts_count==p+1);
 }
 fail_alloc=false;
 // 覆盖暂存阈值两侧的所有故障退出路径。
 for(size_t length=256;length<=257;++length) {
  completed_length=length;
  from_fault=true;unsigned t=transfers;
  assert(call(true,length)==K_EACCES && transfers==t);from_fault=false;
  to_fault=true;result_bytes=99;
  assert(call(false,length)==K_EACCES && result_bytes==99);to_fault=false;
  result_fault=true;
  assert(call(true,length)==K_EACCES);assert(call(false,length)==K_EACCES);
  result_fault=false;
  for(unsigned write=0;write<2;++write) {
   io_status=K_ETIMEDOUT;assert(call(write,length)==K_ETIMEDOUT);
   io_status=K_EPIPE;assert(call(write,length)==K_EPIPE);io_status=K_OK;
   completed_length=length/2;assert(call(write,length)==K_OK && result_bytes==length/2);
   completed_length=0;assert(call(write,length)==K_OK && result_bytes==0);
  }
 }
 invalid_endpoint=true;assert(call(false,1)==K_EBADF);invalid_endpoint=false;
 unsigned p=puts_count;lookup_status=K_EBADF;
 assert(call(true,1)==K_EBADF && puts_count==p);lookup_status=K_OK;
 unsigned l=lookups;active=0;
 assert(call(false,1)==K_EINVAL && lookups==l);active=&process;
 assert(allocations==frees);
 printf("PASS: %u read/write sizes, 514 allocator-independent small calls, isolated nested staging, heap/OOM, user faults, partial/EOF/error results and endpoint cleanup\n",2U*(OS_PIPE_DEFAULT_SIZE+2U));
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-pipe-staging-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'include'), '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all',
                    '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True)
