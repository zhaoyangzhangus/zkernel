"""Exercise production async coordinator with deterministic thread boundaries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/init/userspace.c').read_text()
body = source[source.index('static BOOLEAN device_init_started;'):
              source.index('BOOLEAN userspace_start_window_worker(')]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
typedef int BOOLEAN;
typedef int kstatus_t;
enum { K_OK=0, K_EBUSY=-1, ERROR=-2, K_ETIMEDOUT=-3 };
typedef struct { int started, finished; void (*fn)(void *); } nv_port_thread_t;
static void userspace_pin_gpu_init(nv_port_thread_t *t) { assert(t && !t->started); }
typedef struct { bool signaled; } nv_port_completion_t;
static void (*publish_during_wait)(void *);
static void nv_port_completion_init(nv_port_completion_t *c) { c->signaled=false; }
static void nv_port_completion_signal(nv_port_completion_t *c) { c->signaled=true; }
static int nv_port_completion_wait(nv_port_completion_t *c, uint64_t timeout) {
 assert(timeout==UINT64_MAX);
 if(publish_during_wait) {
  void (*publish)(void *)=publish_during_wait;
  publish_during_wait=0; publish(0);
 }
 if(!c->signaled) return ERROR;
 c->signaled=false; return K_OK;
}
static nv_port_thread_t mock;
static unsigned probes, creates, starts, joins, destroys, sleeps, deferred;
static int failure, early, busy, phase;
#define BOOT_PROFILE_GPU 0
#define kernel_boot_begin(...) ((void)0)
#define kernel_boot_end(...) ((void)0)
static int deferred_start_worker(void) { ++deferred; return failure!=1; }
static int deferred_background_start_worker(void) { return failure!=2; }
static int nv_rm_driver_probe(void) { assert(deferred); ++probes; return ERROR; }
static int nv_port_thread_create(void (*fn)(void *), void *arg, nv_port_thread_t **out) {
 (void)arg; ++creates; if(failure==2) return ERROR;
 mock=(nv_port_thread_t){0,0,fn}; *out=&mock; return K_OK;
}
static int nv_port_thread_start(nv_port_thread_t *t) {
 ++starts; if(failure==3) return ERROR;
 t->started=1;
 if(early) { t->fn(0); t->finished=1; }
 return K_OK;
}
static int nv_port_thread_join(nv_port_thread_t *t, uint64_t timeout) {
 assert(phase==1 && t->started && timeout==0); ++joins;
 if(failure==4) return ERROR;
 if(!t->finished) return K_ETIMEDOUT;
 return K_OK;
}
static int nv_port_thread_destroy(nv_port_thread_t *t) {
 ++destroys; assert(!t->started || t->finished);
 if(t->started && busy-->0) return K_EBUSY;
 return K_OK;
}
static void userspace_device_init_poll(void) {
 ++sleeps;
 if(!mock.finished) { mock.fn(0); mock.finished=1; }
}
''' + body + r'''
int main(void) {
 for(early=0; early<2; ++early) for(failure=0; failure<5; ++failure) {
  device_init_started=0; device_init_thread=0;
  probes=creates=starts=joins=destroys=sleeps=deferred=0; phase=0; busy=2;
  int ok=userspace_start_device_init();
  if(failure==1) { assert(!ok && !creates && !probes); continue; }
  assert(ok && creates==1 && deferred==1);
  unsigned before=sleeps;
  if(!early && failure!=2 && failure!=3) {
   userspace_poll_device_init();
   assert(device_init_thread && sleeps==before && !userspace_device_init_done());
  }
  assert(probes==(unsigned)(early && failure!=2 && failure!=3));
  assert(userspace_start_device_init() && creates==1 && deferred==1);
  /* Independent boot work runs while the late GPU task is still pending. */
  phase=1;
  ok=userspace_finish_device_init();
  if(failure==4) {
   assert(!ok && !destroys && !sleeps);
   failure=0; assert(userspace_finish_device_init()); failure=4;
  } else assert(ok);
  assert(probes==(unsigned)(failure!=2 && failure!=3) && !device_init_thread);
  assert(userspace_device_init_done() && userspace_wait_device_init());
  unsigned old=destroys;
  assert(userspace_finish_device_init() &&
         probes==(unsigned)(failure!=2 && failure!=3) && destroys==old);
  if(failure!=2 && failure!=3) assert(sleeps==(unsigned)(early?2:3));
 }
 /* A waiter that entered before publication must pass the consumed token
  * onward to other sleepers. Late callers must not consume that token. */
 device_init_started=1;
 atomic_store(&device_init_complete,false);
 nv_port_completion_init(&device_init_event);
 publish_during_wait=userspace_gpu_init;
 assert(userspace_wait_device_init());
 assert(device_init_event.signaled && userspace_device_init_done());
 for(unsigned i=0;i<16;++i) {
  assert(userspace_wait_device_init() && device_init_event.signaled);
 }
 puts("Async device init: late/early completion, allocation/start/join failure, blocking reclamation, readiness token relay and idempotence PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='device-init-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-DOS_NVIDIA_PHASE5=1', '-fsanitize=address,undefined',
                    '-no-pie', str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True, timeout=15)

# Exercise the production wait entry with genuinely blocked concurrent callers.
wait_body = source[source.index('BOOLEAN userspace_wait_device_init(void)'):
                   source.index('void userspace_poll_device_init(void)')]
concurrent_program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
typedef int BOOLEAN;
#define K_OK 0
static bool device_init_started=true;
static atomic_bool done;
static atomic_uint waiting;
typedef struct {
 pthread_mutex_t mutex;
 pthread_cond_t condition;
 unsigned count;
} nv_port_completion_t;
static nv_port_completion_t device_init_event={
 PTHREAD_MUTEX_INITIALIZER,PTHREAD_COND_INITIALIZER,0};
static BOOLEAN userspace_device_init_done(void) {
 return atomic_load_explicit(&done,memory_order_acquire);
}
static void nv_port_completion_signal(nv_port_completion_t *c) {
 pthread_mutex_lock(&c->mutex);
 ++c->count; pthread_cond_signal(&c->condition);
 pthread_mutex_unlock(&c->mutex);
}
static int nv_port_completion_wait(nv_port_completion_t *c,uint64_t timeout) {
 assert(timeout==UINT64_MAX);
 pthread_mutex_lock(&c->mutex);
 atomic_fetch_add(&waiting,1);
 while(!c->count) pthread_cond_wait(&c->condition,&c->mutex);
 --c->count; pthread_mutex_unlock(&c->mutex); return K_OK;
}
''' + wait_body + r'''
static void *waiter(void *unused) {
 (void)unused; assert(userspace_wait_device_init()); return 0;
}
int main(void) {
 pthread_t threads[16];
 for(unsigned i=0;i<16;++i)
  assert(!pthread_create(&threads[i],0,waiter,0));
 while(atomic_load(&waiting)!=16) sched_yield();
 atomic_store_explicit(&done,true,memory_order_release);
 nv_port_completion_signal(&device_init_event);
 for(unsigned i=0;i<16;++i) assert(!pthread_join(threads[i],0));
 assert(device_init_event.count==1);
 for(unsigned i=0;i<16;++i) assert(userspace_wait_device_init());
 assert(device_init_event.count==1 && atomic_load(&waiting)==16);
 puts("GPU readiness: all 16 blocked callers released; late callers do not consume the readiness token PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='device-waiters-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(concurrent_program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-DOS_NVIDIA_PHASE5=1', '-pthread',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True, timeout=15)
