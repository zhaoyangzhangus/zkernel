"""Execute the real stack switch and publish migration only on the new stack."""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
core = (root / 'kernel/sched/core.c').read_text()
begin = core.index('void scheduler_finish_handoff(void) {')
handoff = core[begin:core.index('static inline void scheduler_thread_mailbox_lock(', begin)]
assembly = (root / 'kernel/arch/x86_64/cpu/context_switch.S').read_text()
assembly = assembly[assembly.index('x86_switch_context_root:\n'):]
assert assembly.count('movq %rdx, %cr3') == 1
# The sole host substitution is privileged CR3 writing. Stack/register saves,
# restored continuation, conditional callback and ABI alignment are unchanged.
assembly = '.text\n.globl x86_switch_context_root\n' + assembly.replace('movq %rdx, %cr3', 'nop')
assembly += '\n.globl host_worker_start\nhost_worker_start:\ncall host_worker\nud2\n.section .note.GNU-stack,"",@progbits\n'
program = '#include "' + str(root / 'kernel/sched/internal.h') + '"\n'
program += r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
scheduler_cpu_t g_cpus[MAX_CPUS];
uint32_t g_cpu_count=2;
static arch_switch_context_t main_context,worker_context;
static thread_t migrating;
static uintptr_t worker_base,worker_end;
static unsigned publications,fallbacks;
static bool accept;
bool scheduler_current_cpu(uint32_t *cpu) {*cpu=0;return true;}
static bool scheduler_transfer_local(uint32_t source,scheduler_cpu_t *cpu,
                                     thread_t *thread,uint32_t target) {
    uintptr_t marker=(uintptr_t)&source;
    assert(marker>=worker_base&&marker<worker_end);
    assert(main_context.rsp&&!(main_context.rsp>=worker_base&&main_context.rsp<worker_end));
    assert(source==0&&cpu==&g_cpus[0]&&thread==&migrating&&target==1);
    assert(!cpu->switch_handoff);
    ++publications;if(accept)thread->owner_cpu=1;return accept;
}
void enqueue_local_raw(scheduler_cpu_t *cpu,thread_t *thread) {
    assert(thread==&migrating&&thread->migration_pending);
    ++fallbacks;++cpu->queue.nr_running;cpu->queue_snapshot_dirty=true;
}
''' + handoff + r'''
extern void host_worker_start(void);
void host_worker(void) {
    for(;;)x86_switch_context_root(&worker_context,&main_context,paddr_make(0),false);
}
int main(void) {
    void *stack=aligned_alloc(16,65536);assert(stack);
    worker_base=(uintptr_t)stack;worker_end=worker_base+65536;
    uintptr_t top=worker_end-8;*(uintptr_t *)top=(uintptr_t)host_worker_start;
    worker_context.rsp=top;
    for(unsigned trial=0;trial<30000;++trial) {
        bool migrate=trial%3!=0;accept=trial%2!=0;
        migrating.owner_cpu=0;migrating.migration_target_cpu=1;
        migrating.migration_pending=false;
        g_cpus[0].switch_handoff=migrate?&migrating:0;
        g_cpus[0].queue.nr_running=0;g_cpus[0].queue_snapshot_dirty=false;
        atomic_store(&g_cpus[0].need_resched,false);
        unsigned before=publications,failed=fallbacks;
        register uint64_t saved12 __asm__("r12")=UINT64_C(0xaabbccdd00000000)+trial;
        register uint64_t saved13 __asm__("r13")=~saved12;
        __asm__ volatile("" : "+r"(saved12), "+r"(saved13));
        x86_switch_context_root(&main_context,&worker_context,paddr_make(0),migrate);
        __asm__ volatile("" : "+r"(saved12), "+r"(saved13));
        assert(saved12==UINT64_C(0xaabbccdd00000000)+trial&&saved13==~saved12);
        assert(publications==before+migrate&&!g_cpus[0].switch_handoff);
        assert(fallbacks==failed+(migrate&&!accept));
        assert(migrating.owner_cpu==(migrate&&accept?1:0));
        assert(migrating.migration_pending==(migrate&&!accept));
        assert(atomic_load(&g_cpus[0].need_resched)==(migrate&&!accept));
    }
    free(stack);
    puts("Switch handoff: 60000 real assembly stack switches; publication only after context save on new stack; no normal-switch callback; register preservation and failed-publication retry passed");
}
'''
with tempfile.TemporaryDirectory(prefix='scheduler-switch-handoff-') as directory:
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    path = Path(directory)
    (path / 'test.c').write_text(program)
    for label, code in [('production', assembly), ('negative', assembly.replace(
            'x86_switch_context_root:\n',
            'x86_switch_context_root:\nsubq $8,%rsp\ncall scheduler_finish_handoff\naddq $8,%rsp\n', 1))]:
        (path / 'switch.S').write_text(code)
        binary = str(path / label)
        subprocess.run(['cc', '-std=gnu11', '-O2', '-mno-red-zone', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=undefined', '-fno-pie', '-no-pie',
                        '-I' + str(root / 'include'), str(path / 'test.c'),
                        str(path / 'switch.S'), '-o', binary], check=True)
        result = subprocess.run([binary], capture_output=label == 'negative', timeout=20)
        if label == 'production':
            assert result.returncode == 0
        else:
            assert result.returncode == -signal.SIGABRT, result.stderr
            print('Negative control rejected: publishing on the old stack before context save')
