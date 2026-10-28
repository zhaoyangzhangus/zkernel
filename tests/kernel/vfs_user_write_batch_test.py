"""验证用户写入的批量大小、追加原子性及各类失败后的已完成进度。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/file_io.c').read_text()
helpers = source[source.index('static kstatus_t vfs_write_transaction_begin('):
                 source.index('kstatus_t vfs_truncate_node(')]
write = source[source.index('static kstatus_t vfs_write_chunk('):
               source.index('kstatus_t vfs_write_kernel(')]
program = r'''
#include <assert.h>
/* 单句柄测试只桩化偏移锁；共享句柄并发由专门的回归测试覆盖。 */
#define vfs_file_position_init(file) ((void)(file))
#define vfs_file_position_lock(file) ((void)(file), K_OK)
#define vfs_file_position_unlock(file) ((void)(file))
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __user
#define PAGE_SIZE 4096U
#define K_OK 0
#define K_EACCES -13
#define K_EIO -5
#define K_EINVAL -22
#define K_EOVERFLOW -75
#define KOBJECT_TYPE_FILE 1
#define VFS_OPEN_WRITE 2U
#define VFS_OPEN_APPEND 32U
typedef int kstatus_t;
typedef struct {int write_mutex; void *context; uint64_t size;
 int (*write)(void *,uint64_t,const void *,size_t,uint64_t *);} vfs_backend_node_t;
typedef struct {void *fs_private;uint64_t size;} vnode_t;
typedef struct {struct {unsigned type;} object;vnode_t *vnode;unsigned rights,flags;uint64_t position;} file_t;
static unsigned transaction,backend_held,vnode_held,calls,copies,allocations,live,cache_copies;
static bool oom,lock_failure;
static int fail_call,short_call,invalid_call;
static size_t bad_offset;
static const unsigned char *input_base;
static unsigned char input[4*65536+4096],disk[sizeof(input)+4096],cache[sizeof(disk)];
static void *kmalloc(size_t n,unsigned flags){
 (void)flags;assert(!transaction&&!backend_held&&n>=4*PAGE_SIZE&&n<=65536);++allocations;
 if(oom)return NULL;void *p=malloc(n);assert(p);++live;return p;
}
static void kfree(void *p){assert(p&&live&&!transaction&&!backend_held);--live;free(p);}
static int io_mutex_lock(int *m){(void)m;assert(!transaction);if(lock_failure)return K_EIO;transaction=1;return K_OK;}
static void io_mutex_unlock_required(int *m){(void)m;assert(transaction&&!backend_held&&!vnode_held);transaction=0;}
static void vfs_backend_lock(vfs_backend_node_t *b){(void)b;assert(transaction&&!backend_held);backend_held=1;}
static void vfs_backend_unlock(vfs_backend_node_t *b){(void)b;assert(backend_held);backend_held=0;}
static void vfs_vnode_lock(vnode_t *v){(void)v;assert(transaction&&!vnode_held&&!backend_held);vnode_held=1;}
static void vfs_vnode_unlock(vnode_t *v){(void)v;assert(vnode_held);vnode_held=0;}
static bool x86_user_range_valid(const void *p,size_t n){return p||!n;}
static int copy_from_user(void *dst,const void *src,size_t n){
 assert(transaction&&!backend_held);++copies;
 size_t offset=(size_t)((const unsigned char *)src-input_base);
 if(offset>=bad_offset || n>bad_offset-offset)return K_EACCES;
 memcpy(dst,src,n);return K_OK;
}
static void vfs_page_cache_copy_in(vnode_t *v,uint64_t pos,const void *src,size_t n){
 assert(transaction&&!backend_held&&pos+n<=sizeof(cache));++cache_copies;
 memcpy(cache+pos,src,n);if(pos+n>v->size)v->size=pos+n;
}
static int backend_write(void *context,uint64_t pos,const void *src,size_t n,uint64_t *written){
 vfs_backend_node_t *backend=context;assert(transaction&&backend_held&&pos+n<=sizeof(disk));
 unsigned call=calls++;
 if((int)call==fail_call)return K_EIO;
 if((int)call==invalid_call){*written=n+1;return K_OK;}
 *written=(int)call==short_call?n/2:n;memcpy(disk+pos,src,*written);
 if(pos+*written>backend->size)backend->size=pos+*written;
 return K_OK;
}
''' + helpers + write + r'''
static void reset(void){
 assert(!transaction&&!backend_held&&!live);
 calls=copies=allocations=cache_copies=0;oom=lock_failure=false;
 fail_call=short_call=invalid_call=-1;bad_offset=sizeof(input);
 memset(disk,0xcc,sizeof(disk));memset(cache,0xcc,sizeof(cache));input_base=input;
}
static file_t make_file(vfs_backend_node_t *backend,vnode_t *node,uint64_t pos,unsigned flags){
 *backend=(vfs_backend_node_t){.context=backend,.size=pos,.write=backend_write};
 *node=(vnode_t){.fs_private=backend,.size=pos};
 return (file_t){.object={KOBJECT_TYPE_FILE},.vnode=node,.rights=VFS_OPEN_WRITE,.flags=flags,.position=pos};
}
static void check_data(uint64_t start,size_t done){
 assert(!memcmp(disk+start,input,done));assert(!memcmp(cache+start,input,done));
 assert(disk[start+done]==0xcc && cache[start+done]==0xcc);
 assert(!transaction&&!backend_held&&!vnode_held&&!live);
}
int main(void){
 for(size_t i=0;i<sizeof(input);i++)input[i]=(unsigned char)(i*37U+(i>>8));
 const size_t sizes[]={0,1,4096,16383,16384,65535,65536,65537,131072,196733};
 for(unsigned out=0;out<2;out++)for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);i++){
  reset();oom=out;vfs_backend_node_t b;vnode_t v;file_t f=make_file(&b,&v,7,0);
  uint64_t done=999;size_t n=sizes[i];assert(vfs_write(&f,input,n,&done)==K_OK);
  assert(done==n&&f.position==7+n);check_data(7,n);
  size_t batch=n>=16384&&!oom?(n<65536?n:65536):4096;
  assert(calls==(n+batch-1)/batch);assert(allocations==(n>=16384));
 }
 reset();vfs_backend_node_t b;vnode_t v;file_t f=make_file(&b,&v,7,0);uint64_t done;
 assert(vfs_write(&f,input,131072,&done)==K_OK&&done==131072&&calls==2);check_data(7,done);
 /* 首批复制失败时退回旧页面粒度，保留故障前已成功写入的页面。 */
 for(unsigned i=0;i<5;i++){
  const size_t faults[]={0,1,4096,65536+2*4096+500,131072};
  reset();f=make_file(&b,&v,7,0);bad_offset=faults[i];
  assert(vfs_write(&f,input,196733,&done)==K_EACCES);
  size_t expected=(faults[i]/PAGE_SIZE)*PAGE_SIZE;
  assert(done==expected&&f.position==7+expected);check_data(7,done);
 }
 /* 第二个大块发生后端错误、短写或无效返回时，只有已确认字节提交到缓存和偏移。 */
 for(unsigned mode=0;mode<3;mode++){
  reset();f=make_file(&b,&v,7,0);
  if(mode==0)fail_call=1;else if(mode==1)short_call=1;else invalid_call=1;
  int status=vfs_write(&f,input,131072,&done);
  size_t expected=65536+(mode==1?32768:0);
  assert(status==(mode==1?K_OK:K_EIO)&&done==expected&&f.position==7+expected);
  assert(cache_copies==(mode==1?2U:1U));check_data(7,done);
 }
 reset();f=make_file(&b,&v,17,VFS_OPEN_APPEND);f.position=3;
 assert(vfs_write(&f,input,65537,&done)==K_OK&&done==65537&&f.position==17+done);
 check_data(17,done);
 reset();f=make_file(&b,&v,0,0);lock_failure=true;
 assert(vfs_write(&f,input,65536,&done)==K_EIO&&done==0&&!live&&!transaction);
 reset();f=make_file(&b,&v,UINT64_MAX,0);
 assert(vfs_write(&f,input,65536,&done)==K_EOVERFLOW&&done==0&&!live&&!transaction);
 puts("User writes: 128 KiB in two backend calls, OOM fallback, boundary sizes, persistent user faults, errors, short writes, append and cleanup PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-user-write-batch-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-Wno-unused-function', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
