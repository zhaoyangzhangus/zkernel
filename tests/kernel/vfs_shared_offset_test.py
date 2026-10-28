"""使用真实读路径验证共享偏移原子性及独立句柄的并发。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/file_io.c').read_text()
read_paths = source[source.index('static kstatus_t vfs_read_backend_chunk('):
                    source.index('static kstatus_t vfs_write_chunk(')]
write_helpers = source[source.index('static kstatus_t vfs_write_transaction_begin('):
                       source.index('kstatus_t vfs_truncate_node(')]
seek_path = source[source.index('kstatus_t vfs_seek('):
                   source.index('kstatus_t vfs_truncate_kernel(')]
write_paths = source[source.index('static kstatus_t vfs_write_chunk('):
                     source.index('kstatus_t vfs_fsync(')]
program = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define __user
#define PAGE_SIZE 4096U
#define KOBJECT_TYPE_FILE 1U
#define VFS_OPEN_READ 1U
#define VFS_OPEN_WRITE 2U
#define VFS_OPEN_APPEND 32U
#define K_OK 0
#define K_EINVAL (-22)
#define K_EIO (-5)
#define K_EACCES (-13)
#define K_EBADF (-9)
#define K_EISDIR (-21)
#define K_EOVERFLOW (-75)
#define K_ENOMEM (-12)
#define OS_FILE_SEEK_SET 0U
#define OS_FILE_SEEK_CURRENT 1U
#define OS_FILE_SEEK_END 2U
typedef int kstatus_t;
typedef pthread_mutex_t io_mutex_t;
typedef struct {
 void *context;uint64_t size;bool directory;io_mutex_t write_mutex;
 kstatus_t (*read)(void *,uint64_t,void *,size_t,uint64_t *);
 kstatus_t (*write)(void *,uint64_t,const void *,size_t,uint64_t *);
} vfs_backend_node_t;
typedef struct {void *fs_private;uint64_t size;} vnode_t;
typedef struct {
 struct {unsigned type;} object;vnode_t *vnode;unsigned rights,flags;
 uint64_t position;io_mutex_t position_lock;
} file_t;
static bool locks_enabled=true;
static kstatus_t io_mutex_lock(io_mutex_t *m){
 return locks_enabled && pthread_mutex_lock(m) ? K_EIO : K_OK;
}
static void io_mutex_unlock_required(io_mutex_t *m){
 if(locks_enabled)assert(!pthread_mutex_unlock(m));
}
static kstatus_t vfs_file_position_lock(file_t *f){return io_mutex_lock(&f->position_lock);}
static void vfs_file_position_unlock(file_t *f){io_mutex_unlock_required(&f->position_lock);}
static bool vfs_page_cache_present(vnode_t *v){(void)v;return false;}
static bool vfs_page_cache_buffer_alias(vnode_t *v,const void *p,size_t n){
 (void)v;(void)p;(void)n;return false;
}
static kstatus_t vfs_backend_consistent_read_lock(vfs_backend_node_t *b){return io_mutex_lock(&b->write_mutex);}
static void vfs_backend_consistent_read_unlock(vfs_backend_node_t *b){io_mutex_unlock_required(&b->write_mutex);}
static kstatus_t vfs_page_cache_overlay_read(vnode_t *v,uint64_t offset,void *buffer,size_t length){
 (void)v;(void)offset;(void)buffer;(void)length;return K_OK;
}
static pthread_mutex_t backend_mutex=PTHREAD_MUTEX_INITIALIZER;
static void vfs_backend_lock(vfs_backend_node_t *b){(void)b;assert(!pthread_mutex_lock(&backend_mutex));}
static void vfs_backend_unlock(vfs_backend_node_t *b){(void)b;assert(!pthread_mutex_unlock(&backend_mutex));}
static void vfs_vnode_lock(vnode_t *v){(void)v;}
static void vfs_vnode_unlock(vnode_t *v){(void)v;}
static void vfs_page_cache_copy_in(vnode_t *v,uint64_t pos,const void *data,size_t len){
 (void)data;if(pos+len>v->size)v->size=pos+len;
}
static bool x86_user_range_valid(const void *p,size_t n){return p||!n;}
static void *kmalloc(size_t n,unsigned flags){(void)flags;return malloc(n);}
static void kfree(void *p){free(p);}
static void vfs_read_kernel_yield(void){}
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static bool first_copy,second_started,second_done,second_before_copy;
static _Thread_local bool first_thread;
static kstatus_t copy_to_user(void *dst,const void *src,size_t n){
 memcpy(dst,src,n);
 if(first_thread){
  assert(!pthread_mutex_lock(&gate));first_copy=true;pthread_cond_broadcast(&changed);
  while(!second_started)assert(!pthread_cond_wait(&changed,&gate));
  struct timespec deadline;assert(!clock_gettime(CLOCK_REALTIME,&deadline));
  deadline.tv_nsec+=300000000L;
  if(deadline.tv_nsec>=1000000000L){++deadline.tv_sec;deadline.tv_nsec-=1000000000L;}
  while(!second_done){int r=pthread_cond_timedwait(&changed,&gate,&deadline);if(r==ETIMEDOUT)break;assert(!r);}
  second_before_copy=second_done;
  assert(!pthread_mutex_unlock(&gate));
 }
 return K_OK;
}
static kstatus_t copy_from_user(void *dst,const void *src,size_t n){memcpy(dst,src,n);return K_OK;}
static unsigned char data[8];
static kstatus_t read_backend(void *context,uint64_t pos,void *out,size_t len,uint64_t *got){
 (void)context;if(pos>=8){*got=0;return K_OK;}
 if(len>8-pos)len=8-pos;memcpy(out,data+pos,len);*got=len;return K_OK;
}
static kstatus_t write_backend(void *context,uint64_t pos,const void *src,size_t len,uint64_t *written){
 (void)context;assert(pos+len<=sizeof(data));memcpy(data+pos,src,len);*written=len;return K_OK;
}
''' + write_helpers + seek_path + read_paths + write_paths + r'''
static file_t a,b;
static unsigned char first_output[4],second_output[4];
static file_t *second_file;
static unsigned operation;
static uint64_t seek_result;
static void *first_worker(void *arg){
 (void)arg;first_thread=true;uint64_t bytes=0;
 assert(vfs_read(&a,first_output,4,&bytes)==K_OK&&bytes==4);return 0;
}
static void *second_worker(void *arg){
 (void)arg;assert(!pthread_mutex_lock(&gate));
 while(!first_copy)assert(!pthread_cond_wait(&changed,&gate));
 second_started=true;pthread_cond_broadcast(&changed);assert(!pthread_mutex_unlock(&gate));
 uint64_t bytes=0;
 if(operation==0)assert(vfs_read_kernel(second_file,second_output,4,&bytes)==K_OK&&bytes==4);
 else if(operation==1)assert(vfs_write_kernel(second_file,"abcd",4,&bytes)==K_OK&&bytes==4);
 else assert(vfs_seek(second_file,2,OS_FILE_SEEK_CURRENT,&seek_result)==K_OK);
 assert(!pthread_mutex_lock(&gate));second_done=true;pthread_cond_broadcast(&changed);
 assert(!pthread_mutex_unlock(&gate));return 0;
}
static void run(bool same,unsigned action){
 first_copy=second_started=second_done=second_before_copy=false;
 memcpy(data,"ABCDEFGH",8);operation=action;seek_result=0;
 a.position=b.position=0;second_file=same?&a:&b;
 pthread_t first,second;assert(!pthread_create(&first,0,first_worker,0));
 assert(!pthread_create(&second,0,second_worker,0));
 assert(!pthread_join(first,0)&&!pthread_join(second,0));
 assert(!memcmp(first_output,"ABCD",4));
 if(same){
  assert(!second_before_copy);
  if(action==0)assert(!memcmp(second_output,"EFGH",4));
  else if(action==1)assert(!memcmp(data,"ABCDabcd",8));
  else assert(seek_result==6);
  assert(a.position==(action==2?6U:8U));
 } else {
  assert(second_before_copy);
  assert(!memcmp(second_output,"ABCD",4));
  assert(a.position==4&&b.position==4);
 }
}
int main(int argc,char **argv){
 (void)argv;if(argc>1)locks_enabled=false;
 vfs_backend_node_t backend={.size=8,.read=read_backend,.write=write_backend};
 vnode_t node={.fs_private=&backend,.size=8};
 a.object.type=b.object.type=KOBJECT_TYPE_FILE;a.vnode=b.vnode=&node;
 a.rights=b.rights=VFS_OPEN_READ|VFS_OPEN_WRITE;
 assert(!pthread_mutex_init(&backend.write_mutex,0));
 assert(!pthread_mutex_init(&a.position_lock,0));assert(!pthread_mutex_init(&b.position_lock,0));
 run(true,0);run(true,1);run(true,2);run(false,0);
 uint64_t bytes=0,position=0;a.position=UINT64_MAX;
 assert(vfs_write_kernel(&a,"x",1,&bytes)==K_EOVERFLOW&&bytes==0);
 assert(!pthread_mutex_trylock(&a.position_lock));assert(!pthread_mutex_unlock(&a.position_lock));
 assert(!pthread_mutex_trylock(&backend.write_mutex));assert(!pthread_mutex_unlock(&backend.write_mutex));
 assert(vfs_seek(&a,1,OS_FILE_SEEK_CURRENT,&position)==K_EOVERFLOW);
 assert(!pthread_mutex_trylock(&a.position_lock));assert(!pthread_mutex_unlock(&a.position_lock));
 assert(!pthread_mutex_destroy(&a.position_lock));assert(!pthread_mutex_destroy(&b.position_lock));
 assert(!pthread_mutex_destroy(&backend.write_mutex));
 puts("Shared offset: read, write, seek, error cleanup and independent handles passed");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-shared-offset-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    binary = path / 'test'
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-Wno-unused-function','-Wno-misleading-indentation','-pthread',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],cwd=path,capture_output=True,text=True,timeout=10)
    assert result.returncode==0,result.stderr
    print(result.stdout.strip())
    old=subprocess.run([str(binary),'without-lock'],cwd=path,capture_output=True,text=True,timeout=10)
    assert old.returncode!=0 and 'second_before_copy' in old.stderr,old.stderr
    print('负对照通过：缺少共享偏移锁时，两次读取会重叠')
