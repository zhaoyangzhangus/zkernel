"""Actual independent cleanup ring, startup, sleep handshake and ownership."""
from pathlib import Path
import subprocess
import tempfile
import resource

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

root = Path(__file__).resolve().parents[2]
actual = (root/'kernel/irq/background.c').read_text()
begin = actual.index('static uint64_t background_lock(')
end = actual.index('bool deferred_background_worker_started(', begin)
actual = actual[:begin] + r'''
static uint64_t background_lock(void) {
 assert(!depth);pthread_mutex_lock(&mutex);++depth;return 512;
}
static void background_unlock(uint64_t flags) {
 assert(flags==512&&depth==1);--depth;pthread_mutex_unlock(&mutex);
}
''' + actual[end:]
program = r'''
#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned depth;
''' + actual + r'''
static thread_t bootstrap;
static bool have_current,fail_enqueue;
static unsigned starts,registered,blocked;
static atomic_uint wakes,done,executed;
static unsigned seen[40000];
static uint32_t cpu_id;
static uint32_t discovered=4,expected_cpu=3;
static uint64_t online=9;
thread_t *sched_current_thread(void){return have_current?&bootstrap:0;}
uint32_t x86_current_cpu_index(void){return cpu_id;}
uint32_t x86_smp_discovered_count(void){return discovered;}
bool x86_smp_cpu_online(uint32_t cpu){return cpu<64&&(online&(1ULL<<cpu))!=0;}
uint64_t process_allocate_task_id(void){static uint64_t tid=100;return ++tid;}
void sched_initialize_new_thread(thread_t *t,uint32_t cpu,bool pin){
 assert(pin&&cpu==expected_cpu);t->owner_cpu=cpu;t->sched_class=SCHED_CLASS_FAIR;
 atomic_init(&t->state,THREAD_NEW);
}
void x86_kernel_thread_start(void){abort();}
bool sched_enqueue_bootstrap(thread_t *t){
 assert(t==&g_background.worker&&atomic_load(&t->state)==THREAD_READY);
 assert(t->arch.switch_ctx.r12==(uintptr_t)&background_worker_main);
 assert(t->arch.switch_ctx.r14%16==0&&t->owner_cpu==expected_cpu);++starts;return !fail_enqueue;
}
void sched_monitor_register(thread_t *t,const char *name){
 assert(t==&g_background.worker&&!strcmp(name,"background-cleanup"));++registered;
}
void sched_wake(thread_t *t){
 assert(!depth&&t==&g_background.worker);atomic_fetch_add(&wakes,1);
 atomic_store(&t->state,THREAD_READY);
}
bool sched_publish_blocked(thread_t *t){
 assert(depth==1&&t==&g_background.worker&&!g_background.count);
 atomic_store(&t->state,THREAD_BLOCKED);++blocked;return true;
}
void schedule(void){assert(!depth);}
static void callback(void *arg){
 assert(!depth);unsigned i=(uintptr_t)arg;assert(i<40000&&!seen[i]);++seen[i];
 atomic_fetch_add(&executed,1);
}
static void drain(void){background_item_t item;while(background_take(&item))item.function(item.argument);}
static void *producer(void *arg){
 unsigned start=(uintptr_t)arg;
 for(unsigned i=start;i<start+10000;++i)
  while(!deferred_background_schedule(callback,(void *)(uintptr_t)i))sched_yield();
 atomic_fetch_add(&done,1);return NULL;
}
static atomic_bool slow_entered,slow_release;
static unsigned markers;
static void marker(void *arg){(void)arg;assert(!depth);++markers;}
static void slow(void *arg){
 (void)arg;assert(!depth);atomic_store(&slow_entered,true);
 while(!atomic_load(&slow_release))sched_yield();
}
static void *one_cleanup(void *arg){
 (void)arg;background_item_t item;assert(background_take(&item));item.function(item.argument);return NULL;
}
int main(void){
 assert(!deferred_background_start_worker());
 assert(!deferred_background_schedule(callback,0)&&!executed&&!wakes);
 have_current=true;cpu_id=MAX_CPUS;assert(!deferred_background_start_worker());cpu_id=0;
 fail_enqueue=true;assert(!deferred_background_start_worker()&&!registered);
 assert(!deferred_background_schedule(callback,0));
 fail_enqueue=false;assert(deferred_background_start_worker());
 assert(deferred_background_start_worker()&&starts==2&&registered==1);
 assert(!deferred_background_schedule(NULL,0));
 for(unsigned round=0;round<30;++round){
  unsigned before=wakes;
  for(unsigned i=0;i<128;++i)assert(deferred_background_schedule(callback,(void *)(uintptr_t)(round*128+i)));
  assert(wakes==before+1&&!deferred_background_schedule(callback,(void *)39999));
  assert(!background_prepare_sleep());
  for(unsigned i=0;i<128;++i){
   background_item_t item;assert(background_take(&item));
   assert((uintptr_t)item.argument==round*128+i);item.function(item.argument);
  }
  assert(background_prepare_sleep());before=wakes;
  assert(deferred_background_schedule(callback,(void *)(uintptr_t)(5000+round)));
  assert(wakes==before+1&&atomic_load(&g_background.worker.state)==THREAD_READY);drain();
 }
 memset(seen,0,sizeof(seen));executed=0;
 pthread_t producers[4];
 for(unsigned i=0;i<4;++i)assert(!pthread_create(&producers[i],NULL,producer,(void *)(uintptr_t)(i*10000)));
 while(atomic_load(&done)!=4){drain();sched_yield();}
 for(unsigned i=0;i<4;++i)assert(!pthread_join(producers[i],NULL));
 drain();
 assert(executed==40000);for(unsigned i=0;i<40000;++i)assert(seen[i]==1);
 // A blocked cleanup cannot retain the queue lock or consume device work.
 assert(deferred_background_schedule(slow,0));pthread_t cleanup;
 assert(!pthread_create(&cleanup,NULL,one_cleanup,NULL));
 while(!atomic_load(&slow_entered))sched_yield();
 assert(deferred_background_schedule(marker,0));
 assert(g_background.count==1); // the independent device side can continue
 atomic_store(&slow_release,true);assert(!pthread_join(cleanup,NULL));
 background_item_t item;assert(background_take(&item)&&item.function==marker);
 item.function(item.argument);assert(markers==1);
 assert(!background_take(&item));assert(background_prepare_sleep()&&blocked==31);
 // No host worker is running now. Reconstruct deterministic boot layouts;
 // idle CPUs and offline holes must not force an invalid affinity target.
 unsigned layouts=0;
 for(discovered=1;discovered<=4;++discovered)
 for(online=1;online<(1ULL<<discovered);++online)
 for(cpu_id=0;cpu_id<discovered;++cpu_id){
  if(!(online&(1ULL<<cpu_id)))continue;
  expected_cpu=cpu_id;
  for(unsigned cpu=0;cpu<discovered;++cpu)
   if(cpu!=cpu_id&&(online&(1ULL<<cpu)))expected_cpu=cpu;
  memset(&g_background,0,sizeof(g_background));
  starts=registered=0;fail_enqueue=false;
  assert(deferred_background_start_worker()&&starts==1&&registered==1);
  assert(g_background.worker.owner_cpu==expected_cpu);
  assert(deferred_background_schedule(marker,0));drain();
  assert(background_prepare_sleep());
  unsigned before=wakes;
  assert(deferred_background_schedule(marker,0)&&wakes==before+1);drain();
  assert(deferred_background_start_worker()&&starts==1);++layouts;
 }
 assert(layouts==49);
 puts("Background cleanup: startup rollback, CPU placement, FIFO/full/wrap, sleep/wake, 40000 concurrent ownership transfers and unlocked slow callbacks passed");
 printf("Background startup: %u single/multicore, offline-hole and nonzero boot-CPU layouts passed\n",layouts);
}
'''
with tempfile.TemporaryDirectory(prefix='background-cleanup-') as directory:
    binary = str(Path(directory)/'test')
    compiler = ['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                '-pthread','-DOS_KERNEL_BUILD',
                '-fsanitize=address,undefined','-fno-pie','-no-pie',
                '-I'+str(root/'include'),'-x','c','-','-o',binary]
    subprocess.run(compiler,input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    for label, before, after in (
        ('lost empty-queue wake', 'bool notify = g_background.count == 0U;', 'bool notify = false;'),
        ('sleep despite queued cleanup', 'g_background.count == 0U &&', 'true &&'),
    ):
        negative = program.replace(before, after, 1)
        assert negative != program
        subprocess.run(compiler,input=negative,text=True,check=True)
        result = subprocess.run([binary],capture_output=True,text=True,timeout=30)
        assert result.returncode != 0 and 'Assertion' in result.stderr, label
        print('Negative control rejected:',label)
