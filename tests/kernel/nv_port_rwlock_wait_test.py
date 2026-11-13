"""Exercise production NVIDIA rwlock functions with a blocking scheduler model."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/gpu/nvidia/port/nv_port_sync.c').read_text()
start = source.index('void nv_port_rwlock_init(')
end = source.index('\n#ifndef NV_PORT_HOST_TEST\nstatic bool nv_port_semaphore_try', start)
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <threads.h>
#define NV_PORT_RW_WRITER (1U<<31)
#define K_OK 0
typedef struct { mtx_t mutex; cnd_t cond; atomic_uint sleepers; } wait_queue_t;
typedef struct { atomic_uint state, writers_waiting;
                 wait_queue_t readers_waitq, writers_waitq; } nv_port_rwlock_t;
static atomic_uint waits, writes, readers, writer, value;
static void wait_queue_init(wait_queue_t *q) {
 assert(mtx_init(&q->mutex,mtx_plain)==thrd_success);
 assert(cnd_init(&q->cond)==thrd_success);atomic_init(&q->sleepers,0);
}
static int wait_on_queue(wait_queue_t *q,bool (*ready)(void*),void *p,uint64_t timeout) {
 assert(timeout==UINT64_MAX);atomic_fetch_add(&waits,1);
 mtx_lock(&q->mutex);
 while(!ready(p)) {
  atomic_fetch_add(&q->sleepers,1);
  cnd_wait(&q->cond,&q->mutex);
  atomic_fetch_sub(&q->sleepers,1);
 }
 mtx_unlock(&q->mutex);return K_OK;
}
static void wake_one(wait_queue_t *q) {
 mtx_lock(&q->mutex);cnd_signal(&q->cond);mtx_unlock(&q->mutex);
}
static void wake_all(wait_queue_t *q) {
 mtx_lock(&q->mutex);cnd_broadcast(&q->cond);mtx_unlock(&q->mutex);
}
'''
test = r'''
static nv_port_rwlock_t lock;
static int write_once(void *p) {
 (void)p;nv_port_rwlock_write_lock(&lock);
 assert(!atomic_load(&readers));assert(!atomic_exchange(&writer,1));
 atomic_fetch_add(&value,1);atomic_fetch_add(&writes,1);
 atomic_store(&writer,0);nv_port_rwlock_write_unlock(&lock);return 0;
}
static int read_once(void *p) {
 (void)p;nv_port_rwlock_read_lock(&lock);assert(atomic_load(&writes)==1);
 assert(!atomic_load(&writer));nv_port_rwlock_read_unlock(&lock);return 0;
}
static int stress(void *p) {
 uintptr_t id=(uintptr_t)p;
 for(unsigned i=0;i<2000;i++) {
  if(id%3==0)write_once(NULL);
  else {
   nv_port_rwlock_read_lock(&lock);atomic_fetch_add(&readers,1);
   assert(!atomic_load(&writer));unsigned v=atomic_load(&value);
   thrd_yield();assert(v==atomic_load(&value));
   atomic_fetch_sub(&readers,1);nv_port_rwlock_read_unlock(&lock);
  }
 }
 return 0;
}
int main(void) {
 nv_port_rwlock_init(&lock);nv_port_rwlock_read_lock(&lock);
 thrd_t w,r;assert(thrd_create(&w,write_once,NULL)==thrd_success);
 while(!atomic_load(&lock.writers_waitq.sleepers))thrd_yield();
 assert(!nv_port_rwlock_read_try_lock(&lock));
 assert(thrd_create(&r,read_once,NULL)==thrd_success);
 while(!atomic_load(&lock.readers_waitq.sleepers))thrd_yield();
 assert(!atomic_load(&writes));nv_port_rwlock_read_unlock(&lock);
 thrd_join(w,NULL);thrd_join(r,NULL);assert(atomic_load(&writes)==1);
 thrd_t workers[12];
 for(uintptr_t i=0;i<12;i++)assert(thrd_create(&workers[i],stress,(void*)i)==thrd_success);
 for(unsigned i=0;i<12;i++)thrd_join(workers[i],NULL);
 /* 没有写者能够唤醒因 CAS 竞争而错误休眠的读者。 */
 for(unsigned i=0;i<12;i++)assert(thrd_create(&workers[i],stress,(void*)1)==thrd_success);
 for(unsigned i=0;i<12;i++)thrd_join(workers[i],NULL);
 assert(atomic_load(&value)==8001);assert(!atomic_load(&lock.state));
 assert(!atomic_load(&lock.writers_waiting));assert(atomic_load(&waits)>=2);
 assert(!atomic_load(&lock.readers_waitq.sleepers));
 assert(!atomic_load(&lock.writers_waitq.sleepers));
 cnd_destroy(&lock.readers_waitq.cond);mtx_destroy(&lock.readers_waitq.mutex);
 cnd_destroy(&lock.writers_waitq.cond);mtx_destroy(&lock.writers_waitq.mutex);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='nv-rwlock-test-') as directory:
    base = Path(directory)
    (base / 'test.c').write_text(fixture + source[start:end] + test)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize=address,undefined',
                    str(base / 'test.c'), '-o', str(base / 'test')], check=True)
    subprocess.run([str(base / 'test')], check=True, timeout=25)
print('Production NVIDIA rwlock: sleeping contention, writer priority, 48000 mixed/reader-only operations and release/wait races passed')
