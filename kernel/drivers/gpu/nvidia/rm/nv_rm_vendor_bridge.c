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

/* 595.84's immutable Linux MSI-X path caps the GPU at this many vectors
 * (NV_RM_MAX_MSIX_LINES).  The RM ISR is shared here because the vendor
 * entry point does not consume the Linux IRQ number. */
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

/* Keep the kernel-open OS interface header out of this translation unit.
 * Its rs_access.h and the RM NVOC headers are generated from distinct vendor
 * include roots and cannot be included together.  These are the only two
 * OS-interface entry points used by this bridge, declared with their exact
 * vendor ABI types. */
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

    /* rm_isr() is the vendor top-half entry.  It owns interrupt pending
     * inspection and returns the exact bottom-half requirement. */
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

    /* Drain coalesced vendor requests before publishing the queue slot as
     * idle.  The final pending check closes the IRQ/worker hand-off race. */
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
        /* RM probing currently precedes scheduler worker startup.  If an
         * early vendor path requested a bottom half, execute queued work in
         * the existing deferred runner while teardown keeps the RM state
         * alive. */
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

    /* Match the vendor PCI path's MSI-X preference, but use one real vector
     * for the RM interrupt stream.  MSI remains an explicit fallback. */
    if (!OS_NVIDIA_PREFER_MSI && device->msix_capability != 0U &&
        device->msix_table_size != 0U) {
        status = nv_port_irq_init(&context->irq, device, true, 0U, apic_id,
                                  nv_rm_vendor_irq_top, context, NULL, NULL);
        if (status == K_OK)
            status = nv_port_irq_start(&context->irq);
        if (status == K_OK) {
            /* NVIDIA's 595.84 Linux path enables all supported MSI-X table
             * entries before registering the RM ISR.  Keep the System
             * handler shared, but program every entry that the vendor path
             * would request so any GPU interrupt source can reach it. */
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

    /* Match the immutable 595.84 Linux teardown order: disable the running
     * adapter while its RM interrupt path is still available, then tear down
     * the IRQ, and only then release the adapter state.  A failed
     * rm_init_adapter() is not a successful adapter and must not be passed to
     * rm_shutdown_adapter(). */
    if (context->adapter_initialized)
        rm_disable_adapter(context->stack, context->state);

    nv_rm_vendor_stop_irq(context);

    if (context->adapter_initialized)
        rm_shutdown_adapter(context->stack, context->state);

    /* Phase 4 keeps only the real vendor OBJGPU discovery object alive.  It
     * must be deleted before the private RM and global RM state are released;
     * GSP bootstrap and the full adapter path begin in Phase 5. */
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
    /* System has no GPU IOVA-remap domain at this boundary.  Zero is a
     * valid vendor vaspace id, not the sentinel for a direct-physical PCIe
     * path; leave GMMU/VA ownership with NVIDIA by passing the exact vendor
     * "no domain" value. */
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

/* Phase 4 stops at the real vendor OBJGPU/NVOC/HAL discovery boundary.  The
 * object is not inserted into gpumgr's handle table and is deleted by the
 * normal reverse teardown path.  Phase 5 owns the adapter/GSP transition. */
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

    /* 595.84's generated host NVOC graph maps the physical-PF discovery
     * variant to PF_KERNEL_ONLY.  This selects the vendor GB205 HAL only; it
     * does not load firmware, start GSP, or issue an RPC. */
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
    /* Phase 4 deliberately reports the real discovery object constructed by
     * nv_rm_vendor_create_phase4_gpu().  private_state->pGpu belongs to the
     * later adapter path and may be the vendor's pre-attach placeholder; it
     * is not the object whose PF_KERNEL_ONLY NVOC/HAL binding we just proved. */
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

    /* chipInfo is populated by gpuPostConstruct, which is intentionally a
     * later Phase 5 boundary.  The architecture facts used here come from
     * the vendor GpuArch selected by the immutable HAL manager. */
    result->chip_arch = gpu_arch->chipArch;
    result->chip_impl = gpu_arch->chipImpl;
    result->hal_impl = gpu->halImpl;
    /* GpuArch stores the PMC_BOOT_42 architecture code (0x1b for GB200),
     * while GPU_ARCHITECTURE_BLACKWELL_GB2XX is the separate published
     * architecture encoding (0x1b0).  Use the value actually returned by the
     * immutable vendor HAL lookup at this boundary. */
    result->blackwell = result->chip_arch == NV_PMC_BOOT_42_ARCHITECTURE_GB200;
    result->gb205 = result->chip_impl == GPU_IMPLEMENTATION_GB205;
    /* The private-state HAL flags are set by the later adapter path.  Phase 4
     * stops after this real OBJGPU NVOC/HAL binding, so the vendor HAL object
     * and its resolved GB205 implementation are the valid evidence here. */
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

/* One boot snapshot, retained in RAM after failure cleanup. No serial or
 * per-frame output: phase, preemption gate, APIC, top/handled/bottom IRQs,
 * pending bottom half and failed bottom-half scheduling. */
static volatile uint32_t g_nv_rm_boot_diagnostic[8];

static bool nv_rm_vendor_init_adapter(nv_rm_vendor_context_t *context)
{
    uint64_t cpu_flags;
    bool ready;

    /* The interactive boot path skips user-mode self-tests and can arrive
     * with IF clear.  RM's real interrupt verification requires interrupts,
     * after the handler and MSI/MSI-X table have been installed.  RM also
     * waits on semaphores/workers, so this must remain a sleepable context. */
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
    /* The Linux frontend initializes these per-device RM locks before
     * rm_is_supported_device().  System cannot call nv_lock_init_locks(),
     * because that wrapper owns Linux-only nv_linux_state_t storage; the
     * vendor RM event-lock entry points are the portable part of the same
     * lifecycle and operate directly on nv_state_t. */
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
    /* This is the exact vendor-visible prefix used by nv_get_file_private.
     * The Linux-only wrapper is deliberately not imported; the RM ABI only
     * consumes this per-file state at this boundary. */
    nv_file_private_t vendor_private;
    nv_rm_vendor_context_t *context;
    bool control;
    uint32_t minor;
    struct nv_rm_vendor_frontend_file *control_file;
    NvU32 *attached_gpus;
    NvU32 attached_gpu_words;
    /* System owns the file lifetime, so retain the vendor mapping-context
     * shape without importing nv_linux_file_private_t.  The list is used only
     * for RM-validated mmap contexts; it never represents VRAM or GPU VA. */
    nv_alloc_mapping_list_node_t *mapping_list;
    spinlock_t mapping_lock;
    nv_port_event_source_t *event_source;
    struct nv_rm_vendor_frontend_file *event_next;
} nv_rm_vendor_frontend_file_t;

/* Static zero is the unlocked atomic spinlock state. Every access saves IRQ
 * state so a callback cannot interrupt a registration/removal lock owner. */
#define NV_RM_EVENT_BUCKETS 64U
typedef struct nv_rm_event_bucket {
    nv_port_spinlock_t lock;
    nv_rm_vendor_frontend_file_t *files;
} nv_rm_event_bucket_t;
static nv_rm_event_bucket_t g_nv_rm_event_buckets[NV_RM_EVENT_BUCKETS];

static nv_rm_event_bucket_t *nv_rm_vendor_event_bucket(const void *private_data) {
    uintptr_t key = (uintptr_t)private_data;
    /* Ignore allocation alignment and mix page bits. Computing a bucket must
     * never dereference a possibly retired vendor-private pointer. */
    key = (key >> 4) ^ (key >> 12) ^ (key >> 20);
    return &g_nv_rm_event_buckets[key & (NV_RM_EVENT_BUCKETS - 1U)];
}
/* Quiet, debugger-readable counters; never format logs in the IRQ callback. */
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
    /* Never dereference event->nvfp: it can be a retired address. Rechecking
     * active under registration protection rejects stale events after reuse. */
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
    /* Do not object_put here: dropping the last source reference in a hard
     * interrupt could call the heap destructor. Removal instead waits for
     * this protected callback before releasing the owner in thread context. */
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
    /* This ioctl runs in thread context. Pin only the independently owned
     * source: close may now retire the file, but the queue remains alive.
     * Neither payload freeing nor a final destructor holds the registry lock. */
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
 * 595.84's osapi.c calls this after the real RM has validated a CPU mapping.
 * The Linux implementation stores the node in nv_linux_file_private_t.  That
 * wrapper is intentionally not present in System; keep the same ownership and
 * duplicate-rejection rules on the System frontend object instead.
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

    /* RmCreateMmapContextLocked switches pNv to nv_get_ctl_state() for
     * system-memory allocations and passes the resolved allocation private
     * object.  System shares the live nv_state_t between control and actual
     * files, so pAllocPriv is the authoritative discriminator here; the RM
     * control flag remains valid for the vendor's explicit control paths. */
    control_device = ((nv->flags & NV_FLAG_CONTROL) != 0 ||
                      pAllocPriv != NULL) ? NV_TRUE : NV_FALSE;
    vendor_private = nv_get_file_private((NvS32)fd, control_device,
                                          &os_private);
    if (vendor_private == NULL || os_private == NULL) {
        if (os_private != NULL)
            nv_put_file_private(os_private);
        return NV_ERR_INVALID_ARGUMENT;
    }

    /* vendor_private is the first member by contract, so this conversion is
     * the inverse of nv_frontend_file_vendor_private(). */
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
        /* Control-device mappings are RM system-memory allocations.  Keep
         * the vendor private allocation handle and page index exactly as the
         * immutable implementation does; no System memory manager substitute
         * is introduced here. */
        if (pAllocPriv == NULL) {
            kfree(node);
            status = NV_ERR_INVALID_ARGUMENT;
            goto done;
        }
        node->context.alloc = pAllocPriv;
        node->context.page_index = pageIndex;
    } else {
        /* Actual-device mappings carry the RM-validated aperture ranges. */
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

        /* NUMA-backed mappings transfer a page array in the Linux driver.
         * System currently has no NUMA online state, but copy it when the RM
         * supplies one so the context remains self-contained. */
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

    /* This is the System equivalent of the vendor Linux control device's
     * NV_STATE_PTR(&nv_ctl_device).  The state is returned only while the
     * real RM/GSP instance is published as ready; callers must fail closed
     * when the device is not live. */
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

    /* Mirror the immutable Linux frontend's relationship rules: the target
     * must be a control node, both handles must belong to the same RM
     * instance, and a file may have only one control link. */
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
    /* escape.c's RmCreateOsDescriptor pins user memory before the core heap
     * call. Passing a raw virtual address straight to the core is not an
     * OS_PAGE_ARRAY descriptor, and the core correctly rejects it. */
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
    /* Do not expose the kernel's page-array pointer in the user packet.
     * On success the RM owns that pinned payload until resource destruction. */
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
        /* The immutable RM OS layer owns the event list, client association,
         * active/reference state and validation. Do not fabricate an event
         * handle or report success without executing that implementation. */
        uint32_t expected_size = number == NV_ESC_RM_GET_EVENT_DATA ?
            sizeof(NVOS41_PARAMETERS) : number == NV_ESC_ALLOC_OS_EVENT ?
            sizeof(nv_ioctl_alloc_os_event_t) : sizeof(nv_ioctl_free_os_event_t);
        /* 595.84 accepts OS events on actual-device fds too; Vulkan creates
         * a registered nvidia0 fd for its queue event notifications. */
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

        /* This is the real 595.84 NV04_FREE entry point.  The vendor RM
         * performs handle validation, locking, and object destruction; the
         * frontend only supplies the Linux user-parameter security context. */
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

        /* Keep the exact 595.84 NV01_ALLOC_OBJECT path.  Resource-class
         * validation, handle allocation, locking, and any GSP work remain in
         * the immutable RM implementation. */
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

        /* This is the real 595.84 NV04_CONTROL entry point.  Its immutable
         * implementation copies and validates the pointed-to RM parameters,
         * takes the required RM locks, and performs any GSP transaction. */
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

        /* NV_ESC_RM_ALLOC_MEMORY is an actual-device ioctl in 595.84.  The
         * fd is part of the immutable wrapper because the RM registers the
         * resulting system-memory mapping against that exact Linux file. */
        if (file->control || file->minor != 0U ||
            argument_size != sizeof(*api))
            return K_EINVAL;

        {
            API_SECURITY_INFO sec_info;
            NvU32 flags = allocation->flags;
            nv_rm_vendor_frontend_security(file, &sec_info);
            Nv01AllocMemoryWithSecInfo(allocation, sec_info);
            /* Mirror escape.c: immediate system allocations also register a
             * context on the supplied mmap fd. Without this step allocation
             * succeeds but the subsequent control-node mmap is rejected. */
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

        /* This escape is intentionally the same OS-interface call used by
         * immutable 595.84 escape.c.  It updates RM's own mapping record; no
         * System mapping, VRAM, GPU-VA, or GMMU state is synthesized here. */
        update->status = rm_update_device_mapping_info(
            update->hClient, update->hDevice, update->hMemory,
            NvP64_VALUE(update->pOldCpuAddress),
            NvP64_VALUE(update->pNewCpuAddress));
        return K_OK;
    }
    case NV_ESC_RM_IDLE_CHANNELS: {
        NVOS30_PARAMETERS *idle = (NVOS30_PARAMETERS *)argument;

        /* The immutable escape.c path is control-device-only and reports
         * operation errors through NVOS30_PARAMETERS.status. */
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

        /* Keep the 595.84 escape.c ordering.  The RM owns the memory-object
         * validation and CPU mapping; the fd wrapper is only used to attach
         * the resulting mmap context to the requesting actual file. */
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
        /* Match 595.84 escape.c: event classes may be allocated on actual
         * GPU fds. Other resources, including root clients, remain ctl-only.
         * RM still validates the client, parent and event parameters. */
        if (!file->control &&
            allocation->hClass != NV01_EVENT &&
            allocation->hClass != NV01_EVENT_OS_EVENT &&
            allocation->hClass != NV01_EVENT_KERNEL_CALLBACK &&
            allocation->hClass != NV01_EVENT_KERNEL_CALLBACK_EX)
            return K_EINVAL;

        /* NV_ESC_RM_ALLOC with a 48-byte payload is the vendor
         * NV04_ALLOC_WITH_ACCESS entry point.  The P3 vendor closure retains
         * the WithSecInfo export, so use that exact immutable 595.84 RM API
         * with the Linux user-parameter security context.  The vendor
         * implementation performs pointer copying, resource validation,
         * locking, and any required firmware interaction. */
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

        /* Keep the legacy heap ABI in the vendor RM.  This includes the
         * vendor's OS-descriptor path; System does not reimplement its memory
         * manager or translate the RM's heap/GPU-address operations. */
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

        /* Object duplication is a real RM resource operation.  Let the
         * immutable 595.84 implementation validate handles, permissions and
         * locking rather than manufacturing a destination handle. */
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

        /* The immutable Linux frontend accepts a non-empty NvU32 array only
         * on the control node and permits this relationship to be installed
         * once per file. */
        if (!file->control || argument_size == 0U ||
            argument_size % sizeof(NvU32) != 0U ||
            file->attached_gpus != NULL)
            return K_EINVAL;
        gpu_words = argument_size / sizeof(NvU32);
        gpu_ids = (NvU32 *)kzalloc(argument_size, 0U);
        if (gpu_ids == NULL)
            return K_ENOMEM;
        __builtin_memcpy(gpu_ids, argument, argument_size);

        /* nvidia_dev_get() in the Linux layer resolves each non-zero ID and
         * increments the RM external-client count.  System has one published
         * PCIe GPU, so validate every non-zero entry against that real RM
         * identity and invoke the retained RM operation for each occurrence. */
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

        /* NUMA_INFO is an actual-device query.  System has no Linux NUMA
         * memory hotplug layer, so the adapter reports the real RM NUMA
         * placement while truthfully keeping the host NUMA state disabled. */
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
        /* P8 deliberately exposes only operations backed by a real RM or a
         * validated PCI register aperture.  Unsupported RM ioctls must not
         * become fake success while the later userspace phases are absent. */
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

        /* A control-node mmap is valid only after RM has attached a system
         * allocation context to this exact file.  No physical-address
         * fabrication is permitted for this path. */
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

        /* For an actual device node the immutable Linux helper maps the
         * RM-validated MemoryArea.  The ranges are the physical aperture
         * facts returned by RM; this frontend does not allocate VRAM, create
         * GPU VA, program GMMU state, or manufacture a bus address. */
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

    /* This is the real PCI register aperture reported by RM, never BAR1/FB
     * and never an arbitrary physical address.  VRAM mapping belongs to RM. */
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
 * NVKMS RM bridge
 * ----------------
 *
 * The official 595.84 NVKMS objects call this path for every RM operation.
 * Keep the operation dispatch in the immutable vendor implementation;
 * System supplies only the temporary vendor stack and lifetime serialization.
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

    /* This is the exact official RMAPI dispatcher.  It owns the operation
     * switch, RM/API locking, and all VRAM/GPUVA/GMMU/GSP implementation. */
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

    /* KAPI uses reset_aware=TRUE.  User/core NVKMS opens use FALSE and must
     * participate in the vendor RM external-client reset protection count. */
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

    /* Keep the official RM policy as the source of truth. */
    return rm_disable_iomap_wc() == NV_FALSE;
}
