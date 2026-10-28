"""Production getdents baseline: exact records, cursor/errors and VFS calls."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
function = source[source.index('static int64_t linux_getdents64('):source.index('int64_t linux_syscall_dispatch(')]
cache_start = function.index('        if (batch_cursor >= batch_count')
cache_end = function.index('        if (status == K_ENOENT)', cache_start)
previous = function[:cache_start] + '        status = vfs_enumerate_file(entry.file, (uint32_t)index, &info);\n' + function[cache_end:]
previous = previous.replace('static int64_t linux_getdents64(', 'static int64_t previous_getdents(')
previous = previous.replace('    os_file_info_t batch[16U];\n    uint32_t batch_count = 0U;\n    uint32_t batch_cursor = 0U;\n    uint64_t batch_start = 0U;\n', '')
previous = previous.replace('        info = batch[batch_cursor];\n', '').replace('        ++batch_cursor;\n', '')
padding = '''        linux_zero(record + sizeof(header) + name_length + 1U,
                    record_length - sizeof(header) - name_length - 1U);'''
assert padding in previous
previous = previous.replace(padding, '        linux_zero(record, sizeof(record));')
proc = source[source.index('static int64_t linux_proc_getdents64('):source.index('static int64_t linux_getdents64(')]
assert padding in proc
old_proc = proc.replace('linux_proc_getdents64(', 'previous_proc_getdents(').replace(padding, '        linux_zero(record, sizeof(record));')
header_start = source.index('typedef struct linux_dirent64_header {')
header = source[header_start:source.index('linux_dirent64_header_t;', header_start) + len('linux_dirent64_header_t;')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define __user
#define OS_FILE_NAME_MAX 64U
enum {K_OK=0,K_EINVAL=-1,K_ENOENT=-2,K_ENOTDIR=-3,K_EOVERFLOW=-4,K_EIO=-5,LINUX_EFAULT=-6};
enum {LINUX_FD_FILE=1,LINUX_FD_PROC=2,OS_FILE_TYPE_DIRECTORY=2,OS_FILE_TYPE_FIFO=3};
typedef int kstatus_t;
typedef struct {uint64_t position;} file_t;
typedef struct {unsigned type;char name[64];} os_file_info_t;
typedef struct {unsigned kind;file_t *file;void *private_data;} linux_fd_entry_t;
typedef struct {unsigned unused;} process_t;
typedef struct {unsigned node,lock;uint64_t position;} linux_proc_file_t;
''' + header + r'''
static file_t file;
static unsigned entries,enumerations,copies,drops,fail_copy,fail_index;
static bool bad_range;
static unsigned name_length_override;
static size_t zero_bytes;
static unsigned lock_depth;
static unsigned position_lock_depth;
static bool position_lock_failure;
static unsigned kind=LINUX_FD_FILE;
static kstatus_t vfs_file_position_lock(file_t *f){
 assert(f==&file && !position_lock_depth);
 if(position_lock_failure)return K_EIO;
 position_lock_depth=1;return K_OK;
}
static void vfs_file_position_unlock(file_t *f){assert(f==&file&&position_lock_depth==1);position_lock_depth=0;}
static bool x86_user_range_valid(void *p,size_t n){return p&&n&&!bad_range;}
static kstatus_t linux_fd_lookup(process_t *p,int64_t d,linux_fd_entry_t *out){assert(p&&d==3);*out=(linux_fd_entry_t){kind,&file,0};return K_OK;}
static void linux_fd_entry_put(linux_fd_entry_t *e){assert(e->file==&file&&!position_lock_depth);++drops;}
static bool linux_proc_node_is_directory(unsigned node){return node==1;}
static void spinlock_lock(unsigned *lock){(void)lock;assert(!lock_depth);++lock_depth;}
static void spinlock_unlock(unsigned *lock){(void)lock;assert(lock_depth==1);--lock_depth;}
static uint64_t linux_proc_directory_count(linux_proc_file_t *p){assert(p&&lock_depth==1);return entries;}
static bool linux_proc_directory_entry(linux_proc_file_t *p,uint64_t i,char *name,size_t capacity,uint8_t *type,uint64_t *inode){
 assert(p&&lock_depth==1&&capacity==64&&i<entries);
 if(i==fail_index)return false;
 if(name_length_override){memset(name,'a'+i%26,name_length_override);name[name_length_override]=0;}
 else snprintf(name,capacity,"entry%03u",(unsigned)i);
 *type=i%3==0?4:i%3==1?1:8;*inode=i+1;return true;
}
static kstatus_t vfs_enumerate_file(file_t *f,uint32_t i,os_file_info_t *out){
 assert(f==&file&&position_lock_depth==1);++enumerations;if(i==fail_index)return K_EIO;if(i>=entries)return K_ENOENT;
 memset(out,0,sizeof(*out));out->type=i%3==0?OS_FILE_TYPE_DIRECTORY:i%3==1?OS_FILE_TYPE_FIFO:1;
 snprintf(out->name,64,"entry%03u",i);
 if(name_length_override){memset(out->name,'a'+i%26,name_length_override);out->name[name_length_override]=0;}
 return K_OK;
}
static kstatus_t vfs_enumerate_file_batch(file_t *f,uint32_t start,os_file_info_t *out,uint32_t capacity,uint32_t *count){
 assert(f==&file&&capacity==16&&position_lock_depth==1);*count=0;++enumerations;
 if(start==fail_index)return K_EIO;
 if(start>=entries)return K_ENOENT;
 for(unsigned i=start;i<entries&&i<fail_index&&*count<capacity;++i){
  os_file_info_t *info=out+(*count)++;memset(info,0,sizeof(*info));
  info->type=i%3==0?OS_FILE_TYPE_DIRECTORY:i%3==1?OS_FILE_TYPE_FIFO:1;
  snprintf(info->name,64,"entry%03u",i);
  if(name_length_override){memset(info->name,'a'+i%26,name_length_override);info->name[name_length_override]=0;}
 }
 return K_OK;
}
static void linux_zero(void *p,size_t n){zero_bytes+=n;memset(p,0,n);}
static void linux_copy(void *p,const void *s,size_t n){memcpy(p,s,n);}
static kstatus_t copy_to_user(void *p,const void *s,size_t n){++copies;if(copies==fail_copy)return K_EIO;memcpy(p,s,n);return K_OK;}
''' + old_proc + proc + previous + function + r'''
static process_t process;
static uint8_t output[16384];
static void reset(unsigned n){assert(!position_lock_depth);position_lock_failure=false;entries=n;enumerations=copies=drops=0;zero_bytes=0;name_length_override=0;fail_index=UINT32_MAX;fail_copy=0;file.position=0;bad_range=false;kind=LINUX_FD_FILE;memset(output,0x81,sizeof(output));}
static void records(unsigned start,unsigned count){
 for(unsigned i=0;i<count;++i){
  linux_dirent64_header_t h;memcpy(&h,output+i*32,sizeof(h));
  assert(h.inode==start+i+1&&h.offset==start+i+1&&h.record_length==32);
  unsigned type=(start+i)%3;assert(h.type==(type==0?4:type==1?1:8));
  char name[64];snprintf(name,64,"entry%03u",start+i);
  assert(!strcmp((char*)output+i*32+sizeof(h),name));
  for(unsigned j=sizeof(h)+9;j<32;++j)assert(!output[i*32+j]);
 }
 assert(output[count*32]==0x81);
}
static void compare(unsigned n,uint64_t size,unsigned fault,uint64_t start){
 reset(n);fail_copy=fault;file.position=start;
 int64_t expected=previous_getdents(&process,3,(uintptr_t)output,size);
 uint8_t saved[sizeof(output)];memcpy(saved,output,sizeof(output));
 uint64_t cursor=file.position;unsigned reference_drops=drops,copy_calls=copies;
 reset(n);fail_copy=fault;file.position=start;
 assert(linux_getdents64(&process,3,(uintptr_t)output,size)==expected);
 assert(file.position==cursor&&drops==reference_drops&&copies==copy_calls&&!memcmp(output,saved,sizeof(output)));
}
int main(void){
 unsigned cases=0;
 for(unsigned n=0;n<=256;++n){
  reset(n);assert(linux_getdents64(&process,3,(uintptr_t)output,sizeof(output))==(int64_t)n*32);
  assert(file.position==n&&enumerations==(n+15)/16+1&&copies==n&&drops==1);records(0,n);++cases;
  compare(n,sizeof(output),0,0);
 }
 printf("getdents batch: 256 entries require 17 VFS enumerations instead of 257\n");
 for(unsigned capacity=0;capacity<=257;++capacity){
  reset(256);uint64_t size=capacity?capacity:1;
  int64_t got=linux_getdents64(&process,3,(uintptr_t)output,size);
  unsigned count=(unsigned)size/32;
  assert(got==(count?(int64_t)count*32:K_EINVAL)&&file.position==count&&copies==count&&drops==1);
  records(0,count);++cases;
  compare(256,size,0,0);
 }
 for(unsigned failure=0;failure<256;++failure){
  reset(256);fail_index=failure;
  assert(linux_getdents64(&process,3,(uintptr_t)output,sizeof(output))==K_EIO);
  assert(file.position==failure&&copies==failure&&drops==1);records(0,failure);++cases;
  reset(256);fail_copy=failure+1;
  assert(linux_getdents64(&process,3,(uintptr_t)output,sizeof(output))==LINUX_EFAULT);
  assert(file.position==failure&&copies==failure+1&&drops==1);records(0,failure);++cases;
  compare(256,sizeof(output),failure+1,0);
 }
 reset(256);file.position=UINT64_C(1)<<32;
 assert(linux_getdents64(&process,3,(uintptr_t)output,sizeof(output))==K_EOVERFLOW&&!enumerations&&drops==1);
 reset(256);bad_range=true;
 assert(linux_getdents64(&process,3,(uintptr_t)output,sizeof(output))==LINUX_EFAULT&&!enumerations&&!drops);
 reset(256);kind=99;
 assert(linux_getdents64(&process,3,(uintptr_t)output,sizeof(output))==K_ENOTDIR&&drops==1);
 reset(256);position_lock_failure=true;
 assert(linux_getdents64(&process,3,(uintptr_t)output,sizeof(output))==K_EIO&&drops==1&&!enumerations&&!file.position);
 for(unsigned start=0;start<=257;++start)compare(256,sizeof(output),0,start);
 size_t avoided_zero=0;unsigned padding_cases=0;
 for(unsigned length=1;length<64;++length)for(unsigned fault=0;fault<5;++fault)
 for(unsigned capacity=1;capacity<256;++capacity){
  reset(4);name_length_override=length;fail_copy=fault;
  int64_t expected=previous_getdents(&process,3,(uintptr_t)output,capacity);
  uint8_t saved[sizeof(output)];memcpy(saved,output,sizeof(output));
  uint64_t cursor=file.position;unsigned released=drops,copy_calls=copies;size_t before=zero_bytes;
  reset(4);name_length_override=length;fail_copy=fault;
  assert(linux_getdents64(&process,3,(uintptr_t)output,capacity)==expected);
  assert(file.position==cursor&&drops==released&&copies==copy_calls&&!memcmp(output,saved,sizeof(output)));
  assert(zero_bytes<=copy_calls*7&&zero_bytes<=before);avoided_zero+=before-zero_bytes;++padding_cases;
 }
 printf("Directory padding: %u exact record/canary/fault cases across names 1..63; %zu zero writes eliminated; at most 7 initialized padding bytes per copied record\n",padding_cases,avoided_zero);
 unsigned proc_cases=0;
 for(unsigned length=1;length<64;++length)for(unsigned fault=0;fault<5;++fault)
 for(unsigned capacity=1;capacity<256;capacity+=3){
  reset(4);name_length_override=length;fail_copy=fault;lock_depth=0;
  linux_proc_file_t p={.node=1};
  int64_t expected=previous_proc_getdents(&p,(uintptr_t)output,capacity);
  uint8_t saved[sizeof(output)];memcpy(saved,output,sizeof(output));
  uint64_t cursor=p.position;unsigned copy_calls=copies;assert(!lock_depth);
  reset(4);name_length_override=length;fail_copy=fault;p.position=0;
  assert(linux_proc_getdents64(&p,(uintptr_t)output,capacity)==expected);
  assert(p.position==cursor&&copies==copy_calls&&!lock_depth&&!memcmp(output,saved,sizeof(output)));++proc_cases;
 }
 printf("Proc directory padding: %u exact full-output/cursor/copy-failure/lock comparisons passed\n",proc_cases);
 printf("getdents: %u exact record/cursor/error/reference cases passed; partial-buffer and copy failures preserved\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-getdents-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
