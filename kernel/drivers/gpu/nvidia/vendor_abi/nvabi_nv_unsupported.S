/* 第 3 阶段的 NV ABI 安全失败边界。具体声明由 nvabi_contracts.c 检查。 */

    .section .text.nvabi_nv_unsupported,"ax",@progbits
    .extern nvabi_log_unsupported
    .p2align 4

    .globl nv_acpi_d3cold_dsm_for_upstream_port
    .type nv_acpi_d3cold_dsm_for_upstream_port, @function
nv_acpi_d3cold_dsm_for_upstream_port:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_acpi_d3cold_dsm_for_upstream_port(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_acpi_d3cold_dsm_for_upstream_port, .-nv_acpi_d3cold_dsm_for_upstream_port

    .globl nv_acpi_ddc_method
    .type nv_acpi_ddc_method, @function
nv_acpi_ddc_method:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_acpi_ddc_method(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_acpi_ddc_method, .-nv_acpi_ddc_method

    .globl nv_acpi_dod_method
    .type nv_acpi_dod_method, @function
nv_acpi_dod_method:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_acpi_dod_method(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_acpi_dod_method, .-nv_acpi_dod_method

    .globl nv_acpi_dsm_method
    .type nv_acpi_dsm_method, @function
nv_acpi_dsm_method:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_acpi_dsm_method(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_acpi_dsm_method, .-nv_acpi_dsm_method

    .globl nv_acpi_get_powersource
    .type nv_acpi_get_powersource, @function
nv_acpi_get_powersource:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_acpi_get_powersource(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_acpi_get_powersource, .-nv_acpi_get_powersource

    .globl nv_acpi_is_battery_present
    .type nv_acpi_is_battery_present, @function
nv_acpi_is_battery_present:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_acpi_is_battery_present(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nv_acpi_is_battery_present, .-nv_acpi_is_battery_present

    .globl nv_acpi_methods_init
    .type nv_acpi_methods_init, @function
nv_acpi_methods_init:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_acpi_methods_init(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_acpi_methods_init, .-nv_acpi_methods_init

    .globl nv_acpi_methods_uninit
    .type nv_acpi_methods_uninit, @function
nv_acpi_methods_uninit:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_acpi_methods_uninit(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_acpi_methods_uninit, .-nv_acpi_methods_uninit

    .globl nv_acpi_mux_method
    .type nv_acpi_mux_method, @function
nv_acpi_mux_method:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_acpi_mux_method(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_acpi_mux_method, .-nv_acpi_mux_method

    .globl nv_acpi_rom_method
    .type nv_acpi_rom_method, @function
nv_acpi_rom_method:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_acpi_rom_method(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_acpi_rom_method, .-nv_acpi_rom_method

    .globl nv_acquire_fabric_mgmt_cap
    .type nv_acquire_fabric_mgmt_cap, @function
nv_acquire_fabric_mgmt_cap:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_acquire_fabric_mgmt_cap(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_acquire_fabric_mgmt_cap, .-nv_acquire_fabric_mgmt_cap

    .globl nv_acquire_mmap_lock
    .type nv_acquire_mmap_lock, @function
nv_acquire_mmap_lock:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_acquire_mmap_lock(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_acquire_mmap_lock, .-nv_acquire_mmap_lock

    .globl nv_alias_pages
    .type nv_alias_pages, @function
nv_alias_pages:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_alias_pages(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_alias_pages, .-nv_alias_pages

#ifndef OS_NVIDIA_PHASE5
    .globl nv_alloc_kernel_mapping
    .type nv_alloc_kernel_mapping, @function
nv_alloc_kernel_mapping:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_alloc_kernel_mapping(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nv_alloc_kernel_mapping, .-nv_alloc_kernel_mapping
#endif

    .globl nv_allow_runtime_suspend
    .type nv_allow_runtime_suspend, @function
nv_allow_runtime_suspend:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_allow_runtime_suspend(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_allow_runtime_suspend, .-nv_allow_runtime_suspend

    .globl nv_audio_dynamic_power
    .type nv_audio_dynamic_power, @function
nv_audio_dynamic_power:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_audio_dynamic_power(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_audio_dynamic_power, .-nv_audio_dynamic_power

    .globl nv_bpmp_send_mrq
    .type nv_bpmp_send_mrq, @function
nv_bpmp_send_mrq:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_bpmp_send_mrq(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_bpmp_send_mrq, .-nv_bpmp_send_mrq

    .globl nv_cancel_nano_timer
    .type nv_cancel_nano_timer, @function
nv_cancel_nano_timer:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_cancel_nano_timer(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_cancel_nano_timer, .-nv_cancel_nano_timer

    .globl nv_control_soc_irqs
    .type nv_control_soc_irqs, @function
nv_control_soc_irqs:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_control_soc_irqs(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_control_soc_irqs, .-nv_control_soc_irqs

    .globl nv_create_nano_timer
    .type nv_create_nano_timer, @function
nv_create_nano_timer:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_create_nano_timer(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_create_nano_timer, .-nv_create_nano_timer

    .globl nv_destroy_nano_timer
    .type nv_destroy_nano_timer, @function
nv_destroy_nano_timer:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_destroy_nano_timer(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_destroy_nano_timer, .-nv_destroy_nano_timer

    .globl nv_disable_clk
    .type nv_disable_clk, @function
nv_disable_clk:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_disable_clk(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_disable_clk, .-nv_disable_clk

    .globl nv_disallow_runtime_suspend
    .type nv_disallow_runtime_suspend, @function
nv_disallow_runtime_suspend:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_disallow_runtime_suspend(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_disallow_runtime_suspend, .-nv_disallow_runtime_suspend

    .globl nv_dma_cache_invalidate
    .type nv_dma_cache_invalidate, @function
nv_dma_cache_invalidate:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_dma_cache_invalidate(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_dma_cache_invalidate, .-nv_dma_cache_invalidate

    .globl nv_dma_get_dev_pagemap
    .type nv_dma_get_dev_pagemap, @function
nv_dma_get_dev_pagemap:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_dma_get_dev_pagemap(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nv_dma_get_dev_pagemap, .-nv_dma_get_dev_pagemap

    .globl nv_dma_import_dma_buf
    .type nv_dma_import_dma_buf, @function
nv_dma_import_dma_buf:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_dma_import_dma_buf(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_dma_import_dma_buf, .-nv_dma_import_dma_buf

    .globl nv_dma_import_from_fd
    .type nv_dma_import_from_fd, @function
nv_dma_import_from_fd:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_dma_import_from_fd(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_dma_import_from_fd, .-nv_dma_import_from_fd

    .globl nv_dma_import_sgt
    .type nv_dma_import_sgt, @function
nv_dma_import_sgt:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_dma_import_sgt(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_dma_import_sgt, .-nv_dma_import_sgt

    .globl nv_dma_map_alloc
    .type nv_dma_map_alloc, @function
nv_dma_map_alloc:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_dma_map_alloc(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_dma_map_alloc, .-nv_dma_map_alloc

    .globl nv_dma_map_mmio
    .type nv_dma_map_mmio, @function
nv_dma_map_mmio:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_dma_map_mmio(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_dma_map_mmio, .-nv_dma_map_mmio

    .globl nv_dma_map_peer
    .type nv_dma_map_peer, @function
nv_dma_map_peer:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_dma_map_peer(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_dma_map_peer, .-nv_dma_map_peer

    .globl nv_dma_put_dev_pagemap
    .type nv_dma_put_dev_pagemap, @function
nv_dma_put_dev_pagemap:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_dma_put_dev_pagemap(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_dma_put_dev_pagemap, .-nv_dma_put_dev_pagemap

    .globl nv_dma_release_dma_buf
    .type nv_dma_release_dma_buf, @function
nv_dma_release_dma_buf:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_dma_release_dma_buf(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_dma_release_dma_buf, .-nv_dma_release_dma_buf

    .globl nv_dma_release_sgt
    .type nv_dma_release_sgt, @function
nv_dma_release_sgt:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_dma_release_sgt(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_dma_release_sgt, .-nv_dma_release_sgt

    .globl nv_dma_unmap_alloc
    .type nv_dma_unmap_alloc, @function
nv_dma_unmap_alloc:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_dma_unmap_alloc(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_dma_unmap_alloc, .-nv_dma_unmap_alloc

    .globl nv_dma_unmap_mmio
    .type nv_dma_unmap_mmio, @function
nv_dma_unmap_mmio:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_dma_unmap_mmio(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_dma_unmap_mmio, .-nv_dma_unmap_mmio

    .globl nv_dma_unmap_peer
    .type nv_dma_unmap_peer, @function
nv_dma_unmap_peer:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_dma_unmap_peer(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_dma_unmap_peer, .-nv_dma_unmap_peer

    .globl nv_dynamic_power_available
    .type nv_dynamic_power_available, @function
nv_dynamic_power_available:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_dynamic_power_available(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nv_dynamic_power_available, .-nv_dynamic_power_available

    .globl nv_enable_clk
    .type nv_enable_clk, @function
nv_enable_clk:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_enable_clk(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_enable_clk, .-nv_enable_clk

    .globl nv_flush_coherent_cpu_cache_range
    .type nv_flush_coherent_cpu_cache_range, @function
nv_flush_coherent_cpu_cache_range:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_flush_coherent_cpu_cache_range(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_flush_coherent_cpu_cache_range, .-nv_flush_coherent_cpu_cache_range

#ifndef OS_NVIDIA_PHASE5
    .globl nv_free_kernel_mapping
    .type nv_free_kernel_mapping, @function
nv_free_kernel_mapping:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_free_kernel_mapping(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_free_kernel_mapping, .-nv_free_kernel_mapping
#endif

    .globl nv_get_adapter_state
    .type nv_get_adapter_state, @function
nv_get_adapter_state:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_get_adapter_state(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nv_get_adapter_state, .-nv_get_adapter_state

    .globl nv_get_all_mappings_revoked_locked
    .type nv_get_all_mappings_revoked_locked, @function
nv_get_all_mappings_revoked_locked:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_get_all_mappings_revoked_locked(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nv_get_all_mappings_revoked_locked, .-nv_get_all_mappings_revoked_locked

    .globl nv_get_current_irq_priv_data
    .type nv_get_current_irq_priv_data, @function
nv_get_current_irq_priv_data:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_get_current_irq_priv_data(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_get_current_irq_priv_data, .-nv_get_current_irq_priv_data

    .globl nv_get_dev_minor
    .type nv_get_dev_minor, @function
nv_get_dev_minor:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_get_dev_minor(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $-1, %eax
    popq %rbp
    ret
    .size nv_get_dev_minor, .-nv_get_dev_minor

    .globl nv_get_device_memory_config
    .type nv_get_device_memory_config, @function
nv_get_device_memory_config:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_get_device_memory_config(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_get_device_memory_config, .-nv_get_device_memory_config

    .globl nv_get_disp_smmu_stream_ids
    .type nv_get_disp_smmu_stream_ids, @function
nv_get_disp_smmu_stream_ids:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_get_disp_smmu_stream_ids(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_get_disp_smmu_stream_ids, .-nv_get_disp_smmu_stream_ids

    .globl nv_get_egm_info
    .type nv_get_egm_info, @function
nv_get_egm_info:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_get_egm_info(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_get_egm_info, .-nv_get_egm_info

    .globl nv_get_event
    .type nv_get_event, @function
nv_get_event:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_get_event(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $-38, %eax
    popq %rbp
    ret
    .size nv_get_event, .-nv_get_event

#ifndef OS_NVIDIA_PHASE5
    .globl nv_get_firmware
    .type nv_get_firmware, @function
nv_get_firmware:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_get_firmware(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nv_get_firmware, .-nv_get_firmware
#endif

    .globl nv_get_max_freq
    .type nv_get_max_freq, @function
nv_get_max_freq:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_get_max_freq(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_get_max_freq, .-nv_get_max_freq

    .globl nv_get_num_dpaux_instances
    .type nv_get_num_dpaux_instances, @function
nv_get_num_dpaux_instances:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_get_num_dpaux_instances(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_get_num_dpaux_instances, .-nv_get_num_dpaux_instances

    .globl nv_get_syncpoint_aperture
    .type nv_get_syncpoint_aperture, @function
nv_get_syncpoint_aperture:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_get_syncpoint_aperture(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_get_syncpoint_aperture, .-nv_get_syncpoint_aperture

    .globl nv_get_tegra_brightness_level
    .type nv_get_tegra_brightness_level, @function
nv_get_tegra_brightness_level:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_get_tegra_brightness_level(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_get_tegra_brightness_level, .-nv_get_tegra_brightness_level

    .globl nv_get_updated_emu_seg
    .type nv_get_updated_emu_seg, @function
nv_get_updated_emu_seg:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_get_updated_emu_seg(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_get_updated_emu_seg, .-nv_get_updated_emu_seg

    .globl nv_grdma_pci_topology_supported
    .type nv_grdma_pci_topology_supported, @function
nv_grdma_pci_topology_supported:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_grdma_pci_topology_supported(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nv_grdma_pci_topology_supported, .-nv_grdma_pci_topology_supported

    .globl nv_i2c_add_adapter
    .type nv_i2c_add_adapter, @function
nv_i2c_add_adapter:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_i2c_add_adapter(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nv_i2c_add_adapter, .-nv_i2c_add_adapter

    .globl nv_i2c_bus_status
    .type nv_i2c_bus_status, @function
nv_i2c_bus_status:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_i2c_bus_status(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_i2c_bus_status, .-nv_i2c_bus_status

    .globl nv_i2c_del_adapter
    .type nv_i2c_del_adapter, @function
nv_i2c_del_adapter:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_i2c_del_adapter(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_i2c_del_adapter, .-nv_i2c_del_adapter

    .globl nv_i2c_transfer
    .type nv_i2c_transfer, @function
nv_i2c_transfer:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_i2c_transfer(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_i2c_transfer, .-nv_i2c_transfer

    .globl nv_i2c_unregister_clients
    .type nv_i2c_unregister_clients, @function
nv_i2c_unregister_clients:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_i2c_unregister_clients(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_i2c_unregister_clients, .-nv_i2c_unregister_clients

    .globl nv_idle_holdoff
    .type nv_idle_holdoff, @function
nv_idle_holdoff:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_idle_holdoff(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_idle_holdoff, .-nv_idle_holdoff

    .globl nv_imp_enable_disable_rfl
    .type nv_imp_enable_disable_rfl, @function
nv_imp_enable_disable_rfl:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_imp_enable_disable_rfl(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_imp_enable_disable_rfl, .-nv_imp_enable_disable_rfl

    .globl nv_imp_get_import_data
    .type nv_imp_get_import_data, @function
nv_imp_get_import_data:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_imp_get_import_data(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_imp_get_import_data, .-nv_imp_get_import_data

    .globl nv_imp_get_uefi_data
    .type nv_imp_get_uefi_data, @function
nv_imp_get_uefi_data:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_imp_get_uefi_data(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_imp_get_uefi_data, .-nv_imp_get_uefi_data

    .globl nv_imp_icc_set_bw
    .type nv_imp_icc_set_bw, @function
nv_imp_icc_set_bw:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_imp_icc_set_bw(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_imp_icc_set_bw, .-nv_imp_icc_set_bw

    .globl nv_indicate_idle
    .type nv_indicate_idle, @function
nv_indicate_idle:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_indicate_idle(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_indicate_idle, .-nv_indicate_idle

    .globl nv_indicate_not_idle
    .type nv_indicate_not_idle, @function
nv_indicate_not_idle:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_indicate_not_idle(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_indicate_not_idle, .-nv_indicate_not_idle

    .globl nv_is_chassis_notebook
    .type nv_is_chassis_notebook, @function
nv_is_chassis_notebook:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_is_chassis_notebook(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nv_is_chassis_notebook, .-nv_is_chassis_notebook

    .globl nv_log_error
    .type nv_log_error, @function
nv_log_error:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_log_error(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_log_error, .-nv_log_error

    .globl nv_match_gpu_os_info
    .type nv_match_gpu_os_info, @function
nv_match_gpu_os_info:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_match_gpu_os_info(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nv_match_gpu_os_info, .-nv_match_gpu_os_info

    .globl nv_pci_tegra_pm_deinit
    .type nv_pci_tegra_pm_deinit, @function
nv_pci_tegra_pm_deinit:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_pci_tegra_pm_deinit(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_pci_tegra_pm_deinit, .-nv_pci_tegra_pm_deinit

    .globl nv_pci_tegra_pm_init
    .type nv_pci_tegra_pm_init, @function
nv_pci_tegra_pm_init:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_pci_tegra_pm_init(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nv_pci_tegra_pm_init, .-nv_pci_tegra_pm_init

    .globl nv_platform_supports_s0ix
    .type nv_platform_supports_s0ix, @function
nv_platform_supports_s0ix:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_platform_supports_s0ix(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nv_platform_supports_s0ix, .-nv_platform_supports_s0ix

    .globl nv_post_event
    .type nv_post_event, @function
nv_post_event:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_post_event(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_post_event, .-nv_post_event

#ifndef OS_NVIDIA_PHASE5
    .globl nv_put_firmware
    .type nv_put_firmware, @function
nv_put_firmware:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_put_firmware(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_put_firmware, .-nv_put_firmware
#endif

    .globl nv_register_peer_io_mem
    .type nv_register_peer_io_mem, @function
nv_register_peer_io_mem:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_register_peer_io_mem(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_register_peer_io_mem, .-nv_register_peer_io_mem

    .globl nv_register_phys_pages
    .type nv_register_phys_pages, @function
nv_register_phys_pages:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_register_phys_pages(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_register_phys_pages, .-nv_register_phys_pages

    .globl nv_register_sgt
    .type nv_register_sgt, @function
nv_register_sgt:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_register_sgt(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_register_sgt, .-nv_register_sgt

    .globl nv_register_user_pages
    .type nv_register_user_pages, @function
nv_register_user_pages:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_register_user_pages(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_register_user_pages, .-nv_register_user_pages

    .globl nv_release_mmap_lock
    .type nv_release_mmap_lock, @function
nv_release_mmap_lock:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_release_mmap_lock(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_release_mmap_lock, .-nv_release_mmap_lock

    .globl nv_requires_dma_remap
    .type nv_requires_dma_remap, @function
nv_requires_dma_remap:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_requires_dma_remap(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nv_requires_dma_remap, .-nv_requires_dma_remap

    .globl nv_revoke_gpu_mappings
    .type nv_revoke_gpu_mappings, @function
nv_revoke_gpu_mappings:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_revoke_gpu_mappings(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_revoke_gpu_mappings, .-nv_revoke_gpu_mappings

    .globl nv_s2idle_pm_configured
    .type nv_s2idle_pm_configured, @function
nv_s2idle_pm_configured:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_s2idle_pm_configured(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size nv_s2idle_pm_configured, .-nv_s2idle_pm_configured

    .globl nv_schedule_uvm_drain_p2p
    .type nv_schedule_uvm_drain_p2p, @function
nv_schedule_uvm_drain_p2p:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_schedule_uvm_drain_p2p(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_schedule_uvm_drain_p2p, .-nv_schedule_uvm_drain_p2p

    .globl nv_schedule_uvm_isr
    .type nv_schedule_uvm_isr, @function
nv_schedule_uvm_isr:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_schedule_uvm_isr(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_schedule_uvm_isr, .-nv_schedule_uvm_isr

    .globl nv_schedule_uvm_resume_p2p
    .type nv_schedule_uvm_resume_p2p, @function
nv_schedule_uvm_resume_p2p:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_schedule_uvm_resume_p2p(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_schedule_uvm_resume_p2p, .-nv_schedule_uvm_resume_p2p

    .globl nv_set_dma_address_size
    .type nv_set_dma_address_size, @function
nv_set_dma_address_size:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_set_dma_address_size(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_set_dma_address_size, .-nv_set_dma_address_size

    .globl nv_set_freq
    .type nv_set_freq, @function
nv_set_freq:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_set_freq(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_set_freq, .-nv_set_freq

    .globl nv_set_primary_vga_status
    .type nv_set_primary_vga_status, @function
nv_set_primary_vga_status:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_set_primary_vga_status(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_set_primary_vga_status, .-nv_set_primary_vga_status

    .globl nv_set_safe_to_mmap_locked
    .type nv_set_safe_to_mmap_locked, @function
nv_set_safe_to_mmap_locked:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_set_safe_to_mmap_locked(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_set_safe_to_mmap_locked, .-nv_set_safe_to_mmap_locked

    .globl nv_set_tegra_brightness_level
    .type nv_set_tegra_brightness_level, @function
nv_set_tegra_brightness_level:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_set_tegra_brightness_level(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_set_tegra_brightness_level, .-nv_set_tegra_brightness_level

    .globl nv_start_nano_timer
    .type nv_start_nano_timer, @function
nv_start_nano_timer:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_start_nano_timer(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_start_nano_timer, .-nv_start_nano_timer

    .globl nv_start_rc_timer
    .type nv_start_rc_timer, @function
nv_start_rc_timer:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_start_rc_timer(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $-38, %eax
    popq %rbp
    ret
    .size nv_start_rc_timer, .-nv_start_rc_timer

    .globl nv_stop_rc_timer
    .type nv_stop_rc_timer, @function
nv_stop_rc_timer:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_stop_rc_timer(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $-38, %eax
    popq %rbp
    ret
    .size nv_stop_rc_timer, .-nv_stop_rc_timer

    .globl nv_tegra_dce_client_ipc_send_recv
    .type nv_tegra_dce_client_ipc_send_recv, @function
nv_tegra_dce_client_ipc_send_recv:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_tegra_dce_client_ipc_send_recv(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_tegra_dce_client_ipc_send_recv, .-nv_tegra_dce_client_ipc_send_recv

    .globl nv_tegra_dce_register_ipc_client
    .type nv_tegra_dce_register_ipc_client, @function
nv_tegra_dce_register_ipc_client:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_tegra_dce_register_ipc_client(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_tegra_dce_register_ipc_client, .-nv_tegra_dce_register_ipc_client

    .globl nv_tegra_dce_unregister_ipc_client
    .type nv_tegra_dce_unregister_ipc_client, @function
nv_tegra_dce_unregister_ipc_client:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_tegra_dce_unregister_ipc_client(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x56, %eax
    popq %rbp
    ret
    .size nv_tegra_dce_unregister_ipc_client, .-nv_tegra_dce_unregister_ipc_client

    .globl nv_tegra_get_rm_interface_type
    .type nv_tegra_get_rm_interface_type, @function
nv_tegra_get_rm_interface_type:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_tegra_get_rm_interface_type(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    movl $-1, %eax
    popq %rbp
    ret
    .size nv_tegra_get_rm_interface_type, .-nv_tegra_get_rm_interface_type

    .globl nv_unregister_peer_io_mem
    .type nv_unregister_peer_io_mem, @function
nv_unregister_peer_io_mem:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_unregister_peer_io_mem(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_unregister_peer_io_mem, .-nv_unregister_peer_io_mem

    .globl nv_unregister_phys_pages
    .type nv_unregister_phys_pages, @function
nv_unregister_phys_pages:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_unregister_phys_pages(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_unregister_phys_pages, .-nv_unregister_phys_pages

    .globl nv_unregister_sgt
    .type nv_unregister_sgt, @function
nv_unregister_sgt:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_unregister_sgt(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_unregister_sgt, .-nv_unregister_sgt

    .globl nv_unregister_user_pages
    .type nv_unregister_user_pages, @function
nv_unregister_user_pages:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_nv_unregister_user_pages(%rip), %rdi
    leaq .Lnvabi_stage_nv(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size nv_unregister_user_pages, .-nv_unregister_user_pages

    .section .rodata.nvabi_nv_unsupported,"a",@progbits
.Lnvabi_stage_nv:
    .asciz "vendor-abi/nv-unsupported"
.Lnvabi_msg_nv_acpi_d3cold_dsm_for_upstream_port:
    .asciz "nv_acpi_d3cold_dsm_for_upstream_port"
.Lnvabi_msg_nv_acpi_ddc_method:
    .asciz "nv_acpi_ddc_method"
.Lnvabi_msg_nv_acpi_dod_method:
    .asciz "nv_acpi_dod_method"
.Lnvabi_msg_nv_acpi_dsm_method:
    .asciz "nv_acpi_dsm_method"
.Lnvabi_msg_nv_acpi_get_powersource:
    .asciz "nv_acpi_get_powersource"
.Lnvabi_msg_nv_acpi_is_battery_present:
    .asciz "nv_acpi_is_battery_present"
.Lnvabi_msg_nv_acpi_methods_init:
    .asciz "nv_acpi_methods_init"
.Lnvabi_msg_nv_acpi_methods_uninit:
    .asciz "nv_acpi_methods_uninit"
.Lnvabi_msg_nv_acpi_mux_method:
    .asciz "nv_acpi_mux_method"
.Lnvabi_msg_nv_acpi_rom_method:
    .asciz "nv_acpi_rom_method"
.Lnvabi_msg_nv_acquire_fabric_mgmt_cap:
    .asciz "nv_acquire_fabric_mgmt_cap"
.Lnvabi_msg_nv_acquire_mmap_lock:
    .asciz "nv_acquire_mmap_lock"
.Lnvabi_msg_nv_add_mapping_context_to_file:
    .asciz "nv_add_mapping_context_to_file"
.Lnvabi_msg_nv_alias_pages:
    .asciz "nv_alias_pages"
.Lnvabi_msg_nv_alloc_kernel_mapping:
    .asciz "nv_alloc_kernel_mapping"
.Lnvabi_msg_nv_alloc_user_mapping:
    .asciz "nv_alloc_user_mapping"
.Lnvabi_msg_nv_allow_runtime_suspend:
    .asciz "nv_allow_runtime_suspend"
.Lnvabi_msg_nv_audio_dynamic_power:
    .asciz "nv_audio_dynamic_power"
.Lnvabi_msg_nv_bpmp_send_mrq:
    .asciz "nv_bpmp_send_mrq"
.Lnvabi_msg_nv_cancel_nano_timer:
    .asciz "nv_cancel_nano_timer"
.Lnvabi_msg_nv_control_soc_irqs:
    .asciz "nv_control_soc_irqs"
.Lnvabi_msg_nv_create_nano_timer:
    .asciz "nv_create_nano_timer"
.Lnvabi_msg_nv_destroy_nano_timer:
    .asciz "nv_destroy_nano_timer"
.Lnvabi_msg_nv_disable_clk:
    .asciz "nv_disable_clk"
.Lnvabi_msg_nv_disallow_runtime_suspend:
    .asciz "nv_disallow_runtime_suspend"
.Lnvabi_msg_nv_dma_cache_invalidate:
    .asciz "nv_dma_cache_invalidate"
.Lnvabi_msg_nv_dma_get_dev_pagemap:
    .asciz "nv_dma_get_dev_pagemap"
.Lnvabi_msg_nv_dma_import_dma_buf:
    .asciz "nv_dma_import_dma_buf"
.Lnvabi_msg_nv_dma_import_from_fd:
    .asciz "nv_dma_import_from_fd"
.Lnvabi_msg_nv_dma_import_sgt:
    .asciz "nv_dma_import_sgt"
.Lnvabi_msg_nv_dma_map_alloc:
    .asciz "nv_dma_map_alloc"
.Lnvabi_msg_nv_dma_map_mmio:
    .asciz "nv_dma_map_mmio"
.Lnvabi_msg_nv_dma_map_peer:
    .asciz "nv_dma_map_peer"
.Lnvabi_msg_nv_dma_put_dev_pagemap:
    .asciz "nv_dma_put_dev_pagemap"
.Lnvabi_msg_nv_dma_release_dma_buf:
    .asciz "nv_dma_release_dma_buf"
.Lnvabi_msg_nv_dma_release_sgt:
    .asciz "nv_dma_release_sgt"
.Lnvabi_msg_nv_dma_unmap_alloc:
    .asciz "nv_dma_unmap_alloc"
.Lnvabi_msg_nv_dma_unmap_mmio:
    .asciz "nv_dma_unmap_mmio"
.Lnvabi_msg_nv_dma_unmap_peer:
    .asciz "nv_dma_unmap_peer"
.Lnvabi_msg_nv_dynamic_power_available:
    .asciz "nv_dynamic_power_available"
.Lnvabi_msg_nv_enable_clk:
    .asciz "nv_enable_clk"
.Lnvabi_msg_nv_flush_coherent_cpu_cache_range:
    .asciz "nv_flush_coherent_cpu_cache_range"
.Lnvabi_msg_nv_free_kernel_mapping:
    .asciz "nv_free_kernel_mapping"
.Lnvabi_msg_nv_free_user_mapping:
    .asciz "nv_free_user_mapping"
.Lnvabi_msg_nv_get_adapter_state:
    .asciz "nv_get_adapter_state"
.Lnvabi_msg_nv_get_all_mappings_revoked_locked:
    .asciz "nv_get_all_mappings_revoked_locked"
.Lnvabi_msg_nv_get_ctl_state:
    .asciz "nv_get_ctl_state"
.Lnvabi_msg_nv_get_current_irq_priv_data:
    .asciz "nv_get_current_irq_priv_data"
.Lnvabi_msg_nv_get_dev_minor:
    .asciz "nv_get_dev_minor"
.Lnvabi_msg_nv_get_device_memory_config:
    .asciz "nv_get_device_memory_config"
.Lnvabi_msg_nv_get_disp_smmu_stream_ids:
    .asciz "nv_get_disp_smmu_stream_ids"
.Lnvabi_msg_nv_get_egm_info:
    .asciz "nv_get_egm_info"
.Lnvabi_msg_nv_get_event:
    .asciz "nv_get_event"
.Lnvabi_msg_nv_get_file_private:
    .asciz "nv_get_file_private"
.Lnvabi_msg_nv_get_firmware:
    .asciz "nv_get_firmware"
.Lnvabi_msg_nv_get_max_freq:
    .asciz "nv_get_max_freq"
.Lnvabi_msg_nv_get_num_dpaux_instances:
    .asciz "nv_get_num_dpaux_instances"
.Lnvabi_msg_nv_get_syncpoint_aperture:
    .asciz "nv_get_syncpoint_aperture"
.Lnvabi_msg_nv_get_tegra_brightness_level:
    .asciz "nv_get_tegra_brightness_level"
.Lnvabi_msg_nv_get_updated_emu_seg:
    .asciz "nv_get_updated_emu_seg"
.Lnvabi_msg_nv_grdma_pci_topology_supported:
    .asciz "nv_grdma_pci_topology_supported"
.Lnvabi_msg_nv_i2c_add_adapter:
    .asciz "nv_i2c_add_adapter"
.Lnvabi_msg_nv_i2c_bus_status:
    .asciz "nv_i2c_bus_status"
.Lnvabi_msg_nv_i2c_del_adapter:
    .asciz "nv_i2c_del_adapter"
.Lnvabi_msg_nv_i2c_transfer:
    .asciz "nv_i2c_transfer"
.Lnvabi_msg_nv_i2c_unregister_clients:
    .asciz "nv_i2c_unregister_clients"
.Lnvabi_msg_nv_idle_holdoff:
    .asciz "nv_idle_holdoff"
.Lnvabi_msg_nv_imp_enable_disable_rfl:
    .asciz "nv_imp_enable_disable_rfl"
.Lnvabi_msg_nv_imp_get_import_data:
    .asciz "nv_imp_get_import_data"
.Lnvabi_msg_nv_imp_get_uefi_data:
    .asciz "nv_imp_get_uefi_data"
.Lnvabi_msg_nv_imp_icc_set_bw:
    .asciz "nv_imp_icc_set_bw"
.Lnvabi_msg_nv_indicate_idle:
    .asciz "nv_indicate_idle"
.Lnvabi_msg_nv_indicate_not_idle:
    .asciz "nv_indicate_not_idle"
.Lnvabi_msg_nv_is_chassis_notebook:
    .asciz "nv_is_chassis_notebook"
.Lnvabi_msg_nv_is_gpu_accessible:
    .asciz "nv_is_gpu_accessible"
.Lnvabi_msg_nv_log_error:
    .asciz "nv_log_error"
.Lnvabi_msg_nv_match_gpu_os_info:
    .asciz "nv_match_gpu_os_info"
.Lnvabi_msg_nv_pci_tegra_pm_deinit:
    .asciz "nv_pci_tegra_pm_deinit"
.Lnvabi_msg_nv_pci_tegra_pm_init:
    .asciz "nv_pci_tegra_pm_init"
.Lnvabi_msg_nv_platform_supports_s0ix:
    .asciz "nv_platform_supports_s0ix"
.Lnvabi_msg_nv_post_event:
    .asciz "nv_post_event"
.Lnvabi_msg_nv_put_file_private:
    .asciz "nv_put_file_private"
.Lnvabi_msg_nv_put_firmware:
    .asciz "nv_put_firmware"
.Lnvabi_msg_nv_register_peer_io_mem:
    .asciz "nv_register_peer_io_mem"
.Lnvabi_msg_nv_register_phys_pages:
    .asciz "nv_register_phys_pages"
.Lnvabi_msg_nv_register_sgt:
    .asciz "nv_register_sgt"
.Lnvabi_msg_nv_register_user_pages:
    .asciz "nv_register_user_pages"
.Lnvabi_msg_nv_release_mmap_lock:
    .asciz "nv_release_mmap_lock"
.Lnvabi_msg_nv_requires_dma_remap:
    .asciz "nv_requires_dma_remap"
.Lnvabi_msg_nv_revoke_gpu_mappings:
    .asciz "nv_revoke_gpu_mappings"
.Lnvabi_msg_nv_s2idle_pm_configured:
    .asciz "nv_s2idle_pm_configured"
.Lnvabi_msg_nv_schedule_uvm_drain_p2p:
    .asciz "nv_schedule_uvm_drain_p2p"
.Lnvabi_msg_nv_schedule_uvm_isr:
    .asciz "nv_schedule_uvm_isr"
.Lnvabi_msg_nv_schedule_uvm_resume_p2p:
    .asciz "nv_schedule_uvm_resume_p2p"
.Lnvabi_msg_nv_set_dma_address_size:
    .asciz "nv_set_dma_address_size"
.Lnvabi_msg_nv_set_freq:
    .asciz "nv_set_freq"
.Lnvabi_msg_nv_set_primary_vga_status:
    .asciz "nv_set_primary_vga_status"
.Lnvabi_msg_nv_set_safe_to_mmap_locked:
    .asciz "nv_set_safe_to_mmap_locked"
.Lnvabi_msg_nv_set_tegra_brightness_level:
    .asciz "nv_set_tegra_brightness_level"
.Lnvabi_msg_nv_start_nano_timer:
    .asciz "nv_start_nano_timer"
.Lnvabi_msg_nv_start_rc_timer:
    .asciz "nv_start_rc_timer"
.Lnvabi_msg_nv_stop_rc_timer:
    .asciz "nv_stop_rc_timer"
.Lnvabi_msg_nv_tegra_dce_client_ipc_send_recv:
    .asciz "nv_tegra_dce_client_ipc_send_recv"
.Lnvabi_msg_nv_tegra_dce_register_ipc_client:
    .asciz "nv_tegra_dce_register_ipc_client"
.Lnvabi_msg_nv_tegra_dce_unregister_ipc_client:
    .asciz "nv_tegra_dce_unregister_ipc_client"
.Lnvabi_msg_nv_tegra_get_rm_interface_type:
    .asciz "nv_tegra_get_rm_interface_type"
.Lnvabi_msg_nv_unregister_peer_io_mem:
    .asciz "nv_unregister_peer_io_mem"
.Lnvabi_msg_nv_unregister_phys_pages:
    .asciz "nv_unregister_phys_pages"
.Lnvabi_msg_nv_unregister_sgt:
    .asciz "nv_unregister_sgt"
.Lnvabi_msg_nv_unregister_user_pages:
    .asciz "nv_unregister_user_pages"
