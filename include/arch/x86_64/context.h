#ifndef OS_ARCH_X86_64_CONTEXT_H
#define OS_ARCH_X86_64_CONTEXT_H

#include <kernel/base.h>
#include "uefi.h"

/* 规范中的架构线程状态；陷阱帧实际位于线程内核栈，不复制到 thread_t。 */
typedef struct arch_switch_context {
    uint64_t rsp;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} arch_switch_context_t;

typedef struct arch_trap_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} arch_trap_frame_t;

typedef struct arch_thread_state {
    arch_switch_context_t switch_ctx;
    uint64_t fs_base;
    void *xsave_area;
    uint32_t xsave_size;
    uint32_t flags;
    uint32_t syscall_active;
    uint32_t syscall_reserved;
} arch_thread_state_t;

bool x86_fp_state_create(arch_thread_state_t *state);
void x86_fp_state_destroy(arch_thread_state_t *state);
void x86_fp_state_reset_current(arch_thread_state_t *state);
void x86_fp_state_clone_current(arch_thread_state_t *source,
                                arch_thread_state_t *destination);
void x86_fp_switch(arch_thread_state_t *from, arch_thread_state_t *to);

/* 旧版测试调度器的上下文接口，迁移完成后由 arch_switch_context 替代。 */
/* 规范线程只保存被调用者保持寄存器；返回地址位于保存的内核栈上。 */
void x86_switch_context(arch_switch_context_t *from,
                        const arch_switch_context_t *to);
void x86_switch_context_root(arch_switch_context_t *from,
                             const arch_switch_context_t *to,
                             paddr_t root, bool finish_handoff);

/*
 * Pure Ring0 thread first-entry trampoline.
 * R12 carries entry, R13 carries its single void * argument.
 */
__attribute__((noreturn))
void x86_kernel_thread_start(void);

/* 仅用于尚未发布的新内核线程；栈顶下方须有至少八字节可写空间。
 * 切换通过 RET 进入跳板，随后 CALL 保证 C 入口满足 SysV 栈对齐约定。
 * 不改变浮点状态的所有权，也不发布线程或修改调度状态。 */
static inline void x86_prepare_kernel_thread(arch_thread_state_t *state,
                                             uintptr_t stack_top,
                                             void (*entry)(void *),
                                             void *argument) {
    stack_top &= ~(uintptr_t)15U;
    uintptr_t stack = stack_top - sizeof(uint64_t);
    *(uint64_t *)stack = (uint64_t)(uintptr_t)&x86_kernel_thread_start;
    state->switch_ctx = (arch_switch_context_t){
        .rsp = stack,
        .r12 = (uint64_t)(uintptr_t)entry,
        .r13 = (uint64_t)(uintptr_t)argument,
        .r14 = stack_top,
    };
    state->fs_base = 0U;
}

/*
 * 将当前栈切换到同一物理内存的另一个虚拟别名，然后从一个全新的 C 调用链继续执行。
 * 该入口不会返回，适合在撤销引导阶段的低端恒等映射前使用。
 */
typedef void (*x86_stack_continuation_t)(void *context);
__attribute__((noreturn))
void x86_rebase_stack_and_call(uint64_t virtual_delta,
                               x86_stack_continuation_t continuation,
                               void *context);

/* Switch to an explicitly supplied, already mapped stack alias and enter a
 * fresh continuation.  The runtime uses this at the post-scheduler boundary
 * so it does not depend on the caller's scheduler stack remaining mapped. */
__attribute__((noreturn))
void x86_switch_stack_and_call(uint64_t stack_top,
                               x86_stack_continuation_t continuation,
                               void *context);

#endif
