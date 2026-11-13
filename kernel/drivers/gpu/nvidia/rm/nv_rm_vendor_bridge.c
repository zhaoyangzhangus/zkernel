#include "nv_rm_vendor_bridge.h"

#include <nv.h>
#include <nv-ioctl.h>
#include <nv_escape.h>
#include <nv-unix-nvos-params-wrappers.h>
#include <class/cl003e.h>
#include <class/cl0005.h>

#ifndef OS_NVIDIA_PHASE5
#define OS_NVIDIA_PHASE5 0
#endif

#ifndef OS_NVIDIA_PHASE6
#define OS_NVIDIA_PHASE6 0
#endif

#ifndef OS_NVIDIA_PREFER_MSI
#define OS_NVIDIA_PREFER_MSI 0
#endif

/* 不可变的 595.84 Linux MSI-X 路径将 GPU 向量数限制在 NV_RM_MAX_MSIX_LINES。
 * 此处由 RM ISR 共享处理，因为厂商入口不使用 Linux IRQ 编号。 */
#define NV_RM_MSIX_ENTRY_LIMIT 8U

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/console.h>
#include <kernel/deferred.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <gpu/gpu.h>
#include <gpu/mem_mgr/mem_mgr.h>
#include <gpu/gpu_arch.h>
#include <core/hal.h>
#include <core/hal_mgr.h>
#include <core/system.h>
#include <gpu_mgr/gpu_mgr.h>
#include <mem_mgr/io_vaspace.h>
#include <kernel/rmapi/exports.h>
#include <osapi.h>
#include <nv-priv.h>
#include <nv_arch.h>
#include <nv_ref.h>
#include <nvoc/object.h>
#include <nv-gpu-info.h>
#include <nv-kernel-rmapi-ops.h>

#include "../../port/include/nv_port.h"
#include "../../port/include/nv_port_events.h"
#include <kernel/object.h>
#include "nv_rm_driver.h"
#include <uapi/nvidia_frontend.h>

#if OS_NVIDIA_PHASE6
extern kstatus_t nv_rm_vendor_phase6_probe(nv_state_t *state);
#endif

/* 不要在此编译单元中包含内核打开文件用的 OS 接口头文件。其 rs_access.h 与 RM NVOC
 * 头文件来自不同的厂商 include 根目录，不能同时包含。此桥接层只使用这两个 OS 接口入口，
 * 并按厂商 ABI 的准确类型声明。 */
extern void *NV_API_CALL os_pci_init_handle(NvU32, NvU8, NvU8, NvU8,
                                             NvU16 *, NvU16 *);
extern void NV_API_CALL os_pci_remove(void *);

extern NV_STATUS NV_API_CALL nvabi_get_system_mapping_pages(
    void *, NvU64, NvU64, const uint64_t **, NvU32 *);
extern NV_STATUS NV_API_CALL os_lock_user_pages(void *, NvU64, void **, NvU32);
extern NV_STATUS NV_API_CALL os_unlock_user_pages(NvU64, void *, NvU32);

typedef struct nv_rm_vendor_context {
    nvidia_stack_t *stack;
    nv_state_t *state;
    void *pci_handle;
    nv_port_irq_t irq;
    nvidia_stack_t *isr_stack;
    nvidia_stack_t *isr_bh_stack;
    atomic_bool irq_bottom_pending;
    atomic_bool irq_bottom_queued;
    atomic_bool irq_bottom_schedule_failed;
    atomic_uint irq_top_hits;
    atomic_uint irq_rm_handled_hits;
    atomic_uint irq_bottom_hits;
    uint32_t irq_flags;
    bool irq_started;
    bool rm_initialized;
    bool event_locks_initialized;
    bool private_initialized;
    bool adapter_initialized;
    bool kms_lock_initialized;
    nv_port_mutex_t kms_rm_lock;
    atomic_uint kms_gpu_refs;
    OBJGPU *phase4_gpu;
} nv_rm_vendor_context_t;

static void nv_rm_vendor_irq_bottom_half(void *argument);

static void nv_rm_vendor_schedule_irq_bottom_half(
    nv_rm_vendor_context_t *context)
{
    bool expected = false;

    if (context == NULL ||
        !atomic_compare_exchange_strong_explicit(
            &context->irq_bottom_queued, &expected, true,
            memory_order_acq_rel, memory_order_acquire))
        return;

    if (!deferred_try_schedule(nv_rm_vendor_irq_bottom_half, context)) {
        atomic_store_explicit(&context->irq_bottom_queued, false,
                              memory_order_release);
        atomic_store_explicit(&context->irq_bottom_schedule_failed, true,
                              memory_order_release);
    }
}

static void nv_rm_vendor_irq_top(void *argument)
{
    nv_rm_vendor_context_t *context =
        (nv_rm_vendor_context_t *)argument;
    NvU32 need_bottom_half = NV_FALSE;

    if (context == NULL || context->state == NULL ||
        context->isr_stack == NULL || !context->irq_started)
        return;

    atomic_fetch_add_explicit(&context->irq_top_hits, 1U,
                              memory_order_relaxed);

    /* rm_isr() 是厂商的中断顶半部入口。它负责检查待处理中断，并返回是否确实需要运行
     * 底半部。 */
    if (rm_isr(context->isr_stack, context->state, &need_bottom_half)) {
        atomic_fetch_add_explicit(&context->irq_rm_handled_hits, 1U,
                                  memory_order_relaxed);
    }
    if (need_bottom_half != NV_FALSE) {
        atomic_store_explicit(&context->irq_bottom_pending, true,
                              memory_order_release);
        nv_rm_vendor_schedule_irq_bottom_half(context);
    }
}

static void nv_rm_vendor_irq_bottom_half(void *argument)
{
    nv_rm_vendor_context_t *context =
        (nv_rm_vendor_context_t *)argument;

    if (context == NULL)
        return;

    /* 将合并的厂商请求全部处理后，再将队列槽位发布为空闲状态。最后一次待处理检查用于
     * 关闭 IRQ 与工作线程交接时的竞争窗口。 */
    for (;;) {
        if (!atomic_exchange_explicit(&context->irq_bottom_pending, false,
                                      memory_order_acq_rel))
            break;
        atomic_fetch_add_explicit(&context->irq_bottom_hits, 1U,
                                  memory_order_relaxed);
        if (context->state != NULL && context->isr_bh_stack != NULL)
            rm_isr_bh(context->isr_bh_stack, context->state);
    }

    atomic_store_explicit(&context->irq_bottom_queued, false,
                          memory_order_release);
    if (atomic_load_explicit(&context->irq_bottom_pending,
                             memory_order_acquire))
        nv_rm_vendor_schedule_irq_bottom_half(context);
}

static void nv_rm_vendor_drain_irq_work(nv_rm_vendor_context_t *context)
{
    if (context == NULL)
        return;

    while (atomic_load_explicit(&context->irq_bottom_queued,
                                memory_order_acquire)) {
        /* 当前 RM 探测先于调度器工作线程启动。如果厂商早期路径请求执行底半部，则在拆除流程
         * 保持 RM 状态有效期间，使用现有的延迟执行器运行排队任务。 */
        if (!deferred_worker_started() && deferred_run(64U) != 0U)
            continue;
        __asm__ volatile ("pause");
    }
}

static void nv_rm_vendor_stop_irq(nv_rm_vendor_context_t *context)
{
    kstatus_t status;

    if (context == NULL || !context->irq_started)
        return;

    status = nv_port_irq_stop(&context->irq);
    nv_rm_vendor_drain_irq_work(context);

    if (context->irq.use_msix && context->irq.device != NULL) {
        kstatus_t disable = pci_msix_disable((pci_device_t *)
                                              context->irq.device);
        if (status == K_OK)
            status = disable;
    }

    if (status != K_OK)
        kernel_serial_printf_serial_only(
            "OS_NVIDIA_IRQ_STOP_FAIL status=%d\r\n", (int)status);

    if (context->state != NULL)
        context->state->flags &= ~context->irq_flags;
    context->irq_flags = 0U;
    context->irq_started = false;
}

static kstatus_t nv_rm_vendor_start_irq(
    const pci_device_t *device, nv_rm_vendor_context_t *context)
{
    uint32_t apic_id;
    kstatus_t status = K_ENOENT;

    if (device == NULL || context == NULL || context->state == NULL)
        return K_EINVAL;

    apic_id = x86_current_apic_id();

    /* 遵循厂商 PCI 路径对 MSI-X 的优先选择，但 RM 中断流只使用一个真实向量。MSI 仍是
     * 显式的回退方式。 */
    if (!OS_NVIDIA_PREFER_MSI && device->msix_capability != 0U &&
        device->msix_table_size != 0U) {
        status = nv_port_irq_init(&context->irq, device, true, 0U, apic_id,
                                  nv_rm_vendor_irq_top, context, NULL, NULL);
        if (status == K_OK)
            status = nv_port_irq_start(&context->irq);
        if (status == K_OK) {
            /* NVIDIA 595.84 Linux 路径会在注册 RM ISR 前启用所有受支持的 MSI-X 表项。
             * System 仍共用一个处理程序，但会配置厂商路径请求的所有表项，使任何 GPU
             * 中断源都能到达该处理程序。 */
            paddr_t table = {0};
            uint16_t entries = 0U;
            uint16_t active_entries = 0U;
            if (pci_msix_table(device, &table, &entries) != K_OK ||
                entries == 0U) {
                (void)nv_port_irq_stop(&context->irq);
                (void)pci_msix_disable((pci_device_t *)device);
                status = K_EIO;
            } else {
                active_entries = entries < NV_RM_MSIX_ENTRY_LIMIT ?
                    entries : NV_RM_MSIX_ENTRY_LIMIT;
                for (uint16_t entry = 1U; entry < active_entries; ++entry) {
                    status = pci_msix_configure((pci_device_t *)device, entry,
                                                apic_id, context->irq.vector);
                    if (status != K_OK) break;
                }
                if (status != K_OK) {
                    (void)nv_port_irq_stop(&context->irq);
                    (void)pci_msix_disable((pci_device_t *)device);
                }
            }
        }
        if (status == K_OK) {
            context->irq_flags = NV_FLAG_USES_MSIX;
            context->state->flags |= context->irq_flags;
            context->irq_started = true;
            {
                paddr_t table = {0};
                uint16_t entries = 0U;
                uint64_t message_address = 0U;
                uint32_t message_data = 0U;
                uint32_t vector_control = 0U;
                if (pci_msix_table(device, &table, &entries) == K_OK &&
                    pci_msix_read_entry(device, context->irq.msix_entry,
                                        &message_address, &message_data,
                                        &vector_control) == K_OK) {
                    kernel_serial_printf_serial_only(
                        "OS_NVIDIA_IRQ_CONFIG mode=msix table=%llx entries=%u "
                        "active=%u entry=%u addr=%llx data=%x ctl=%x apic=%u vector=%u\r\n",
                        (unsigned long long)table.value,
                        (unsigned)entries,
                        (unsigned)(entries < NV_RM_MSIX_ENTRY_LIMIT ?
                                   entries : NV_RM_MSIX_ENTRY_LIMIT),
                        (unsigned)context->irq.msix_entry,
                        (unsigned long long)message_address,
                        (unsigned)message_data,
                        (unsigned)vector_control,
                        (unsigned)context->irq.apic_id,
                        (unsigned)context->irq.vector);
                }
            }
            kernel_serial_printf_serial_only(
                "OS_NVIDIA_IRQ_READY MSI-X vector=%u\r\n",
                (unsigned)context->irq.vector);
            return K_OK;
        }
        (void)pci_msix_disable((pci_device_t *)device);
    }

    if (device->msi_capability != 0U) {
        status = nv_port_irq_init(&context->irq, device, false, 0U,
                                  apic_id, nv_rm_vendor_irq_top, context,
                                  NULL, NULL);
        if (status == K_OK)
            status = nv_port_irq_start(&context->irq);
        if (status == K_OK) {
            context->irq_flags = NV_FLAG_USES_MSI;
            context->state->flags |= context->irq_flags;
            context->irq_started = true;
            kernel_serial_printf_serial_only(
                "OS_NVIDIA_IRQ_READY MSI vector=%u\r\n",
                (unsigned)context->irq.vector);
            return K_OK;
        }
    }

    return status == K_ENOENT ? K_EIO : status;
}

