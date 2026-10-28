"""真实写入和缓存回写竞争时，较旧映射页不能覆盖已成功写入的数据。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
io = (root / 'kernel/fs/vfs/file_io.c').read_text()
pages = (root / 'kernel/fs/vfs/page_cache.c').read_text()
helpers = io[io.index('static kstatus_t vfs_write_transaction_begin('):
             io.index('/* 直接 I/O 写入会更新页缓存视图')]
write = io[io.index('static kstatus_t vfs_write_chunk('):
           io.index('kstatus_t vfs_write(file_t')]
write += io[io.index('kstatus_t vfs_write_kernel('):io.index('kstatus_t vfs_fsync(')]
lookup = pages[pages.index('static uint32_t vfs_page_cache_bucket('):
               pages.index('static void vfs_vm_mapping_retain(')]
flush = pages[pages.index('static kstatus_t vfs_page_cache_flush_entry('):
              pages.index('static kstatus_t vfs_file_page_get_batch(')]
copy_in = pages[pages.index('static void vfs_page_cache_copy_bytes('):]

code = r'''
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
#define PAGE_SIZE 4096U
#define VFS_PAGE_CACHE_BUCKETS 256U
#define K_OK 0
#define K_EINVAL (-22)
#define K_EIO (-5)
#define K_EACCES (-13)
#define K_EOVERFLOW (-75)
#define KOBJECT_TYPE_FILE 1U
#define VFS_OPEN_WRITE 2U
#define VFS_OPEN_APPEND 4U
typedef int kstatus_t;
typedef pthread_mutex_t io_mutex_t;
typedef struct entry {uint64_t index;void *page;bool dirty;
 struct entry *next,*hash_next;uint64_t writeback_generation;} vfs_cached_page_t;
typedef struct {vfs_cached_page_t *head,*buckets[256],**expanded_buckets;
 uint32_t bucket_count;uint64_t entry_count,growth_retry_at,writeback_generation;} vfs_page_cache_t;
typedef struct {void *context;uint64_t size;
 kstatus_t (*write)(void*,uint64_t,const void*,size_t,uint64_t*);
 kstatus_t (*fsync)(void*);io_mutex_t write_mutex;} vfs_backend_node_t;
typedef struct {void *fs_private,*page_cache;uint64_t size;} vnode_t;
typedef struct {struct {unsigned type;} object;vnode_t *vnode;
 unsigned rights,flags;uint64_t position;} file_t;
static pthread_mutex_t vnode_guard=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t backend_guard=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static _Thread_local unsigned transaction_depth,vnode_depth,backend_depth;
static _Thread_local bool direct_writer,sync_worker;
static bool writer_waiting,sync_reached,sync_without_transaction,mapped,reverse;
static bool sync_waiting,writer_reached;
static unsigned writes,syncs;
static unsigned mode,partial;
static unsigned char disk[PAGE_SIZE],input[PAGE_SIZE],memory[PAGE_SIZE];
static void *kzalloc(size_t n,unsigned flags){(void)flags;return calloc(1,n);}
static void kfree(void *p){free(p);}
static void signal_sync(bool unprotected){
 assert(!pthread_mutex_lock(&gate));sync_reached=true;
 sync_without_transaction=unprotected;pthread_cond_broadcast(&changed);
 assert(!pthread_mutex_unlock(&gate));
}
static kstatus_t io_mutex_lock(io_mutex_t *m){
 assert(!transaction_depth&&!vnode_depth&&!backend_depth);
 if(sync_worker)signal_sync(false);
 if(direct_writer&&reverse){
  assert(!pthread_mutex_lock(&gate));writer_reached=true;pthread_cond_broadcast(&changed);
  assert(!pthread_mutex_unlock(&gate));
 }
 assert(!pthread_mutex_lock(m));transaction_depth=1;return K_OK;
}
static void io_mutex_unlock_required(io_mutex_t *m){
 assert(transaction_depth&&!vnode_depth&&!backend_depth);transaction_depth=0;
 assert(!pthread_mutex_unlock(m));
}
static kstatus_t vfs_file_position_lock(file_t *f){(void)f;return K_OK;}
static void vfs_file_position_unlock(file_t *f){(void)f;}
static void vfs_vnode_lock(vnode_t *v){
 assert(v&&!vnode_depth&&!backend_depth);assert(!pthread_mutex_lock(&vnode_guard));vnode_depth=1;
}
static void vfs_vnode_unlock(vnode_t *v){
 assert(v&&vnode_depth&&!backend_depth);vnode_depth=0;assert(!pthread_mutex_unlock(&vnode_guard));
 if(sync_worker&&!transaction_depth)signal_sync(true);
}
static void vfs_backend_lock(vfs_backend_node_t *b){
 assert(b&&!backend_depth);assert(!pthread_mutex_lock(&backend_guard));backend_depth=1;
}
static void vfs_backend_unlock(vfs_backend_node_t *b){
 assert(b&&backend_depth);backend_depth=0;assert(!pthread_mutex_unlock(&backend_guard));
 if(sync_worker&&reverse&&vnode_depth){
  assert(!pthread_mutex_lock(&gate));sync_waiting=true;pthread_cond_broadcast(&changed);
  while(!writer_reached)assert(!pthread_cond_wait(&changed,&gate));
  assert(!pthread_mutex_unlock(&gate));
 }
 if(direct_writer&&!reverse){
  direct_writer=false;assert(!pthread_mutex_lock(&gate));writer_waiting=true;
  pthread_cond_broadcast(&changed);
  while(!sync_reached)assert(!pthread_cond_wait(&changed,&gate));
  assert(!pthread_mutex_unlock(&gate));
 }
}
static bool vfs_page_cache_still_mapped(vfs_cached_page_t *e){(void)e;return mapped;}
static void *page_to_phys(void *p){return p;}
static void *phys_to_direct(void *p){return p;}
static kstatus_t backend_write(void *ctx,uint64_t off,const void *data,size_t n,uint64_t *bytes){
 (void)ctx;assert(backend_depth&&off+n<=sizeof(disk));++writes;
 memcpy(disk+off,data,n);*bytes=n;return K_OK;
}
static kstatus_t backend_sync(void *ctx){(void)ctx;assert(backend_depth);++syncs;return K_OK;}
''' + lookup + flush + copy_in + helpers + write + r'''
static vfs_backend_node_t backend;
static vnode_t node;
static file_t file;
static void *writer(void *unused){
 (void)unused;uint64_t written=0;
 if(reverse){
  assert(!pthread_mutex_lock(&gate));while(!sync_waiting)assert(!pthread_cond_wait(&changed,&gate));
  assert(!pthread_mutex_unlock(&gate));
 }
 direct_writer=true;
 size_t length=partial?128U:sizeof(input);
 assert(vfs_write_kernel(&file,input,length,&written)==K_OK&&written==length);
 return 0;
}
static void *syncer(void *unused){
 (void)unused;
 if(!reverse){
  assert(!pthread_mutex_lock(&gate));while(!writer_waiting)assert(!pthread_cond_wait(&changed,&gate));
  assert(!pthread_mutex_unlock(&gate));
 }
 sync_worker=true;
 if(mode==2)assert(vfs_page_cache_writeback(&node)==K_OK);
 else assert(vfs_page_cache_sync(&node,0,mode==0?1:UINT64_MAX)==K_OK);
 return 0;
}
int main(int argc,char **argv){
 unsigned cases=0;
 unsigned first=argc>1?(unsigned)strtoul(argv[1],0,10):0U;
 unsigned last=argc>1?first+1U:3U;
 assert(first<3U);
 for(mode=first;mode<last;++mode)for(partial=0;partial<2;++partial)
 for(unsigned m=0;m<2;++m)for(unsigned order=0;order<2;++order){
  mapped=m;reverse=order;sync_waiting=writer_reached=false;
  writer_waiting=sync_reached=sync_without_transaction=false;writes=syncs=0;
  memset(memory,0x28,sizeof(memory));memset(input,0x42,sizeof(input));memset(disk,0x11,sizeof(disk));
  unsigned char expected[PAGE_SIZE];memset(expected,0x28,sizeof(expected));
  size_t off=partial?64U:0U,length=partial?128U:sizeof(input);
  memcpy(expected+off,input,length);
  vfs_page_cache_t cache={0};vfs_cached_page_t entry={.index=0,.page=memory,.dirty=true};
  vfs_page_cache_insert(&cache,&entry);
  backend=(vfs_backend_node_t){.size=sizeof(disk),.write=backend_write,.fsync=backend_sync,
                               .write_mutex=PTHREAD_MUTEX_INITIALIZER};
  node=(vnode_t){&backend,&cache,sizeof(disk)};
  file=(file_t){{KOBJECT_TYPE_FILE},&node,VFS_OPEN_WRITE,0,off};
  pthread_t a,b;assert(!pthread_create(&a,0,writer,0));assert(!pthread_create(&b,0,syncer,0));
  assert(!pthread_join(a,0));assert(!pthread_join(b,0));
  assert(!memcmp(disk,expected,sizeof(disk))&&!memcmp(memory,expected,sizeof(memory)));
  assert(!sync_without_transaction&&entry.dirty==mapped);
  assert(file.position==off+length&&syncs==(mode!=2));
  assert(!pthread_mutex_destroy(&backend.write_mutex));++cases;
 }
 printf("完整和部分直接写入与范围、全文件、全局回写按事务顺序执行：%u 个场景通过\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-writeback-race-') as directory:
    binary = str(Path(directory) / 'test')
    def run(program, negative=False, args=()):
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-pthread', '-fsanitize=address,undefined',
                        '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary],
                       input=program, text=True, check=True)
        result = subprocess.run([binary, *args], capture_output=True, text=True, timeout=8,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        if negative:
            assert result.returncode != 0 and 'memcmp' in result.stderr, result.stderr
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
    run(code)
    assert flush.count('io_mutex_lock(&backend->write_mutex)') == 2
    start = flush.index('kstatus_t vfs_page_cache_sync(')
    split = flush.index('kstatus_t vfs_page_cache_writeback(')
    for section, modes in ((flush[start:split], ('0', '1')), (flush[split:], ('2',))):
        unsafe = section.replace('io_mutex_lock(&backend->write_mutex)', 'K_OK')
        unsafe = unsafe.replace('io_mutex_unlock_required(&backend->write_mutex);', '(void)backend;')
        assert unsafe != section
        for mode_name in modes:
            run(code.replace(section, unsafe), negative=True, args=(mode_name,))
    print('三条路径的负对照通过：缺少事务锁会让旧映射覆盖直接写入')
