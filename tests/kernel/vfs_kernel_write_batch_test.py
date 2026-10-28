"""验证真实内核批量写路径的连续缓冲、计数提交和后端故障边界。"""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/file_io.c').read_text()
chunk = source[source.index('static kstatus_t vfs_write_chunk('):source.index('kstatus_t vfs_write(file_t')]
write = source[source.index('kstatus_t vfs_write_kernel('):source.index('kstatus_t vfs_fsync(')]
program = r'''
#include <assert.h>
/* 单句柄测试只桩化偏移锁；共享句柄并发由专门的回归测试覆盖。 */
#define vfs_file_position_init(file) ((void)(file))
#define vfs_file_position_lock(file) ((void)(file), K_OK)
#define vfs_file_position_unlock(file) ((void)(file))
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PAGE_SIZE 4096U
#define BATCH (1024U*1024U)
enum {K_OK=0,K_EIO=-5,K_EACCES=-13,KOBJECT_TYPE_FILE=1,VFS_OPEN_WRITE=2};
typedef int kstatus_t;
typedef struct {void *fs_private;} vnode_t;
typedef struct {struct {unsigned type;} object;vnode_t *vnode;unsigned rights;uint64_t position;} file_t;
typedef struct {
 void *context;int (*write)(void *,uint64_t,const void *,size_t,uint64_t *);
} vfs_backend_node_t;
static unsigned calls,cache_calls,stop_at,mode;
static bool held,transaction;
static uint64_t committed;
static const unsigned char *base;
static int vfs_file_write_begin(file_t *f,vfs_backend_node_t *b,size_t n,bool *locked){
 (void)f;(void)b;(void)n;assert(!transaction&&!held);transaction=true;*locked=true;return K_OK;
}
static void vfs_write_transaction_end(vfs_backend_node_t *b,bool locked){
 (void)b;assert(locked&&transaction&&!held);transaction=false;
}
static void vfs_backend_lock(vfs_backend_node_t *b){(void)b;assert(transaction&&!held);held=true;}
static void vfs_backend_unlock(vfs_backend_node_t *b){(void)b;assert(held);held=false;}
static int backend_write(void *ctx,uint64_t pos,const void *buf,size_t len,uint64_t *bytes){
 (void)ctx;assert(held&&transaction&&pos==17+committed&&buf==base+committed);
 assert(len&&len<=BATCH);unsigned call=calls++;*bytes=len;
 if(call==stop_at){
  if(mode==1){*bytes=len;return K_EIO;}
  if(mode==2)*bytes=0;
  if(mode==3)*bytes=len/2;
  if(mode==4)*bytes=len+1;
 }
 return K_OK;
}
static void vfs_page_cache_copy_in(vnode_t *v,uint64_t pos,const void *buf,size_t len){
 assert(v&&transaction&&!held&&pos==17+committed&&buf==base+committed);
 ++cache_calls;committed+=len;
}
''' + chunk + write + r'''
int main(void){
 static unsigned char input[3*BATCH+31];base=input;
 const size_t sizes[]={0,1,PAGE_SIZE-1,PAGE_SIZE,PAGE_SIZE+1,BATCH-1,BATCH,BATCH+1,3*BATCH+31};
 unsigned cases=0;
 for(unsigned n=0;n<sizeof(sizes)/sizeof(*sizes);++n)
 for(stop_at=0;stop_at<5;++stop_at)for(mode=0;mode<5;++mode){
  vfs_backend_node_t b={NULL,backend_write};vnode_t v={&b};
  file_t f={{KOBJECT_TYPE_FILE},&v,VFS_OPEN_WRITE,17};
  calls=cache_calls=0;committed=0;transaction=held=false;
  size_t expected=0;unsigned expected_calls=0,expected_cache=0;int expected_status=K_OK;
  while(expected<sizes[n]){
   size_t length=sizes[n]-expected;if(length>BATCH)length=BATCH;
   unsigned call=expected_calls++;
   if(call==stop_at&&(mode==1||mode==4)){expected_status=K_EIO;break;}
   if(call==stop_at&&mode==2)break;
   if(call==stop_at&&mode==3){expected+=length/2;if(length/2)++expected_cache;break;}
   expected+=length;++expected_cache;
  }
  uint64_t bytes=UINT64_MAX;
  assert(vfs_write_kernel(&f,input,sizes[n],&bytes)==expected_status);
  assert(bytes==expected&&committed==expected&&f.position==17+expected);
  assert(calls==expected_calls&&cache_calls==expected_cache&&!transaction&&!held);++cases;
 }
 printf("Kernel writes: %u length/failure layouts; direct contiguous buffers, 1MiB bound, short/zero/invalid counts, exact cache/offset commit PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-kernel-write-batch-') as directory:
    command = ['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-x', 'c', '-', '-o', directory + '/test']
    subprocess.run(command, input=program, text=True, check=True)
    subprocess.run([directory + '/test'], check=True, timeout=30)
    for label, old, new in (
        ('page-at-a-time write', 'if (chunk > 1024U * 1024U) chunk = 1024U * 1024U;',
         'if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;'),
        ('oversized backend count accepted', 'status != K_OK || *bytes > length', 'status != K_OK'),
    ):
        broken = program.replace(old, new)
        assert broken != program
        subprocess.run(command, input=broken, text=True, check=True)
        result = subprocess.run([directory + '/test'], capture_output=True,
            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)), timeout=30)
        assert result.returncode == -signal.SIGABRT
        print('Negative control rejected:', label)
