"""Execute production deferred-worker construction and concurrent-start guards."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/irq/deferred.c').read_text()
layout = source[source.index('#define DEFERRED_QUEUE_CAPACITY'):
                source.index('static void deferred_debug_set(')]
startup = source[source.index('bool deferred_start_worker(void) {'):
                 source.index('bool deferred_worker_started(void) {')]
program = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <arch/x86_64/context.h>
#include <kernel/deferred.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
''' + layout + r'''
static uint8_t g_deferred_worker_stack[DEFERRED_WORKER_STACK_SIZE]
    __attribute__((aligned(16)));
static thread_t bootstrap;
static bool have_current,fail_enqueue;
static uint32_t cpu;
static atomic_uint constructors,enqueues,monitors;
static atomic_bool pause_constructor,entered,release_constructor;
static _Thread_local bool constructing;
bool deferred_init(void){return true;}
thread_t *sched_current_thread(void){return have_current?&bootstrap:NULL;}
uint32_t x86_current_cpu_index(void){return cpu;}
static __attribute__((noreturn)) void deferred_worker_main(void *arg){(void)arg;abort();}
void x86_kernel_thread_start(void){abort();}
uint64_t process_allocate_task_id(void){
    assert(!constructing);constructing=true;
    unsigned n=atomic_fetch_add(&constructors,1)+1;
    /* 在 READY 尚未发布时重入，不得清零正在初始化的线程。 */
    assert(!deferred_start_worker());
    atomic_store(&entered,true);
    while(atomic_load(&pause_constructor)&&!atomic_load(&release_constructor))sched_yield();
    constructing=false;return 100+n;
}
void sched_initialize_new_thread(thread_t *t,uint32_t c,bool pinned){
    assert(t==&g_deferred.worker&&c==cpu&&pinned);
    t->owner_cpu=c;atomic_init(&t->state,THREAD_NEW);
}
bool sched_enqueue_bootstrap(thread_t *t){
    assert(t==&g_deferred.worker&&atomic_load(&t->state)==THREAD_READY);
    assert(atomic_load(&g_deferred.worker_started));
    assert(t->object.type==KOBJECT_TYPE_THREAD&&!t->process);
    assert(t->arch.switch_ctx.r12==(uintptr_t)deferred_worker_main);
    assert(t->arch.switch_ctx.r14==t->kernel_stack_top);
    assert(t->kernel_stack_base==g_deferred_worker_stack);
    assert(t->kernel_stack_size==sizeof(g_deferred_worker_stack));
    atomic_fetch_add(&enqueues,1);return !fail_enqueue;
}
void sched_monitor_register(thread_t *t,const char *name){
    assert(t==&g_deferred.worker&&!strcmp(name,"irq-deferred"));
    atomic_fetch_add(&monitors,1);
}
void *start(void *arg){return (void *)(uintptr_t)deferred_start_worker();}
''' + startup + r'''
int main(void){
    assert(!deferred_start_worker()&&!constructors);
    have_current=true;cpu=MAX_CPUS;
    assert(!deferred_start_worker()&&!constructors);cpu=0;
    fail_enqueue=true;
    assert(!deferred_start_worker()&&constructors==1&&enqueues==1&&!monitors);
    assert(!atomic_load(&g_deferred.worker_started));
    assert(!atomic_load(&g_deferred.worker_starting));
    fail_enqueue=false;
    assert(deferred_start_worker()&&constructors==2&&enqueues==2&&monitors==1);
    assert(deferred_start_worker()&&constructors==2&&enqueues==2&&monitors==1);
    for(cpu=0;cpu<MAX_CPUS;++cpu){
        atomic_store(&g_deferred.worker_started,false);
        atomic_store(&g_deferred.worker_starting,false);
        atomic_store(&pause_constructor,true);atomic_store(&entered,false);
        atomic_store(&release_constructor,false);
        unsigned before=constructors;
        pthread_t owner,competitors[8];
        assert(!pthread_create(&owner,NULL,start,NULL));
        while(!atomic_load(&entered))sched_yield();
        for(unsigned i=0;i<8;++i)assert(!pthread_create(&competitors[i],NULL,start,NULL));
        for(unsigned i=0;i<8;++i){
            void *result;assert(!pthread_join(competitors[i],&result));assert(!result);
        }
        assert(constructors==before+1&&!atomic_load(&g_deferred.worker_started));
        atomic_store(&release_constructor,true);
        void *result;assert(!pthread_join(owner,&result)&&result==(void *)1);
        assert(!atomic_load(&g_deferred.worker_starting));
        assert(deferred_start_worker()&&constructors==before+1);
    }
    assert(constructors==MAX_CPUS+2&&enqueues==constructors&&monitors==constructors-1);
    puts("Deferred startup: every CPU, 8 concurrent callers, reentry, failed enqueue/retry and idempotent success PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='deferred-startup-') as directory:
    binary = directory + '/test'
    command = ['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-variable', '-Wno-unused-parameter', '-pthread',
               '-DOS_KERNEL_BUILD', '-fsanitize=address,undefined',
               '-fno-pie', '-no-pie', '-I' + str(root / 'include'),
               '-x', 'c', '-', '-o', binary]
    subprocess.run(command, input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
    before = 'if (!atomic_compare_exchange_strong_explicit(&g_deferred.worker_starting,'
    after = 'if (false && !atomic_compare_exchange_strong_explicit(&g_deferred.worker_starting,'
    broken = program.replace(before, after)
    assert broken != program
    subprocess.run(command, input=broken, text=True, check=True)
    result = subprocess.run([binary], capture_output=True, timeout=60,
                            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode != 0
    print('Negative control rejected: concurrent worker reconstruction')
