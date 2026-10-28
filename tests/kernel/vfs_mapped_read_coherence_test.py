"""真实普通读取必须看到尚未回写的共享映射，并保留连续后端读取。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
io = (root / 'kernel/fs/vfs/file_io.c').read_text()
internal = (root / 'kernel/fs/vfs/internal.h').read_text()
assert 'return io_mutex_lock(&backend->write_mutex);' in internal
assert 'io_mutex_lock(&backend->write_mutex)' in io
reads = io[io.index('static kstatus_t vfs_read_backend_chunk('):
           io.index('static kstatus_t vfs_write_chunk(')]
cache = (root / 'kernel/fs/vfs/page_cache.c').read_text()
alias_marker = 'bool vfs_page_cache_buffer_alias('
alias = ''
if alias_marker in cache:
    start = cache.index(alias_marker)
    alias = cache[start:cache.index('\n}', start) + 2]
marker = 'kstatus_t vfs_page_cache_overlay_read('
overlay = ''
if marker in cache:
    start = cache.index(marker)
    overlay = cache[start:cache.index('\n}', start) + 2]

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
#define K_OK 0
#define K_EINVAL (-22)
#define K_EIO (-5)
#define K_EACCES (-13)
#define K_ENOMEM (-12)
#define PAGE_OWNER_FILE 4U
typedef int kstatus_t;
typedef uintptr_t vaddr_t;
typedef struct {uint64_t value;} paddr_t;
static paddr_t paddr_make(uint64_t v){return (paddr_t){v};}
typedef struct {unsigned char data[PAGE_SIZE];unsigned owner;struct {struct {void *mapping;} file;} u;} page_t;
typedef struct vfs_cached_page {
 uint64_t index;page_t *page;bool dirty;struct vfs_cached_page *next;
} vfs_cached_page_t;
typedef struct {vfs_cached_page_t *head;uint64_t entry_count;} vfs_page_cache_t;
typedef struct {
 void *context;uint64_t size;
 kstatus_t (*read)(void *,uint64_t,void *,size_t,uint64_t *);
} vfs_backend_node_t;
typedef struct {void *fs_private,*page_cache;uint64_t size;} vnode_t;
typedef struct {struct {unsigned type;} object;vnode_t *vnode;unsigned rights;uint64_t position;} file_t;
static _Thread_local unsigned backend_depth,vnode_depth,transaction_depth;
static unsigned backend_reads,cache_lookups;
static unsigned transaction_acquisitions;
static bool fail_transaction,fail_allocation,force_translation,fail_translation;
static vnode_t *publish_node;
static void *publish_cache;
static page_t *alias_first,*alias_third;
static pthread_mutex_t transaction_guard=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t backend_guard=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t vnode_guard=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static _Thread_local bool controlled_reader;
static bool reader_waiting,writer_started,writer_done,writer_entered_early;
static kstatus_t vfs_file_position_lock(file_t *f){(void)f;return K_OK;}
static void vfs_file_position_unlock(file_t *f){(void)f;}
static bool vfs_page_cache_present(vnode_t *v){return __atomic_load_n(&v->page_cache,__ATOMIC_ACQUIRE)!=0;}
static kstatus_t vfs_backend_consistent_read_lock(vfs_backend_node_t *b){
 (void)b;assert(!transaction_depth);
 if(fail_transaction)return K_EIO;
 assert(!pthread_mutex_lock(&transaction_guard));transaction_depth=1;++transaction_acquisitions;return K_OK;
}
static void vfs_backend_consistent_read_unlock(vfs_backend_node_t *b){
 (void)b;assert(transaction_depth&&!vnode_depth&&!backend_depth);transaction_depth=0;
 assert(!pthread_mutex_unlock(&transaction_guard));
}
static void vfs_backend_lock(vfs_backend_node_t *b){
 (void)b;assert(!backend_depth);assert(!pthread_mutex_lock(&backend_guard));backend_depth=1;
}
static void vfs_backend_unlock(vfs_backend_node_t *b){
 (void)b;assert(backend_depth);backend_depth=0;assert(!pthread_mutex_unlock(&backend_guard));
 if(controlled_reader){
  controlled_reader=false;assert(!pthread_mutex_lock(&gate));
  reader_waiting=true;pthread_cond_broadcast(&changed);
  while(!writer_started)assert(!pthread_cond_wait(&changed,&gate));
  struct timespec deadline;assert(!clock_gettime(CLOCK_REALTIME,&deadline));deadline.tv_nsec+=300000000L;
  if(deadline.tv_nsec>=1000000000L){++deadline.tv_sec;deadline.tv_nsec-=1000000000L;}
  while(!writer_done){int r=pthread_cond_timedwait(&changed,&gate,&deadline);if(r==ETIMEDOUT)break;assert(!r);}
  writer_entered_early=writer_done;assert(!pthread_mutex_unlock(&gate));
 }
}
static void vfs_vnode_lock(vnode_t *v){
 (void)v;assert(!vnode_depth&&!backend_depth);assert(!pthread_mutex_lock(&vnode_guard));vnode_depth=1;
}
static void vfs_vnode_unlock(vnode_t *v){
 (void)v;assert(vnode_depth);vnode_depth=0;assert(!pthread_mutex_unlock(&vnode_guard));
}
static vfs_cached_page_t *vfs_page_cache_lookup(vfs_page_cache_t *c,uint64_t index){
 assert(vnode_depth);++cache_lookups;
 for(vfs_cached_page_t *e=c?c->head:0;e;e=e->next)if(e->index==index)return e;
 return 0;
}
static bool vfs_page_cache_use_range_index(vfs_page_cache_t *c,uint64_t count){
 return count<=16U || count<=c->entry_count/4U;
}
static uintptr_t page_to_phys(page_t *p){assert(vnode_depth);return (uintptr_t)p;}
static void *phys_to_direct(uintptr_t p){assert(vnode_depth);return (void *)p;}
static paddr_t direct_to_phys(const void *p){return paddr_make(force_translation?UINT64_MAX:(uintptr_t)p);}
static paddr_t x86_current_root_table(void){return paddr_make(1);}
static kstatus_t x86_translate_page(paddr_t root,vaddr_t address,paddr_t *physical,void *flags){
 (void)root;(void)flags;if(fail_translation)return K_EIO;*physical=paddr_make(address);return K_OK;
}
static page_t *phys_to_page(paddr_t p){
 if(alias_first&&p.value>=(uintptr_t)alias_first->data&&p.value-(uintptr_t)alias_first->data<PAGE_SIZE)return alias_first;
 if(alias_third&&p.value>=(uintptr_t)alias_third->data&&p.value-(uintptr_t)alias_third->data<PAGE_SIZE)return alias_third;
 return 0;
}
static bool x86_user_range_valid(const void *p,size_t n){return p||!n;}
static int copy_to_user(void *dst,const void *src,size_t n){memcpy(dst,src,n);return K_OK;}
static void *kmalloc(size_t n,unsigned flags){(void)flags;return fail_allocation?0:malloc(n);}
static void kfree(void *p){free(p);}
static void vfs_read_kernel_yield(void){}
static unsigned char disk[64*PAGE_SIZE+321],expected[sizeof(disk)];
static kstatus_t read_backend(void *context,uint64_t pos,void *out,size_t len,uint64_t *got){
 (void)context;assert(backend_depth&&pos<sizeof(disk));++backend_reads;
 if(len>sizeof(disk)-pos)len=sizeof(disk)-pos;
 memcpy(out,disk+pos,len);*got=len;
 if(publish_node){__atomic_store_n(&publish_node->page_cache,publish_cache,__ATOMIC_RELEASE);publish_node=0;}
 return K_OK;
}
''' + alias + '\n' + overlay + '\n' + reads + r'''
static vfs_backend_node_t *controlled_backend;
static vnode_t *controlled_node;
static page_t *controlled_page;
static file_t *controlled_file;
static unsigned char controlled_output[64];
static void *reader_thread(void *unused){
 (void)unused;controlled_reader=true;uint64_t bytes=0;
 assert(vfs_read_kernel(controlled_file,controlled_output,sizeof(controlled_output),&bytes)==K_OK);
 assert(bytes==sizeof(controlled_output));return 0;
}
static void *writer_thread(void *unused){
 (void)unused;assert(!pthread_mutex_lock(&gate));
 while(!reader_waiting)assert(!pthread_cond_wait(&changed,&gate));
 writer_started=true;pthread_cond_broadcast(&changed);assert(!pthread_mutex_unlock(&gate));
 assert(vfs_backend_consistent_read_lock(controlled_backend)==K_OK);
 vfs_backend_lock(controlled_backend);memset(disk,0xe4,sizeof(controlled_output));vfs_backend_unlock(controlled_backend);
 vfs_vnode_lock(controlled_node);memset(controlled_page->data,0xe4,sizeof(controlled_output));vfs_vnode_unlock(controlled_node);
 vfs_backend_consistent_read_unlock(controlled_backend);
 assert(!pthread_mutex_lock(&gate));writer_done=true;pthread_cond_broadcast(&changed);assert(!pthread_mutex_unlock(&gate));
 return 0;
}
int main(int argc,char **argv){
 (void)argv;
 for(unsigned i=0;i<sizeof(disk);++i)disk[i]=(unsigned char)(i*17+3);
 memcpy(expected,disk,sizeof(disk));
 _Alignas(PAGE_SIZE) page_t first,third;memcpy(first.data,disk,PAGE_SIZE);
 memcpy(third.data,disk+2*PAGE_SIZE,PAGE_SIZE);
 memset(first.data+29,0x93,131);memset(expected+29,0x93,131);
 memset(third.data+11,0x51,211);memset(expected+2*PAGE_SIZE+11,0x51,211);
 vfs_cached_page_t c3={2,&third,true,0},c1={0,&first,false,&c3};
 vfs_page_cache_t cache={&c1,2};
 vfs_backend_node_t backend={0,sizeof(disk),read_backend};
 vnode_t node={&backend,&cache,sizeof(disk)};
 first.owner=third.owner=PAGE_OWNER_FILE;first.u.file.mapping=third.u.file.mapping=&node;
 alias_first=&first;alias_third=&third;
 file_t file={{KOBJECT_TYPE_FILE},&node,VFS_OPEN_READ,0};
 const unsigned starts[]={0,1,28,29,159,160,4090,4096,8190,8200,8403,12000};
 const unsigned lengths[]={1,31,132,511,4096,9000,14000,131072,262144,300000};
 unsigned cases=0;unsigned char out[sizeof(disk)+32];
 for(unsigned kernel=0;kernel<2;++kernel)for(unsigned s=0;s<sizeof(starts)/sizeof(*starts);++s)
 for(unsigned n=0;n<sizeof(lengths)/sizeof(*lengths);++n){
  file.position=starts[s];memset(out,0xa5,sizeof(out));uint64_t bytes=123;
  size_t count=sizeof(disk)-starts[s];if(count>lengths[n])count=lengths[n];
  int status=kernel?vfs_read_kernel(&file,out+16,lengths[n],&bytes):
                    vfs_read(&file,out+16,lengths[n],&bytes);
  assert(status==K_OK&&bytes==count&&file.position==starts[s]+count);
  assert(!memcmp(out+16,expected+starts[s],count));
  for(unsigned i=0;i<16;++i)assert(out[i]==0xa5);
  for(size_t i=16+count;i<sizeof(out);++i)assert(out[i]==0xa5);
  assert(!backend_depth&&!vnode_depth&&!transaction_depth);++cases;
 }
 assert(backend_reads>0);
 /* 内核目的缓冲若正是映射页，后端读取不能先覆盖尚未回写的源内容。 */
 assert(vfs_page_cache_buffer_alias(&node,first.data+7,64));
 assert(!vfs_page_cache_buffer_alias(&node,out,64));
 force_translation=true;assert(vfs_page_cache_buffer_alias(&node,first.data+7,64));
 fail_translation=true;assert(vfs_page_cache_buffer_alias(&node,out,64));
 fail_translation=force_translation=false;
 file.position=0;uint64_t alias_bytes=99;unsigned old_calls=backend_reads;
 fail_allocation=true;
 assert(vfs_read_kernel(&file,first.data,160,&alias_bytes)==K_ENOMEM&&!alias_bytes&&!file.position&&backend_reads==old_calls);
 fail_allocation=false;
 file.position=0;alias_bytes=0;
 assert(vfs_read_kernel(&file,first.data,160,&alias_bytes)==K_OK&&alias_bytes==160);
 assert(!memcmp(first.data,expected,160));
 uint64_t bytes=0;
 if(argc==1){
 /* 未映射文件保持一次后端读取，不获取额外事务锁。 */
 __atomic_store_n(&node.page_cache,0,__ATOMIC_RELEASE);
 file.position=0;unsigned calls=backend_reads,transactions=transaction_acquisitions;
 assert(vfs_read_kernel(&file,out,160,&bytes)==K_OK&&bytes==160&&!memcmp(out,disk,160));
 assert(backend_reads==calls+1&&transaction_acquisitions==transactions);
 /* 读取中首次发布缓存时，重读并合并，不能返回首次读取的旧数据。 */
 file.position=0;calls=backend_reads;transactions=transaction_acquisitions;
 publish_node=&node;publish_cache=&cache;
 assert(vfs_read_kernel(&file,out,160,&bytes)==K_OK&&bytes==160&&!memcmp(out,expected,160));
 assert(backend_reads==calls+2&&transaction_acquisitions==transactions+1&&!publish_node);
 /* 事务锁失败不改变文件偏移，也不读取后端。 */
 file.position=0;bytes=99;calls=backend_reads;fail_transaction=true;
 assert(vfs_read_kernel(&file,out,160,&bytes)==K_EIO&&!bytes&&!file.position&&backend_reads==calls);
 fail_transaction=false;
 }
 /* 直接写入的后端更新和缓存更新不能插入同一次读取的两个阶段之间。 */
 controlled_backend=&backend;controlled_node=&node;controlled_page=&first;controlled_file=&file;
 file.position=0;pthread_t reader,writer;
 assert(!pthread_create(&reader,0,reader_thread,0));assert(!pthread_create(&writer,0,writer_thread,0));
 assert(!pthread_join(reader,0));assert(!pthread_join(writer,0));
 assert(!writer_entered_early&&!memcmp(controlled_output,expected,sizeof(controlled_output)));
 file.position=0;bytes=0;
 assert(vfs_read_kernel(&file,controlled_output,sizeof(controlled_output),&bytes)==K_OK&&bytes==sizeof(controlled_output));
 for(unsigned i=0;i<sizeof(controlled_output);++i)assert(controlled_output[i]==0xe4);
 printf("共享映射读取：%u 个跨页、边界、EOF 及双 API 场景通过\n",cases);
 puts("未映射快速路径、读取期间建立映射及锁失败恢复通过");
 puts("读取和直接写入的后端/缓存更新保持完整事务顺序");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-mapped-read-') as directory:
    path = Path(directory)
    binary = path / 'test'
    command = ['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-Wno-unused-function','-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',str(binary)]
    def run(code, fail=None, args=()):
        subprocess.run(command,input=code,text=True,check=True)
        result=subprocess.run([str(binary),*args],capture_output=True,text=True,timeout=10,
                              preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        if fail:
            assert result.returncode != 0 and fail in result.stderr,result.stderr
        else:
            assert result.returncode == 0,result.stderr
            print(result.stdout.strip())
    run(program)
    guard='status = vfs_page_cache_overlay_read(vnode, position, destination, (size_t)*bytes);'
    assert guard in program
    run(program.replace(guard,'{ status = K_OK; (void)vnode; }'),'memcmp')
    print('负对照通过：跳过页缓存合并会读到旧内容')
    acquire='kstatus_t status = transaction ? vfs_backend_consistent_read_lock(backend) : K_OK;'
    release='        if (transaction) vfs_backend_consistent_read_unlock(backend);'
    assert program.count(acquire)==1 and program.count(release)==1
    run(program.replace(acquire,'kstatus_t status = K_OK;').replace(release,''),
        'writer_entered_early',('without-lock',))
    print('负对照通过：跳过事务锁会穿插直接写入')
