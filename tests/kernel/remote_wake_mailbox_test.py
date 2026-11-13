import pathlib
import resource
import subprocess
import tempfile

source = pathlib.Path('kernel/sched/core.c').read_text()
methods = source[source.index('static void scheduler_remote_wake_link('):source.index('static void scheduler_priority_link(')]
release = source[source.index('static inline void scheduler_thread_mailbox_lock('):source.index('static bool scheduler_command_ring_try_pop(')]
release = release.replace('scheduler_thread_mailbox_lock(thread);',
                         'scheduler_thread_mailbox_lock(thread); assert(!held_locks); ++held_locks; atomic_fetch_add(&lock_acquisitions,1);')
release = release.replace('scheduler_thread_mailbox_unlock(thread);',
                         'assert(held_locks==1); --held_locks; scheduler_thread_mailbox_unlock(thread);')
methods = methods.replace('scheduler_thread_mailbox_lock(thread);',
                         'scheduler_thread_mailbox_lock(thread); assert(!held_locks); ++held_locks; atomic_fetch_add(&lock_acquisitions,1);')
methods = methods.replace('scheduler_thread_mailbox_unlock(thread);',
                         'assert(held_locks==1); --held_locks; scheduler_thread_mailbox_unlock(thread);')
program = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#define THREAD_BLOCKED 2U
#define SCHED_UNLIKELY(x) (x)
typedef struct thread {
 atomic_bool remote_wake_queued;
 atomic_uint remote_wake_lock, block_epoch, state;
 struct thread *remote_wake_next;
 uint32_t remote_wake_epoch, owner_cpu;
 atomic_int refs;
 struct thread *command_release_next;
 uint32_t command_release_count;
} thread_t;
typedef struct {
 thread_t * _Atomic remote_wake_head;
 atomic_bool command_pending;
 thread_t *command_release_head,*command_release_tail;
 atomic_bool command_release_pending;
} scheduler_cpu_t;
static scheduler_cpu_t g_cpus[2];
static uint32_t g_cpu_count=2;
static unsigned notifications;
static _Thread_local uint32_t current_cpu;
static _Thread_local unsigned held_locks;
static atomic_uint lock_acquisitions;
static _Thread_local bool irq=true;
static uint64_t scheduler_irq_save(void) { bool before=irq;irq=false;return before?1ULL<<9:0; }
static void scheduler_irq_restore(uint64_t flags) { irq=(flags&(1ULL<<9))!=0; }
static bool scheduler_current_cpu(uint32_t *cpu) { *cpu=current_cpu;return true; }
static uint32_t x86_current_cpu_index(void) { return current_cpu; }
static bool deferred_background_worker_started(void) { return false; }
static bool deferred_background_schedule(void (*function)(void *),void *argument) {
 (void)function;(void)argument;assert(!"本测试不启用后台销毁工作线程");return false;
}
static bool scheduler_cpu_available(uint32_t cpu) { return cpu<g_cpu_count; }
static bool x86_smp_request_reschedule(uint32_t cpu) { assert(cpu<g_cpu_count);++notifications;return true; }
static bool sched_preempt_disabled(void) { return false; }
static bool x86_tlb_shootdown_active(void) { return false; }
static bool object_try_get(thread_t *t) { atomic_fetch_add(&t->refs,1); return true; }
static void object_put(thread_t *t) { assert(!held_locks);assert(atomic_fetch_sub(&t->refs,1)>1); }
static bool scheduler_complete_wake_local(scheduler_cpu_t *c, thread_t *t,
                                         uint32_t e, bool check) {
 (void)c; assert(check);
 if(atomic_load(&t->block_epoch)!=e || atomic_load(&t->state)!=THREAD_BLOCKED) return false;
 atomic_store(&t->state,3U); return true;
}
''' + release + methods + r'''
static void drain_cpu(uint32_t cpu) {
 current_cpu=cpu;uint64_t flags=scheduler_irq_save();
 scheduler_remote_wake_drain(cpu,&g_cpus[cpu]);scheduler_irq_restore(flags);
 scheduler_command_release_drain(cpu);
}
#define stress_cpu g_cpus[1]
static thread_t stress_threads[4];
static atomic_uint finished;
static atomic_bool command_done;
static void *produce(void *arg) {
 thread_t *t=arg;
 for(unsigned epoch=1;epoch<=5000;++epoch) {
  atomic_store(&t->block_epoch,epoch);
  atomic_store(&t->state,THREAD_BLOCKED);
  assert(scheduler_remote_wake_publish(&stress_cpu,t,epoch));
  while(atomic_load(&t->state)==THREAD_BLOCKED || atomic_load(&t->remote_wake_queued)) sched_yield();
 }
 atomic_fetch_add(&finished,1); return NULL;
}
static void *consume(void *arg) {
 (void)arg;current_cpu=1;
 while(atomic_load(&finished)!=4 || atomic_load(&stress_cpu.remote_wake_head)) {
  uint64_t flags=scheduler_irq_save();
  scheduler_remote_wake_drain(1,&stress_cpu);
  scheduler_irq_restore(flags);
  scheduler_command_release_drain(1);sched_yield();
 }
 do {scheduler_command_release_drain(1);sched_yield();}
 while(!atomic_load(&command_done)||atomic_load(&stress_cpu.command_release_pending));
 return NULL;
}
static void *stale_commands(void *arg) {
 (void)arg;current_cpu=0;
 for(unsigned n=0;n<100000;++n) {
  thread_t *t=&stress_threads[n%4];
  assert(object_try_get(t));uint64_t flags=scheduler_irq_save();
  scheduler_command_release_enqueue_local(&g_cpus[0],t);
  scheduler_irq_restore(flags);scheduler_command_release_drain(0);
 }
 atomic_store(&command_done,true);
 do {scheduler_command_release_drain(0);sched_yield();}
 while(atomic_load(&finished)!=4||atomic_load(&g_cpus[0].command_release_pending));
 return NULL;
}
int main(void) {
 current_cpu=1;
 scheduler_cpu_t *cpu=&g_cpus[1];
 thread_t *threads=calloc(10000,sizeof(*threads)); assert(threads);
 for(unsigned i=0;i<10000;++i) {
  thread_t *t=&threads[i]; t->refs=1; t->owner_cpu=1;
  atomic_init(&t->block_epoch,7); atomic_init(&t->state,THREAD_BLOCKED);
  assert(scheduler_remote_wake_publish(cpu,t,7));
  assert(scheduler_remote_wake_publish(cpu,t,7)); assert(t->refs==2);
 }
 /* 旧节点仍待处理时重新阻塞，必须保留新的唤醒。 */
 atomic_store(&threads[0].block_epoch,8);
 assert(!scheduler_remote_wake_publish(cpu,&threads[0],7));
 assert(scheduler_remote_wake_publish(cpu,&threads[0],8));
 unsigned locks_before=atomic_load(&lock_acquisitions);
 uint64_t flags=scheduler_irq_save();scheduler_remote_wake_drain(1,cpu);
 assert(atomic_load(&lock_acquisitions)-locks_before==10000);
 for(unsigned i=0;i<10000;++i)
  assert(threads[i].refs==2&&threads[i].command_release_count==1);
 scheduler_irq_restore(flags);scheduler_command_release_drain(1);
 assert(!atomic_load(&cpu->remote_wake_head));
 for(unsigned i=0;i<10000;++i) {
  assert(threads[i].refs==1 && atomic_load(&threads[i].state)==3);
  assert(!atomic_load(&threads[i].remote_wake_queued));
 }
 atomic_store(&threads[0].state,THREAD_BLOCKED);
 atomic_store(&threads[0].block_epoch,9);
 assert(scheduler_remote_wake_publish(cpu,&threads[0],9));
 atomic_store(&threads[0].block_epoch,10);
 flags=scheduler_irq_save();scheduler_remote_wake_drain(1,cpu);
 scheduler_irq_restore(flags);scheduler_command_release_drain(1);
 assert(atomic_load(&threads[0].state)==THREAD_BLOCKED && threads[0].refs==1);
 /* 通知排入旧 CPU 后发生迁移，必须携带原有引用转发，不能清掉 queued 后丢弃。 */
 thread_t *migrated=&threads[1];
 atomic_store(&migrated->state,THREAD_BLOCKED);atomic_store(&migrated->block_epoch,11);
 assert(scheduler_remote_wake_publish(cpu,migrated,11));
 migrated->owner_cpu=0;
 flags=scheduler_irq_save();scheduler_remote_wake_drain(1,cpu);scheduler_irq_restore(flags);
 assert(atomic_load(&migrated->state)==THREAD_BLOCKED);
 assert(atomic_load(&g_cpus[0].remote_wake_head)==migrated);
 assert(atomic_load(&migrated->remote_wake_queued)&&migrated->refs==2);
 assert(!migrated->command_release_count&&notifications==1);
 current_cpu=0;flags=scheduler_irq_save();scheduler_remote_wake_drain(0,&g_cpus[0]);
 scheduler_irq_restore(flags);scheduler_command_release_drain(0);
 assert(atomic_load(&migrated->state)==3&&migrated->refs==1);
 assert(!atomic_load(&migrated->remote_wake_queued));
 /* 连续迁移并更新阻塞代次：同一节点始终只有一个引用，只在最终所属 CPU 入队。 */
 for(unsigned round=0;round<256;++round){
  migrated->owner_cpu=1;atomic_store(&migrated->state,THREAD_BLOCKED);
  uint32_t epoch=100+round*10;atomic_store(&migrated->block_epoch,epoch);
  assert(scheduler_remote_wake_publish(&g_cpus[1],migrated,epoch));
  uint32_t queued_cpu=1;
  for(unsigned hop=0;hop<8;++hop){
   migrated->owner_cpu=1-queued_cpu;drain_cpu(queued_cpu);queued_cpu=1-queued_cpu;
   assert(atomic_load(&g_cpus[queued_cpu].remote_wake_head)==migrated);
   assert(migrated->refs==2&&!migrated->command_release_count);
   assert(atomic_load(&migrated->state)==THREAD_BLOCKED);
   atomic_store(&migrated->block_epoch,++epoch);
   assert(scheduler_remote_wake_publish(&g_cpus[queued_cpu],migrated,epoch));
   assert(migrated->refs==2);
  }
  drain_cpu(queued_cpu);
  assert(atomic_load(&migrated->state)==3&&migrated->refs==1);
  assert(!atomic_load(&migrated->remote_wake_queued)&&!migrated->remote_wake_next);
 }
 /* 已过期、已唤醒、已退出或无有效所属 CPU 的节点不得被反复转发。 */
 for(unsigned kind=0;kind<4;++kind){
  migrated->owner_cpu=1;atomic_store(&migrated->state,THREAD_BLOCKED);
  atomic_store(&migrated->block_epoch,4000);
  assert(scheduler_remote_wake_publish(&g_cpus[1],migrated,4000));
  migrated->owner_cpu=kind==3?UINT32_MAX:0;
  if(kind==0)atomic_store(&migrated->block_epoch,4001);
  if(kind==1)atomic_store(&migrated->state,3);
  if(kind==2)atomic_store(&migrated->state,4);
  unsigned before=notifications;drain_cpu(1);
  assert(migrated->refs==1&&notifications==before);
  assert(!atomic_load(&migrated->remote_wake_queued));
 }
 /* 一个捕获批次同时包含本地唤醒和迁移转发，next 快照必须保留整条旧链。 */
 for(unsigned i=1;i<=4;++i){
  thread_t *t=&threads[i];t->owner_cpu=1;
  atomic_store(&t->state,THREAD_BLOCKED);atomic_store(&t->block_epoch,5000);
  assert(scheduler_remote_wake_publish(&g_cpus[1],t,5000));t->owner_cpu=i%2;
 }
 drain_cpu(1);
 for(unsigned i=1;i<=4;++i){
  assert(atomic_load(&threads[i].state)==(i%2?3:THREAD_BLOCKED));
  assert(threads[i].refs==(i%2?1:2));
 }
 drain_cpu(0);
 for(unsigned i=1;i<=4;++i)assert(threads[i].refs==1&&atomic_load(&threads[i].state)==3);
 free(threads);
 pthread_t producers[4], consumer,commands;
 for(unsigned i=0;i<4;++i) { stress_threads[i].refs=1; stress_threads[i].owner_cpu=1; }
 assert(!pthread_create(&consumer,NULL,consume,NULL));
 assert(!pthread_create(&commands,NULL,stale_commands,NULL));
 for(unsigned i=0;i<4;++i) assert(!pthread_create(&producers[i],NULL,produce,&stress_threads[i]));
 for(unsigned i=0;i<4;++i) assert(!pthread_join(producers[i],NULL));
 assert(!pthread_join(consumer,NULL));
 assert(!pthread_join(commands,NULL));
 for(unsigned i=0;i<4;++i) {
  assert(stress_threads[i].refs==1 && !atomic_load(&stress_threads[i].remote_wake_queued));
  assert(!stress_threads[i].command_release_count&&!stress_threads[i].command_release_next);
  assert(!atomic_load(&stress_threads[i].remote_wake_lock));
 }
 for(unsigned i=0;i<2;++i)
  assert(!g_cpus[i].command_release_head&&!g_cpus[i].command_release_tail&&
         !atomic_load(&g_cpus[i].command_release_pending));
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-wake-mailbox-') as directory:
    binary = str(pathlib.Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
    negative=program.replace('target != cpu_id && scheduler_cpu_available(target)',
                             'false && scheduler_cpu_available(target)')
    assert negative != program
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True,timeout=30,
                          preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode!=0 and b'remote_wake_head)==migrated' in result.stderr
print('Wake migration: initial lost-wake reproduction, 2048 forwarding hops with updated epochs, mixed local/remote chains, stale/exit cleanup and reference balance passed')
print('Negative control rejected: dropping wake after owner migration')
print('Remote wake: 10000 nodes, coalescing, reblocking, stale epochs and reference balance passed')
print('Mailbox transfer: 10000 nodes require 10000 lock acquisitions instead of 20000; destruction remains deferred')
print('Concurrent wake: four producers, 20000 blocking generations and node reuse passed')
print('Integrated release: 100000 concurrent stale-command refs, actual delayed FIFOs, shared-lock ownership and final reference balance passed')
