"""Actual wait subsystem initialization: publication and atomic traffic."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/sync/wait.c').read_text()
initialize = source[source.index('static void wait_global_init('):
                    source.index('static bool deadline_reached(')]
queue_init = source[source.index('void wait_queue_init('):
                    source.index('static bool wait_without_scheduler(')]
code = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <kernel/wait.h>
static spinlock_t g_timeout_lock;
static list_head_t g_timeout_waiters;
static atomic_uint_fast64_t g_timeout_next_deadline;
static atomic_uint g_wait_init_state,attempts;
static _Bool parallel;
static _Bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
 atomic_fetch_add(&attempts,1);
 if(parallel)while(atomic_load(&attempts)<8)sched_yield();
 return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + initialize.replace('atomic_compare_exchange_strong_explicit(', 'counted_cas(') + queue_init + r'''
static void *worker(void *unused) {
 (void)unused;wait_queue_t queue;wait_queue_init(&queue);
 assert(atomic_load(&g_wait_init_state)==2 && !atomic_load(&g_timeout_lock.state));
 assert(g_timeout_waiters.next==&g_timeout_waiters && g_timeout_waiters.prev==&g_timeout_waiters);
 assert(atomic_load(&g_timeout_next_deadline)==UINT64_MAX);
 assert(queue.waiters.next==&queue.waiters && queue.waiters.prev==&queue.waiters);
 assert(!atomic_load(&queue.lock.state) && !atomic_load(&queue.sequence));
 return 0;
}
int main(void) {
 g_timeout_waiters.next=g_timeout_waiters.prev=(void*)1;
 pthread_t threads[8];parallel=true;
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,worker,0));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 parallel=false;assert(atomic_load(&attempts)==8);
 for(unsigned i=0;i<10000;++i)worker(0);
 wait_queue_init(0);assert(atomic_load(&attempts)==8);
 puts("Wait initialization: eight concurrent initializers/publication and 10000 queue initializations without repeated CAS passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-wait-init-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
