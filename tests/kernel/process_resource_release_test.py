"""Production once-only resource release: read-only repeats and racing winners."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/process/process.c').read_text()
body=source[source.index('void process_release_runtime_resources('):
            source.index('static bool process_destruction_self_test_round(')]
body=body.replace('__atomic_fetch_or(', 'claim_resources(')
fixture=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define PROCESS_FLAG_RESOURCES_RELEASED 2U
typedef struct { atomic_uint refs; } vm_space_t;
typedef struct { uint32_t flags; uint64_t pid; unsigned handles,linux_fds; vm_space_t *vm; pthread_mutex_t lock; } process_t;
static _Thread_local unsigned held;
static atomic_uint claims,handles,fds,vm_puts;
static pthread_barrier_t barrier;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static bool pause_release,entered,allow;
static uint32_t claim_resources(uint32_t *flags,uint32_t mask,int order) {
 ++claims;return __atomic_fetch_or(flags,mask,order);
}
static void process_thread_lock(process_t *p) { assert(!held);pthread_mutex_lock(&p->lock);++held; }
static void process_thread_unlock(process_t *p) { assert(held==1);--held;pthread_mutex_unlock(&p->lock); }
static void handle_table_destroy(unsigned *table) {
 assert(!held && table);++handles;
 pthread_mutex_lock(&gate);
 if(pause_release) { entered=true;pthread_cond_broadcast(&condition);while(!allow)pthread_cond_wait(&condition,&gate); }
 pthread_mutex_unlock(&gate);
}
static void linux_fd_table_destroy(unsigned *table) { assert(!held && table);++fds; }
static void file_lock_release_owner(uint64_t owner) { assert(!held && owner == 0 && !fds); }
static void vm_space_put(vm_space_t *space) { assert(!held && atomic_fetch_sub(&space->refs,1)==1);++vm_puts; }
'''
checks=r'''
static void *release_job(void *p) { process_release_runtime_resources(p);return NULL; }
static void *race_job(void *p) {
 int status=pthread_barrier_wait(&barrier);assert(!status || status==PTHREAD_BARRIER_SERIAL_THREAD);
 for(unsigned i=0;i<1000;++i)process_release_runtime_resources(p);
 return NULL;
}
int main(void) {
 const unsigned workers=16;
 for(unsigned round=0;round<100;++round) {
  vm_space_t space;atomic_init(&space.refs,1);
  process_t process={.flags=0x100U,.vm=round%2?&space:NULL};
  pthread_mutex_init(&process.lock,NULL);pthread_barrier_init(&barrier,NULL,workers);
  claims=handles=fds=vm_puts=0;pthread_t jobs[16];
  for(unsigned i=0;i<workers;++i)assert(!pthread_create(&jobs[i],NULL,race_job,&process));
  for(unsigned i=0;i<workers;++i)assert(!pthread_join(jobs[i],NULL));
  assert(process.flags==(0x100U|PROCESS_FLAG_RESOURCES_RELEASED) && !process.vm);
  assert(atomic_load(&handles)==1 && atomic_load(&fds)==1 && atomic_load(&vm_puts)==round%2);
  unsigned before=atomic_load(&claims);assert(before>=1 && before<=workers);
  for(unsigned i=0;i<10000;++i)process_release_runtime_resources(&process);
  assert(atomic_load(&claims)==before && atomic_load(&handles)==1);
  pthread_barrier_destroy(&barrier);pthread_mutex_destroy(&process.lock);
 }
 // claim 并不保证回调已经完成：保留现有语义。
 process_t process={0};pthread_mutex_init(&process.lock,NULL);
 claims=handles=fds=vm_puts=0;pause_release=true;entered=allow=false;
 pthread_t job;assert(!pthread_create(&job,NULL,release_job,&process));
 pthread_mutex_lock(&gate);while(!entered)pthread_cond_wait(&condition,&gate);pthread_mutex_unlock(&gate);
 assert(atomic_load(&claims)==1 && atomic_load(&handles)==1 && !atomic_load(&fds));
 process_release_runtime_resources(&process);
 assert(atomic_load(&claims)==1 && !atomic_load(&fds));
 pthread_mutex_lock(&gate);allow=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&gate);
 assert(!pthread_join(job,NULL));assert(atomic_load(&fds)==1);
 pthread_mutex_destroy(&process.lock);process_release_runtime_resources(NULL);
 puts("100 races with 16 workers, 1M completed-release repeats, null VM and in-progress callbacks: one teardown, no redundant atomic writes passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-resource-release-') as directory:
    path=Path(directory)
    def run(actual):
        (path/'test.c').write_text(fixture+actual+checks)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-pthread',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        str(path/'test.c'),'-o',str(path/'test')],check=True)
        return subprocess.run([str(path/'test')],capture_output=True,text=True)
    result=run(body)
    assert result.returncode==0,result.stdout+result.stderr
    print(result.stdout.strip())
    probe='''    if ((__atomic_load_n(&process->flags, __ATOMIC_ACQUIRE) &
         PROCESS_FLAG_RESOURCES_RELEASED) != 0U) return;'''
    assert probe in body
    assert run(body.replace(probe,'')).returncode!=0,'Old repeated atomic claim accepted'
    print('Negative control rejected: redundant atomic writes after resources are claimed')
