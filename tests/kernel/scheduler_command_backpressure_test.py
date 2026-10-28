"""Actual single-attempt publisher: saturated migration must not spin or put inline."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sched/core.c').read_text()
begin = source.index('static inline bool scheduler_command_publish_snapshot(')
rings = source[begin:source.index('static bool scheduler_remote_command_push(', begin)]
begin = source.index('static bool scheduler_remote_command_try_push_once(')
once = source[begin:source.index('static bool scheduler_remote_command_try_push(', begin)]
assert 'object_put(' not in once and 'for (' not in once and 'while (' not in once
program = '#include "' + str(root / 'kernel/sched/internal.h') + '"\n'
program += r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
scheduler_cpu_t g_cpus[MAX_CPUS];
uint32_t g_cpu_count=2;
static sched_cmd_channel_t channels[2][2];
static thread_t threads[2];
static unsigned deferred_refs[2];
static atomic_uint notifications;
static bool release_foreign;
bool object_try_get(void *object) {
    thread_t *t=object;
    unsigned refs=atomic_load(&t->object.refs.value);
    if(!refs)return false;
    atomic_fetch_add(&t->object.refs.value,1);
    if(release_foreign)atomic_fetch_sub(&t->object.refs.value,1);
    return true;
}
void object_put(void *object) {(void)object;assert(!"inline final put in IRQ-masked publisher");}
static void scheduler_command_release_enqueue_local(scheduler_cpu_t *cpu,thread_t *t) {
    unsigned index=(unsigned)(cpu-g_cpus);
    assert(index<2&&t==&threads[index]);++deferred_refs[index];
}
bool x86_smp_request_reschedule(uint32_t target) {
    assert(target<2);atomic_fetch_add(&notifications,1);return true;
}
''' + rings + once + r'''
static void reset(void) {
    memset(g_cpus,0,sizeof(g_cpus));memset(channels,0,sizeof(channels));
    memset(threads,0,sizeof(threads));memset(deferred_refs,0,sizeof(deferred_refs));
    atomic_store(&notifications,0);release_foreign=false;
    for(unsigned c=0;c<2;++c) {
        g_cpus[c].command_inbox=channels[c];refcount_init(&threads[c].object.refs,1);
    }
}
static void fill(unsigned source) {
    sched_cmd_channel_t *q=&channels[1-source][source];
    q->overflow_active=true;
    atomic_store(&q->primary.head,UINT32_MAX-3);
    atomic_store(&q->primary.tail,UINT32_MAX-3+SCHED_CMD_SLOTS);
    atomic_store(&q->overflow.head,UINT32_MAX-30);
    atomic_store(&q->overflow.tail,UINT32_MAX-30+SCHED_CMD_OVERFLOW_SLOTS);
}
static void *full_sender(void *argument) {
    unsigned cpu=(unsigned)(uintptr_t)argument;
    for(unsigned i=0;i<20000;++i)
        assert(!scheduler_remote_command_try_push_once(cpu,1-cpu,&threads[cpu],SCHED_CMD_ENQUEUE,i));
    return 0;
}
int main(void) {
    reset();
    assert(!scheduler_remote_command_try_push_once(2,0,&threads[0],2,0));
    assert(!scheduler_remote_command_try_push_once(0,2,&threads[0],2,0));
    assert(!scheduler_remote_command_try_push_once(0,0,&threads[0],2,0));
    assert(!scheduler_remote_command_try_push_once(0,1,0,2,0));
    g_cpus[1].command_inbox=0;
    assert(!scheduler_remote_command_try_push_once(0,1,&threads[0],2,0));
    assert(atomic_load(&threads[0].object.refs.value)==1&&!deferred_refs[0]);
    g_cpus[1].command_inbox=channels[1];
    atomic_store(&threads[0].object.refs.value,0);
    assert(!scheduler_remote_command_try_push_once(0,1,&threads[0],2,0));
    assert(!deferred_refs[0]&&!atomic_load(&notifications));
    reset();
    assert(scheduler_remote_command_try_push_once(0,1,&threads[0],SCHED_CMD_ENQUEUE,37));
    assert(atomic_load(&threads[0].object.refs.value)==2&&!deferred_refs[0]);
    assert(atomic_load(&g_cpus[1].command_pending)&&!atomic_load(&notifications));
    assert(channels[1][0].primary.slots[0].object==(uintptr_t)&threads[0]);
    reset();fill(0);fill(1);
    pthread_t a,b;
    assert(!pthread_create(&a,0,full_sender,(void *)0));
    assert(!pthread_create(&b,0,full_sender,(void *)1));
    assert(!pthread_join(a,0)&&!pthread_join(b,0));
    for(unsigned c=0;c<2;++c) {
        assert(deferred_refs[c]==20000);
        assert(atomic_load(&threads[c].object.refs.value)==20001);
        assert(g_cpus[c].command_ring_full_count==20000);
        assert(!atomic_load(&g_cpus[c].command_pending));
        atomic_fetch_sub(&threads[c].object.refs.value,deferred_refs[c]);
        assert(atomic_load(&threads[c].object.refs.value)==1);
    }
    assert(atomic_load(&notifications)==40000);
    /* The external owner can leave after try_get: the retained command pin
     * must be handed to deferred release, not destroy the thread inline. */
    reset();fill(0);release_foreign=true;
    assert(!scheduler_remote_command_try_push_once(0,1,&threads[0],2,0));
    assert(atomic_load(&threads[0].object.refs.value)==1&&deferred_refs[0]==1);
    /* Retry after capacity returns transfers exactly one new pin. */
    release_foreign=false;
    atomic_store(&channels[1][0].overflow.head,atomic_load(&channels[1][0].overflow.tail));
    atomic_store(&channels[1][0].primary.head,atomic_load(&channels[1][0].primary.tail));
    assert(scheduler_remote_command_try_push_once(0,1,&threads[0],2,99));
    assert(atomic_load(&threads[0].object.refs.value)==2&&deferred_refs[0]==1);
    puts("Command backpressure: 40000 simultaneous bidirectional full-queue rejections terminate without spinning; exact deferred pins, foreign last-owner exit, wrap, retry and invalid inputs passed");
}
'''
with tempfile.TemporaryDirectory(prefix='scheduler-command-backpressure-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-pthread', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-I' + str(root / 'include'),
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=20)
