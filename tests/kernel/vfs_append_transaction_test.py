"""Production append/write/truncate APIs with independently opened handles."""
from pathlib import Path
import subprocess
import tempfile
import resource

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/fs/vfs/file_io.c').read_text()
helpers = source[source.index('static kstatus_t vfs_write_transaction_begin('):
                 source.index('kstatus_t vfs_seek(')]
writes = source[source.index('static kstatus_t vfs_write_chunk('):
                source.index('kstatus_t vfs_fsync(')]
fixture = r'''
#include <assert.h>
/* 单句柄测试只桩化偏移锁；共享句柄并发由专门的回归测试覆盖。 */
#define vfs_file_position_init(file) ((void)(file))
#define vfs_file_position_lock(file) ((void)(file), K_OK)
#define vfs_file_position_unlock(file) ((void)(file))
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#define __user
#define PAGE_SIZE 4096U
#define K_OK 0
#define K_EACCES -13
#define K_EINVAL -22
#define K_EIO -5
#define K_EISDIR -21
#define K_EOVERFLOW -75
#define K_EAGAIN -11
#define K_EPERM -1
#define KOBJECT_TYPE_FILE 1
#define KOBJECT_TYPE_VNODE 2
#define VFS_OPEN_WRITE 2U
#define VFS_OPEN_APPEND 32U
#define RECORD 8321U
#define WRITERS 4U
#define RECORDS 100U
typedef int kstatus_t;
typedef struct {pthread_mutex_t native;} kmutex_t;
typedef struct {
 void *context;uint64_t size;bool directory;kmutex_t write_mutex;
 int(*write)(void*,uint64_t,const void*,size_t,uint64_t*);
 int(*truncate)(void*,uint64_t);
} vfs_backend_node_t;
typedef struct {struct{unsigned type;}object;void *fs_private;uint64_t size;} vnode_t;
typedef struct {struct{unsigned type;}object;vnode_t *vnode;unsigned rights,flags;uint64_t position;} file_t;
static vfs_backend_node_t backend;
static vnode_t node;
static unsigned char disk[WRITERS*RECORDS*RECORD+PAGE_SIZE];
static pthread_mutex_t backend_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cache_mutex=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned transaction_depth,backend_depth,cache_depth,copies;
static _Thread_local int copy_failure,write_failure,lock_failure;
static _Thread_local const unsigned char *copy_fault;
static bool boot,idle_boot;
static void *kmalloc(size_t n,unsigned flags){(void)flags;assert(!transaction_depth);return malloc(n);}
static void kfree(void *p){assert(!transaction_depth);free(p);}
static void *sched_current_thread(void){return boot?NULL:(void*)1;}
static int kmutex_lock(kmutex_t *m,uint64_t timeout){
 assert(timeout==UINT64_MAX&&!transaction_depth&&!backend_depth&&!cache_depth);
 if(idle_boot)return K_EPERM;
 if(lock_failure)return K_EIO;
 assert(!pthread_mutex_lock(&m->native));transaction_depth=1;return K_OK;
}
static int kmutex_try_lock(kmutex_t *m){
 assert(!transaction_depth&&!backend_depth&&!cache_depth);
 if(lock_failure)return K_EAGAIN;
 int result=pthread_mutex_trylock(&m->native);
 if(result==EBUSY)return K_EAGAIN;
 assert(!result);transaction_depth=1;return K_OK;
}
static int kmutex_unlock(kmutex_t *m){
 assert(transaction_depth&&!backend_depth&&!cache_depth);transaction_depth=0;
 assert(!pthread_mutex_unlock(&m->native));return K_OK;
}
static int io_mutex_lock(kmutex_t *m){
 if(!sched_current_thread())return K_OK;
 int status=kmutex_try_lock(m);return status==K_EAGAIN?kmutex_lock(m,UINT64_MAX):status;
}
static void io_mutex_unlock_required(kmutex_t *m){
 if(sched_current_thread())assert(kmutex_unlock(m)==K_OK);
}
static void vfs_backend_lock(vfs_backend_node_t *b){
 assert(b==&backend&&!backend_depth);assert(!pthread_mutex_lock(&backend_mutex));backend_depth=1;
}
static void vfs_backend_unlock(vfs_backend_node_t *b){
 assert(b==&backend&&backend_depth);backend_depth=0;assert(!pthread_mutex_unlock(&backend_mutex));
}
static void vfs_vnode_lock(vnode_t *v){
 assert(v==&node&&!cache_depth&&!backend_depth);assert(!pthread_mutex_lock(&cache_mutex));cache_depth=1;
}
static void vfs_vnode_unlock(vnode_t *v){assert(v==&node&&cache_depth);cache_depth=0;assert(!pthread_mutex_unlock(&cache_mutex));}
static void vfs_page_cache_copy_in(vnode_t *v,uint64_t p,const void *s,size_t n){
 (void)s;assert(transaction_depth||boot);vfs_vnode_lock(v);
 if(p+n>v->size)v->size=p+n;vfs_vnode_unlock(v);
 sched_yield(); // Force competing handles to attempt writes between chunks.
}
static int vfs_page_cache_flush_prefix(vnode_t *v,uint64_t size){assert(size&&v==&node&&cache_depth&&(transaction_depth||boot));return K_OK;}
static void vfs_page_cache_resize(vnode_t *v,uint64_t n){vfs_vnode_lock(v);v->size=n;vfs_vnode_unlock(v);}
static bool x86_user_range_valid(const void *p,size_t n){return p||!n;}
static int copy_from_user(void *d,const void *s,size_t n){
 assert(!backend_depth&&!cache_depth&&(transaction_depth||boot));
 ++copies;
 if(copy_failure && ((uintptr_t)s>=(uintptr_t)copy_fault ||
    n>(uintptr_t)copy_fault-(uintptr_t)s))return K_EACCES;
 memcpy(d,s,n);return K_OK;
}
static int write_backend(void *c,uint64_t p,const void *s,size_t n,uint64_t *got){
 assert(c==&backend&&backend_depth&&(transaction_depth||boot));
 if(write_failure&&p>=PAGE_SIZE)return K_EIO;
 assert(p+n<=sizeof(disk));memcpy(disk+p,s,n);*got=n;
 if(p+n>backend.size)backend.size=p+n;return K_OK;
}
static int truncate_backend(void *c,uint64_t n){assert(c==&backend&&backend_depth&&(transaction_depth||boot));backend.size=n;return K_OK;}
'''
test = r'''
static file_t handle(void){file_t f={{KOBJECT_TYPE_FILE},&node,VFS_OPEN_WRITE,VFS_OPEN_APPEND,0};return f;}
static void *writer(void *argument){
 unsigned id=(unsigned)(uintptr_t)argument;file_t f=handle();unsigned char record[RECORD];memset(record,id,sizeof(record));
 for(unsigned i=0;i<RECORDS;++i){
  f.position=0;uint64_t written=0;
  int result=id&1?vfs_write_kernel(&f,record,sizeof(record),&written):vfs_write(&f,record,sizeof(record),&written);
  assert(result==K_OK&&written==RECORD&&!transaction_depth&&!backend_depth&&!cache_depth);
 }
 return NULL;
}
int main(void){
 assert(!pthread_mutex_init(&backend.write_mutex.native,NULL));backend.context=&backend;
 backend.write=write_backend;backend.truncate=truncate_backend;node.object.type=KOBJECT_TYPE_VNODE;node.fs_private=&backend;
 pthread_t threads[WRITERS];
 for(unsigned i=0;i<WRITERS;++i)assert(!pthread_create(&threads[i],NULL,writer,(void*)(uintptr_t)(i+1)));
 for(unsigned i=0;i<WRITERS;++i)assert(!pthread_join(threads[i],NULL));
 assert(node.size==WRITERS*RECORDS*RECORD&&backend.size==node.size);
 unsigned counts[WRITERS]={0};
 for(size_t offset=0;offset<node.size;offset+=RECORD){
  unsigned id=disk[offset];assert(id>=1&&id<=WRITERS);++counts[id-1];
  for(unsigned j=0;j<RECORD;++j)assert(disk[offset+j]==id);
 }
 for(unsigned i=0;i<WRITERS;++i)assert(counts[i]==RECORDS);
 file_t f=handle(),other=handle();uint64_t written;
 assert(vfs_truncate_node(&node,0)==K_OK&&!transaction_depth);
 assert(vfs_write_kernel(&f,"AA",2,&written)==K_OK&&written==2);
 assert(vfs_write_kernel(&other,"BB",2,&written)==K_OK&&written==2);
 assert(node.size==4&&!memcmp(disk,"AABB",4));
 assert(vfs_truncate_node(&node,1)==K_OK);
 assert(vfs_write(&other,"C",1,&written)==K_OK&&node.size==2&&!memcmp(disk,"AC",2));
 assert(vfs_truncate_node(&node,0)==K_OK);unsigned char record[RECORD];memset(record,7,sizeof(record));
 copy_failure=1;copies=0;copy_fault=record+PAGE_SIZE;
 assert(vfs_write(&f,record,sizeof(record),&written)==K_EACCES&&written==PAGE_SIZE&&!transaction_depth);
 copy_failure=0;write_failure=1;
 assert(vfs_write_kernel(&f,record,1,&written)==K_EIO&&!written&&!transaction_depth);
 write_failure=0;
 assert(vfs_write_kernel(&f,"Z",1,&written)==K_OK&&written==1&&disk[PAGE_SIZE]=='Z');
 f.position=11;lock_failure=1;
 assert(vfs_write_kernel(&f,"x",1,&written)==K_EIO&&!written&&f.position==11&&!transaction_depth);
 lock_failure=0;f.flags=0;f.position=UINT64_MAX;
 assert(vfs_write_kernel(&f,"x",1,&written)==K_EOVERFLOW&&!written&&!transaction_depth);
 f.flags=VFS_OPEN_APPEND;f.position=11;
 assert(vfs_write_kernel(&f,NULL,0,&written)==K_OK&&!written&&f.position==11);
 assert(vfs_truncate_node(&node,0)==K_OK);boot=true;
 assert(vfs_write_kernel(&f,"boot",4,&written)==K_OK&&written==4&&!transaction_depth);
 boot=false;idle_boot=true;
 assert(vfs_write_kernel(&f,"idle",4,&written)==K_OK&&written==4&&!transaction_depth);
 idle_boot=false;assert(!pthread_mutex_destroy(&backend.write_mutex.native));return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-append-transaction-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(fixture+helpers+writes+test)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                    '-pthread','-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=20)
    # 跨越新的内核批次边界，验证不同句柄的追加记录仍不会交错。
    large_fixture = fixture.replace('#define RECORD 8321U', '#define RECORD 1048607U')
    large_fixture = large_fixture.replace('#define RECORDS 100U', '#define RECORDS 3U')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                    '-pthread','-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',str(path/'large')],
                   input=large_fixture+helpers+writes+test,text=True,check=True)
    subprocess.run([str(path/'large')],check=True,timeout=20)
    # Prove the fixture rejects the exact stale-offset bug, not just races
    # introduced by the test scheduler.
    broken = helpers.replace('if ((file->flags & VFS_OPEN_APPEND) != 0U)',
                              'if (false)')
    assert broken != helpers
    (path/'broken.c').write_text(fixture+broken+writes+test)
    subprocess.run(['cc','-std=c11','-pthread',str(path/'broken.c'),
                    '-o',str(path/'broken')],check=True)
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    result = subprocess.run([str(path/'broken')],capture_output=True,
                            timeout=20,preexec_fn=no_core)
    assert result.returncode != 0 and b'node.size==' in result.stderr
print('Append transactions: 400 concurrent multi-page records, separate handles, user/kernel APIs, seek/truncate, partial failure, overflow, zero-write and bootstrap passed')
print('Large append: 12 concurrent 1MiB+31-byte records preserve whole-write serialization across batch boundaries')
