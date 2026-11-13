"""Production queue lookup/once initialization: publication and atomic traffic."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/block/queue.c').read_text()
state = source[source.index('#define BLOCK_DEVICE_QUEUE_LIMIT'):
               source.index('static bool block_request_completed(')]
functions = source[source.index('static void block_initialize('):
                   source.index('static bool bio_page_vectors_valid(')]
functions = functions.replace('for (uint32_t i = 0; i < g_block_queue_count; ++i) {',
    'for (uint32_t i = 0; i < g_block_queue_count; ++i) { atomic_fetch_add(&scan_visits,1);')
code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <kernel/spinlock.h>
struct device {unsigned id;};
typedef struct {unsigned marker;} wait_queue_t;
typedef struct {uint32_t cpu_count;} x86_acpi_platform_t;
static x86_acpi_platform_t platform={3};
static bool missing_platform,parallel_init;
static atomic_uint init_attempts,waitq_inits,scan_visits;
static _Thread_local unsigned depth;
static void sched_preempt_disable(void) { ++depth; }
static void sched_preempt_enable(void) { assert(depth); --depth; }
static const x86_acpi_platform_t *x86_acpi_platform(void) {
 return missing_platform?0:&platform;
}
static void wait_queue_init(wait_queue_t *q) {
 atomic_fetch_add(&waitq_inits,1);
 if(parallel_init)while(atomic_load(&init_attempts)<8)sched_yield();
 q->marker=0x75;
}
''' + state + r'''
static bool counted_compare(atomic_uint *p,unsigned *expected,unsigned value,
                           memory_order success,memory_order failure) {
 assert(p==&g_block_init_state);atomic_fetch_add(&init_attempts,1);
 return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
#undef atomic_compare_exchange_strong_explicit
#define atomic_compare_exchange_strong_explicit counted_compare
''' + functions + r'''
static struct device devices[65];
static block_queue_state_t *results[8];
static block_queue_state_t *shared_results[8];
static pthread_barrier_t barrier;
static void *worker(void *context) {
 unsigned id=(unsigned)(uintptr_t)context;
 int status=pthread_barrier_wait(&barrier);
 assert(status==0 || status==PTHREAD_BARRIER_SERIAL_THREAD);
 block_queue_state_t *p=block_queue_for(&devices[id]);
 assert(p && p->device==&devices[id] && p->queue_count==3 && !depth);
 assert(g_block_completion_queue.marker==0x75);
 shared_results[id]=block_queue_for(&devices[8]);assert(shared_results[id]);
 for(unsigned i=0;i<10000;++i)assert(block_queue_for(&devices[id])==p);
 assert(!depth); results[id]=p;return 0;
}
int main(void) {
 assert(!block_queue_for(0) && !atomic_load(&init_attempts));
 parallel_init=true;pthread_t threads[8];assert(!pthread_barrier_init(&barrier,0,8));
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,worker,(void*)(uintptr_t)i));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 assert(!pthread_barrier_destroy(&barrier));parallel_init=false;
 assert(atomic_load(&waitq_inits)==1 && atomic_load(&init_attempts)==8);
 assert(atomic_load(&g_block_init_state)==2 && !atomic_load(&g_block_lock.state));
 assert(g_block_queue_count==9);
 for(unsigned i=0;i<8;++i)for(unsigned j=0;j<i;++j)assert(results[i]!=results[j]);
 for(unsigned i=0;i<8;++i)assert(shared_results[i]==shared_results[0]);
 atomic_store(&init_attempts,0);
 for(unsigned i=0;i<64;++i) {
  block_queue_state_t *p=block_queue_for(&devices[i]);assert(p && p->device==&devices[i]);
  assert(p->queue_count==3 && !atomic_load(&p->next_queue));
 }
 assert(!block_queue_for(&devices[64]));
 for(unsigned i=0;i<64;++i)assert(block_queue_for(&devices[i]));
 assert(!atomic_load(&init_attempts) && !atomic_load(&g_block_lock.state));
 for(unsigned nesting=0;nesting<32;++nesting) {
  depth=nesting;
  assert(!block_queue_for(0) && depth==nesting);
  assert(block_queue_for(&devices[0])==results[0] && depth==nesting);
  assert(!block_queue_for(&devices[64]) && depth==nesting);
  assert(!atomic_load(&g_block_lock.state));
 }
 depth=0;
 atomic_store(&g_block_init_state,0); atomic_store(&scan_visits,0);
 for(unsigned i=0;i<64;++i) {
  unsigned before=atomic_load(&scan_visits);
  assert(block_queue_for(&devices[i])==&g_block_queues[i]);
  assert(atomic_load(&scan_visits)-before==i && g_block_queue_count==i+1);
 }
 assert(atomic_load(&scan_visits)==2016);
 for(unsigned i=0;i<64;++i) {
  unsigned before=atomic_load(&scan_visits);
  assert(block_queue_for(&devices[i])==&g_block_queues[i]);
  assert(atomic_load(&scan_visits)-before==i+1);
 }
 const unsigned counts[]={0,1,3,8,9,UINT32_MAX};
 for(unsigned i=0;i<sizeof(counts)/sizeof(counts[0]);++i) {
  /* 仅在所有工作线程都已汇合后重置，以测试彼此独立的启动状态。 */
  atomic_store(&g_block_init_state,0);platform.cpu_count=counts[i];
  block_queue_state_t *p=block_queue_for(&devices[0]);
  unsigned expected=counts[i]?counts[i]:1;if(expected>8)expected=8;
  assert(p && p->queue_count==expected && !atomic_load(&p->next_queue));
 }
 atomic_store(&g_block_init_state,0);missing_platform=true;
 assert(block_queue_for(&devices[0])->queue_count==1);
 puts("Block queues: eight-way once publication, 80000 lookups without extra initialization CAS, unique/stable slots, 64-slot exhaustion/CPU caps, 32 nested depths and append scans 2016 versus 4096 passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-block-queue-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
