"""Actual affinity update: invalid input isolation and unchanged migration routes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sched/core.c').read_text()
start = source.index('kstatus_t sched_set_affinity(')
function = source[start:source.index('void sched_set_effective_priority(', start)]
previous = function
previous = previous.replace('        if (requested.bits[word] != 0) any = true;',
                            '        thread->affinity.bits[word] = mask->bits[word];\n'
                            '        if (mask->bits[word] != 0) any = true;')
previous = previous.replace('    thread->affinity = requested;\n', '')
previous = previous.replace('sched_set_affinity(', 'previous_set_affinity(', 1)
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#define MAX_CPUS 256
#define K_EINVAL -22
#define K_EBUSY -16
#define K_OK 0
#define THREAD_NEW 0
#define THREAD_DEAD 1
#define THREAD_READY 2
#define THREAD_RUNNING 3
#define THREAD_BLOCKED 4
#define SCHED_CMD_MIGRATE 8
typedef int kstatus_t;
typedef struct {uint64_t bits[MAX_CPUS/64];} cpumask_t;
typedef struct {cpumask_t affinity;atomic_uint state;uint16_t owner_cpu;
                bool migration_pending;unsigned untouched;} thread_t;
typedef struct {void *command_inbox;} scheduler_cpu_t;
static scheduler_cpu_t g_cpus[MAX_CPUS];
static unsigned g_cpu_count=4,current_cpu,target_cpu;
static bool current_valid=true,push_ok=true,local_pending;
static bool select_found=true;
static unsigned offline_cpu=UINT32_MAX;
static thread_t *preflight_thread;
static cpumask_t preflight_before;
static unsigned current_calls,choose_calls,push_calls,migrate_calls,ipi_calls,irq_depth;
static bool scheduler_current_cpu(uint32_t *out) {
    ++current_calls;*out=current_cpu;return current_valid;
}
static bool scheduler_cpu_available(unsigned cpu) {return cpu<g_cpu_count&&cpu!=offline_cpu;}
static bool scheduler_affinity_mask_allows(const cpumask_t *m,unsigned cpu) {
    return m&&cpu<MAX_CPUS&&(m->bits[cpu/64]&(1ULL<<(cpu%64)));
}
static bool scheduler_select_cpu_mask(const cpumask_t *m,unsigned current,unsigned *selected) {
    assert(m&&current==current_cpu);
    assert(preflight_thread&&!memcmp(&preflight_thread->affinity,&preflight_before,sizeof(preflight_before)));
    ++choose_calls;if(select_found)*selected=target_cpu;return select_found;
}
static uint64_t scheduler_irq_save(void) {assert(!irq_depth);irq_depth=1;return 37;}
static void scheduler_irq_restore(uint64_t f) {assert(f==37&&irq_depth==1);irq_depth=0;}
static bool scheduler_remote_command_try_push(unsigned current,unsigned owner,
                                              thread_t *t,unsigned op,unsigned target) {
    assert(current==current_cpu&&owner==t->owner_cpu&&target==target_cpu);
    assert(op==SCHED_CMD_MIGRATE&&irq_depth==1);++push_calls;return push_ok;
}
static void scheduler_migrate_local(unsigned owner,scheduler_cpu_t *cpu,
                                   thread_t *t,unsigned target) {
    assert(owner==current_cpu&&cpu==&g_cpus[owner]&&target==target_cpu&&irq_depth==1);
    ++migrate_calls;t->migration_pending=local_pending;
}
static bool x86_smp_request_reschedule(unsigned cpu) {
    assert(cpu<g_cpu_count&&!irq_depth);++ipi_calls;return true;
}
'''+function+previous+r'''
static void reset(void) {
    current_calls=choose_calls=push_calls=migrate_calls=ipi_calls=irq_depth=0;
}
int main(void) {
    cpumask_t empty={0},mask={0};thread_t t,before;unsigned cases=0;
    for(unsigned i=0;i<1000;++i) {
        memset(&t,(unsigned char)(i%254+1),sizeof(t));atomic_init(&t.state,THREAD_READY);
        memcpy(&before,&t,sizeof(t));reset();
        assert(sched_set_affinity(&t,&empty)==K_EINVAL);
        assert(!memcmp(&t,&before,sizeof(t))&&!current_calls&&!choose_calls&&!ipi_calls);
        ++cases;
    }
    assert(previous_set_affinity(&t,&empty)==K_EINVAL);
    assert(memcmp(&t.affinity,&before.affinity,sizeof(t.affinity))!=0);
    puts("Negative control reproduces the old rejected-empty-mask state corruption");
    memcpy(&t,&before,sizeof(t));reset();
    assert(sched_set_affinity(0,&mask)==K_EINVAL);
    assert(sched_set_affinity(&t,0)==K_EINVAL&&!memcmp(&t,&before,sizeof(t)));
    g_cpu_count=0;assert(sched_set_affinity(&t,&mask)==K_EINVAL&&!memcmp(&t,&before,sizeof(t)));
    g_cpu_count=4;mask.bits[3]=1;current_valid=false;
    assert(sched_set_affinity(&t,&mask)==K_EINVAL&&!memcmp(&t,&before,sizeof(t)));
    assert(current_calls==1&&!choose_calls&&!ipi_calls);current_valid=true;
    for(unsigned state=THREAD_NEW;state<=THREAD_BLOCKED;++state)
    for(unsigned owner=0;owner<4;++owner)
    for(unsigned target=0;target<4;++target)
    for(unsigned inbox=0;inbox<2;++inbox)
    for(unsigned success=0;success<2;++success)
    for(unsigned pending=0;pending<2;++pending) {
        t=(thread_t){.owner_cpu=owner,.migration_pending=true,.untouched=0x12345678};
        atomic_init(&t.state,state);mask=(cpumask_t){0};mask.bits[0]=1ULL<<target;
        for(unsigned cpu=0;cpu<4;++cpu)g_cpus[cpu].command_inbox=inbox?&t:0;
        current_cpu=0;target_cpu=target;push_ok=success;local_pending=pending;
        preflight_thread=&t;preflight_before=t.affinity;reset();
        int expected=K_OK;
        if(owner!=target&&state!=THREAD_NEW&&state!=THREAD_DEAD&&owner!=0)
            expected=inbox&&success?K_OK:K_EBUSY;
        assert(sched_set_affinity(&t,&mask)==expected);
        assert(!memcmp(&t.affinity,&mask,sizeof(mask))&&t.untouched==0x12345678);
        assert(atomic_load(&t.state)==state&&current_calls==1&&!irq_depth);
        assert(choose_calls==(owner!=target));
        if(owner==target)assert(t.owner_cpu==owner&&t.migration_pending&&!push_calls&&!migrate_calls&&!ipi_calls);
        else if(state==THREAD_NEW||state==THREAD_DEAD)
            assert(t.owner_cpu==target&&!t.migration_pending&&!push_calls&&!migrate_calls&&!ipi_calls);
        else if(owner==0)assert(migrate_calls==1&&!push_calls&&ipi_calls==pending&&t.migration_pending==pending);
        else assert(!migrate_calls&&push_calls==inbox&&ipi_calls==(inbox&&success));
        ++cases;
    }
    for(unsigned state=THREAD_NEW;state<=THREAD_BLOCKED;++state)
    for(unsigned cpu=1;cpu<MAX_CPUS;++cpu) {
        t=(thread_t){.owner_cpu=0,.migration_pending=true,.untouched=29};
        atomic_init(&t.state,state);t.affinity.bits[0]=1;before=t;
        mask=(cpumask_t){0};mask.bits[cpu/64]=1ULL<<(cpu%64);
        /* 重新验证选中目标，处理 CPU 下线竞争；只包含未发现 CPU 的掩码应报告未选中，
         * 且不得发布回退 CPU。 */
        offline_cpu=cpu<g_cpu_count?cpu:UINT32_MAX;
        target_cpu=cpu<g_cpu_count?cpu:current_cpu;
        select_found=cpu<g_cpu_count;
        preflight_thread=&t;preflight_before=t.affinity;reset();
        assert(sched_set_affinity(&t,&mask)==K_EINVAL);
        assert(!memcmp(&t,&before,sizeof(t))&&choose_calls==1);
        assert(!push_calls&&!migrate_calls&&!ipi_calls&&!irq_depth);++cases;
    }
    offline_cpu=UINT32_MAX;select_found=true;
    /* 自身掩码别名操作恰好使用经过验证的快照。 */
    for(unsigned cpu=0;cpu<4;++cpu) {
        t=(thread_t){.owner_cpu=cpu,.untouched=19};atomic_init(&t.state,THREAD_NEW);
        t.affinity.bits[0]=1ULL<<cpu;before=t;reset();
        assert(sched_set_affinity(&t,&t.affinity)==K_OK&&!memcmp(&t,&before,sizeof(t)));
        ++cases;
    }
    printf("Affinity update: %u empty-mask/layout/migration/alias cases, no invalid-input mutation, IRQ balance and existing queue/IPC routes passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-affinity-update-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
