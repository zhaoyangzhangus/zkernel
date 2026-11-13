/* 第 3 阶段的 NVIDIA NVSwitch OS ABI 安全失败边界。具体声明由 nvabi_contracts.c 检查。 */

    .section .text.nvabi_nvswitch_unsupported,"ax",@progbits
    .extern nvabi_log_unsupported
    .p2align 4

    .globl nvswitch_os_acquire_fabric_mgmt_cap
    .type nvswitch_os_acquire_fabric_mgmt_cap, @function
nvswitch_os_acquire_fabric_mgmt_cap:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_acquire_fabric_mgmt_cap(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-13, %eax
    popq %rbp
    ret
    .size nvswitch_os_acquire_fabric_mgmt_cap, .-nvswitch_os_acquire_fabric_mgmt_cap

    .globl nvswitch_os_add_client_event
    .type nvswitch_os_add_client_event, @function
nvswitch_os_add_client_event:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_add_client_event(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-13, %eax
    popq %rbp
    ret
    .size nvswitch_os_add_client_event, .-nvswitch_os_add_client_event

    .globl nvswitch_os_alloc_contig_memory
    .type nvswitch_os_alloc_contig_memory, @function
nvswitch_os_alloc_contig_memory:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_alloc_contig_memory(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-13, %eax
    popq %rbp
    ret
    .size nvswitch_os_alloc_contig_memory, .-nvswitch_os_alloc_contig_memory

    .globl nvswitch_os_assert_log
    .type nvswitch_os_assert_log, @function
nvswitch_os_assert_log:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_assert_log(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nvswitch_os_assert_log, .-nvswitch_os_assert_log

    .globl nvswitch_os_free
    .type nvswitch_os_free, @function
nvswitch_os_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_free(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nvswitch_os_free, .-nvswitch_os_free

    .globl nvswitch_os_free_contig_memory
    .type nvswitch_os_free_contig_memory, @function
nvswitch_os_free_contig_memory:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_free_contig_memory(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nvswitch_os_free_contig_memory, .-nvswitch_os_free_contig_memory

    .globl nvswitch_os_get_os_version
    .type nvswitch_os_get_os_version, @function
nvswitch_os_get_os_version:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_get_os_version(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-13, %eax
    popq %rbp
    ret
    .size nvswitch_os_get_os_version, .-nvswitch_os_get_os_version

    .globl nvswitch_os_get_pid
    .type nvswitch_os_get_pid, @function
nvswitch_os_get_pid:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_get_pid(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-13, %eax
    popq %rbp
    ret
    .size nvswitch_os_get_pid, .-nvswitch_os_get_pid

    .globl nvswitch_os_get_platform_time
    .type nvswitch_os_get_platform_time, @function
nvswitch_os_get_platform_time:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_get_platform_time(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movq $-1, %rax
    popq %rbp
    ret
    .size nvswitch_os_get_platform_time, .-nvswitch_os_get_platform_time

    .globl nvswitch_os_get_platform_time_epoch
    .type nvswitch_os_get_platform_time_epoch, @function
nvswitch_os_get_platform_time_epoch:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_get_platform_time_epoch(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movq $-1, %rax
    popq %rbp
    ret
    .size nvswitch_os_get_platform_time_epoch, .-nvswitch_os_get_platform_time_epoch

    .globl nvswitch_os_get_supported_register_events_params
    .type nvswitch_os_get_supported_register_events_params, @function
nvswitch_os_get_supported_register_events_params:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_get_supported_register_events_params(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-13, %eax
    popq %rbp
    ret
    .size nvswitch_os_get_supported_register_events_params, .-nvswitch_os_get_supported_register_events_params

    .globl nvswitch_os_is_admin
    .type nvswitch_os_is_admin, @function
nvswitch_os_is_admin:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_is_admin(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-38, %eax
    popq %rbp
    ret
    .size nvswitch_os_is_admin, .-nvswitch_os_is_admin

    .globl nvswitch_os_is_fabric_manager
    .type nvswitch_os_is_fabric_manager, @function
nvswitch_os_is_fabric_manager:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_is_fabric_manager(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-38, %eax
    popq %rbp
    ret
    .size nvswitch_os_is_fabric_manager, .-nvswitch_os_is_fabric_manager

    .globl nvswitch_os_is_uuid_in_blacklist
    .type nvswitch_os_is_uuid_in_blacklist, @function
nvswitch_os_is_uuid_in_blacklist:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_is_uuid_in_blacklist(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nvswitch_os_is_uuid_in_blacklist, .-nvswitch_os_is_uuid_in_blacklist

    .globl nvswitch_os_malloc_trace
    .type nvswitch_os_malloc_trace, @function
nvswitch_os_malloc_trace:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_malloc_trace(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nvswitch_os_malloc_trace, .-nvswitch_os_malloc_trace

    .globl nvswitch_os_map_dma_region
    .type nvswitch_os_map_dma_region, @function
nvswitch_os_map_dma_region:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_map_dma_region(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-13, %eax
    popq %rbp
    ret
    .size nvswitch_os_map_dma_region, .-nvswitch_os_map_dma_region

    .globl nvswitch_os_mem_read32
    .type nvswitch_os_mem_read32, @function
nvswitch_os_mem_read32:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_mem_read32(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-1, %eax
    popq %rbp
    ret
    .size nvswitch_os_mem_read32, .-nvswitch_os_mem_read32

    .globl nvswitch_os_mem_write32
    .type nvswitch_os_mem_write32, @function
nvswitch_os_mem_write32:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_mem_write32(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nvswitch_os_mem_write32, .-nvswitch_os_mem_write32

    .globl nvswitch_os_memcpy
    .type nvswitch_os_memcpy, @function
nvswitch_os_memcpy:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_memcpy(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nvswitch_os_memcpy, .-nvswitch_os_memcpy

    .globl nvswitch_os_memset
    .type nvswitch_os_memset, @function
nvswitch_os_memset:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_memset(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nvswitch_os_memset, .-nvswitch_os_memset

    .globl nvswitch_os_notify_client_event
    .type nvswitch_os_notify_client_event, @function
nvswitch_os_notify_client_event:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_notify_client_event(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-13, %eax
    popq %rbp
    ret
    .size nvswitch_os_notify_client_event, .-nvswitch_os_notify_client_event

    .globl nvswitch_os_override_platform
    .type nvswitch_os_override_platform, @function
nvswitch_os_override_platform:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_override_platform(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nvswitch_os_override_platform, .-nvswitch_os_override_platform

    .globl nvswitch_os_print
    .type nvswitch_os_print, @function
nvswitch_os_print:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_print(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nvswitch_os_print, .-nvswitch_os_print

    .globl nvswitch_os_read_registry_dword
    .type nvswitch_os_read_registry_dword, @function
nvswitch_os_read_registry_dword:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_read_registry_dword(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-13, %eax
    popq %rbp
    ret
    .size nvswitch_os_read_registry_dword, .-nvswitch_os_read_registry_dword

    .globl nvswitch_os_remove_client_event
    .type nvswitch_os_remove_client_event, @function
nvswitch_os_remove_client_event:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_remove_client_event(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-13, %eax
    popq %rbp
    ret
    .size nvswitch_os_remove_client_event, .-nvswitch_os_remove_client_event

    .globl nvswitch_os_report_error
    .type nvswitch_os_report_error, @function
nvswitch_os_report_error:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_report_error(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nvswitch_os_report_error, .-nvswitch_os_report_error

    .globl nvswitch_os_set_dma_mask
    .type nvswitch_os_set_dma_mask, @function
nvswitch_os_set_dma_mask:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_set_dma_mask(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-13, %eax
    popq %rbp
    ret
    .size nvswitch_os_set_dma_mask, .-nvswitch_os_set_dma_mask

    .globl nvswitch_os_sleep
    .type nvswitch_os_sleep, @function
nvswitch_os_sleep:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_sleep(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nvswitch_os_sleep, .-nvswitch_os_sleep

    .globl nvswitch_os_snprintf
    .type nvswitch_os_snprintf, @function
nvswitch_os_snprintf:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_snprintf(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-38, %eax
    popq %rbp
    ret
    .size nvswitch_os_snprintf, .-nvswitch_os_snprintf

    .globl nvswitch_os_strlen
    .type nvswitch_os_strlen, @function
nvswitch_os_strlen:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_strlen(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movq $-1, %rax
    popq %rbp
    ret
    .size nvswitch_os_strlen, .-nvswitch_os_strlen

    .globl nvswitch_os_strncmp
    .type nvswitch_os_strncmp, @function
nvswitch_os_strncmp:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_strncmp(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-38, %eax
    popq %rbp
    ret
    .size nvswitch_os_strncmp, .-nvswitch_os_strncmp

    .globl nvswitch_os_strncpy
    .type nvswitch_os_strncpy, @function
nvswitch_os_strncpy:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_strncpy(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nvswitch_os_strncpy, .-nvswitch_os_strncpy

    .globl nvswitch_os_sync_dma_region_for_cpu
    .type nvswitch_os_sync_dma_region_for_cpu, @function
nvswitch_os_sync_dma_region_for_cpu:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_sync_dma_region_for_cpu(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-13, %eax
    popq %rbp
    ret
    .size nvswitch_os_sync_dma_region_for_cpu, .-nvswitch_os_sync_dma_region_for_cpu

    .globl nvswitch_os_sync_dma_region_for_device
    .type nvswitch_os_sync_dma_region_for_device, @function
nvswitch_os_sync_dma_region_for_device:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_sync_dma_region_for_device(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-13, %eax
    popq %rbp
    ret
    .size nvswitch_os_sync_dma_region_for_device, .-nvswitch_os_sync_dma_region_for_device

    .globl nvswitch_os_unmap_dma_region
    .type nvswitch_os_unmap_dma_region, @function
nvswitch_os_unmap_dma_region:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_unmap_dma_region(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-13, %eax
    popq %rbp
    ret
    .size nvswitch_os_unmap_dma_region, .-nvswitch_os_unmap_dma_region

    .globl nvswitch_os_vsnprintf
    .type nvswitch_os_vsnprintf, @function
nvswitch_os_vsnprintf:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nvswitch_os_vsnprintf(%rip), %rdi
    leaq .Lnvabi_stage_nvswitch(%rip), %rsi
    call nvabi_log_unsupported
    movl $-38, %eax
    popq %rbp
    ret
    .size nvswitch_os_vsnprintf, .-nvswitch_os_vsnprintf

    .section .rodata.nvabi_nvswitch_unsupported,"a",@progbits
.Lnvabi_stage_nvswitch:
    .asciz "vendor-abi/nvswitch-unsupported"
.Lnvabi_msg_nvswitch_os_acquire_fabric_mgmt_cap:
    .asciz "nvswitch_os_acquire_fabric_mgmt_cap"
.Lnvabi_msg_nvswitch_os_add_client_event:
    .asciz "nvswitch_os_add_client_event"
.Lnvabi_msg_nvswitch_os_alloc_contig_memory:
    .asciz "nvswitch_os_alloc_contig_memory"
.Lnvabi_msg_nvswitch_os_assert_log:
    .asciz "nvswitch_os_assert_log"
.Lnvabi_msg_nvswitch_os_free:
    .asciz "nvswitch_os_free"
.Lnvabi_msg_nvswitch_os_free_contig_memory:
    .asciz "nvswitch_os_free_contig_memory"
.Lnvabi_msg_nvswitch_os_get_os_version:
    .asciz "nvswitch_os_get_os_version"
.Lnvabi_msg_nvswitch_os_get_pid:
    .asciz "nvswitch_os_get_pid"
.Lnvabi_msg_nvswitch_os_get_platform_time:
    .asciz "nvswitch_os_get_platform_time"
.Lnvabi_msg_nvswitch_os_get_platform_time_epoch:
    .asciz "nvswitch_os_get_platform_time_epoch"
.Lnvabi_msg_nvswitch_os_get_supported_register_events_params:
    .asciz "nvswitch_os_get_supported_register_events_params"
.Lnvabi_msg_nvswitch_os_is_admin:
    .asciz "nvswitch_os_is_admin"
.Lnvabi_msg_nvswitch_os_is_fabric_manager:
    .asciz "nvswitch_os_is_fabric_manager"
.Lnvabi_msg_nvswitch_os_is_uuid_in_blacklist:
    .asciz "nvswitch_os_is_uuid_in_blacklist"
.Lnvabi_msg_nvswitch_os_malloc_trace:
    .asciz "nvswitch_os_malloc_trace"
.Lnvabi_msg_nvswitch_os_map_dma_region:
    .asciz "nvswitch_os_map_dma_region"
.Lnvabi_msg_nvswitch_os_mem_read32:
    .asciz "nvswitch_os_mem_read32"
.Lnvabi_msg_nvswitch_os_mem_write32:
    .asciz "nvswitch_os_mem_write32"
.Lnvabi_msg_nvswitch_os_memcpy:
    .asciz "nvswitch_os_memcpy"
.Lnvabi_msg_nvswitch_os_memset:
    .asciz "nvswitch_os_memset"
.Lnvabi_msg_nvswitch_os_notify_client_event:
    .asciz "nvswitch_os_notify_client_event"
.Lnvabi_msg_nvswitch_os_override_platform:
    .asciz "nvswitch_os_override_platform"
.Lnvabi_msg_nvswitch_os_print:
    .asciz "nvswitch_os_print"
.Lnvabi_msg_nvswitch_os_read_registry_dword:
    .asciz "nvswitch_os_read_registry_dword"
.Lnvabi_msg_nvswitch_os_remove_client_event:
    .asciz "nvswitch_os_remove_client_event"
.Lnvabi_msg_nvswitch_os_report_error:
    .asciz "nvswitch_os_report_error"
.Lnvabi_msg_nvswitch_os_set_dma_mask:
    .asciz "nvswitch_os_set_dma_mask"
.Lnvabi_msg_nvswitch_os_sleep:
    .asciz "nvswitch_os_sleep"
.Lnvabi_msg_nvswitch_os_snprintf:
    .asciz "nvswitch_os_snprintf"
.Lnvabi_msg_nvswitch_os_strlen:
    .asciz "nvswitch_os_strlen"
.Lnvabi_msg_nvswitch_os_strncmp:
    .asciz "nvswitch_os_strncmp"
.Lnvabi_msg_nvswitch_os_strncpy:
    .asciz "nvswitch_os_strncpy"
.Lnvabi_msg_nvswitch_os_sync_dma_region_for_cpu:
    .asciz "nvswitch_os_sync_dma_region_for_cpu"
.Lnvabi_msg_nvswitch_os_sync_dma_region_for_device:
    .asciz "nvswitch_os_sync_dma_region_for_device"
.Lnvabi_msg_nvswitch_os_unmap_dma_region:
    .asciz "nvswitch_os_unmap_dma_region"
.Lnvabi_msg_nvswitch_os_vsnprintf:
    .asciz "nvswitch_os_vsnprintf"
