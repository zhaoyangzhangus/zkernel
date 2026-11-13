    .text
    .p2align 4
    .globl syscall_entry
    .globl x86_return_to_user
    .extern syscall_dispatch
    .extern x86_syscall_return_mode
    .extern x86_validate_user_frame
    .extern x86_syscall_bad_frame
    .extern g_syscall_return_progress
    .extern g_syscall_return_number
    .extern g_syscall_return_rip
    .extern g_socket_return_progress
    .extern g_socket_return_active
    .extern g_socket_return_rip

/* arch_trap_frame_t 的固定大小和字段偏移。 */
    .set FRAME_SIZE,    176
    .set FRAME_R15,       0
    .set FRAME_R14,       8
    .set FRAME_R13,      16
    .set FRAME_R12,      24
    .set FRAME_R11,      32
    .set FRAME_R10,      40
    .set FRAME_R9,       48
    .set FRAME_R8,       56
    .set FRAME_RSI,      64
    .set FRAME_RDI,      72
    .set FRAME_RBP,      80
    .set FRAME_RDX,      88
    .set FRAME_RCX,      96
    .set FRAME_RBX,     104
    .set FRAME_RAX,     112
    .set FRAME_VECTOR,  120
    .set FRAME_ERROR,   128
    .set FRAME_RIP,     136
    .set FRAME_CS,      144
    .set FRAME_RFLAGS,  152
    .set FRAME_RSP,     160
    .set FRAME_SS,      168

/*
 * 原始 x86_64 ABI：RAX 为编号，RDI/RSI/RDX/R10/R8/R9 为六个参数。
 * SYSCALL 不会切换 RSP，所以先借助 swapgs 取得当前线程的内核栈。
 */
syscall_entry:
    swapgs
    movq %rsp, %gs:0
    movq %gs:8, %rsp
    subq $FRAME_SIZE, %rsp

    movq %r15, FRAME_R15(%rsp)
    movq %r14, FRAME_R14(%rsp)
    movq %r13, FRAME_R13(%rsp)
    movq %r12, FRAME_R12(%rsp)
    movq %r11, FRAME_R11(%rsp)
    movq %r10, FRAME_R10(%rsp)
    movq %r9,  FRAME_R9(%rsp)
    movq %r8,  FRAME_R8(%rsp)
    movq %rsi, FRAME_RSI(%rsp)
    movq %rdi, FRAME_RDI(%rsp)
    movq %rbp, FRAME_RBP(%rsp)
    movq %rdx, FRAME_RDX(%rsp)
    movq %rcx, FRAME_RCX(%rsp)
    movq %rbx, FRAME_RBX(%rsp)
    movq %rax, FRAME_RAX(%rsp)
    movq $0x100, FRAME_VECTOR(%rsp)
    movq $0, FRAME_ERROR(%rsp)
    movq %rcx, FRAME_RIP(%rsp)
    movq $0x23, FRAME_CS(%rsp)
    movq %r11, FRAME_RFLAGS(%rsp)
    movq %gs:0, %rax
    movq %rax, FRAME_RSP(%rsp)
    movq $0x1b, FRAME_SS(%rsp)

    movl %gs:48, %eax
    leaq g_syscall_return_progress(%rip), %rdx
    movl $1, (%rdx,%rax,4)
    movq FRAME_RAX(%rsp), %rcx
    leaq g_syscall_return_number(%rip), %rdx
    movq %rcx, (%rdx,%rax,8)
    movq FRAME_RIP(%rsp), %rcx
    leaq g_syscall_return_rip(%rip), %rdx
    movq %rcx, (%rdx,%rax,8)
    movq FRAME_RAX(%rsp), %rcx
    cmpq $0x500, %rcx
    jne .Lsocket_return_entry_done
    movl %gs:48, %eax
    leaq g_socket_return_active(%rip), %rdx
    movl $1, (%rdx,%rax,4)
    leaq g_socket_return_progress(%rip), %rdx
    movl $1, (%rdx,%rax,4)
    movq FRAME_RIP(%rsp), %rcx
    leaq g_socket_return_rip(%rip), %rdx
    movq %rcx, (%rdx,%rax,8)
