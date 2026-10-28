.section .text
.globl libc_crt_entry
.extern __libc_start_main

/* 内核提供 SysV 风格初始栈：argc、argv[]、envp[]。 */
libc_crt_entry:
    movq (%rsp), %rdi
    leaq 8(%rsp), %rsi
    leaq 8(%rsi,%rdi,8), %rdx
    call __libc_start_main
    movq %rax, %rdi
    movabsq $0x8000000000000001, %rax
    syscall
1:
    pause
    jmp 1b
