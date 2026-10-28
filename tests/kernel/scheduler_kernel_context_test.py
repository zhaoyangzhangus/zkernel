"""执行真实内核入口上下文助手，验证寄存器、栈边界及独立架构状态。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root / 'include/arch/x86_64/context.h').read_text()
start = header.index('static inline void x86_prepare_kernel_thread(')
end = header.index('\n}', start) + 2
helper = header[start:end]
scheduler = (root / 'kernel/sched/core.c').read_text()
idle_start = scheduler.index('static void scheduler_init_idle_context(')
idle_end = scheduler.index('\n}', idle_start) + 2
idle = scheduler[idle_start:idle_end]
program = r'''
#include <arch/x86_64/context.h>
#include <kernel/process.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
void x86_kernel_thread_start(void) { __builtin_trap(); }
static void entry(void *argument) { (void)argument; }
static void scheduler_idle_main(void *argument) { (void)argument; }
IDLE_IMPLEMENTATION
int main(void) {
    _Alignas(16) unsigned char stack[256];
    unsigned cases = 0;
    for (unsigned offset = 0; offset < 16; ++offset)
    for (unsigned argument = 0; argument < 64; ++argument) {
        memset(stack, 0x5a, sizeof(stack));
        arch_thread_state_t state, before;
        memset(&state, 0xa5, sizeof(state));
        before = state;
        uintptr_t supplied = (uintptr_t)stack + 128 + offset;
        uintptr_t aligned = supplied & ~(uintptr_t)15;
        x86_prepare_kernel_thread(&state, supplied, entry,
                                   (void *)(uintptr_t)argument);
        assert(state.switch_ctx.rsp == aligned - 8);
        assert(state.switch_ctx.r14 == aligned);
        assert(state.switch_ctx.r12 == (uintptr_t)&entry);
        assert(state.switch_ctx.r13 == argument);
        assert(!state.switch_ctx.rbx && !state.switch_ctx.rbp && !state.switch_ctx.r15);
        assert(!state.fs_base);
        assert(state.xsave_area == before.xsave_area && state.xsave_size == before.xsave_size);
        assert(state.flags == before.flags && state.syscall_active == before.syscall_active);
        assert(state.syscall_reserved == before.syscall_reserved);
        assert(*(uint64_t *)(aligned - 8) == (uintptr_t)&x86_kernel_thread_start);
        /* RET 弹出跳板地址后栈对齐，CALL 再压入八字节返回地址。 */
        assert((state.switch_ctx.rsp + 8) % 16 == 0);
        assert((state.switch_ctx.rsp + 8 - 8) % 16 == 8);
        for (unsigned i = 0; i < sizeof(stack); ++i)
            if (i < 120 || i >= 128) assert(stack[i] == 0x5a);
        ++cases;
    }
    thread_t idle, original;
    memset(&idle, 0xa5, sizeof(idle)); original = idle;
    scheduler_init_idle_context(NULL, (uintptr_t)stack + 128);
    for (unsigned invalid = 0; invalid < 16; ++invalid) {
        scheduler_init_idle_context(&idle, invalid);
        assert(!memcmp(&idle, &original, sizeof(idle)));
    }
    for (unsigned offset = 1; offset < 16; ++offset) {
        scheduler_init_idle_context(&idle, (uintptr_t)stack + 128 + offset);
        assert(!memcmp(&idle, &original, sizeof(idle)));
    }
    scheduler_init_idle_context(&idle, (uintptr_t)stack + 128);
    assert(idle.arch.switch_ctx.r12 == (uintptr_t)&scheduler_idle_main);
    assert(idle.arch.switch_ctx.rsp == (uintptr_t)stack + 120);
    assert(!idle.arch.switch_ctx.r13 && !idle.arch.fs_base);
    assert(idle.arch.xsave_area == original.arch.xsave_area);
    printf("Kernel entry context: %u alignment/argument cases, register reset, stack canaries, FP ownership and idle guards PASS\n", cases);
}
'''.replace('IDLE_IMPLEMENTATION', idle)
with tempfile.TemporaryDirectory(prefix='kernel-context-') as directory:
    binary = Path(directory) / 'test'
    def run(code, negative=False):
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I' + str(root / 'include'), '-I' + str(root),
                        '-x', 'c', '-', '-o', str(binary)],
                       input=code, text=True, check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        if negative:
            assert result.returncode and 'switch_ctx.r13' in result.stderr, result.stderr
            print('Negative control rejected: lost kernel entry argument')
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
    run(program)
    # 替换头文件中的真实实现，其余类型及调用约定仍来自正式头文件。
    bad = helper.replace('(uint64_t)(uintptr_t)argument,', '0U,')
    bad = bad.replace('    stack_top &=', '    (void)argument;\n    stack_top &=')
    assert bad != helper
    bad = bad.replace('x86_prepare_kernel_thread(', 'broken_prepare_kernel_thread(', 1)
    run(program.replace('int main(void)', bad + '\nint main(void)').replace(
        '        x86_prepare_kernel_thread(', '        broken_prepare_kernel_thread('), True)