static void nv_rm_vendor_cleanup(nv_rm_vendor_context_t *context)
{
    if (context == NULL)
        return;

    /* 遵循不可变的 595.84 Linux 拆除顺序：先在 RM 中断路径仍可用时禁用运行中的适配器，
     * 再拆除 IRQ，最后释放适配器状态。rm_init_adapter() 失败意味着适配器未成功初始化，
     * 不得将其传递给 rm_shutdown_adapter()。 */
    if (context->adapter_initialized)
        rm_disable_adapter(context->stack, context->state);

    nv_rm_vendor_stop_irq(context);

    if (context->adapter_initialized)
        rm_shutdown_adapter(context->stack, context->state);

    /* 第 4 阶段只保留真实的厂商 OBJGPU 发现对象。释放私有 RM 和全局 RM 状态前，必须先
     * 删除该对象；GSP 引导和完整适配器路径从第 5 阶段开始。 */
    if (context->phase4_gpu != NULL) {
        objDelete(context->phase4_gpu);
        context->phase4_gpu = NULL;
    }

    if (context->private_initialized)
        rm_free_private_state(context->stack, context->state);

    if (context->event_locks_initialized)
        rm_destroy_event_locks(context->stack, context->state);

    if (context->rm_initialized)
        rm_shutdown_rm(context->stack);

    if (context->pci_handle != NULL) {
        os_pci_remove(context->pci_handle);
        context->pci_handle = NULL;
    }

    if (context->state != NULL) {
        kfree(context->state);
        context->state = NULL;
    }

    if (context->isr_bh_stack != NULL) {
        kfree(context->isr_bh_stack);
        context->isr_bh_stack = NULL;
    }

    if (context->isr_stack != NULL) {
        kfree(context->isr_stack);
        context->isr_stack = NULL;
    }

    if (context->stack != NULL) {
        kfree(context->stack);
        context->stack = NULL;
    }

    kfree(context);
}

static void nv_rm_vendor_fill_aperture(nv_aperture_t *aperture,
                                       const pci_bar_t *bar,
                                       uint32_t bar_index)
{
    aperture->cpu_address = bar->address;
    aperture->size = bar->length;
    aperture->offset = (NvU32)(0x10U + (bar_index * 4U));
    aperture->map = NULL;
    aperture->map_u = 0;
}

static kstatus_t nv_rm_vendor_fill_pci(const pci_device_t *device,
                                       nv_rm_vendor_context_t *context)
{
    nv_state_t *state = context->state;
    uint32_t memory_bar = 0U;
    uint32_t i;
    uint32_t subsystem;
    uint8_t interrupt_line;
    NvU16 vendor = 0U;
    NvU16 device_id = 0U;

    state->pci_info.domain = (NvU32)device->segment;
    state->pci_info.bus = device->bus;
    state->pci_info.slot = device->slot;
    state->pci_info.function = device->function;
    state->pci_info.vendor_id = device->vendor_id;
    state->pci_info.device_id = device->device_id;

    for (i = 0U; i < NVRM_PCICFG_NUM_DWORDS; ++i) {
        kstatus_t status = pci_read_config32(device, (uint16_t)(i * 4U),
                                              &state->pci_cfg_space[i]);
        if (status != K_OK)
            return status;
    }

    subsystem = state->pci_cfg_space[0x2cU / 4U];
    state->subsystem_vendor = (NvU16)(subsystem & 0xffffU);
    state->subsystem_id = (NvU16)(subsystem >> 16U);

    if (pci_read_config8(device, 0x3cU, &interrupt_line) == K_OK &&
        interrupt_line != 0xffU)
        state->interrupt_line = interrupt_line;

    for (i = 0U; i < PCI_MAX_BARS && memory_bar < NV_GPU_NUM_BARS; ++i) {
        const pci_bar_t *bar = &device->bars[i];

        if ((bar->flags & PCI_RESOURCE_MEMORY) == 0U ||
            bar->length == 0U || bar->address == 0U)
            continue;

        nv_rm_vendor_fill_aperture(&state->bars[memory_bar], bar, i);
        ++memory_bar;
    }

    if (memory_bar < 2U)
        return K_EIO;

    state->regs = &state->bars[0];
    state->fb = &state->bars[1];
    /* 在此边界处，System 没有 GPU IOVA 重映射域。0 是有效的厂商 vaspace ID，不能作为
     * 直接物理 PCIe 路径的哨兵值；传递厂商定义的准确“无域”值，将 GMMU/VA 所有权留给 NVIDIA。 */
    state->iovaspace_id = NV_IOVA_DOMAIN_NONE;
    state->cpu_numa_node_id = (NvS32)-1;
#if OS_NVIDIA_PHASE5
    state->request_firmware = NV_TRUE;
#else
    state->request_firmware = NV_FALSE;
#endif
    state->request_fw_client_rm = NV_FALSE;
    state->allow_fallback_to_monolithic_rm = NV_FALSE;
    state->enable_firmware_logs = NV_FALSE;
    state->os_state = context;

    context->pci_handle = os_pci_init_handle(
        state->pci_info.domain, state->pci_info.bus, state->pci_info.slot,
        state->pci_info.function, &vendor, &device_id);
    if (context->pci_handle == NULL)
        return K_EIO;

    if (vendor != state->pci_info.vendor_id ||
        device_id != state->pci_info.device_id)
        return K_EIO;

    state->handle = context->pci_handle;
    return K_OK;
}

static void nv_rm_vendor_log_boot_registers(const nv_state_t *state)
{
    nv_port_mmio_mapping_t mapping = {0};

    if (state == NULL || state->regs == NULL)
        return;

    if (nv_port_mmio_map(paddr_make(state->regs->cpu_address), 0x1000U,
                         NV_PORT_CACHE_UC, &mapping) != K_OK) {
        kernel_serial_write_serial_only(
            "OS_NVIDIA_BOOT_REGS_READ_FAILED\r\n");
        return;
    }

    kernel_serial_printf_serial_only(
        "OS_NVIDIA_BOOT_REGS boot0=%x boot1=%x boot42=%x\r\n",
        (unsigned)nv_port_mmio_read32(mapping.base, 0x000U),
        (unsigned)nv_port_mmio_read32(mapping.base, 0x004U),
        (unsigned)nv_port_mmio_read32(mapping.base, 0xa00U));
    (void)nv_port_mmio_unmap(&mapping);
}

static void nv_rm_vendor_log_hal_state(const nv_state_t *state)
{
    const nv_priv_t *private_state;
    OBJSYS *sys;
    OBJHALMGR *hal_mgr;
    OBJGPUMGR *gpu_mgr;
    GpuArch *gpu_arch = NULL;
    NvU64 encoded_dbdf = 0ULL;
    NvU32 probed_gpu_matches = 0U;
    NvU32 probed_dbdf_matches = 0U;
    NvU32 registered_gpu_id = 0U;
    NvU64 registered_dbdf = 0ULL;
    NvU32 matched_impl = 0U;
    NV_STATUS match_status = (NV_STATUS)0xffffffffU;

    if (state == NULL)
        return;

    private_state = (const nv_priv_t *)NV_GET_NV_PRIV(state);
    sys = SYS_GET_INSTANCE();
    hal_mgr = sys != NULL ? SYS_GET_HALMGR(sys) : NULL;
    gpu_mgr = sys != NULL ? SYS_GET_GPUMGR(sys) : NULL;
    if (private_state != NULL && hal_mgr != NULL) {
        match_status = halmgrGetHalForGpu(
            hal_mgr, private_state->pmc_boot_0,
            private_state->pmc_boot_42, &matched_impl);
    }
    encoded_dbdf = gpuEncodeDomainBusDevice(
        state->pci_info.domain, state->pci_info.bus, state->pci_info.slot);
    if (private_state != NULL && gpu_mgr != NULL) {
        gpu_arch = gpumgrGetGpuArch(private_state->pmc_boot_42, 0U,
                                    TEGRA_CHIP_TYPE_DEFAULT);
        for (NvU32 i = 0U; i < NV_ARRAY_ELEMENTS(gpu_mgr->probedGpus); ++i) {
            if (gpu_mgr->probedGpus[i].gpuId == state->gpu_id) {
                ++probed_gpu_matches;
                registered_gpu_id = gpu_mgr->probedGpus[i].gpuId;
                registered_dbdf = gpu_mgr->probedGpus[i].gpuDomainBusDevice;
            }
            if (gpu_mgr->probedGpus[i].gpuDomainBusDevice == encoded_dbdf)
                ++probed_dbdf_matches;
        }
    }

    kernel_serial_printf_serial_only(
        "OS_NVIDIA_HAL_STATE sys=%llx mgr=%llx gb205=%llx match=%x impl=%x dbdf=%llx gpu=%x probed=%u/%u reg=%x/%llx arch=%llx chip=%x/%x\r\n",
        (unsigned long long)(uintptr_t)sys,
        (unsigned long long)(uintptr_t)hal_mgr,
        hal_mgr != NULL
            ? (unsigned long long)(uintptr_t)hal_mgr->pHalList[HAL_IMPL_GB205]
            : 0ULL,
        (unsigned)match_status, (unsigned)matched_impl,
        (unsigned long long)encoded_dbdf, private_state != NULL
            ? (unsigned)state->gpu_id : 0U,
        (unsigned)probed_gpu_matches, (unsigned)probed_dbdf_matches,
        (unsigned)registered_gpu_id, (unsigned long long)registered_dbdf,
        (unsigned long long)(uintptr_t)gpu_arch,
        gpu_arch != NULL ? (unsigned)gpu_arch->chipArch : 0U,
        gpu_arch != NULL ? (unsigned)gpu_arch->chipImpl : 0U);
}

/* 第 4 阶段止于真实的厂商 OBJGPU/NVOC/HAL 发现边界。该对象不会插入 gpumgr 句柄表，
 * 而是由常规逆序拆除路径删除。适配器/GSP 转换由第 5 阶段负责。 */
