/* 第 3 阶段的 OS ABI 安全失败边界。具体声明由 nvabi_contracts.c 检查。 */

    .section .text.nvabi_os_unsupported,"ax",@progbits
    .extern nvabi_log_unsupported
    .p2align 4

    .globl os_add_record_for_crashLog
    .type os_add_record_for_crashLog, @function
os_add_record_for_crashLog:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_add_record_for_crashLog(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size os_add_record_for_crashLog, .-os_add_record_for_crashLog

    .globl os_alloc_pages_node
    .type os_alloc_pages_node, @function
os_alloc_pages_node:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_alloc_pages_node(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_alloc_pages_node, .-os_alloc_pages_node

    .globl os_call_vgpu_vfio
    .type os_call_vgpu_vfio, @function
os_call_vgpu_vfio:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_call_vgpu_vfio(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_call_vgpu_vfio, .-os_call_vgpu_vfio

    .globl os_close_file
    .type os_close_file, @function
os_close_file:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_close_file(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size os_close_file, .-os_close_file

    .globl os_count_tail_pages
    .type os_count_tail_pages, @function
os_count_tail_pages:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_count_tail_pages(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $-1, %eax
    popq %rbp
    ret
    .size os_count_tail_pages, .-os_count_tail_pages

    .globl os_delete_record_for_crashLog
    .type os_delete_record_for_crashLog, @function
os_delete_record_for_crashLog:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_delete_record_for_crashLog(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size os_delete_record_for_crashLog, .-os_delete_record_for_crashLog

    .globl os_device_vm_present
    .type os_device_vm_present, @function
os_device_vm_present:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_device_vm_present(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_device_vm_present, .-os_device_vm_present

    .globl os_flush_work_queue
    .type os_flush_work_queue, @function
os_flush_work_queue:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_flush_work_queue(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_flush_work_queue, .-os_flush_work_queue

    .globl os_get_acpi_rsdp_from_uefi
    .type os_get_acpi_rsdp_from_uefi, @function
os_get_acpi_rsdp_from_uefi:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_get_acpi_rsdp_from_uefi(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_get_acpi_rsdp_from_uefi, .-os_get_acpi_rsdp_from_uefi

    .globl os_get_numa_node_memory_usage
    .type os_get_numa_node_memory_usage, @function
os_get_numa_node_memory_usage:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_get_numa_node_memory_usage(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_get_numa_node_memory_usage, .-os_get_numa_node_memory_usage

    .globl os_get_page
    .type os_get_page, @function
os_get_page:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_get_page(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_get_page, .-os_get_page

    .globl os_get_page_refcount
    .type os_get_page_refcount, @function
os_get_page_refcount:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_get_page_refcount(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $-1, %eax
    popq %rbp
    ret
    .size os_get_page_refcount, .-os_get_page_refcount

    .globl os_get_random_bytes
    .type os_get_random_bytes, @function
os_get_random_bytes:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_get_random_bytes(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_get_random_bytes, .-os_get_random_bytes

    .globl os_get_smbios_header
    .type os_get_smbios_header, @function
os_get_smbios_header:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_get_smbios_header(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_get_smbios_header, .-os_get_smbios_header

    .globl os_imex_channel_count
    .type os_imex_channel_count, @function
os_imex_channel_count:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_imex_channel_count(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $-38, %eax
    popq %rbp
    ret
    .size os_imex_channel_count, .-os_imex_channel_count

    .globl os_imex_channel_get
    .type os_imex_channel_get, @function
os_imex_channel_get:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_imex_channel_get(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $-38, %eax
    popq %rbp
    ret
    .size os_imex_channel_get, .-os_imex_channel_get

    .globl os_iommu_sva_bind
    .type os_iommu_sva_bind, @function
os_iommu_sva_bind:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_iommu_sva_bind(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_iommu_sva_bind, .-os_iommu_sva_bind

    .globl os_iommu_sva_unbind
    .type os_iommu_sva_unbind, @function
os_iommu_sva_unbind:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_iommu_sva_unbind(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size os_iommu_sva_unbind, .-os_iommu_sva_unbind

    .globl os_is_queue_flush_ongoing
    .type os_is_queue_flush_ongoing, @function
os_is_queue_flush_ongoing:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_is_queue_flush_ongoing(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size os_is_queue_flush_ongoing, .-os_is_queue_flush_ongoing

    .globl os_lock_user_pages
    .type os_lock_user_pages, @function
os_lock_user_pages:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_lock_user_pages(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_lock_user_pages, .-os_lock_user_pages

    .globl os_lookup_user_io_memory
    .type os_lookup_user_io_memory, @function
os_lookup_user_io_memory:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_lookup_user_io_memory(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_lookup_user_io_memory, .-os_lookup_user_io_memory

    .globl os_numa_add_gpu_memory
    .type os_numa_add_gpu_memory, @function
os_numa_add_gpu_memory:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_numa_add_gpu_memory(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_numa_add_gpu_memory, .-os_numa_add_gpu_memory

    .globl os_numa_memblock_size
    .type os_numa_memblock_size, @function
os_numa_memblock_size:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_numa_memblock_size(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_numa_memblock_size, .-os_numa_memblock_size

    .globl os_numa_remove_gpu_memory
    .type os_numa_remove_gpu_memory, @function
os_numa_remove_gpu_memory:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_numa_remove_gpu_memory(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_numa_remove_gpu_memory, .-os_numa_remove_gpu_memory

    .globl os_nv_cap_close_fd
    .type os_nv_cap_close_fd, @function
os_nv_cap_close_fd:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_nv_cap_close_fd(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size os_nv_cap_close_fd, .-os_nv_cap_close_fd

    .globl os_nv_cap_create_dir_entry
    .type os_nv_cap_create_dir_entry, @function
os_nv_cap_create_dir_entry:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_nv_cap_create_dir_entry(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size os_nv_cap_create_dir_entry, .-os_nv_cap_create_dir_entry

    .globl os_nv_cap_create_file_entry
    .type os_nv_cap_create_file_entry, @function
os_nv_cap_create_file_entry:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_nv_cap_create_file_entry(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size os_nv_cap_create_file_entry, .-os_nv_cap_create_file_entry

    .globl os_nv_cap_destroy_entry
    .type os_nv_cap_destroy_entry, @function
os_nv_cap_destroy_entry:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_nv_cap_destroy_entry(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size os_nv_cap_destroy_entry, .-os_nv_cap_destroy_entry

    .globl os_nv_cap_validate_and_dup_fd
    .type os_nv_cap_validate_and_dup_fd, @function
os_nv_cap_validate_and_dup_fd:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_nv_cap_validate_and_dup_fd(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $-38, %eax
    popq %rbp
    ret
    .size os_nv_cap_validate_and_dup_fd, .-os_nv_cap_validate_and_dup_fd

    .globl os_offline_page_at_address
    .type os_offline_page_at_address, @function
os_offline_page_at_address:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_offline_page_at_address(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_offline_page_at_address, .-os_offline_page_at_address

    .globl os_open_temporary_file
    .type os_open_temporary_file, @function
os_open_temporary_file:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_open_temporary_file(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_open_temporary_file, .-os_open_temporary_file

    .globl os_put_page
    .type os_put_page, @function
os_put_page:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_put_page(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_put_page, .-os_put_page

    .globl os_queue_work_item
    .type os_queue_work_item, @function
os_queue_work_item:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_queue_work_item(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_queue_work_item, .-os_queue_work_item

    .globl os_read_file
    .type os_read_file, @function
os_read_file:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_read_file(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_read_file, .-os_read_file

    .globl os_supports_kernel_suspend_notifiers
    .type os_supports_kernel_suspend_notifiers, @function
os_supports_kernel_suspend_notifiers:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_supports_kernel_suspend_notifiers(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size os_supports_kernel_suspend_notifiers, .-os_supports_kernel_suspend_notifiers

    .globl os_tegra_igpu_perf_boost
    .type os_tegra_igpu_perf_boost, @function
os_tegra_igpu_perf_boost:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_tegra_igpu_perf_boost(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_tegra_igpu_perf_boost, .-os_tegra_igpu_perf_boost

    .globl os_unlock_user_pages
    .type os_unlock_user_pages, @function
os_unlock_user_pages:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_unlock_user_pages(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_unlock_user_pages, .-os_unlock_user_pages

    .globl os_write_file
    .type os_write_file, @function
os_write_file:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_os_write_file(%rip), %rdi
    leaq .Lnvabi_stage_os(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size os_write_file, .-os_write_file

    .section .rodata.nvabi_os_unsupported,"a",@progbits
.Lnvabi_stage_os:
    .asciz "vendor-abi/os-unsupported"
.Lnvabi_msg_os_add_record_for_crashLog:
    .asciz "os_add_record_for_crashLog"
.Lnvabi_msg_os_alloc_pages_node:
    .asciz "os_alloc_pages_node"
.Lnvabi_msg_os_call_vgpu_vfio:
    .asciz "os_call_vgpu_vfio"
.Lnvabi_msg_os_close_file:
    .asciz "os_close_file"
.Lnvabi_msg_os_count_tail_pages:
    .asciz "os_count_tail_pages"
.Lnvabi_msg_os_delete_record_for_crashLog:
    .asciz "os_delete_record_for_crashLog"
.Lnvabi_msg_os_device_vm_present:
    .asciz "os_device_vm_present"
.Lnvabi_msg_os_find_ns_pid:
    .asciz "os_find_ns_pid"
.Lnvabi_msg_os_flush_work_queue:
    .asciz "os_flush_work_queue"
.Lnvabi_msg_os_get_acpi_rsdp_from_uefi:
    .asciz "os_get_acpi_rsdp_from_uefi"
.Lnvabi_msg_os_get_numa_node_memory_usage:
    .asciz "os_get_numa_node_memory_usage"
.Lnvabi_msg_os_get_page:
    .asciz "os_get_page"
.Lnvabi_msg_os_get_page_refcount:
    .asciz "os_get_page_refcount"
.Lnvabi_msg_os_get_pid_info:
    .asciz "os_get_pid_info"
.Lnvabi_msg_os_get_random_bytes:
    .asciz "os_get_random_bytes"
.Lnvabi_msg_os_get_smbios_header:
    .asciz "os_get_smbios_header"
.Lnvabi_msg_os_imex_channel_count:
    .asciz "os_imex_channel_count"
.Lnvabi_msg_os_imex_channel_get:
    .asciz "os_imex_channel_get"
.Lnvabi_msg_os_iommu_sva_bind:
    .asciz "os_iommu_sva_bind"
.Lnvabi_msg_os_iommu_sva_unbind:
    .asciz "os_iommu_sva_unbind"
.Lnvabi_msg_os_is_init_ns:
    .asciz "os_is_init_ns"
.Lnvabi_msg_os_is_queue_flush_ongoing:
    .asciz "os_is_queue_flush_ongoing"
.Lnvabi_msg_os_lock_user_pages:
    .asciz "os_lock_user_pages"
.Lnvabi_msg_os_lookup_user_io_memory:
    .asciz "os_lookup_user_io_memory"
.Lnvabi_msg_os_match_mmap_offset:
    .asciz "os_match_mmap_offset"
.Lnvabi_msg_os_numa_add_gpu_memory:
    .asciz "os_numa_add_gpu_memory"
.Lnvabi_msg_os_numa_memblock_size:
    .asciz "os_numa_memblock_size"
.Lnvabi_msg_os_numa_remove_gpu_memory:
    .asciz "os_numa_remove_gpu_memory"
.Lnvabi_msg_os_nv_cap_close_fd:
    .asciz "os_nv_cap_close_fd"
.Lnvabi_msg_os_nv_cap_create_dir_entry:
    .asciz "os_nv_cap_create_dir_entry"
.Lnvabi_msg_os_nv_cap_create_file_entry:
    .asciz "os_nv_cap_create_file_entry"
.Lnvabi_msg_os_nv_cap_destroy_entry:
    .asciz "os_nv_cap_destroy_entry"
.Lnvabi_msg_os_nv_cap_validate_and_dup_fd:
    .asciz "os_nv_cap_validate_and_dup_fd"
.Lnvabi_msg_os_offline_page_at_address:
    .asciz "os_offline_page_at_address"
.Lnvabi_msg_os_open_temporary_file:
    .asciz "os_open_temporary_file"
.Lnvabi_msg_os_put_page:
    .asciz "os_put_page"
.Lnvabi_msg_os_put_pid_info:
    .asciz "os_put_pid_info"
.Lnvabi_msg_os_queue_work_item:
    .asciz "os_queue_work_item"
.Lnvabi_msg_os_read_file:
    .asciz "os_read_file"
.Lnvabi_msg_os_supports_kernel_suspend_notifiers:
    .asciz "os_supports_kernel_suspend_notifiers"
.Lnvabi_msg_os_tegra_igpu_perf_boost:
    .asciz "os_tegra_igpu_perf_boost"
.Lnvabi_msg_os_unlock_user_pages:
    .asciz "os_unlock_user_pages"
.Lnvabi_msg_os_write_file:
    .asciz "os_write_file"
