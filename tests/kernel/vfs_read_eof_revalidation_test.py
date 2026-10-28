"""Production reads revalidate EOF under the backend lock without losing progress."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/fs/vfs/file_io.c').read_text()
production = source[source.index('static void vfs_read_kernel_yield('):source.index('static kstatus_t vfs_write_chunk(')]
program = r'''
#include <assert.h>
/* 非缓存测试只桩化读取合并；映射一致性由单独的真实代码回归测试覆盖。 */
#define vfs_page_cache_present(vnode) ((void)(vnode), false)
#define K_ENOMEM (-12)
#define vfs_page_cache_buffer_alias(vnode,buffer,length) \
    ((void)(vnode),(void)(buffer),(void)(length),false)
#define vfs_backend_consistent_read_lock(backend) ((void)(backend), K_OK)
#define vfs_backend_consistent_read_unlock(backend) ((void)(backend))
#define vfs_page_cache_overlay_read(vnode,offset,buffer,length) \
    ((void)(vnode),(void)(offset),(void)(buffer),(void)(length),K_OK)
/* 单句柄测试只桩化偏移锁；共享句柄并发由专门的回归测试覆盖。 */
#define vfs_file_position_init(file) ((void)(file))
#define vfs_file_position_lock(file) ((void)(file), K_OK)
#define vfs_file_position_unlock(file) ((void)(file))
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
static void *kmalloc(size_t n,unsigned flags){(void)flags;return malloc(n);}
static void kfree(void *p){free(p);}
#define __user
#define PAGE_SIZE 4096U
enum {K_OK=0,K_EINVAL=-22,K_EIO=-5,K_EACCES=-13,KOBJECT_TYPE_FILE=1,VFS_OPEN_READ=1};
typedef int kstatus_t;
typedef struct {unsigned sched_class;} thread_t;
#define SCHED_CLASS_IDLE 2U
static thread_t *sched_current_thread(void){return NULL;}
static bool sched_preempt_disabled(void){return false;}
static void schedule(void){assert(!"boot test must not yield");}
typedef struct {
 void *context;uint64_t size;
 kstatus_t (*read)(void *,uint64_t,void *,size_t,uint64_t *);
} vfs_backend_node_t;
typedef struct {void *fs_private;} vnode_t;
typedef struct {struct {unsigned type;} object;vnode_t *vnode;unsigned rights;uint64_t position;} file_t;
static unsigned depth,locks,calls,mode;
static uint64_t current_position;
static unsigned char disk[16384];
static void vfs_backend_lock(vfs_backend_node_t *b){
 assert(!depth);depth=1;++locks;
 // Model truncate winning the lock before the first or second chunk.
 if(mode==1 && locks==1)b->size=0;
 if(mode==2 && calls==1)b->size=current_position?current_position-1:0;
}
static void vfs_backend_unlock(vfs_backend_node_t *b){(void)b;assert(depth);depth=0;}
static bool x86_user_range_valid(const void *p,size_t len){return p || !len;}
static kstatus_t copy_to_user(void *dst,const void *src,size_t len){assert(!depth);memcpy(dst,src,len);return K_OK;}
static kstatus_t read_backend(void *raw,uint64_t offset,void *dst,size_t len,uint64_t *out){
 vfs_backend_node_t *b=raw;assert(depth==1);++calls;
 // Memory backends reject an offset past EOF; invoking them there is the old bug.
 if(offset>b->size)return K_EINVAL;
 assert(offset<b->size); // EOF itself must not perform a redundant backend lookup.
 uint64_t got=b->size-offset;if(got>len)got=len;
 assert(offset+got<=sizeof(disk));memcpy(dst,disk+offset,(size_t)got);*out=got;
 current_position=offset+got;return K_OK;
}
''' + production + r'''
int main(void){
 const uint64_t sizes[]={0,1,4095,4096,4097,8192,12288,16384};
 const uint64_t offsets[]={0,1,4095,4096,8191,8192,16384,UINT64_MAX};
 const size_t lengths[]={0,1,4095,4096,4097,8192,12289,20000};
 unsigned char buffer[20032];unsigned cases=0;
 for(unsigned i=0;i<sizeof(disk);++i)disk[i]=(unsigned char)(i*17+3);
 for(unsigned kernel=0;kernel<2;++kernel)for(unsigned s=0;s<8;++s)
 for(unsigned o=0;o<8;++o)for(unsigned n=0;n<8;++n)for(unsigned m=0;m<3;++m){
  vfs_backend_node_t b={.size=sizes[s],.read=read_backend};b.context=&b;
  vnode_t v={&b};file_t f={{KOBJECT_TYPE_FILE},&v,VFS_OPEN_READ,offsets[o]};
  calls=depth=locks=0;mode=m;current_position=f.position;memset(buffer,0xa5,sizeof(buffer));
  uint64_t expected=offsets[o]<sizes[s]?sizes[s]-offsets[o]:0;
  if(expected>lengths[n])expected=lengths[n];
  size_t unit=expected>=4U*PAGE_SIZE?(size_t)expected:PAGE_SIZE;
  if(m==1)expected=0;
  if(m==2 && !kernel && expected>unit)expected=unit;
  uint64_t bytes=123;
  int status=kernel?vfs_read_kernel(&f,buffer+16,lengths[n],&bytes):vfs_read(&f,buffer+16,lengths[n],&bytes);
  assert(status==K_OK && bytes==expected && f.position==offsets[o]+expected && !depth);
  unsigned expected_calls=expected?kernel?1:(unsigned)((expected+unit-1)/unit):0;
  assert(calls==expected_calls);
  for(unsigned i=0;i<sizeof(buffer);++i)
   assert(buffer[i]==(i>=16 && i<16+expected?disk[offsets[o]+i-16]:0xa5));
  ++cases;
 }
 printf("EOF revalidation: %u user/kernel read cases, truncate-before-lock/between-chunks, exact EOF, no redundant backend calls and progress preservation passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-read-eof-') as directory:
    path = Path(directory)
    def run(name, code, fail=False):
        (path/(name+'.c')).write_text(code)
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path/(name+'.c')), '-o', str(path/name)], check=True)
        result=subprocess.run([str(path/name)], capture_output=True, text=True,
                              preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        if fail:
            assert result.returncode != 0 and 'Assertion' in result.stderr, result.stderr
            print('Negative control rejected: '+name)
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
    run('production', program)
    guard='status = position >= backend->size ? K_OK :'
    assert guard in program
    run('unrevalidated-eof', program.replace(guard,'status ='), True)
