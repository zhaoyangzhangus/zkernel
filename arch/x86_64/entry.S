/*
 * SysV ABI:
 *   rdi = VM_DIRECT_MAP_BASE
 *   rsi = high-half BOOT_INFO *
 *   rdx = continuation
 *
 * 当前低地址 stack 已在 final CR3 中具有 direct-map alias。
 * 切换 RSP 后不返回旧 C frame，直接跳到 continuation。
 */
.section .text.entry,"ax",@progbits
.globl arch_rebase_stack_and_jump
.type arch_rebase_stack_and_jump,@function
arch_rebase_stack_and_jump:
    addq %rdi, %rsp

    /* 旧 frame chain 保存低地址，直接终止它。 */
    xorl %ebp, %ebp

    /* continuation(high_bi) */
    movq %rsi, %rdi
    jmp *%rdx
.size arch_rebase_stack_and_jump, .-arch_rebase_stack_and_jump

.section .note.GNU-stack,"",@progbits
