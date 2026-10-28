"""验证 PI 优先级邮箱在命令环已满、合并、迁移和退出时仍可前进。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sched/core.c').read_text()


def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2] + '\n'


production = ''.join(function(signature) for signature in (
    'static inline void scheduler_thread_mailbox_lock(',
    'static inline void scheduler_thread_mailbox_unlock(',
    'static void scheduler_priority_link(',
    'static void scheduler_priority_drain(',
    'void sched_set_effective_priority('))
fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/sched/internal.h"
scheduler_cpu_t g_cpus[MAX_CPUS];
uint32_t g_cpu_count=4;
static _Thread_local uint32_t current_cpu;
static _Thread_local bool irq=true;
static bool migrate_on_save,fail_get;
static unsigned applied,notifies,released;
static uint64_t scheduler_irq_save(void){
    bool old=irq;irq=false;
    if(migrate_on_save){current_cpu=1;migrate_on_save=false;}
    return old?1ULL<<9:0;
}
static void scheduler_irq_restore(uint64_t flags){irq=(flags&(1ULL<<9))!=0;}
bool scheduler_current_cpu(uint32_t *out){assert(!irq);*out=current_cpu;return true;}
bool scheduler_cpu_available(uint32_t cpu){return cpu<g_cpu_count;}
bool object_try_get(void *object){
    assert(!irq);if(fail_get)return false;
    thread_t *thread=object;atomic_fetch_add(&thread->object.refs.value,1U);return true;
}
static void scheduler_apply_priority_local(scheduler_cpu_t *cpu,thread_t *thread,
                                           uint8_t cls,uint8_t priority){
    assert(!irq && cpu==&g_cpus[current_cpu] && thread->owner_cpu==current_cpu);
    assert(atomic_load(&thread->state)!=THREAD_DEAD);
    thread->sched_class=cls;thread->rt_priority=cls==SCHED_CLASS_RT?priority:0;++applied;
}
static void scheduler_command_release_enqueue_locked(scheduler_cpu_t *cpu,thread_t *thread){
    assert(!irq && cpu==&g_cpus[current_cpu] && atomic_load(&thread->remote_wake_lock));
    ++thread->command_release_count;
}
bool x86_smp_request_reschedule(uint32_t cpu){assert(cpu<4);__atomic_fetch_add(&notifies,1U,__ATOMIC_RELAXED);return true;}
''' + production + r'''
static thread_t threads[256];
static void initialize(thread_t *thread,uint32_t owner){
    memset(thread,0,sizeof(*thread));atomic_init(&thread->object.refs.value,1U);
    atomic_init(&thread->state,THREAD_BLOCKED);atomic_init(&thread->priority_queued,false);
    atomic_init(&thread->priority_request,0U);atomic_init(&thread->remote_wake_lock,0U);
    thread->owner_cpu=owner;thread->sched_class=SCHED_CLASS_FAIR;
}
static void drain(uint32_t cpu){
    current_cpu=cpu;uint64_t flags=scheduler_irq_save();
    scheduler_priority_drain(cpu,&g_cpus[cpu]);scheduler_irq_restore(flags);
}
static void release(thread_t *thread){
    assert(irq && !atomic_load(&thread->priority_queued));
    while(thread->command_release_count){--thread->command_release_count;
        assert(atomic_fetch_sub(&thread->object.refs.value,1U)>1);++released;}
    assert(atomic_load(&thread->object.refs.value)==1);
}
static void *producer(void *arg){
    current_cpu=(uint32_t)(uintptr_t)arg;
    for(unsigned n=0;n<10000;++n)
        sched_set_effective_priority(&threads[n%256],SCHED_CLASS_RT,(uint8_t)(n%RT_PRIORITY_LEVELS));
    return 0;
}
int main(void){
    /* 所有有界环都已占满；优先级发布不得读取或等待它们的空槽。 */
    sched_cmd_channel_t full[4];memset(full,0,sizeof(full));
    for(unsigned cpu=0;cpu<4;++cpu){
        atomic_init(&full[cpu].primary.tail,SCHED_CMD_SLOTS);
        atomic_init(&full[cpu].overflow.tail,SCHED_CMD_OVERFLOW_SLOTS);
        g_cpus[cpu].command_inbox=full;
        atomic_init(&g_cpus[cpu].priority_head,0);
        atomic_init(&g_cpus[cpu].command_pending,false);
    }
    thread_t *t=&threads[0];initialize(t,2);
    for(unsigned i=0;i<10000;++i)sched_set_effective_priority(t,SCHED_CLASS_RT,3);
    sched_set_effective_priority(t,SCHED_CLASS_FAIR,0);
    assert(atomic_load(&t->object.refs.value)==2 && atomic_load(&t->priority_queued));
    drain(2);assert(applied==1 && t->sched_class==SCHED_CLASS_FAIR);
    assert(atomic_load(&t->state)==THREAD_BLOCKED);release(t);

    /* 多个生产者同时更新超过环容量的线程，每个线程仍只保留一个节点。 */
    for(unsigned i=0;i<256;++i)initialize(&threads[i],2);
    pthread_t workers[3];const unsigned cpus[]={0,1,3};
    for(unsigned i=0;i<3;++i)assert(!pthread_create(&workers[i],0,producer,(void*)(uintptr_t)cpus[i]));
    for(unsigned i=0;i<3;++i)assert(!pthread_join(workers[i],0));
    unsigned before=applied;drain(2);assert(applied-before==256);
    for(unsigned i=0;i<256;++i)release(&threads[i]);

    initialize(t,2);current_cpu=0;sched_set_effective_priority(t,SCHED_CLASS_RT,4);
    t->owner_cpu=1;before=applied;drain(2);assert(applied==before && atomic_load(&t->priority_queued));
    current_cpu=3;sched_set_effective_priority(t,SCHED_CLASS_RT,2);
    drain(1);assert(t->rt_priority==2 && applied==before+1);release(t);

    /* 本地恢复必须覆盖迁移前邮箱中的旧捐赠，不能稍后重新升优先级。 */
    initialize(t,2);current_cpu=0;sched_set_effective_priority(t,SCHED_CLASS_RT,4);
    t->owner_cpu=1;current_cpu=0;migrate_on_save=true;
    sched_set_effective_priority(t,SCHED_CLASS_FAIR,0);assert(current_cpu==1 && t->sched_class==SCHED_CLASS_FAIR);
    drain(2);drain(1);assert(t->sched_class==SCHED_CLASS_FAIR);release(t);

    initialize(t,2);current_cpu=0;sched_set_effective_priority(t,SCHED_CLASS_RT,1);
    atomic_store(&t->state,THREAD_DEAD);before=applied;drain(2);assert(applied==before);release(t);
    initialize(t,2);current_cpu=0;fail_get=true;
    sched_set_effective_priority(t,SCHED_CLASS_RT,1);assert(!atomic_load(&t->priority_queued));fail_get=false;
    sched_set_effective_priority(0,SCHED_CLASS_RT,1);
    sched_set_effective_priority(t,SCHED_CLASS_IDLE,0);
    sched_set_effective_priority(t,SCHED_CLASS_RT,RT_PRIORITY_LEVELS);
    assert(irq && !atomic_load(&t->priority_queued));
    assert(released==260 && notifies==262);
    puts("Priority mailbox: full rings, 40000 updates, 256 threads/three producers, latest restore, migration, exit and balanced references passed");
}
'''
with tempfile.TemporaryDirectory(prefix='scheduler-priority-mailbox-') as directory:
    binary = str(Path(directory) / 'test')
    command = ['cc', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
               '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-I'+str(root/'include'), '-I'+str(root), '-x', 'c', '-', '-o', binary]
    subprocess.run(command, input=fixture, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
    # 负对照模拟再次回退到等待有界命令环，必须被满环断言拒绝。
    negative = fixture.replace('scheduler_priority_link(&g_cpus[cpu_id], thread);',
                               'assert(!"priority waits on full command ring");')
    assert negative != fixture
    subprocess.run(command, input=negative, text=True, check=True)
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    result = subprocess.run([binary], capture_output=True, timeout=10, preexec_fn=no_core)
    assert result.returncode != 0 and b'priority waits on full command ring' in result.stderr
    print('Negative control rejected: priority publication waits on full command ring')
