.text
.globl wget_entry
.extern wget_main
.extern __libc_thread_init

/*
 * System exec 提供 SysV 风格的初始进程栈：
 *   [rsp+0] = argc
 *   [rsp+8] = argv[0]
 *
 * 用户态 C 使用 -mabi=sysv 编译，因此将初始栈转换为 SysV 参数 RDI/RSI。
 * 初始栈已按 16 字节对齐；call 指令会提供 ABI 所需的返回地址槽。
 */
wget_entry:
    call __libc_thread_init
    testq %rax, %rax
    jnz .wget_init_failed
    movq (%rsp), %rdi
    leaq 8(%rsp), %rsi
    call wget_main

    /* OS_SYS_THREAD_EXIT = 0x0000，状态码通过系统调用 ABI 的 RDI 传递。 */
    movq %rax, %rdi
    xorl %eax, %eax
    syscall
1:
    pause
    jmp 1b

.wget_init_failed:
    movq $1, %rdi
    xorl %eax, %eax
    syscall
2:
    pause
    jmp 2b
