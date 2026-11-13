/* 第 3 阶段的 nvlink ABI 安全失败边界。具体声明由 nvabi_contracts.c 检查。 */

    .section .text.nvabi_nvlink_unsupported,"ax",@progbits
    .extern nvabi_log_unsupported
    .p2align 4

    .globl nvlink_acquire_fabric_mgmt_cap
    .type nvlink_acquire_fabric_mgmt_cap, @function
nvlink_acquire_fabric_mgmt_cap:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvlink_acquire_fabric_mgmt_cap(%rip), %rdi
    leaq .Lnvabi_stage_nvlink(%rip), %rsi
    call nvabi_log_unsupported
    movl $-13, %eax
    popq %rbp
    ret
    .size nvlink_acquire_fabric_mgmt_cap, .-nvlink_acquire_fabric_mgmt_cap

    .globl nvlink_acquireLock
    .type nvlink_acquireLock, @function
nvlink_acquireLock:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvlink_acquireLock(%rip), %rdi
    leaq .Lnvabi_stage_nvlink(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nvlink_acquireLock, .-nvlink_acquireLock

    .globl nvlink_allocLock
    .type nvlink_allocLock, @function
nvlink_allocLock:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvlink_allocLock(%rip), %rdi
    leaq .Lnvabi_stage_nvlink(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nvlink_allocLock, .-nvlink_allocLock

    .globl nvlink_assert
    .type nvlink_assert, @function
nvlink_assert:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvlink_assert(%rip), %rdi
    leaq .Lnvabi_stage_nvlink(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nvlink_assert, .-nvlink_assert

    .globl nvlink_free
    .type nvlink_free, @function
nvlink_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvlink_free(%rip), %rdi
    leaq .Lnvabi_stage_nvlink(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nvlink_free, .-nvlink_free

    .globl nvlink_freeLock
    .type nvlink_freeLock, @function
nvlink_freeLock:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvlink_freeLock(%rip), %rdi
    leaq .Lnvabi_stage_nvlink(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nvlink_freeLock, .-nvlink_freeLock

    .globl nvlink_get_platform_time
    .type nvlink_get_platform_time, @function
nvlink_get_platform_time:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvlink_get_platform_time(%rip), %rdi
    leaq .Lnvabi_stage_nvlink(%rip), %rsi
    call nvabi_log_unsupported
    movq $-1, %rax
    popq %rbp
    ret
    .size nvlink_get_platform_time, .-nvlink_get_platform_time

    .globl nvlink_is_admin
    .type nvlink_is_admin, @function
nvlink_is_admin:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvlink_is_admin(%rip), %rdi
    leaq .Lnvabi_stage_nvlink(%rip), %rsi
    call nvabi_log_unsupported
    movl $-38, %eax
    popq %rbp
    ret
    .size nvlink_is_admin, .-nvlink_is_admin

    .globl nvlink_is_fabric_manager
    .type nvlink_is_fabric_manager, @function
nvlink_is_fabric_manager:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvlink_is_fabric_manager(%rip), %rdi
    leaq .Lnvabi_stage_nvlink(%rip), %rsi
    call nvabi_log_unsupported
    movl $-38, %eax
    popq %rbp
    ret
    .size nvlink_is_fabric_manager, .-nvlink_is_fabric_manager

    .globl nvlink_malloc
    .type nvlink_malloc, @function
nvlink_malloc:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvlink_malloc(%rip), %rdi
    leaq .Lnvabi_stage_nvlink(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nvlink_malloc, .-nvlink_malloc

    .globl nvlink_memcpy
    .type nvlink_memcpy, @function
nvlink_memcpy:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvlink_memcpy(%rip), %rdi
    leaq .Lnvabi_stage_nvlink(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nvlink_memcpy, .-nvlink_memcpy

    .globl nvlink_memset
    .type nvlink_memset, @function
nvlink_memset:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvlink_memset(%rip), %rdi
    leaq .Lnvabi_stage_nvlink(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nvlink_memset, .-nvlink_memset

    .globl nvlink_releaseLock
    .type nvlink_releaseLock, @function
nvlink_releaseLock:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvlink_releaseLock(%rip), %rdi
    leaq .Lnvabi_stage_nvlink(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nvlink_releaseLock, .-nvlink_releaseLock

    .globl nvlink_sleep
    .type nvlink_sleep, @function
nvlink_sleep:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvlink_sleep(%rip), %rdi
    leaq .Lnvabi_stage_nvlink(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nvlink_sleep, .-nvlink_sleep

    .globl nvlink_strcmp
    .type nvlink_strcmp, @function
nvlink_strcmp:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvlink_strcmp(%rip), %rdi
    leaq .Lnvabi_stage_nvlink(%rip), %rsi
    call nvabi_log_unsupported
    movl $-38, %eax
    popq %rbp
    ret
    .size nvlink_strcmp, .-nvlink_strcmp

    .globl nvlink_strcpy
    .type nvlink_strcpy, @function
nvlink_strcpy:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvlink_strcpy(%rip), %rdi
    leaq .Lnvabi_stage_nvlink(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nvlink_strcpy, .-nvlink_strcpy

    .globl nvlink_strlen
    .type nvlink_strlen, @function
nvlink_strlen:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvlink_strlen(%rip), %rdi
    leaq .Lnvabi_stage_nvlink(%rip), %rsi
    call nvabi_log_unsupported
    movq $-1, %rax
    popq %rbp
    ret
    .size nvlink_strlen, .-nvlink_strlen

    .section .rodata.nvabi_nvlink_unsupported,"a",@progbits
.Lnvabi_stage_nvlink:
    .asciz "vendor-abi/nvlink-unsupported"
.Lnvabi_msg_nvlink_acquire_fabric_mgmt_cap:
    .asciz "nvlink_acquire_fabric_mgmt_cap"
.Lnvabi_msg_nvlink_acquireLock:
    .asciz "nvlink_acquireLock"
.Lnvabi_msg_nvlink_allocLock:
    .asciz "nvlink_allocLock"
.Lnvabi_msg_nvlink_assert:
    .asciz "nvlink_assert"
.Lnvabi_msg_nvlink_free:
    .asciz "nvlink_free"
.Lnvabi_msg_nvlink_freeLock:
    .asciz "nvlink_freeLock"
.Lnvabi_msg_nvlink_get_platform_time:
    .asciz "nvlink_get_platform_time"
.Lnvabi_msg_nvlink_is_admin:
    .asciz "nvlink_is_admin"
.Lnvabi_msg_nvlink_is_fabric_manager:
    .asciz "nvlink_is_fabric_manager"
.Lnvabi_msg_nvlink_malloc:
    .asciz "nvlink_malloc"
.Lnvabi_msg_nvlink_memcpy:
    .asciz "nvlink_memcpy"
.Lnvabi_msg_nvlink_memset:
    .asciz "nvlink_memset"
.Lnvabi_msg_nvlink_releaseLock:
    .asciz "nvlink_releaseLock"
.Lnvabi_msg_nvlink_sleep:
    .asciz "nvlink_sleep"
.Lnvabi_msg_nvlink_strcmp:
    .asciz "nvlink_strcmp"
.Lnvabi_msg_nvlink_strcpy:
    .asciz "nvlink_strcpy"
.Lnvabi_msg_nvlink_strlen:
    .asciz "nvlink_strlen"
