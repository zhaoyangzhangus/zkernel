    .text
    .p2align 4
    .globl x86_switch_context
    .globl x86_switch_context_root
    .globl x86_kernel_thread_start
    .globl x86_rebase_stack_and_call
    .globl x86_switch_stack_and_call
#ifndef __ELF__
    .def x86_switch_context; .scl 2; .type 32; .endef
    .def x86_switch_context_root; .scl 2; .type 32; .endef
    .def x86_kernel_thread_start; .scl 2; .type 32; .endef
    .def x86_rebase_stack_and_call; .scl 2; .type 32; .endef
    .def x86_switch_stack_and_call; .scl 2; .type 32; .endef
#endif

/*
 * 纯内核态调度线程的首次入口。
 *
 * 初始 arch_switch_context_t 约定：
 *   R12 = void (*入口)(void *)
 *   R13 = 参数
 *
 * x86_switch_context_root() 恢复 RSP/R12/R13，并通过 ret 跳转到此过渡入口。
 * 保存的栈使这里的 RSP 按 16 字节对齐；随后 CALL 会让 C 入口满足 SysV
 * ABI 要求的 RSP % 16 == 8。
 */
    .p2align 4
x86_kernel_thread_start:
    sti
    xorq %rbp, %rbp

    /*
    * 首次进入的内核线程不会从 schedule() 的上下文切换后续路径恢复执行，
    * 因此必须在此显式完成前一线程延迟的执行引用清理。
     *
    * 此时已在新的内核栈上执行且 IF=1，因此 sched_finish_switch() 可以安全地
    * 释放旧线程及其进程虚拟内存，并完成由此产生的 TLB 清理。
     *
    * R12/R13 是 SysV 被调用者保存寄存器，因此工作线程入口和参数在调用后仍有效。
     */
    call sched_finish_switch

    movq %r13, %rdi
    call *%r12

    /* 持续运行的内核工作线程不允许返回。 */
    cli
1:
    hlt
    jmp 1b

/*
 * SysV 参数：RDI=虚拟地址增量，RSI=延续函数，RDX=延续函数参数。
 * 调用返回地址也位于栈中；先整体切换到高地址别名，再丢弃该返回地址，
 * 从而让延续函数拥有独立且 ABI 对齐正确的调用链。
 */
    .p2align 4
x86_rebase_stack_and_call:
    cli
    addq %rdi, %rsp
    xorq %rbp, %rbp
    addq $8, %rsp
    movq %rdx, %rdi
    call *%rsi
    cli
1:
    hlt
    jmp 1b

/* SysV：RDI=目标栈顶，RSI=续接函数，RDX=续接上下文。
 * 安装新栈并进入新的 C 调用链期间保持 IF 屏蔽。 */
    .p2align 4
x86_switch_stack_and_call:
    cli
    movq %rdi, %rsp
    andq $-16, %rsp
    movq %rdx, %rdi
    call *%rsi
    cli
1:
    hlt
    jmp 1b

/*
 * arch_switch_context_t：RSP=0，随后依次为 RBX/RBP/R12-R15。
 * call 指令压入的返回地址跟随 RSP 一起保存，因此恢复端只需 ret。
 */
    .p2align 4
x86_switch_context:
    movq %rsp,  0(%rdi)
    movq %rbx,  8(%rdi)
    movq %rbp, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 32(%rdi)
    movq %r14, 40(%rdi)
    movq %r15, 48(%rdi)

    movq  0(%rsi), %rsp
    movq  8(%rsi), %rbx
    movq 16(%rsi), %rbp
    movq 24(%rsi), %r12
    movq 32(%rsi), %r13
    movq 40(%rsi), %r14
    movq 48(%rsi), %r15
    ret

/*
 * 地址空间切换必须和 RSP 切换不可分割：旧栈可能不在新 CR3 中，而新栈可能
 * 不在旧 CR3 中。写 CR3 后的第一条访存指令就是从目标栈执行 ret。
 */
    .p2align 4
x86_switch_context_root:
    movq %rsp,  0(%rdi)
    movq %rbx,  8(%rdi)
    movq %rbp, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 32(%rdi)
    movq %r14, 40(%rdi)
    movq %r15, 48(%rdi)

    movq  0(%rsi), %rsp
    movq  8(%rsi), %rbx
    movq 16(%rsi), %rbp
    movq 24(%rsi), %r12
    movq 32(%rsi), %r13
    movq 40(%rsi), %r14
    movq 48(%rsi), %r15
    movq %rdx, %cr3
    testb %cl, %cl
    jz .Lswitch_root_return
    /* 旧上下文已完整保存，旧栈也不再处于活动状态。
     * 此时其他 CPU 才可以接收并运行迁移中的线程。恢复后的 RSP 指向返回地址
     * （模 16 余 8），因此需要对齐 CALL。SysV 被调用者保存寄存器保留了新续接
     * 函数的值。 */
    subq $8, %rsp
    call scheduler_finish_handoff
    addq $8, %rsp
.Lswitch_root_return:
    ret
