#include "nv_rm_driver.h"

#include <kernel/console.h>
#include "nv_rm_vendor_bridge.h"

#ifndef OS_NVIDIA_PHASE5
#define OS_NVIDIA_PHASE5 0
#endif

#define NVIDIA_VENDOR_ID 0x10deU
#define NVIDIA_GB205_DEVICE_ID 0x2f04U

#ifndef NULL
#define NULL ((void *)0)
#endif

/* The normal PE image deliberately uses the fallback boundary below; the
 * vendor bridge is linked separately by the ELF NVIDIA phase builds.  GCC's
 * weak-definition model is not usable as a fallback with MinGW PE objects:
 * its linker leaves references to those symbols unresolved.  Keep the weak
 * definitions for ELF so a strong vendor bridge can replace them, but emit
 * ordinary definitions for the self-contained MinGW image. */
#if defined(__MINGW32__) || defined(__MINGW64__)
#define NV_RM_WEAK
#else
#define NV_RM_WEAK __attribute__((weak))
#endif

enum nv_rm_failure_stage {
    NV_RM_FAILURE_NONE = 0,
    NV_RM_FAILURE_PCI,
    NV_RM_FAILURE_BAR,
    NV_RM_FAILURE_RM,
    NV_RM_FAILURE_HAL,
};

static nv_gpu_device_t g_nv_gpu;
static atomic_uint_fast64_t g_nv_vram_bytes;
static nv_rm_screen_info_t g_nv_screen_info;

void nv_rm_driver_set_screen_info(uint64_t physical_base, uint32_t width,
                                  uint32_t height, uint32_t depth,
                                  uint32_t pitch, uint64_t size)
{
    nv_rm_screen_info_t info = {0};
    uint64_t required_size;

    if (physical_base == 0U || width == 0U || height == 0U ||
        depth == 0U || pitch == 0U || size == 0U ||
        (uint64_t)width > UINT64_MAX / (uint64_t)(depth / 8U == 0U ? 1U : depth / 8U)) {
        g_nv_screen_info = info;
        return;
    }

    required_size = (uint64_t)pitch * (uint64_t)height;
    if (required_size == 0U || required_size > size ||
        pitch < (uint32_t)(((uint64_t)width * (uint64_t)depth) / 8U)) {
        g_nv_screen_info = info;
        return;
    }

    info.physical_base = physical_base;
    info.width = width;
    info.height = height;
    info.depth = depth;
    info.pitch = pitch;
    info.size = size;
    info.valid = true;
    g_nv_screen_info = info;
}

bool nv_rm_driver_get_screen_info(nv_rm_screen_info_t *info)
{
    if (info == NULL)
        return false;
    *info = g_nv_screen_info;
    return info->valid;
}

static void nv_rm_log_status(const char *prefix, kstatus_t status)
{
    kernel_serial_printf_serial_only("%s status=%d\r\n", prefix, (int)status);
}

/*
 * The production System image does not link the vendor RM object.  Keep that
 * image honest and buildable: the unresolved vendor boundary is an explicit
 * failure, never a successful no-op.  The Phase 4 image supplies the strong
 * implementation from the immutable 595.84 object set.
 */
NV_RM_WEAK kstatus_t nv_rm_vendor_start(const pci_device_t *device,
                                                   void **context_out,
                                                   nv_rm_vendor_result_t *result)
{
    (void)device;
    if (context_out != NULL)
        *context_out = NULL;
    if (result != NULL)
        *result = (nv_rm_vendor_result_t){0};

    kernel_serial_write_serial_only("OS_NVIDIA_RM_NOT_LINKED\r\n");
    return K_ENOSYS;
}

NV_RM_WEAK void nv_rm_vendor_stop(void *context)
{
    (void)context;
}

NV_RM_WEAK kstatus_t nv_rm_vendor_frontend_open(
    bool control, uint32_t minor, void **file_out)
{
    (void)control;
    (void)minor;
    if (file_out != NULL)
        *file_out = NULL;
    return K_ENOSYS;
}

