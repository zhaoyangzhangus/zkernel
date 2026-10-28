"""验证实际 VFS 锁：短时命名空间自旋锁与可抢占的节点、后端 I/O 锁。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/internal.h').read_text()
start = source.index('static inline void vfs_state_lock(')
end = source.index('\nsize_t vfs_string_length', start)
core = (root / 'kernel/fs/vfs/core.c').read_text()
namespace = core[core.index('static void vfs_lock('):core.index('\nsize_t vfs_string_length')]
program = r'''
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef pthread_mutex_t io_mutex_t;
typedef struct {io_mutex_t lock;} vnode_t;
typedef struct {io_mutex_t io_lock;} vfs_backend_node_t;
static struct {atomic_uint state;} g_vfs_lock;
static _Thread_local unsigned depth;
static atomic_uint active;
static unsigned visits;
static void sched_preempt_disable(void){++depth;}
static void sched_preempt_enable(void){assert(depth);--depth;}
static void io_mutex_lock_required(io_mutex_t *mutex){
 assert(!depth);assert(!pthread_mutex_lock(mutex));assert(!depth);
}
static void io_mutex_unlock_required(io_mutex_t *mutex){
 assert(!depth);assert(!pthread_mutex_unlock(mutex));
}
''' + source[start:end] + namespace + r'''
static vnode_t vnode={PTHREAD_MUTEX_INITIALIZER};
static vfs_backend_node_t backend={PTHREAD_MUTEX_INITIALIZER};
static void *contender(void *arg){
 unsigned kind=(unsigned)(uintptr_t)arg;
 for(unsigned i=0;i<10000;++i){
  if(kind==0){vfs_vnode_lock(&vnode);vfs_backend_lock(&backend);}
  else if(kind==1)vfs_backend_lock(&backend);
  else vfs_lock();
  assert(depth==(kind==2));
  assert(atomic_fetch_add(&active,1U)==0U);++visits;
  assert(atomic_fetch_sub(&active,1U)==1U);
  if(kind==0){vfs_backend_unlock(&backend);vfs_vnode_unlock(&vnode);}
  else if(kind==1)vfs_backend_unlock(&backend);
  else vfs_unlock();
  assert(!depth);
 }
 return 0;
}
int main(void){
 pthread_t threads[4];
 for(unsigned kind=0;kind<3;++kind){
  for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,contender,(void*)(uintptr_t)kind));
  for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
 }
 sched_preempt_disable();vfs_lock();assert(depth==2);
 vfs_unlock();assert(depth==1);sched_preempt_enable();
 assert(!depth && !atomic_load(&g_vfs_lock.state) && visits==120000);
 puts("VFS guards: 120000 concurrent visits; vnode/backend nesting leaves I/O preemptible; namespace remains short guarded state");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-io-guards-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