static kstatus_t nv_rm_vendor_create_phase4_gpu(
    nv_rm_vendor_context_t *context)
{
    const nv_priv_t *private_state;
    OBJSYS *sys;
    GpuArch *gpu_arch;
    OBJGPU *probe_gpu = NULL;
    NV_STATUS create_status;
    NV_STATUS bind_status = (NV_STATUS)0xffffffffU;

    if (context == NULL || context->state == NULL)
        return K_EINVAL;

    private_state = (const nv_priv_t *)NV_GET_NV_PRIV(context->state);
    sys = SYS_GET_INSTANCE();
    if (private_state == NULL || sys == NULL)
        return K_EIO;

    gpu_arch = gpumgrGetGpuArch(private_state->pmc_boot_42, 0U,
                                TEGRA_CHIP_TYPE_DEFAULT);
    if (gpu_arch == NULL)
        return K_EIO;

    /* 595.84 生成的主机 NVOC 图会将物理 PF 发现变体映射到 PF_KERNEL_ONLY。此操作只会
     * 选择厂商 GB205 HAL，不会加载固件、启动 GSP 或发送 RPC。 */
    create_status = objCreate(&probe_gpu, sys, OBJGPU,
                              gpu_arch->chipArch,
                              gpu_arch->chipImpl,
                              gpu_arch->hidrev,
                              gpu_arch->tegraType,
                              RM_RUNTIME_VARIANT_PF_KERNEL_ONLY,
                              0U,
                              0U,
                              context->state->gpu_id,
                              NULL,
                              gpu_arch);
    if (create_status == NV_OK && probe_gpu != NULL)
        bind_status = gpuBindHalLegacy(probe_gpu,
                                       private_state->pmc_boot_0,
                                       private_state->pmc_boot_42,
                                       0U);

    kernel_serial_printf_serial_only(
        "OS_NVIDIA_PHASE4_GPU_OBJECT create=%x bind=%x obj=%llx hal=%llx impl=%x arch=%x chip=%x\r\n",
        (unsigned)create_status,
        (unsigned)bind_status,
        (unsigned long long)(uintptr_t)probe_gpu,
        probe_gpu != NULL ? (unsigned long long)(uintptr_t)probe_gpu->pHal : 0ULL,
        probe_gpu != NULL ? (unsigned)probe_gpu->halImpl : 0U,
        (unsigned)gpu_arch->chipArch, (unsigned)gpu_arch->chipImpl);

    if (create_status != NV_OK || bind_status != NV_OK || probe_gpu == NULL) {
        if (probe_gpu != NULL)
            objDelete(probe_gpu);
        return K_EIO;
    }

    context->phase4_gpu = probe_gpu;
    kernel_serial_write_serial_only(
        "OS_NVIDIA_PHASE4_GPU_OBJECT_READY\r\n");
    return K_OK;
}

static void nv_rm_vendor_set_result(nv_rm_vendor_context_t *context,
                                    nv_rm_vendor_result_t *result)
{
    nv_priv_t *private_state = (nv_priv_t *)NV_GET_NV_PRIV(context->state);
    /* 第 4 阶段特意报告由 nv_rm_vendor_create_phase4_gpu() 构造的真实发现对象。
     * private_state->pGpu 属于后续适配器路径，可能只是厂商预先挂接的占位对象；它不是刚刚
     * 验证过 PF_KERNEL_ONLY NVOC/HAL 绑定的对象。 */
    OBJGPU *gpu = context != NULL ? context->phase4_gpu : NULL;
    GpuArch *gpu_arch = gpu != NULL ? gpu->pGpuArch : NULL;

    if (result == NULL)
        return;

    *result = (nv_rm_vendor_result_t){0};
    result->context = context;
    result->rm_gpu = gpu;
    result->rm_device = gpu != NULL && gpu->hInternalDevice != 0U
                            ? (void *)(uintptr_t)gpu->hInternalDevice
                            : NULL;

    if (gpu == NULL || gpu_arch == NULL)
        return;

    /* chipInfo 由 gpuPostConstruct 填充，该调用特意留在后续第 5 阶段边界。此处使用的架构
     * 信息来自不可变 HAL 管理器选出的厂商 GpuArch。 */
    result->chip_arch = gpu_arch->chipArch;
    result->chip_impl = gpu_arch->chipImpl;
    result->hal_impl = gpu->halImpl;
    /* GpuArch 保存 PMC_BOOT_42 架构代码（GB200 为 0x1b），而
     * GPU_ARCHITECTURE_BLACKWELL_GB2XX 是单独发布的架构编码（0x1b0）。此边界应使用
     * 不可变厂商 HAL 查找实际返回的值。 */
    result->blackwell = result->chip_arch == NV_PMC_BOOT_42_ARCHITECTURE_GB200;
    result->gb205 = result->chip_impl == GPU_IMPLEMENTATION_GB205;
    /* 私有状态中的 HAL 标志由后续适配器路径设置。第 4 阶段在真实 OBJGPU NVOC/HAL 绑定后
     * 就会停止，因此此处有效的依据是厂商 HAL 对象及其解析出的 GB205 实现。 */
    result->hal_valid = gpu->pHal != NULL && gpu->halImpl == HAL_IMPL_GB205;
    kernel_serial_printf_serial_only(
        "OS_NVIDIA_PHASE4_RESULT gpu=%llx arch=%llx hal=%llx pga=%llx impl=%x flags=%x valid=%u blackwell=%u gb205=%u\r\n",
        (unsigned long long)(uintptr_t)gpu,
        (unsigned long long)(uintptr_t)gpu_arch,
        (unsigned long long)(uintptr_t)gpu->pHal,
        (unsigned long long)(uintptr_t)gpu->pGpuArch,
        (unsigned)gpu->halImpl,
        private_state != NULL ? (unsigned)private_state->flags : 0U,
        result->hal_valid ? 1U : 0U,
        result->blackwell ? 1U : 0U,
        result->gb205 ? 1U : 0U);
}

/* 保存在 RAM 中的一份启动快照，在故障清理后仍会保留。不输出串口日志或逐帧信息：
 * 记录阶段、抢占门控、APIC、IRQ 顶半部/已处理/底半部、待处理底半部及底半部调度失败。 */
static volatile uint32_t g_nv_rm_boot_diagnostic[8];

static bool nv_rm_vendor_init_adapter(nv_rm_vendor_context_t *context)
{
    uint64_t cpu_flags;
    bool ready;

    /* 交互式启动会跳过用户态自检，因此 IF 可能仍为清除状态。安装处理程序和 MSI/MSI-X
     * 表后，RM 的真实中断验证需要开启中断。RM 还会等待信号量和工作线程，因此此处必须处于
     * 可休眠上下文。 */
    g_nv_rm_boot_diagnostic[0] = 1U;
    g_nv_rm_boot_diagnostic[1] = sched_preempt_disabled() ? 1U : 0U;
    g_nv_rm_boot_diagnostic[2] = x86_current_apic_id();
    if (g_nv_rm_boot_diagnostic[1] != 0U)
        return false;
    __asm__ volatile ("pushfq; popq %0" : "=r"(cpu_flags) : : "memory");
    __asm__ volatile ("sti" : : : "memory");
    ready = rm_init_adapter(context->stack, context->state) != NV_FALSE;
    g_nv_rm_boot_diagnostic[3] = atomic_load(&context->irq_top_hits);
    g_nv_rm_boot_diagnostic[4] = atomic_load(&context->irq_rm_handled_hits);
    g_nv_rm_boot_diagnostic[5] = atomic_load(&context->irq_bottom_hits);
    g_nv_rm_boot_diagnostic[6] = atomic_load(&context->irq_bottom_pending);
    g_nv_rm_boot_diagnostic[7] = atomic_load(&context->irq_bottom_schedule_failed);
    g_nv_rm_boot_diagnostic[0] = ready ? 2U : 3U;
    if ((cpu_flags & (1ULL << 9)) == 0U)
        __asm__ volatile ("cli" : : : "memory");
    else
        __asm__ volatile ("sti" : : : "memory");
    return ready;
}

kstatus_t nv_rm_vendor_start(const pci_device_t *device,
                             void **context_out,
                             nv_rm_vendor_result_t *result)
{
    nv_rm_vendor_context_t *context;
    NV_STATUS vendor_status;

    if (context_out == NULL || result == NULL || device == NULL)
        return K_EINVAL;

    *context_out = NULL;
    *result = (nv_rm_vendor_result_t){0};

    context = (nv_rm_vendor_context_t *)kzalloc(sizeof(*context), 0U);
    if (context == NULL)
        return K_ENOMEM;

    context->stack = (nvidia_stack_t *)kzalloc(sizeof(*context->stack), 0U);
    context->state = (nv_state_t *)kzalloc(sizeof(*context->state), 0U);
    context->isr_stack = (nvidia_stack_t *)kzalloc(
        sizeof(*context->isr_stack), 0U);
    context->isr_bh_stack = (nvidia_stack_t *)kzalloc(
        sizeof(*context->isr_bh_stack), 0U);
    atomic_init(&context->irq_bottom_pending, false);
    atomic_init(&context->irq_bottom_queued, false);
    atomic_init(&context->irq_bottom_schedule_failed, false);
    atomic_init(&context->irq_top_hits, 0U);
    atomic_init(&context->irq_rm_handled_hits, 0U);
    atomic_init(&context->irq_bottom_hits, 0U);
    nv_port_mutex_init(&context->kms_rm_lock);
    context->kms_lock_initialized = true;
    atomic_init(&context->kms_gpu_refs, 0U);
    if (context->stack == NULL || context->state == NULL ||
        context->isr_stack == NULL || context->isr_bh_stack == NULL) {
        nv_rm_vendor_cleanup(context);
        return K_ENOMEM;
    }
    context->isr_stack->size = (NvU32)sizeof(context->isr_stack->stack);
    context->isr_stack->top =
        context->isr_stack->stack + context->isr_stack->size;
    context->isr_bh_stack->size =
        (NvU32)sizeof(context->isr_bh_stack->stack);
    context->isr_bh_stack->top =
        context->isr_bh_stack->stack + context->isr_bh_stack->size;

    context->stack->size = (NvU32)sizeof(context->stack->stack);
    context->stack->top = context->stack->stack + context->stack->size;

    if (nv_rm_vendor_fill_pci(device, context) != K_OK) {
        nv_rm_vendor_cleanup(context);
        return K_EIO;
    }
    if (!rm_init_rm(context->stack)) {
        nv_rm_vendor_cleanup(context);
        return K_EIO;
    }
    context->rm_initialized = true;
    /* Linux 前端会在调用 rm_is_supported_device() 前初始化这些设备级 RM 锁。System 不能
     * 调用 nv_lock_init_locks()，因为该包装函数拥有仅供 Linux 使用的 nv_linux_state_t
     * 存储；厂商 RM 事件锁入口是相同生命周期中可移植的部分，可直接操作 nv_state_t。 */
    if (!rm_init_event_locks(context->stack, context->state)) {
        kernel_serial_write_serial_only(
            "OS_NVIDIA_EVENT_LOCKS_FAILED\r\n");
        nv_rm_vendor_cleanup(context);
        return K_EIO;
    }
    context->event_locks_initialized = true;
    vendor_status = rm_is_supported_device(context->stack, context->state);
    if (vendor_status != NV_OK) {
        kernel_serial_printf_serial_only(
            "OS_NVIDIA_RM_UNSUPPORTED status=%x\r\n",
            (unsigned)vendor_status);
        nv_rm_vendor_cleanup(context);
        return K_EIO;
    }
    if (!rm_init_private_state(context->stack, context->state)) {
        nv_rm_vendor_cleanup(context);
        return K_EIO;
    }
    context->private_initialized = true;
    nv_rm_vendor_log_boot_registers(context->state);
    nv_rm_vendor_log_hal_state(context->state);

    if (nv_rm_vendor_create_phase4_gpu(context) != K_OK) {
        kernel_serial_write_serial_only(
            "OS_NVIDIA_PHASE4_GPU_OBJECT_FAIL\r\n");
        nv_rm_vendor_cleanup(context);
        return K_EIO;
    }
    nv_rm_vendor_set_result(context, result);
    if (!result->hal_valid) {
        nv_rm_vendor_cleanup(context);
        *result = (nv_rm_vendor_result_t){0};
        return K_EIO;
    }

    kernel_serial_write_serial_only(
        "OS_NVIDIA_PHASE4_HAL_READY\r\n");
#if OS_NVIDIA_PHASE5
    if (nv_rm_vendor_start_irq(device, context) != K_OK) {
        kernel_serial_write_serial_only("OS_NVIDIA_GSP_IRQ_FAIL\r\n");
        nv_rm_vendor_cleanup(context);
        *result = (nv_rm_vendor_result_t){0};
        return K_EIO;
    }
    kernel_serial_write_serial_only("OS_NVIDIA_GSP_BOOT_START\r\n");
    {
        uint64_t cpu_flags;
        __asm__ volatile ("pushfq; popq %0" : "=r"(cpu_flags));
        kernel_serial_printf_serial_only(
            "OS_NVIDIA_INIT_IRQ_STATE enabled=%u apic=%u\r\n",
            (unsigned)((cpu_flags >> 9) & 1U), (unsigned)x86_current_apic_id());
    }
    if (!nv_rm_vendor_init_adapter(context)) {
        kernel_serial_printf_serial_only(
            "OS_NVIDIA_IRQ_DIAG top=%u handled=%u bottom=%u mode=%s vector=%u\r\n",
            (unsigned)atomic_load_explicit(&context->irq_top_hits,
                                           memory_order_acquire),
            (unsigned)atomic_load_explicit(&context->irq_rm_handled_hits,
                                           memory_order_acquire),
            (unsigned)atomic_load_explicit(&context->irq_bottom_hits,
                                           memory_order_acquire),
            context->irq.use_msix ? "msix" : "msi",
            (unsigned)context->irq.vector);
        kernel_serial_write_serial_only("OS_NVIDIA_GSP_BOOT_FAIL\r\n");
        nv_rm_vendor_cleanup(context);
        *result = (nv_rm_vendor_result_t){0};
        return K_EIO;
    }
    context->adapter_initialized = true;
    /* GSP 已完成初始化，读取其实际显存描述，不使用可调整的 BAR1 窗口。 */
    nv_priv_t *private_state=(nv_priv_t *)NV_GET_NV_PRIV(context->state);
    OBJGPU *active_gpu=private_state ? private_state->pGpu : NULL;
    MemoryManager *memory_manager=active_gpu ? GPU_GET_MEMORY_MANAGER(active_gpu) : NULL;
    if (memory_manager && memory_manager->Ram.fbTotalMemSizeMb <= (UINT64_MAX >> 20U))
        result->vram_bytes=(uint64_t)memory_manager->Ram.fbTotalMemSizeMb << 20U;
    kernel_serial_write_serial_only(
        "OS_NVIDIA_GSP_BOOT_FINISH status=ready\r\n");
#if OS_NVIDIA_PHASE6
    if (nv_rm_vendor_phase6_probe(context->state) != K_OK) {
        kernel_serial_write_serial_only("OS_NVIDIA_PHASE6_FAIL\r\n");
        nv_rm_vendor_cleanup(context);
        *result = (nv_rm_vendor_result_t){0};
        return K_EIO;
    }
#endif
#endif
    *context_out = context;
    return K_OK;
}