NV_RM_WEAK kstatus_t nv_rm_vendor_frontend_abi_private(
    void *file, void **private_out)
{
    (void)file;
    if (private_out != NULL)
        *private_out = NULL;
    return K_ENOSYS;
}

NV_RM_WEAK bool nv_rm_vendor_frontend_is_control(void *file)
{
    (void)file;
    return false;
}

NV_RM_WEAK void *nv_rm_vendor_ctl_state(void)
{
    return NULL;
}

NV_RM_WEAK uint32_t nv_rm_vendor_frontend_poll_state(void *file,
                                                    bool consume_dataless)
{
    (void)file;
    (void)consume_dataless;
    return 2U; /* closed: the vendor implementation is not linked */
}

NV_RM_WEAK void *nv_rm_vendor_frontend_wait_object(void *file)
{
    (void)file;
    return NULL;
}

NV_RM_WEAK kstatus_t nv_rm_vendor_frontend_register_fd(
    void *file, void *control_file)
{
    (void)file;
    (void)control_file;
    return K_ENOSYS;
}

NV_RM_WEAK kstatus_t nv_rm_vendor_frontend_ioctl(
    void *file, uint32_t request, void *argument, uint32_t argument_size)
{
    (void)file;
    (void)request;
    (void)argument;
    (void)argument_size;
    return K_ENOSYS;
}

NV_RM_WEAK kstatus_t nv_rm_vendor_frontend_mmap(
    void *file, uint64_t offset, uint64_t length, uint32_t protection,
    nv_rm_vendor_frontend_mapping_t *mapping)
{
    (void)file;
    (void)offset;
    (void)length;
    (void)protection;
    if (mapping != NULL)
        *mapping = (nv_rm_vendor_frontend_mapping_t){0};
    return K_ENOSYS;
}

NV_RM_WEAK void nv_rm_vendor_frontend_close(void *file)
{
    (void)file;
}

static void nv_rm_driver_reset(void)
{
    atomic_store_explicit(&g_nv_vram_bytes,0U,memory_order_release);
    g_nv_gpu = (nv_gpu_device_t){0};
    g_nv_gpu.state = NV_GPU_NEW;
}

static void nv_rm_driver_fail(uint32_t stage, kstatus_t status)
{
    atomic_store_explicit(&g_nv_vram_bytes,0U,memory_order_release);
    g_nv_gpu.failure_stage = stage;
    g_nv_gpu.failure_status = status;
    g_nv_gpu.state = NV_GPU_FAILED;
    nv_rm_log_status("OS_NVIDIA_INIT_FAILED", status);
}

static void nv_rm_driver_unwind(void)
{
    if (g_nv_gpu.vendor_context != NULL) {
        nv_rm_vendor_stop(g_nv_gpu.vendor_context);
        g_nv_gpu.vendor_context = NULL;
        g_nv_gpu.rm_gpu = NULL;
        g_nv_gpu.rm_device = NULL;
    }

    if (g_nv_gpu.bar0_mapping.active) {
        nv_port_mmio_unmap(&g_nv_gpu.bar0_mapping);
        g_nv_gpu.bar0_va = NULL;
    }
}

static bool nv_rm_memory_bar(const pci_bar_t *bar)
{
    return bar != NULL &&
           (bar->flags & PCI_RESOURCE_MEMORY) != 0U &&
           (bar->length != 0U) &&
           (bar->address != 0U);
}