.Lsocket_return_entry_done:


    cld
    /*
     * SYSCALL 进入时，IF 已由 IA32_FMASK 屏蔽。现在栈帧已经完整，因此 C 函数体
     * 可以安全地接收定时器、重新调度和 TLB IPI。上方的栈帧构造以及下方的返回帧
     * 汇编必须保持不可中断；只有系统调用函数体属于可中断的内核区域。
     */
    movl $1, %gs:80
    sti
    movq %rsp, %rdi
    call syscall_dispatch
    cli
    movq %rax, FRAME_RAX(%rsp)
    movl %gs:48, %eax
    leaq g_syscall_return_progress(%rip), %rdx
    movl $2, (%rdx,%rax,4)
    movl %gs:48, %ecx
    leaq g_socket_return_active(%rip), %rdx
    cmpl $0, (%rdx,%rcx,4)
    je .Lsocket_return_after_dispatch
    leaq g_socket_return_progress(%rip), %rdx
    movl $2, (%rdx,%rcx,4)
.Lsocket_return_after_dispatch:
    movl $0, %gs:80

    /* 仅供当前引导期 Ring3 自检返回原 C 调用链；正式线程退出不会走这里。 */
    cmpq $0, %gs:24
    jne .Lreturn_to_bootstrap

    /*
     * 系统调用期间收到的重新调度 IPI 会一直保持待处理状态，直到常规中断或调度器
     * 边界到来。此处不要调用 schedule()：上下文切换会将 RSP 改为另一线程保存的
     * 内核栈，而当前路径仍持有本次系统调用的返回帧。
     */
    movl %gs:48, %ecx
    leaq g_syscall_return_progress(%rip), %rdx
    movl $4, (%rdx,%rcx,4)
    leaq g_socket_return_active(%rip), %rdx
    cmpl $0, (%rdx,%rcx,4)
    je .Lsocket_return_after_reschedule_check
    leaq g_socket_return_progress(%rip), %rdx
    movl $4, (%rdx,%rcx,4)
.Lsocket_return_after_reschedule_check:
.Lsyscall_return_ready:

    movl %gs:48, %ecx
    leaq g_syscall_return_progress(%rip), %rdx
    movl $6, (%rdx,%rcx,4)
    leaq g_socket_return_active(%rip), %rdx
    cmpl $0, (%rdx,%rcx,4)
    je .Lsocket_return_before_mode
    leaq g_socket_return_progress(%rip), %rdx
    movl $6, (%rdx,%rcx,4)
.Lsocket_return_before_mode:
    movq %rsp, %rdi
    call x86_syscall_return_mode
    movl %gs:48, %ecx
    leaq g_syscall_return_progress(%rip), %rdx
    movl $7, (%rdx,%rcx,4)
    leaq g_socket_return_active(%rip), %rdx
    cmpl $0, (%rdx,%rcx,4)
    je .Lsocket_return_after_mode
    leaq g_socket_return_progress(%rip), %rdx
    movl $7, (%rdx,%rcx,4)
    leaq g_socket_return_active(%rip), %rdx
    movl $0, (%rdx,%rcx,4)
.Lsocket_return_after_mode:
    testl %eax, %eax
    js .Lbad_user_frame
    jz .Liret_return