void nv_rm_vendor_stop(void *opaque_context)
{
    nv_rm_vendor_cleanup((nv_rm_vendor_context_t *)opaque_context);
}

typedef struct nv_rm_vendor_frontend_file {
    /* 这是 nv_get_file_private 使用的厂商可见前缀。此处特意不导入仅供 Linux 使用的包装函数；
     * RM ABI 在此边界只会使用此文件级状态。 */
    nv_file_private_t vendor_private;
    nv_rm_vendor_context_t *context;
    bool control;
    uint32_t minor;
    struct nv_rm_vendor_frontend_file *control_file;
    NvU32 *attached_gpus;
    NvU32 attached_gpu_words;
    /* 文件生命周期由 System 管理，因此保留厂商映射上下文的结构，但不导入
     * nv_linux_file_private_t。此列表只用于 RM 验证过的 mmap 上下文，不表示 VRAM 或 GPU VA。 */
    nv_alloc_mapping_list_node_t *mapping_list;
    spinlock_t mapping_lock;
    nv_port_event_source_t *event_source;
    struct nv_rm_vendor_frontend_file *event_next;
} nv_rm_vendor_frontend_file_t;

/* 静态零值表示原子自旋锁处于未锁定状态。每次访问都会保存 IRQ 状态，避免回调中断正在
 * 执行注册/移除操作的锁所有者。 */
#define NV_RM_EVENT_BUCKETS 64U
typedef struct nv_rm_event_bucket {
    nv_port_spinlock_t lock;
    nv_rm_vendor_frontend_file_t *files;
} nv_rm_event_bucket_t;
static nv_rm_event_bucket_t g_nv_rm_event_buckets[NV_RM_EVENT_BUCKETS];

static nv_rm_event_bucket_t *nv_rm_vendor_event_bucket(const void *private_data) {
    uintptr_t key = (uintptr_t)private_data;
    /* 忽略分配对齐信息并混合页面位。计算桶索引时绝不能解引用可能已回收的厂商私有指针。 */
    key = (key >> 4) ^ (key >> 12) ^ (key >> 20);
    return &g_nv_rm_event_buckets[key & (NV_RM_EVENT_BUCKETS - 1U)];
}
/* 静默且可由调试器读取的计数器；绝不在 IRQ 回调中格式化日志。 */
static atomic_uint_fast64_t g_nv_rm_event_posts;
static atomic_uint_fast64_t g_nv_rm_event_drops;
static atomic_uint_fast64_t g_nv_rm_event_reports;

static void nv_rm_vendor_event_register(nv_rm_vendor_frontend_file_t *file) {
    nv_rm_event_bucket_t *bucket = nv_rm_vendor_event_bucket(&file->vendor_private);
    uint64_t flags = nv_port_spin_lock_irqsave(&bucket->lock);
    file->event_next = bucket->files;
    bucket->files = file;
    nv_port_spin_unlock_irqrestore(&bucket->lock, flags);
}

static void nv_rm_vendor_event_unregister(nv_rm_vendor_frontend_file_t *file) {
    nv_rm_event_bucket_t *bucket = nv_rm_vendor_event_bucket(&file->vendor_private);
    uint64_t flags = nv_port_spin_lock_irqsave(&bucket->lock);
    nv_rm_vendor_frontend_file_t **link = &bucket->files;
    while (*link != NULL && *link != file) link = &(*link)->event_next;
    if (*link == file) *link = file->event_next;
    file->event_next = NULL;
    nv_port_spin_unlock_irqrestore(&bucket->lock, flags);
}

void NV_API_CALL nv_post_event(nv_event_t *event, NvHandle handle,
                               NvU32 index, NvU32 info32, NvU16 info16,
                               NvBool data_valid) {
    if (event == NULL || event->nvfp == NULL) return;
    nv_port_event_record_t record = {
        .parent = event->hParent, .object = handle, .index = index,
        .info32 = info32, .info16 = info16,
    };
    nv_rm_event_bucket_t *bucket = nv_rm_vendor_event_bucket(event->nvfp);
    uint64_t flags = nv_port_spin_lock_irqsave(&bucket->lock);
    /* 绝不解引用 event->nvfp：它可能已是回收后的地址。在注册保护下重新检查 active，
     * 可拒绝复用后到达的过期事件。 */
    if (__atomic_load_n(&event->active, __ATOMIC_ACQUIRE) != 0) {
        for (nv_rm_vendor_frontend_file_t *file = bucket->files;
             file != NULL; file = file->event_next) {
            if (&file->vendor_private != event->nvfp) continue;
            kstatus_t status = nv_port_event_source_post_irq(file->event_source,
                                                             data_valid ? &record : NULL);
            atomic_fetch_add_explicit(status == K_OK ? &g_nv_rm_event_posts :
                                       &g_nv_rm_event_drops, 1U, memory_order_relaxed);
            break;
        }
    }
    /* 此处不要调用 object_put：在硬中断中释放最后一个源引用可能调用堆析构函数。
     * 移除操作会先等待此受保护回调结束，再在线程上下文中释放所有者。 */
    nv_port_spin_unlock_irqrestore(&bucket->lock, flags);
}

NvS32 NV_API_CALL nv_get_event(nv_file_private_t *private_data,
                              nv_event_t *event, NvU32 *pending) {
    if (pending != NULL) *pending = 0U;
    if (private_data == NULL || event == NULL || pending == NULL)
        return NV_ERR_INVALID_ARGUMENT;
    nv_port_event_record_t record;
    bool more = false;
    kstatus_t status = K_ENOENT;
    nv_port_event_source_t *source = NULL;
    nv_rm_event_bucket_t *bucket = nv_rm_vendor_event_bucket(private_data);
    uint64_t flags = nv_port_spin_lock_irqsave(&bucket->lock);
    for (nv_rm_vendor_frontend_file_t *file = bucket->files;
         file != NULL; file = file->event_next) {
        if (&file->vendor_private != private_data) continue;
        source = file->event_source;
        object_get(nv_port_event_source_wait_object(source));
        break;
    }
    nv_port_spin_unlock_irqrestore(&bucket->lock, flags);
    /* 此 ioctl 在线程上下文中运行。只固定独立拥有的事件源：此时关闭操作可以回收文件，
     * 但队列仍保持有效。释放负载和调用最终析构函数时都不持有注册表锁。 */
    if (source != NULL) {
        status = nv_port_event_source_take(source, &record, &more);
        object_put(nv_port_event_source_wait_object(source));
    }
    if (status != K_OK) return NV_ERR_GENERIC;
    *event = (nv_event_t){0};
    event->hParent = record.parent;
    event->hObject = record.object;
    event->index = record.index;
    event->info32 = record.info32;
    event->info16 = record.info16;
    *pending = more ? 1U : 0U;
    return NV_OK;
}

uint32_t nv_rm_vendor_frontend_poll_state(void *opaque_file,
                                          bool consume_dataless) {
    nv_rm_vendor_frontend_file_t *file = opaque_file;
    uint32_t state = nv_port_event_source_poll(file != NULL ? file->event_source : NULL,
                                               consume_dataless);
    if (consume_dataless && (state & NV_PORT_EVENT_READ) != 0U)
        atomic_fetch_add_explicit(&g_nv_rm_event_reports, 1U, memory_order_relaxed);
    return state;
}

void *nv_rm_vendor_frontend_wait_object(void *opaque_file) {
    nv_rm_vendor_frontend_file_t *file = opaque_file;
    return file != NULL ? nv_port_event_source_wait_object(file->event_source) : NULL;
}

static nvidia_stack_t *nv_rm_vendor_frontend_stack_create(void)
{
    nvidia_stack_t *stack = (nvidia_stack_t *)kzalloc(sizeof(*stack), 0U);
    if (stack == NULL)
        return NULL;
    stack->size = (NvU32)sizeof(stack->stack);
    stack->top = stack->stack + stack->size;
    return stack;
}

static bool nv_rm_vendor_frontend_range_contains(
    NvU64 base, NvU64 size, NvU64 offset, NvU64 length)
{
    if (size == 0U || length == 0U || (offset & (PAGE_SIZE - 1ULL)) != 0U ||
        (length & (PAGE_SIZE - 1ULL)) != 0U || offset < base ||
        offset > UINT64_MAX - length || base > UINT64_MAX - size)
        return false;
    return offset + length <= base + size;
}

static bool nv_rm_vendor_frontend_memarea_size(
    const MemoryArea *area, uint64_t *size_out)
{
    uint64_t total = 0U;

    if (area == NULL || size_out == NULL || area->pRanges == NULL ||
        area->numRanges == 0U)
        return false;
    for (NvU64 index = 0U; index < area->numRanges; ++index) {
        uint64_t start = (uint64_t)area->pRanges[index].start;
        uint64_t size = (uint64_t)area->pRanges[index].size;
        if (size == 0U || (start & (PAGE_SIZE - 1ULL)) != 0U ||
            (size & (PAGE_SIZE - 1ULL)) != 0U ||
            start > UINT64_MAX - size || total > UINT64_MAX - size)
            return false;
        total += size;
    }
    if (total == 0U) return false;
    *size_out = total;
    return true;
}

static uint32_t nv_rm_vendor_frontend_cache_mode(NvU32 caching)
{
    if (caching == NV_MEMORY_CACHED || caching == NV_MEMORY_WRITEBACK)
        return X86_CACHE_WB;
    if (caching == NV_MEMORY_WRITECOMBINED)
        return X86_CACHE_WC;
    return X86_CACHE_UC;
}