static kstatus_t nv_rm_find_memory_bars(pci_device_t *device)
{
    uint32_t memory_bar = 0U;
    uint32_t i;

    for (i = 0U; i < PCI_MAX_BARS; ++i) {
        const pci_bar_t *bar = &device->bars[i];
        if (bar->length != 0U || bar->address != 0U || bar->flags != 0U) {
            kernel_serial_printf_serial_only(
                "OS_NVIDIA_BAR index=%u flags=%x pa=%llx size=%llx\r\n",
                (unsigned)i, (unsigned)bar->flags,
                (unsigned long long)bar->address,
                (unsigned long long)bar->length);
        }
        if (!nv_rm_memory_bar(&device->bars[i]))
            continue;

        if (memory_bar == 0U) {
            g_nv_gpu.bar0_index = (uint8_t)i;
            g_nv_gpu.bar0_pa = device->bars[i].address;
            g_nv_gpu.bar0_size = device->bars[i].length;
        } else if (memory_bar == 1U) {
            g_nv_gpu.bar1_index = (uint8_t)i;
            g_nv_gpu.bar1_pa = device->bars[i].address;
            g_nv_gpu.bar1_size = device->bars[i].length;
        }

        ++memory_bar;
    }

    if (memory_bar < 2U || g_nv_gpu.bar0_size == 0U ||
        g_nv_gpu.bar1_size == 0U)
        return K_EIO;

    return K_OK;
}