/* 已验证的常见路径使用 SYSRETQ。RCX/R11 按 ABI 作为破坏寄存器恢复返回状态。 */
.Lsysret_return:
    movq FRAME_R15(%rsp), %r15
    movq FRAME_R14(%rsp), %r14
    movq FRAME_R13(%rsp), %r13
    movq FRAME_R12(%rsp), %r12
    movq FRAME_R10(%rsp), %r10
    movq FRAME_R9(%rsp),  %r9
    movq FRAME_R8(%rsp),  %r8
    movq FRAME_RSI(%rsp), %rsi
    movq FRAME_RDI(%rsp), %rdi
    movq FRAME_RBP(%rsp), %rbp
    movq FRAME_RDX(%rsp), %rdx
    movq FRAME_RBX(%rsp), %rbx
    movq FRAME_RAX(%rsp), %rax
    movq FRAME_RIP(%rsp), %rcx
    movq FRAME_RFLAGS(%rsp), %r11
    movq FRAME_RSP(%rsp), %rsp
    swapgs
    sysretq

/* TF/RF 等慢路径使用完整 IRETQ 帧。连续五次 168(%rsp) 指向依次所需的字段。 */
.Liret_return:
    /* 以原始帧指针取值，避免每次 push 后 rsp 改变导致偏移失效。 */
    /*
     * 返回帧尚未完整时不能接收定时器中断：中断入口会把当前栈顶
     * 误认为硬件异常帧，破坏待 iretq 的 CS/SS。iretq 会从帧中恢复 IF。
     */
    cli
    movq %rsp, %rdx
    pushq FRAME_SS(%rdx)
    pushq FRAME_RSP(%rdx)
    pushq FRAME_RFLAGS(%rdx)
    pushq FRAME_CS(%rdx)
    pushq FRAME_RIP(%rdx)
    movq 40(%rsp),  %r15
    movq 48(%rsp),  %r14
    movq 56(%rsp),  %r13
    movq 64(%rsp),  %r12
    movq 72(%rsp),  %r11
    movq 80(%rsp),  %r10
    movq 88(%rsp),  %r9
    movq 96(%rsp),  %r8
    movq 104(%rsp), %rsi
    movq 112(%rsp), %rdi
    movq 120(%rsp), %rbp
    movq 128(%rsp), %rdx
    movq 136(%rsp), %rcx
    movq 144(%rsp), %rbx
    movq 152(%rsp), %rax
    swapgs
    iretq

.Lreturn_to_bootstrap:
    movq $0, %gs:24
    movq %gs:16, %rsp
    /* 用户调用链已经丢弃，直接恢复内核时保持 CPU-local GS。 */
    ret

.Lbad_user_frame:
    call x86_syscall_bad_frame
    ud2

/*
 * 初次进入 Ring3 一律使用 IRETQ。调用者提供完整 arch_trap_frame_t，
 * 此函数仅在 C 验证通过后消费它，因此不会把非规范地址交给 IRETQ。
 */
x86_return_to_user:
    movq %rdi, %r12
    call x86_validate_user_frame
    testb %al, %al
    jz .Lbad_user_frame
    /* 下面开始构造硬件返回帧，直到 iretq 前必须保持不可中断。 */
    cli
    movq %r12, %rdx
    pushq FRAME_SS(%rdx)
    pushq FRAME_RSP(%rdx)
    pushq FRAME_RFLAGS(%rdx)
    pushq FRAME_CS(%rdx)
    pushq FRAME_RIP(%rdx)
    movq FRAME_R15(%rdx), %r15
    movq FRAME_R14(%rdx), %r14
    movq FRAME_R13(%rdx), %r13
    movq FRAME_R12(%rdx), %r12
    movq FRAME_R11(%rdx), %r11
    movq FRAME_R10(%rdx), %r10
    movq FRAME_R9(%rdx),  %r9
    movq FRAME_R8(%rdx),  %r8
    movq FRAME_RSI(%rdx), %rsi
    movq FRAME_RDI(%rdx), %rdi
    movq FRAME_RBP(%rdx), %rbp
    movq FRAME_RCX(%rdx), %rcx
    movq FRAME_RBX(%rdx), %rbx
    movq FRAME_RAX(%rdx), %rax
    movq FRAME_RDX(%rdx), %rdx
    swapgs
    iretq
