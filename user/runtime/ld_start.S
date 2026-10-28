.section .text
.globl ld_entry
.extern runtime_linker_main

/* 加载器的文件系统/VM 操作继续使用 System 原生系统调用 ABI。
 * 加载器完成所有映像重定位并即将进入 main 后，内核才会将此进程切换为 Linux 调用号。 */
.equ OS_SYS_LINUX_ACTIVATE, 0x000c

/* 内核为 PT_INTERP 保留未经修改的 SysV 初始栈。 */
ld_entry:
    movq %rsp, %r12
    movq %rsp, %rdi
    call runtime_linker_main
    testq %rax, %rax
    jz 1f
    movq %rax, %r13
    movl $OS_SYS_LINUX_ACTIVATE, %eax
    syscall
    testq %rax, %rax
    js 1f
    movq %r12, %rsp
    jmp *%r13
1:
    movl $1, %edi
    xorl %eax, %eax
    syscall
2:
    pause
    jmp 2b