kstatus_t nv_rm_driver_probe(void)
{
    const pci_host_t *host;
    const pci_device_t *found;
    nv_rm_vendor_result_t result = {0};
    kstatus_t status;

    host = pci_current_host();

    if (g_nv_gpu.state == NV_GPU_HAL_READY ||
        g_nv_gpu.state == NV_GPU_GSP_READY)
        return K_OK;

    if (g_nv_gpu.state == NV_GPU_FAILED)
        nv_rm_driver_reset();

    if (host == NULL || !host->initialized) {
        kernel_serial_write_serial_only("OS_NVIDIA_PCI_UNAVAILABLE\r\n");
        return K_OK;
    }

    kernel_serial_printf_serial_only(
        "OS_NVIDIA_PROBE_HOST init=%u devices=%u ecams=%u\r\n",
        host->initialized ? 1U : 0U, (unsigned)host->device_count,
        (unsigned)host->ecam_count);
    kernel_serial_write_serial_only("OS_NVIDIA_PROBE_FIND_BEGIN\r\n");
    found = nv_port_pci_find(host, NVIDIA_VENDOR_ID,
                              NVIDIA_GB205_DEVICE_ID);
    kernel_serial_write_serial_only("OS_NVIDIA_PROBE_FIND_DONE\r\n");
    if (found == NULL) {
        kernel_serial_write_serial_only("OS_NVIDIA_PCI_ABSENT\r\n");
        return K_OK;
    }

    g_nv_gpu.pci = (pci_device_t *)(void *)found;
    g_nv_gpu.segment = found->segment;
    g_nv_gpu.bus = found->bus;
    g_nv_gpu.slot = found->slot;
    g_nv_gpu.function = found->function;
    g_nv_gpu.vendor_id = found->vendor_id;
    g_nv_gpu.device_id = found->device_id;

    kernel_serial_printf_serial_only(
        "OS_NVIDIA_PCI_FOUND %x:%x:%x.%u ID=%x:%x\r\n",
        (unsigned)g_nv_gpu.segment, (unsigned)g_nv_gpu.bus,
        (unsigned)g_nv_gpu.slot, (unsigned)g_nv_gpu.function,
        (unsigned)g_nv_gpu.vendor_id, (unsigned)g_nv_gpu.device_id);
    status = nv_rm_find_memory_bars(g_nv_gpu.pci);
    if (status != K_OK) {
        nv_rm_driver_fail(NV_RM_FAILURE_BAR, status);
        return status;
    }
    status = nv_port_pci_enable(g_nv_gpu.pci);
    if (status != K_OK) {
        nv_rm_driver_fail(NV_RM_FAILURE_PCI, status);
        return status;
    }
    g_nv_gpu.state = NV_GPU_PCI_READY;
    kernel_serial_printf_serial_only(
        "OS_NVIDIA_BAR1_METADATA index=%u pa=%llx size=%llx\r\n",
        (unsigned)g_nv_gpu.bar1_index,
        (unsigned long long)g_nv_gpu.bar1_pa,
        (unsigned long long)g_nv_gpu.bar1_size);

    kernel_serial_printf_serial_only(
        "OS_NVIDIA_BAR0_REQUEST index=%u pa=%llx size=%llx\r\n",
        (unsigned)g_nv_gpu.bar0_index,
        (unsigned long long)g_nv_gpu.bar0_pa,
        (unsigned long long)g_nv_gpu.bar0_size);

    status = nv_port_mmio_map_bar(g_nv_gpu.pci, g_nv_gpu.bar0_index,
                                  NV_PORT_CACHE_UC, &g_nv_gpu.bar0_mapping);
    if (status != K_OK) {
        nv_rm_log_status("OS_NVIDIA_BAR0_MAP_FAILED", status);
        nv_rm_driver_fail(NV_RM_FAILURE_BAR, status);
        return status;
    }
    g_nv_gpu.bar0_va = g_nv_gpu.bar0_mapping.base;
    g_nv_gpu.state = NV_GPU_BAR_READY;
    kernel_serial_printf_serial_only(
        "OS_NVIDIA_BAR0_READY index=%u pa=%llx va=%p size=%llx\r\n",
        (unsigned)g_nv_gpu.bar0_index,
        (unsigned long long)g_nv_gpu.bar0_pa, g_nv_gpu.bar0_va,
         (unsigned long long)g_nv_gpu.bar0_size);
    status = nv_rm_vendor_start(g_nv_gpu.pci, &g_nv_gpu.vendor_context,
                                &result);
    if (status != K_OK) {
        nv_rm_driver_unwind();
        nv_rm_driver_fail(NV_RM_FAILURE_RM, status);
        return status;
    }

    /* Phase 4 stops at the vendor OBJGPU/NVOC/HAL discovery object.  The
     * internal device handle is created by the later adapter/GSP path and is
     * intentionally not required at this phase boundary. */
    if (g_nv_gpu.vendor_context == NULL || result.rm_gpu == NULL) {
        nv_rm_driver_unwind();
        nv_rm_driver_fail(NV_RM_FAILURE_RM, K_EIO);
        return K_EIO;
    }
    g_nv_gpu.rm_gpu = result.rm_gpu;
    g_nv_gpu.rm_device = result.rm_device;
    g_nv_gpu.state = NV_GPU_RM_READY;
    kernel_serial_write_serial_only("OS_NVIDIA_PHASE4_RM_READY\r\n");

    kernel_serial_printf_serial_only(
        "OS_NVIDIA_ARCH=%x IMPL=%x HAL=%x\r\n",
        (unsigned)result.chip_arch, (unsigned)result.chip_impl,
        (unsigned)result.hal_impl);

    if (!result.blackwell || !result.gb205 || !result.hal_valid) {
        nv_rm_driver_unwind();
        nv_rm_driver_fail(NV_RM_FAILURE_HAL, K_EIO);
        return K_EIO;
    }

    g_nv_gpu.state = NV_GPU_HAL_READY;
    kernel_serial_write_serial_only(
        "OS_NVIDIA_PHASE4_HAL_READY BLACKWELL GB205\r\n");
#if OS_NVIDIA_PHASE5
    g_nv_gpu.state = NV_GPU_GSP_READY;
    atomic_store_explicit(&g_nv_vram_bytes,result.vram_bytes,memory_order_release);
    kernel_serial_write_serial_only("OS_NVIDIA_GSP_READY\r\n");
#endif
    return K_OK;
}

uint64_t nv_rm_driver_memory_bytes(const pci_device_t *device)
{
    uint64_t bytes=atomic_load_explicit(&g_nv_vram_bytes,memory_order_acquire);
    return bytes && device==g_nv_gpu.pci ? bytes : 0U;
}

const nv_gpu_device_t *nv_rm_driver_current(void)
{
    return &g_nv_gpu;
}