static void nv_rm_vendor_frontend_security(
    nv_rm_vendor_frontend_file_t *file, API_SECURITY_INFO *sec_info)
{
    *sec_info = (API_SECURITY_INFO){0};
    sec_info->privLevel = RS_PRIV_LEVEL_USER_ROOT;
    sec_info->paramLocation = PARAM_LOCATION_USER;
    sec_info->clientOSInfo = file->vendor_private.ctl_nvfp != NULL ?
        file->vendor_private.ctl_nvfp : &file->vendor_private;
}

/*
 * 真实 RM 验证 CPU 映射后，595.84 的 osapi.c 会调用此函数。Linux 实现会将节点保存在
 * nv_linux_file_private_t 中。System 特意不引入该包装类型；改由 System 前端对象遵循相同的
 * 所有权和重复项拒绝规则。
 */
NV_STATUS NV_API_CALL nv_add_mapping_context_to_file(
    nv_state_t *nv,
    nv_usermap_access_params_t *nvuap,
    NvU32 prot,
    void *pAllocPriv,
    NvU64 pageIndex,
    NvU32 fd)
{
    NvBool control_device;
    nv_file_private_t *vendor_private;
    void *os_private = NULL;
    nv_rm_vendor_frontend_file_t *file;
    nv_alloc_mapping_list_node_t *node;
    NV_STATUS status = NV_OK;

    if (nv == NULL || nvuap == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    /* 对系统内存分配，RmCreateMmapContextLocked 会将 pNv 切换为 nv_get_ctl_state()，并传入
     * 已解析的分配私有对象。System 在控制文件和实际设备文件之间共用活动 nv_state_t，因此
     * pAllocPriv 是此处的权威判别依据；RM 控制标志仍适用于厂商显式指定的控制路径。 */
    control_device = ((nv->flags & NV_FLAG_CONTROL) != 0 ||
                      pAllocPriv != NULL) ? NV_TRUE : NV_FALSE;
    vendor_private = nv_get_file_private((NvS32)fd, control_device,
                                          &os_private);
    if (vendor_private == NULL || os_private == NULL) {
        if (os_private != NULL)
            nv_put_file_private(os_private);
        return NV_ERR_INVALID_ARGUMENT;
    }

    /* 根据约定，vendor_private 是首个成员，因此此处转换与
     * nv_frontend_file_vendor_private() 互为逆操作。 */
    file = (nv_rm_vendor_frontend_file_t *)vendor_private;
    if (file->context == NULL || file->context->state == NULL ||
        (control_device == NV_FALSE && file->context->state != nv)) {
        status = NV_ERR_INVALID_ARGUMENT;
        goto done;
    }

    node = (nv_alloc_mapping_list_node_t *)kzalloc(sizeof(*node), 0U);
    if (node == NULL) {
        status = NV_ERR_NO_MEMORY;
        goto done;
    }
    __builtin_memset(node, 0, sizeof(*node));

    if (control_device != NV_FALSE) {
        /* 控制设备映射属于 RM 系统内存分配。完全按照不可变实现保留厂商私有分配句柄和页面索引；
         * 此处不引入 System 内存管理器作为替代。 */
        if (pAllocPriv == NULL) {
            kfree(node);
            status = NV_ERR_INVALID_ARGUMENT;
            goto done;
        }
        node->context.alloc = pAllocPriv;
        node->context.page_index = pageIndex;
    } else {
        /* 实际设备映射携带经 RM 验证的显存窗口范围。 */
        if (nvuap->memArea.numRanges == 0U ||
            nvuap->memArea.pRanges == NULL ||
            nvuap->memArea.numRanges >
                (NvU64)(SIZE_MAX / sizeof(MemoryRange))) {
            kfree(node);
            status = NV_ERR_INVALID_ARGUMENT;
            goto done;
        }
        node->context.memArea.pRanges = (MemoryRange *)kzalloc(
            (size_t)nvuap->memArea.numRanges * sizeof(MemoryRange), 0U);
        if (node->context.memArea.pRanges == NULL) {
            kfree(node);
            status = NV_ERR_NO_MEMORY;
            goto done;
        }
        node->context.memArea.numRanges = nvuap->memArea.numRanges;
        __builtin_memcpy(node->context.memArea.pRanges,
                         nvuap->memArea.pRanges,
                         (size_t)nvuap->memArea.numRanges *
                             sizeof(MemoryRange));

        /* Linux 驱动会在 NUMA 后备映射中转移页面数组。System 当前没有 NUMA 在线状态，
         * 但如果 RM 提供该数组，仍需复制它，以保持上下文自包含。 */
        if (nvuap->page_array != NULL && nvuap->num_pages != 0U) {
            if (nvuap->num_pages >
                (NvU64)(SIZE_MAX / sizeof(NvU64))) {
                kfree(node->context.memArea.pRanges);
                kfree(node);
                status = NV_ERR_INVALID_ARGUMENT;
                goto done;
            }
            node->context.page_array = (NvU64 *)kzalloc(
                (size_t)nvuap->num_pages * sizeof(NvU64), 0U);
            if (node->context.page_array == NULL) {
                kfree(node->context.memArea.pRanges);
                kfree(node);
                status = NV_ERR_NO_MEMORY;
                goto done;
            }
            node->context.num_pages = nvuap->num_pages;
            __builtin_memcpy(node->context.page_array, nvuap->page_array,
                             (size_t)nvuap->num_pages * sizeof(NvU64));
        }
        node->context.access_start = nvuap->access_start;
        node->context.access_size = nvuap->access_size;
    }

    node->context.prot = prot;
    node->context.valid = NV_TRUE;
    node->context.caching = nvuap->caching;
    if (nvuap->memArea.numRanges != 0U && nvuap->memArea.pRanges != NULL)
        node->fileRange = nvuap->memArea.pRanges[0];

    spinlock_lock(&file->mapping_lock);
    if (file->mapping_list != NULL) {
        spinlock_unlock(&file->mapping_lock);
        if (node->context.page_array != NULL)
            kfree(node->context.page_array);
        if (node->context.memArea.pRanges != NULL)
            kfree(node->context.memArea.pRanges);
        kfree(node);
        status = NV_ERR_STATE_IN_USE;
        goto done;
    }
    file->mapping_list = node;
    spinlock_unlock(&file->mapping_lock);

done:
    nv_put_file_private(os_private);
    return status;
}

kstatus_t nv_rm_vendor_frontend_open(bool control, uint32_t minor,
                                     void **file_out)
{
    const nv_gpu_device_t *gpu = nv_rm_driver_current();
    nv_rm_vendor_frontend_file_t *file;
    nv_rm_vendor_context_t *context;

    if (file_out == NULL)
        return K_EINVAL;
    *file_out = NULL;
    if (gpu == NULL || gpu->state != NV_GPU_GSP_READY ||
        gpu->vendor_context == NULL)
        return K_ENOENT;
    if (!control && (minor != 0U || gpu->bar0_size == 0U))
        return K_ENOENT;

    context = (nv_rm_vendor_context_t *)gpu->vendor_context;
    if (context->state == NULL || context->stack == NULL)
        return K_EIO;
    file = (nv_rm_vendor_frontend_file_t *)kzalloc(sizeof(*file), 0U);
    if (file == NULL)
        return K_ENOMEM;
    file->context = context;
    file->control = control;
    file->minor = minor;
    file->vendor_private.deviceInstance = minor;
    file->vendor_private.gpuInstanceId = context->state->gpu_id;
    file->attached_gpus = NULL;
    file->attached_gpu_words = 0U;
    file->mapping_list = NULL;
    spinlock_init(&file->mapping_lock);
    file->event_source = nv_port_event_source_create();
    if (file->event_source == NULL) {
        kfree(file);
        return K_ENOMEM;
    }
    nv_rm_vendor_event_register(file);
    *file_out = file;
    return K_OK;
}

kstatus_t nv_rm_vendor_frontend_abi_private(void *opaque_file,
                                            void **private_out)
{
    nv_rm_vendor_frontend_file_t *file =
        (nv_rm_vendor_frontend_file_t *)opaque_file;

    if (private_out == NULL)
        return K_EINVAL;
    *private_out = NULL;
    if (file == NULL || file->context == NULL || file->context->state == NULL)
        return K_EBADF;
    *private_out = &file->vendor_private;
    return K_OK;
}

bool nv_rm_vendor_frontend_is_control(void *opaque_file)
{
    nv_rm_vendor_frontend_file_t *file =
        (nv_rm_vendor_frontend_file_t *)opaque_file;
    return file != NULL && file->control;
}

void *nv_rm_vendor_ctl_state(void)
{
    const nv_gpu_device_t *gpu = nv_rm_driver_current();
    nv_rm_vendor_context_t *context;

    /* 这相当于厂商 Linux 控制设备上的 NV_STATE_PTR(&nv_ctl_device)。只有真实 RM/GSP 实例
     * 已发布为就绪状态时才返回该状态；设备未运行时，调用方必须安全失败。 */
    if (gpu == NULL || gpu->state != NV_GPU_GSP_READY ||
        gpu->vendor_context == NULL)
        return NULL;
    context = (nv_rm_vendor_context_t *)gpu->vendor_context;
    if (context->state == NULL)
        return NULL;
    return context->state;
}

kstatus_t nv_rm_vendor_frontend_register_fd(void *opaque_file,
                                            void *opaque_control_file)
{
    nv_rm_vendor_frontend_file_t *file =
        (nv_rm_vendor_frontend_file_t *)opaque_file;
    nv_rm_vendor_frontend_file_t *control_file =
        (nv_rm_vendor_frontend_file_t *)opaque_control_file;

    /* 遵循不可变 Linux 前端的关联规则：目标必须是控制节点，两个句柄必须属于同一个 RM
     * 实例，且一个文件只能关联一个控制节点。 */
    if (file == NULL || control_file == NULL || file == control_file ||
        file->context == NULL || file->context != control_file->context ||
        !control_file->control || file->control_file != NULL ||
        control_file->control_file != NULL)
        return K_EINVAL;
    file->control_file = control_file;
    file->vendor_private.ctl_nvfp = &control_file->vendor_private;
    file->vendor_private.register_or_refcount = NVFP_TYPE_REGISTERED;
    return K_OK;
}

static void nv_rm_vendor_frontend_import_host_memory(
    nv_rm_vendor_frontend_file_t *file, NVOS32_PARAMETERS *heap)
{
    /* escape.c 中的 RmCreateOsDescriptor 会在调用核心堆函数前固定用户内存。直接向核心传递原始
     * 虚拟地址并不构成 OS_PAGE_ARRAY 描述符，因此核心会正确地拒绝它。 */
    NvP64 original = heap->data.AllocOsDesc.descriptor;
    NvU32 original_type = heap->data.AllocOsDesc.descriptorType;
    NvU64 limit = heap->data.AllocOsDesc.limit;
    if (original == 0U) {
        heap->status = NV_ERR_INVALID_ADDRESS;
        return;
    }
    if (original_type != NVOS32_DESCRIPTOR_TYPE_VIRTUAL_ADDRESS ||
        ((NvUPtr)NvP64_VALUE(original) & (PAGE_SIZE - 1ULL)) != 0U) {
        heap->status = NV_ERR_NOT_SUPPORTED;
        return;
    }
    if (limit == UINT64_MAX) {
        heap->status = NV_ERR_INVALID_LIMIT;
        return;
    }
    NvU64 pages = 1ULL + limit / PAGE_SIZE;
    NvU32 flags = FLD_TEST_DRF(OS32, _ATTR2, _PROTECTION_USER,
                              _READ_WRITE, heap->data.AllocOsDesc.attr2) ? 1U : 0U;
    void *page_array = NULL;
    NV_STATUS status = os_lock_user_pages(NvP64_VALUE(original), pages,
                                           &page_array, flags);
    if (status != NV_OK) {
        heap->status = status;
        return;
    }
    if (page_array == NULL) {
        heap->status = NV_ERR_INVALID_ADDRESS;
        return;
    }
    heap->data.AllocOsDesc.descriptor = (NvP64)(NvUPtr)page_array;
    heap->data.AllocOsDesc.descriptorType = NVOS32_DESCRIPTOR_TYPE_OS_PAGE_ARRAY;
    API_SECURITY_INFO security;
    nv_rm_vendor_frontend_security(file, &security);
    Nv04VidHeapControlWithSecInfo(heap, security);
    if (heap->status != NV_OK)
        (void)os_unlock_user_pages(pages, page_array, flags);
    /* 不要在用户数据包中暴露内核页面数组指针。成功后，RM 会持有该固定负载，直到资源销毁。 */
    heap->data.AllocOsDesc.descriptor = original;
    heap->data.AllocOsDesc.descriptorType = original_type;
}

kstatus_t nv_rm_vendor_frontend_ioctl(void *opaque_file, uint32_t request,
                                      void *argument, uint32_t argument_size)
{
    nv_rm_vendor_frontend_file_t *file =
        (nv_rm_vendor_frontend_file_t *)opaque_file;
    nv_rm_vendor_context_t *context;
    nv_state_t *state;
    uint32_t number;
    uint32_t encoded_size;

    if (file == NULL || file->context == NULL || argument == NULL)
        return K_EBADF;
    context = file->context;
    state = context->state;
    if (state == NULL)
        return K_EIO;
    if (((request >> NV_FRONTEND_IOC_TYPESHIFT) & 0xffU) !=
        (uint32_t)(uint8_t)NV_FRONTEND_IOCTL_MAGIC)
        return K_ENOSYS;
    number = NV_FRONTEND_IOCTL_NR(request);
    encoded_size = NV_FRONTEND_IOCTL_SIZE(request);
    if (encoded_size != argument_size)
        return K_EINVAL;

    switch (number) {
    case NV_ESC_RM_GET_EVENT_DATA:
    case NV_ESC_ALLOC_OS_EVENT:
    case NV_ESC_FREE_OS_EVENT: {
        /* 不可变的 RM OS 层负责管理事件列表、客户端关联、活动/引用状态和校验。若未执行该实现，
         * 不得伪造事件句柄或报告成功。 */
        uint32_t expected_size = number == NV_ESC_RM_GET_EVENT_DATA ?
            sizeof(NVOS41_PARAMETERS) : number == NV_ESC_ALLOC_OS_EVENT ?
            sizeof(nv_ioctl_alloc_os_event_t) : sizeof(nv_ioctl_free_os_event_t);
        /* 595.84 也允许在实际设备 fd 上创建 OS 事件；Vulkan 会创建已注册的 nvidia0 fd，
         * 用于队列事件通知。 */
        if (argument_size != expected_size)
            return K_EINVAL;
        nvidia_stack_t *stack = nv_rm_vendor_frontend_stack_create();
        if (stack == NULL) return K_ENOMEM;
        NV_STATUS vendor_status = rm_ioctl(stack, state, &file->vendor_private,
                                            number, argument, argument_size);
        kfree(stack);
        return vendor_status == NV_OK ? K_OK : K_EIO;
    }
    case NV_ESC_CHECK_VERSION_STR: {
        nvidia_stack_t *stack;
        NV_STATUS status;
        if (!file->control || argument_size != sizeof(nv_ioctl_rm_api_version_t))
            return K_EINVAL;
        stack = nv_rm_vendor_frontend_stack_create();
        if (stack == NULL)
            return K_ENOMEM;
        status = rm_perform_version_check(stack, argument, argument_size);
        kfree(stack);
        return status == NV_OK ? K_OK : K_EINVAL;
    }
    case NV_ESC_CARD_INFO: {
        nv_ioctl_card_info_t *info = (nv_ioctl_card_info_t *)argument;
        const nv_aperture_t *regs = state->regs;
        const nv_aperture_t *fb = state->fb;
        size_t count;
        if (!file->control || argument_size < sizeof(*info) ||
            argument_size % sizeof(*info) != 0U)
            return K_EINVAL;
        count = argument_size / sizeof(*info);
        __builtin_memset(argument, 0, argument_size);
        if (count == 0U)
            return K_EINVAL;
        info->valid = NV_TRUE;
        info->pci_info.domain = state->pci_info.domain;
        info->pci_info.bus = state->pci_info.bus;
        info->pci_info.slot = state->pci_info.slot;
        info->pci_info.function = state->pci_info.function;
        info->pci_info.vendor_id = state->pci_info.vendor_id;
        info->pci_info.device_id = state->pci_info.device_id;
        info->gpu_id = state->gpu_id;
        info->interrupt_line = state->interrupt_line;
        info->reg_address = regs != NULL ? regs->cpu_address : 0U;
        info->reg_size = regs != NULL ? regs->size : 0U;
        info->fb_address = fb != NULL ? fb->cpu_address : 0U;
        info->fb_size = fb != NULL ? fb->size : 0U;
        info->minor_number = file->minor;
        return info->reg_address != 0U && info->reg_size != 0U ?
               K_OK : K_EIO;
    }
    case NV_ESC_QUERY_DEVICE_INTR: {
        nv_ioctl_query_device_intr_t *query =
            (nv_ioctl_query_device_intr_t *)argument;
        const nv_gpu_device_t *gpu = nv_rm_driver_current();
        volatile NvU32 *registers = NULL;
        if (file->control || file->minor != 0U ||
            argument_size != sizeof(*query) || gpu == NULL)
            return K_EINVAL;
        if (state->regs != NULL && state->regs->map != NULL)
            registers = (volatile NvU32 *)(uintptr_t)state->regs->map;
        else if (gpu->bar0_va != NULL)
            registers = (volatile NvU32 *)gpu->bar0_va;
        if (registers == NULL)
            return K_EIO;
        query->intrStatus = registers[NV_RM_DEVICE_INTR_ADDRESS >> 2U];
        query->status = NV_OK;
        return K_OK;
    }
    case NV_ESC_WAIT_OPEN_COMPLETE: {
        nv_ioctl_wait_open_complete_t *wait =
            (nv_ioctl_wait_open_complete_t *)argument;
        if (argument_size != sizeof(*wait))
            return K_EINVAL;
        wait->rc = 0;
        wait->adapterStatus = 0U;
        return K_OK;
    }
    case NV_FRONTEND_ESC_RM_FREE: {
        NVOS00_PARAMETERS *release = (NVOS00_PARAMETERS *)argument;
        if (!file->control || argument_size != sizeof(*release))
            return K_EINVAL;

        /* 这是 595.84 的真实 NV04_FREE 入口。厂商 RM 负责校验句柄、加锁和销毁对象；前端只提供
         * Linux 用户参数安全上下文。 */
        {
            API_SECURITY_INFO sec_info = {0};
            sec_info.privLevel = RS_PRIV_LEVEL_USER_ROOT;
            sec_info.paramLocation = PARAM_LOCATION_USER;
            sec_info.clientOSInfo = &file->vendor_private;
            Nv01FreeWithSecInfo(release, sec_info);
        }
        return K_OK;
    }
    case NV_ESC_RM_ALLOC_OBJECT: {
        NVOS05_PARAMETERS *allocation = (NVOS05_PARAMETERS *)argument;
        if (!file->control || argument_size != sizeof(*allocation))
            return K_EINVAL;

        /* 保留 595.84 原有的 NV01_ALLOC_OBJECT 路径。资源类校验、句柄分配、加锁及所需的 GSP
         * 操作仍由不可变 RM 实现负责。 */
        {
            API_SECURITY_INFO sec_info;
            nv_rm_vendor_frontend_security(file, &sec_info);
            Nv01AllocObjectWithSecInfo(allocation, sec_info);
        }
        return K_OK;
    }
    case NV_FRONTEND_ESC_RM_CONTROL: {
        NVOS54_PARAMETERS *control = (NVOS54_PARAMETERS *)argument;
        if (!file->control || argument_size != sizeof(*control))
            return K_EINVAL;

        /* 这是 595.84 的真实 NV04_CONTROL 入口。不可变实现会复制并校验指向的 RM 参数、获取
         * 所需 RM 锁，并执行必要的 GSP 事务。 */
        {
            API_SECURITY_INFO sec_info = {0};
            nv_rm_vendor_frontend_security(file, &sec_info);
            Nv04ControlWithSecInfo(control, sec_info);
        }
        return K_OK;
    }
    case NV_ESC_RM_ALLOC_MEMORY: {
        nv_ioctl_nvos02_parameters_with_fd *api =
            (nv_ioctl_nvos02_parameters_with_fd *)argument;
        NVOS02_PARAMETERS *allocation = &api->params;

        /* 在 595.84 中，NV_ESC_RM_ALLOC_MEMORY 是实际设备 ioctl。fd 属于不可变包装层的一部分，
         * 因为 RM 会针对该 Linux 文件注册生成的系统内存映射。 */
        if (file->control || file->minor != 0U ||
            argument_size != sizeof(*api))
            return K_EINVAL;

        {
            API_SECURITY_INFO sec_info;
            NvU32 flags = allocation->flags;
            nv_rm_vendor_frontend_security(file, &sec_info);
            Nv01AllocMemoryWithSecInfo(allocation, sec_info);
            /* 遵循 escape.c：立即系统内存分配也要在给定的 mmap fd 上注册上下文。缺少这一步，
             * 分配虽然成功，后续控制节点 mmap 仍会被拒绝。 */
            if (allocation->hClass == NV01_MEMORY_SYSTEM &&
                !FLD_TEST_DRF(OS02, _FLAGS, _ALLOC, _NONE, flags) &&
                !FLD_TEST_DRF(OS02, _FLAGS, _MAPPING, _NO_MAP, flags) &&
                allocation->status == NV_OK) {
                if (rm_create_mmap_context(allocation->hRoot,
                        allocation->hObjectParent, allocation->hObjectNew,
                        allocation->pMemory, allocation->limit + 1U, 0U,
                        NV_MEMORY_DEFAULT, (NvU32)api->fd) != NV_OK)
                    return K_EINVAL;
            }
        }
        return K_OK;
    }
    case NV_ESC_RM_ALLOC_CONTEXT_DMA2: {
        NVOS39_PARAMETERS *allocation = (NVOS39_PARAMETERS *)argument;
        if (!file->control || argument_size != sizeof(*allocation))
            return K_EINVAL;

        {
            API_SECURITY_INFO sec_info;
            nv_rm_vendor_frontend_security(file, &sec_info);
            Nv04AllocContextDmaWithSecInfo(allocation, sec_info);
        }
        return K_OK;
    }
    case NV_ESC_RM_BIND_CONTEXT_DMA: {
        NVOS49_PARAMETERS *binding = (NVOS49_PARAMETERS *)argument;
        if (!file->control || argument_size != sizeof(*binding))
            return K_EINVAL;

        {
            API_SECURITY_INFO sec_info;
            nv_rm_vendor_frontend_security(file, &sec_info);
            Nv04BindContextDmaWithSecInfo(binding, sec_info);
        }
        return K_OK;
    }
    case NV_ESC_RM_MAP_MEMORY_DMA: {
        NVOS46_PARAMETERS *mapping = (NVOS46_PARAMETERS *)argument;
        if (!file->control || argument_size != sizeof(*mapping))
            return K_EINVAL;

        {
            API_SECURITY_INFO sec_info;
            nv_rm_vendor_frontend_security(file, &sec_info);
            Nv04MapMemoryDmaWithSecInfo(mapping, sec_info);
        }
        return K_OK;
    }
    case NV_ESC_RM_UNMAP_MEMORY_DMA: {
        NVOS47_PARAMETERS *mapping = (NVOS47_PARAMETERS *)argument;
        if (!file->control || argument_size != sizeof(*mapping))
            return K_EINVAL;

        {
            API_SECURITY_INFO sec_info;
            nv_rm_vendor_frontend_security(file, &sec_info);
            Nv04UnmapMemoryDmaWithSecInfo(mapping, sec_info);
        }
        return K_OK;
    }
    case NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO: {
        NVOS56_PARAMETERS *update = (NVOS56_PARAMETERS *)argument;
        if (!file->control || argument_size != sizeof(*update))
            return K_EINVAL;

        /* 此扩展入口特意调用与不可变 595.84 escape.c 相同的 OS 接口函数。它会更新 RM 自有的
         * 映射记录；此处不会生成 System 映射、VRAM、GPU VA 或 GMMU 状态。 */
        update->status = rm_update_device_mapping_info(
            update->hClient, update->hDevice, update->hMemory,
            NvP64_VALUE(update->pOldCpuAddress),
            NvP64_VALUE(update->pNewCpuAddress));
        return K_OK;
    }
    case NV_ESC_RM_IDLE_CHANNELS: {
        NVOS30_PARAMETERS *idle = (NVOS30_PARAMETERS *)argument;

        /* 不可变的 escape.c 路径仅适用于控制设备，并通过 NVOS30_PARAMETERS.status 报告操作错误。 */
        if (!file->control || argument_size != sizeof(*idle))
            return K_EINVAL;

        {
            API_SECURITY_INFO sec_info;
            nv_rm_vendor_frontend_security(file, &sec_info);
            Nv04IdleChannelsWithSecInfo(idle, sec_info);
        }
        return K_OK;
    }
    case NV_ESC_RM_MAP_MEMORY: {
        nv_ioctl_nvos33_parameters_with_fd *api =
            (nv_ioctl_nvos33_parameters_with_fd *)argument;
        NVOS33_PARAMETERS *mapping = &api->params;

        /* 保持 595.84 escape.c 的操作顺序。RM 负责校验内存对象并建立 CPU 映射；fd 包装层只用于
         * 将生成的 mmap 上下文关联到请求操作的实际文件。 */
        if (!file->control || argument_size != sizeof(*api))
            return K_EINVAL;

        mapping->flags = FLD_SET_DRF(OS33, _FLAGS, _CACHING_TYPE, _DEFAULT,
                                     mapping->flags);
        {
            API_SECURITY_INFO sec_info;
            nv_rm_vendor_frontend_security(file, &sec_info);
            Nv04MapMemoryWithSecInfo(mapping, sec_info);
            if (mapping->status == NV_OK) {
                mapping->status = rm_create_mmap_context(
                    mapping->hClient, mapping->hDevice, mapping->hMemory,
                    mapping->pLinearAddress, mapping->length,
                    mapping->offset,
                    DRF_VAL(OS33, _FLAGS, _CACHING_TYPE, mapping->flags),
                    (NvU32)api->fd);
                if (mapping->status != NV_OK) {
                    NVOS34_PARAMETERS unmap = {0};
                    unmap.hClient = mapping->hClient;
                    unmap.hDevice = mapping->hDevice;
                    unmap.hMemory = mapping->hMemory;
                    unmap.pLinearAddress = mapping->pLinearAddress;
                    unmap.flags = mapping->flags;
                    Nv04UnmapMemoryWithSecInfo(&unmap, sec_info);
                }
            }
        }
        return K_OK;
    }
    case NV_ESC_RM_UNMAP_MEMORY: {
        NVOS34_PARAMETERS *unmap = (NVOS34_PARAMETERS *)argument;
        if (!file->control || argument_size != sizeof(*unmap))
            return K_EINVAL;

        {
            API_SECURITY_INFO sec_info;
            nv_rm_vendor_frontend_security(file, &sec_info);
            Nv04UnmapMemoryWithSecInfo(unmap, sec_info);
        }
        return K_OK;
    }
    case NV_FRONTEND_ESC_RM_ALLOC: {
        NVOS64_PARAMETERS *allocation = (NVOS64_PARAMETERS *)argument;
        if (argument_size != sizeof(*allocation))
            return K_EINVAL;
        /* 遵循 595.84 escape.c：事件类对象可以在实际 GPU fd 上分配。其他资源（包括根客户端）
         * 仍只能通过控制节点操作。RM 仍会验证客户端、父对象和事件参数。 */
        if (!file->control &&
            allocation->hClass != NV01_EVENT &&
            allocation->hClass != NV01_EVENT_OS_EVENT &&
            allocation->hClass != NV01_EVENT_KERNEL_CALLBACK &&
            allocation->hClass != NV01_EVENT_KERNEL_CALLBACK_EX)
            return K_EINVAL;

        /* 负载为 48 字节的 NV_ESC_RM_ALLOC 是厂商 NV04_ALLOC_WITH_ACCESS 入口。P3 厂商闭包保留了
         * WithSecInfo 导出符号，因此应使用不可变 595.84 RM API，并附带 Linux 用户参数安全上下文。
         * 厂商实现负责复制指针内容、校验资源、加锁及执行所需的固件交互。 */
        {
            API_SECURITY_INFO sec_info;
            nv_rm_vendor_frontend_security(file, &sec_info);
            Nv04AllocWithAccessSecInfo(allocation, sec_info);
        }
        return K_OK;
    }
    case NV_ESC_RM_VID_HEAP_CONTROL: {
        NVOS32_PARAMETERS *heap = (NVOS32_PARAMETERS *)argument;
        if (!file->control || argument_size != sizeof(*heap))
            return K_EINVAL;

        /* 旧版堆 ABI 仍由厂商 RM 负责，包括厂商的 OS 描述符路径。System 不会重新实现其内存管理器，
         * 也不会转换 RM 的堆/GPU 地址操作。 */
        {
            API_SECURITY_INFO sec_info;
            nv_rm_vendor_frontend_security(file, &sec_info);
            if (heap->function == NVOS32_FUNCTION_ALLOC_OS_DESCRIPTOR)
                nv_rm_vendor_frontend_import_host_memory(file, heap);
            else
                Nv04VidHeapControlWithSecInfo(heap, sec_info);
        }
        return K_OK;
    }
    case NV_ESC_RM_DUP_OBJECT: {
        NVOS55_PARAMETERS *duplicate = (NVOS55_PARAMETERS *)argument;
        if (!file->control || argument_size != sizeof(*duplicate))
            return K_EINVAL;

        /* 对象复制属于真实的 RM 资源操作。应由不可变的 595.84 实现校验句柄、权限和锁，而不是
         * 伪造目标句柄。 */
        {
            API_SECURITY_INFO sec_info;
            nv_rm_vendor_frontend_security(file, &sec_info);
            Nv04DupObjectWithSecInfo(duplicate, sec_info);
        }
        return K_OK;
    }
    case NV_ESC_ATTACH_GPUS_TO_FD: {
        NvU32 *gpu_ids;
        NvU32 gpu_words;
        NvU32 attached = 0U;
        nvidia_stack_t *stack;
        NV_STATUS vendor_status = NV_OK;

        /* 不可变的 Linux 前端只允许在控制节点上提交非空 NvU32 数组，并且每个文件只能安装一次
         * 此关联。 */
        if (!file->control || argument_size == 0U ||
            argument_size % sizeof(NvU32) != 0U ||
            file->attached_gpus != NULL)
            return K_EINVAL;
        gpu_words = argument_size / sizeof(NvU32);
        gpu_ids = (NvU32 *)kzalloc(argument_size, 0U);
        if (gpu_ids == NULL)
            return K_ENOMEM;
        __builtin_memcpy(gpu_ids, argument, argument_size);

        /* Linux 层的 nvidia_dev_get() 会解析每个非零 ID，并递增 RM 外部客户端计数。System
         * 只发布一块 PCIe GPU，因此需使用真实 RM 标识校验每个非零条目，并对每个条目调用保留的
         * RM 操作。 */
        stack = nv_rm_vendor_frontend_stack_create();
        if (stack == NULL) {
            kfree(gpu_ids);
            return K_ENOMEM;
        }
        for (NvU32 index = 0U; index < gpu_words; ++index) {
            if (gpu_ids[index] == 0U)
                continue;
            if (gpu_ids[index] != state->gpu_id) {
                vendor_status = NV_ERR_OBJECT_NOT_FOUND;
                break;
            }
            vendor_status = rm_set_external_kernel_client_count(
                stack, state, NV_TRUE);
            if (vendor_status != NV_OK)
                break;
            ++attached;
        }
        kfree(stack);
        if (vendor_status != NV_OK) {
            if (attached != 0U) {
                stack = nv_rm_vendor_frontend_stack_create();
                if (stack != NULL) {
                    for (NvU32 index = 0U; index < gpu_words; ++index) {
                        if (gpu_ids[index] == state->gpu_id && attached != 0U) {
                            (void)rm_set_external_kernel_client_count(
                                stack, state, NV_FALSE);
                            --attached;
                        }
                    }
                    kfree(stack);
                }
            }
            kfree(gpu_ids);
            return K_EINVAL;
        }
        file->attached_gpus = gpu_ids;
        file->attached_gpu_words = gpu_words;
        return K_OK;
    }
    case NV_ESC_NUMA_INFO: {
        nv_ioctl_numa_info_t *numa_info =
            (nv_ioctl_numa_info_t *)argument;
        nvidia_stack_t *stack;
        NV_STATUS vendor_status;

        /* NUMA_INFO 用于查询实际设备。System 没有 Linux NUMA 内存热插拔层，因此适配器会报告
         * RM 返回的真实 NUMA 位置，同时如实保持宿主 NUMA 状态为禁用。 */
        if (file->control || file->minor != 0U ||
            argument_size != sizeof(*numa_info) ||
            numa_info->offline_addresses.numEntries >
                NV_IOCTL_NUMA_INFO_MAX_OFFLINE_ADDRESSES)
            return K_EINVAL;

        numa_info->status = NV_IOCTL_NUMA_STATUS_DISABLED;
        numa_info->memblock_size = 0U;
        numa_info->use_auto_online = 0U;

        stack = nv_rm_vendor_frontend_stack_create();
        if (stack == NULL)
            return K_ENOMEM;
        vendor_status = rm_get_gpu_numa_info(stack, state, numa_info);
        kfree(stack);
        return vendor_status == NV_OK ? K_OK : K_EBUSY;
    }
    default:
        /* P8 特意只公开由真实 RM 或经验证的 PCI 寄存器窗口支持的操作。后续用户态阶段尚未实现时，
         * 不支持的 RM ioctl 不能伪装成成功。 */
        return K_ENOSYS;
    }
}

kstatus_t nv_rm_vendor_frontend_mmap(void *opaque_file, uint64_t offset,
                                     uint64_t length, uint32_t protection,
                                     nv_rm_vendor_frontend_mapping_t *mapping)
{
    nv_rm_vendor_frontend_file_t *file =
        (nv_rm_vendor_frontend_file_t *)opaque_file;
    nv_alloc_mapping_context_t context = {0};
    bool has_context = false;
    const nv_aperture_t *regs;

    if (file == NULL || file->context == NULL || mapping == NULL)
        return K_EBADF;
    *mapping = (nv_rm_vendor_frontend_mapping_t){0};
    if (file->context->state == NULL)
        return K_EINVAL;
    if ((protection & VM_PROT_USER) == 0U ||
        (protection & VM_PROT_EXEC) != 0U ||
        (protection & (VM_PROT_READ | VM_PROT_WRITE)) == 0U)
        return K_EACCES;

    spinlock_lock(&file->mapping_lock);
    if (file->mapping_list != NULL) {
        context = file->mapping_list->context;
        has_context = context.valid != NV_FALSE;
    }
    spinlock_unlock(&file->mapping_lock);

    if (file->control) {
        const uint64_t *pages = NULL;
        NvU32 cache_mode = X86_CACHE_WB;
        uint64_t page_count;
        NV_STATUS vendor_status;

        /* 只有 RM 已将系统分配上下文关联到此文件后，控制节点 mmap 才有效。此路径不允许伪造
         * 物理地址。 */
        if (!has_context || context.alloc == NULL || offset != 0U ||
            (length & (PAGE_SIZE - 1ULL)) != 0U || length == 0U)
            return K_EINVAL;
        page_count = length >> PAGE_SHIFT;
        vendor_status = nvabi_get_system_mapping_pages(
            context.alloc, context.page_index, page_count, &pages,
            &cache_mode);
        if (vendor_status != NV_OK || pages == NULL)
            return K_EINVAL;
        mapping->physical_pages = pages;
        mapping->page_count = page_count;
        mapping->length = length;
        mapping->cache_mode = cache_mode;
        return K_OK;
    }
    if (file->minor != 0U)
        return K_EINVAL;
    if (has_context) {
        uint64_t mapping_size;
        uint64_t page_count;

        /* 对于实际设备节点，不可变 Linux 辅助函数会映射经 RM 验证的 MemoryArea。范围信息是 RM
         * 返回的物理窗口事实；此前端不会分配 VRAM、创建 GPU VA、配置 GMMU 状态或伪造总线地址。 */
        if (context.alloc != NULL || offset != 0U ||
            (length & (PAGE_SIZE - 1ULL)) != 0U || length == 0U ||
            !nv_rm_vendor_frontend_memarea_size(&context.memArea,
                                                 &mapping_size) ||
            length != mapping_size)
            return K_EINVAL;
        page_count = length >> PAGE_SHIFT;
        mapping->length = length;
        mapping->cache_mode =
            nv_rm_vendor_frontend_cache_mode(context.caching);
        if (context.memArea.numRanges == 1U) {
            mapping->physical = paddr_make(
                (uint64_t)context.memArea.pRanges[0].start);
            return K_OK;
        }

        if (page_count > UINT64_MAX / sizeof(uint64_t) ||
            page_count > (uint64_t)(SIZE_MAX / sizeof(uint64_t)))
            return K_EOVERFLOW;
        uint64_t *pages = (uint64_t *)kzalloc(
            (size_t)page_count * sizeof(*pages), 0U);
        if (pages == NULL)
            return K_ENOMEM;
        uint64_t page_index = 0U;
        for (NvU64 range_index = 0U;
             range_index < context.memArea.numRanges; ++range_index) {
            uint64_t start = (uint64_t)
                context.memArea.pRanges[range_index].start;
            uint64_t range_pages = (uint64_t)
                context.memArea.pRanges[range_index].size >> PAGE_SHIFT;
            for (uint64_t range_page = 0U; range_page < range_pages;
                 ++range_page)
                pages[page_index++] = start + (range_page << PAGE_SHIFT);
        }
        if (page_index != page_count) {
            kfree(pages);
            return K_EINVAL;
        }
        mapping->physical_pages = pages;
        mapping->page_count = page_count;
        mapping->physical_pages_owned = true;
        return K_OK;
    }
    regs = file->context->state->regs;
    if (regs == NULL || !nv_rm_vendor_frontend_range_contains(
            regs->cpu_address, regs->size, offset, length))
        return K_EINVAL;

    /* 这是 RM 报告的真实 PCI 寄存器窗口，绝不是 BAR1/FB，也不是任意物理地址。VRAM 映射由 RM
     * 负责。 */
    mapping->physical = paddr_make(offset);
    mapping->length = length;
    mapping->cache_mode = X86_CACHE_UC;
    return K_OK;
}

void nv_rm_vendor_frontend_close(void *opaque_file)
{
    nv_rm_vendor_frontend_file_t *file =
        (nv_rm_vendor_frontend_file_t *)opaque_file;
    nv_alloc_mapping_list_node_t *mapping_list;
    if (file == NULL)
        return;
    nv_rm_vendor_event_unregister(file);
    nv_port_event_source_close(file->event_source);
    if (file->context != NULL && file->context->stack != NULL &&
        file->context->state != NULL &&
        (file->vendor_private.bCleanupRmapi != NV_FALSE ||
         file->vendor_private.handles != NULL ||
         file->vendor_private.ctl_nvfp != NULL)) {
        rm_cleanup_file_private(file->context->stack, file->context->state,
                                &file->vendor_private);
    }
    if (file->attached_gpus != NULL) {
        nvidia_stack_t *stack = nv_rm_vendor_frontend_stack_create();
        if (stack != NULL && file->context != NULL &&
            file->context->state != NULL) {
            for (NvU32 index = 0U; index < file->attached_gpu_words; ++index) {
                if (file->attached_gpus[index] != 0U &&
                    file->attached_gpus[index] == file->context->state->gpu_id)
                    (void)rm_set_external_kernel_client_count(
                        stack, file->context->state, NV_FALSE);
            }
            kfree(stack);
        }
        kfree(file->attached_gpus);
        file->attached_gpus = NULL;
        file->attached_gpu_words = 0U;
    }

    spinlock_lock(&file->mapping_lock);
    mapping_list = file->mapping_list;
    file->mapping_list = NULL;
    spinlock_unlock(&file->mapping_lock);
    while (mapping_list != NULL) {
        nv_alloc_mapping_list_node_t *next = mapping_list->pNext;
        if (mapping_list->context.page_array != NULL)
            kfree(mapping_list->context.page_array);
        if (mapping_list->context.memArea.pRanges != NULL)
            kfree(mapping_list->context.memArea.pRanges);
        kfree(mapping_list);
        mapping_list = next;
    }
    object_put(nv_port_event_source_wait_object(file->event_source));
    file->event_source = NULL;
    kfree(file);
}

/*
 * NVKMS RM 桥接层
 * ----------------
 *
 * 官方 595.84 NVKMS 对象会通过此路径执行所有 RM 操作。操作分发仍由不可变的厂商实现负责；
 * System 只提供临时厂商栈和生命周期串行化。
 */
kstatus_t nv_rm_vendor_kms_call_rm(void *opaque_context, void *ops)
{
    nv_rm_vendor_context_t *context =
        (nv_rm_vendor_context_t *)opaque_context;
    nvidia_stack_t *stack;
    kstatus_t status;

    if (context == NULL || ops == NULL || context->state == NULL ||
        !context->rm_initialized || !context->adapter_initialized ||
        !context->kms_lock_initialized)
        return K_EIO;

    status = nv_port_mutex_lock(&context->kms_rm_lock, UINT64_MAX);
    if (status != K_OK)
        return status;

    stack = nv_rm_vendor_frontend_stack_create();
    if (stack == NULL) {
        (void)nv_port_mutex_unlock(&context->kms_rm_lock);
        return K_ENOMEM;
    }

    /* 这是官方 RMAPI 分发函数，负责选择操作、管理 RM/API 锁，并实现所有 VRAM/GPUVA/GMMU/GSP
     * 功能。 */
    rm_kernel_rmapi_op(stack, ops);
    kfree(stack);

    (void)nv_port_mutex_unlock(&context->kms_rm_lock);
    return K_OK;
}

kstatus_t nv_rm_vendor_kms_open_gpu(void *opaque_context, uint32_t gpu_id,
                                    bool reset_aware)
{
    nv_rm_vendor_context_t *context =
        (nv_rm_vendor_context_t *)opaque_context;
    nvidia_stack_t *stack = NULL;
    NV_STATUS vendor_status = NV_OK;
    kstatus_t status;
    uint32_t refs;

    if (context == NULL || context->state == NULL ||
        !context->adapter_initialized || gpu_id != context->state->gpu_id ||
        !context->kms_lock_initialized)
        return K_ENOENT;

    status = nv_port_mutex_lock(&context->kms_rm_lock, UINT64_MAX);
    if (status != K_OK)
        return status;

    refs = atomic_load_explicit(&context->kms_gpu_refs, memory_order_acquire);
    if (refs == UINT32_MAX) {
        (void)nv_port_mutex_unlock(&context->kms_rm_lock);
        return K_EOVERFLOW;
    }

    /* KAPI 使用 reset_aware=TRUE。用户态/核心 NVKMS 打开句柄时使用 FALSE，必须参与厂商 RM
     * 外部客户端复位保护计数。 */
    if (!reset_aware) {
        stack = nv_rm_vendor_frontend_stack_create();
        if (stack == NULL) {
            (void)nv_port_mutex_unlock(&context->kms_rm_lock);
            return K_ENOMEM;
        }
        vendor_status = rm_set_external_kernel_client_count(
            stack, context->state, NV_TRUE);
        kfree(stack);
        if (vendor_status != NV_OK) {
            (void)nv_port_mutex_unlock(&context->kms_rm_lock);
            return K_EIO;
        }
    }

    atomic_store_explicit(&context->kms_gpu_refs, refs + 1U,
                          memory_order_release);
    (void)nv_port_mutex_unlock(&context->kms_rm_lock);
    return K_OK;
}

void nv_rm_vendor_kms_close_gpu(void *opaque_context, uint32_t gpu_id,
                                bool reset_aware)
{
    nv_rm_vendor_context_t *context =
        (nv_rm_vendor_context_t *)opaque_context;
    nvidia_stack_t *stack = NULL;
    NV_STATUS vendor_status = NV_OK;
    kstatus_t status;
    uint32_t refs;

    if (context == NULL || context->state == NULL ||
        gpu_id != context->state->gpu_id || !context->kms_lock_initialized)
        return;

    status = nv_port_mutex_lock(&context->kms_rm_lock, UINT64_MAX);
    if (status != K_OK)
        return;

    refs = atomic_load_explicit(&context->kms_gpu_refs, memory_order_acquire);
    if (refs == 0U) {
        (void)nv_port_mutex_unlock(&context->kms_rm_lock);
        return;
    }

    if (!reset_aware) {
        stack = nv_rm_vendor_frontend_stack_create();
        if (stack != NULL) {
            vendor_status = rm_set_external_kernel_client_count(
                stack, context->state, NV_FALSE);
            kfree(stack);
        } else {
            vendor_status = NV_ERR_NO_MEMORY;
        }
        if (vendor_status != NV_OK) {
            kernel_serial_printf_serial_only(
                "OS_NVIDIA_NVKMS_GPU_CLOSE_FAIL status=%x\r\n",
                (unsigned)vendor_status);
            (void)nv_port_mutex_unlock(&context->kms_rm_lock);
            return;
        }
    }

    atomic_store_explicit(&context->kms_gpu_refs, refs - 1U,
                          memory_order_release);
    (void)nv_port_mutex_unlock(&context->kms_rm_lock);
}

uint32_t nv_rm_vendor_kms_enumerate_gpus(void *opaque_context, void *gpu_info)
{
    nv_rm_vendor_context_t *context =
        (nv_rm_vendor_context_t *)opaque_context;
    nv_gpu_info_t *info = (nv_gpu_info_t *)gpu_info;

    if (context == NULL || context->state == NULL || info == NULL ||
        !context->adapter_initialized)
        return 0U;

    info->gpu_id = context->state->gpu_id;
    info->pci_info.domain = context->state->pci_info.domain;
    info->pci_info.bus = context->state->pci_info.bus;
    info->pci_info.slot = context->state->pci_info.slot;
    info->pci_info.function = context->state->pci_info.function;
    info->needs_numa_setup = NV_FALSE;
    info->is_soc_disp = NV_IS_SOC_DISPLAY_DEVICE(context->state);
    info->os_device_ptr = context;
    return 1U;
}

bool nv_rm_vendor_kms_allow_write_combining(void *opaque_context)
{
    nv_rm_vendor_context_t *context =
        (nv_rm_vendor_context_t *)opaque_context;

    if (context == NULL || context->state == NULL ||
        !context->adapter_initialized)
        return false;

    /* 以官方 RM 策略作为唯一可信来源。 */
    return rm_disable_iomap_wc() == NV_FALSE;
}
