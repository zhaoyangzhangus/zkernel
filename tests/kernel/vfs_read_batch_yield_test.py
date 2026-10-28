"""Exercise actual kernel-read scheduling boundaries, including boot callers."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/file_io.c').read_text()
helper = source[source.index('static void vfs_read_kernel_yield('):
                source.index('static kstatus_t vfs_read_backend_chunk(')]
flags_read = '__asm__ volatile ("pushfq; popq %0" : "=r"(flags));'
assert helper.count(flags_read) == 1
helper = helper.replace(flags_read, 'flags = mock_flags;')
read = source[source.index('kstatus_t vfs_read_kernel('):
              source.index('static kstatus_t vfs_write_chunk(')]
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
enum {K_OK=0,K_EINVAL=-22,K_EIO=-5,KOBJECT_TYPE_FILE=1,VFS_OPEN_READ=1,
      SCHED_CLASS_RT=0,SCHED_CLASS_FAIR=1,SCHED_CLASS_IDLE=2};
typedef int kstatus_t;
typedef struct {unsigned sched_class;} thread_t;
typedef struct {uint64_t size;int read;} vfs_backend_node_t;
typedef struct {void *fs_private;} vnode_t;
typedef struct {
 struct {unsigned type;} object;vnode_t *vnode;
 unsigned rights;uint64_t position;
} file_t;
static thread_t current;
static bool has_current;
static unsigned preempt,depth,calls,yields;
static uint64_t mock_flags,completed;
static unsigned stop_at,mode;
static file_t *active_file;
static unsigned char *base;
static thread_t *sched_current_thread(void){return has_current?&current:NULL;}
static bool sched_preempt_disabled(void){return preempt!=0;}
static void schedule(void){
 assert(!depth && !preempt && has_current && current.sched_class!=SCHED_CLASS_IDLE);
 assert(mock_flags & (1ULL<<9));
 assert(active_file->position==13U+completed); /* Commit precedes the switch. */
 ++yields;
}
static kstatus_t vfs_read_backend_chunk(vnode_t *vnode,vfs_backend_node_t *backend,
 uint64_t position,void *buffer,size_t length,uint64_t *bytes,bool kernel_buffer){
 (void)vnode;(void)backend;assert(kernel_buffer);
 assert(!depth && position==13U+completed && buffer==base+completed);
 assert(length>0 && length<=1024U*1024U);
 ++depth;++preempt;
 unsigned call=calls++;
 int status=K_OK;
 *bytes=length;
 if(call==stop_at){
  if(mode==1){*bytes=0;status=K_EIO;}
  if(mode==2)*bytes=0;
  if(mode==3)*bytes=length/2;
 }
 if(status==K_OK)completed+=*bytes;
 --preempt;--depth;
 return status;
}
''' + helper + read + r'''
int main(void){
 const size_t batch=1024U*1024U;
 /* The backend never dereferences the destination. Keep pointer arithmetic
  * inside one real object while avoiding stack allocation of several MiB. */
 static unsigned char output[3U*1024U*1024U+29U];
 base=output;
 vfs_backend_node_t backend={UINT64_MAX,1};vnode_t vnode={&backend};
 file_t file={{KOBJECT_TYPE_FILE},&vnode,VFS_OPEN_READ,13};active_file=&file;
 unsigned cases=0;
 const size_t sizes[]={0,1,batch-1,batch,batch+1,2*batch,3*batch+29};
 for(unsigned state=0;state<4;++state)for(unsigned nesting=0;nesting<3;++nesting)
 for(unsigned irq=0;irq<2;++irq)for(unsigned n=0;n<7;++n)
 for(unsigned stop=0;stop<4;++stop)for(unsigned outcome=0;outcome<4;++outcome){
  has_current=state!=0;current.sched_class=state?state-1:0;
  preempt=nesting;mock_flags=0x46U | (irq?(1ULL<<9):0);
  calls=yields=depth=0;completed=0;file.position=13;
  stop_at=stop;mode=outcome;
  bool eligible=has_current && current.sched_class!=SCHED_CLASS_IDLE && !nesting && irq;
  size_t expected=0;unsigned expected_calls=0,expected_yields=0;
  int expected_status=K_OK;
  while(expected<sizes[n]){
   size_t chunk=sizes[n]-expected;if(chunk>batch)chunk=batch;
   unsigned call=expected_calls++;
   if(call==stop && outcome==1){expected_status=K_EIO;break;}
   if(call==stop && outcome==2)break;
   if(call==stop && outcome==3){expected+=chunk/2;break;}
   expected+=chunk;
   if(expected<sizes[n] && eligible)++expected_yields;
  }
  uint64_t bytes=UINT64_MAX;
  int status=vfs_read_kernel(&file,output,sizes[n],&bytes);
  assert(status==expected_status && bytes==expected && completed==expected);
  assert(file.position==13U+expected && calls==expected_calls && yields==expected_yields);
  assert(!depth && preempt==nesting && mock_flags==(0x46U | (irq?(1ULL<<9):0)));
  ++cases;
 }
 printf("Kernel read batch yield: %u boot/idle/RT/FAIR, IRQ/nesting, final/short/failed batch and committed-offset cases passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-read-yield-') as directory:
    path = Path(directory)
    def run(name, code, negative=False):
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-x', 'c', '-', '-o', str(path/name)],
                       input=code, text=True, check=True)
        result = subprocess.run([str(path/name)], capture_output=True, text=True,
                                timeout=30, preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        if negative:
            assert result.returncode != 0 and 'Assertion' in result.stderr, result.stderr
            print('Negative control rejected: '+name)
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
    run('production', program)
    controls = {
        'missing-scheduling-point': ('if (total < len) vfs_read_kernel_yield();',
                                   'if (total > len) vfs_read_kernel_yield();'),
        'outer-preemption-lost': ('if (sched_preempt_disabled()) return;',
                                 'if (false && sched_preempt_disabled()) return;'),
        'irq-disabled-switch': ('if ((flags & (1ULL << 9)) != 0U) schedule();',
                               'if (flags != UINT64_MAX) schedule();'),
        'idle-continuation-switch': ('current == 0 || current->sched_class == SCHED_CLASS_IDLE',
                                    'current == 0'),
    }
    for name,(old,new) in controls.items():
        assert program.count(old) == 1
        run(name, program.replace(old,new), True)
