#include <kernel/bootinfo.h>
#include <kernel/boot_profile.h>
#include "arch/x86_64/cpu.h"
#include <arch/x86_64/interrupt.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/uaccess.h>
#include "arch/x86_64/apic.h"
#include <arch/x86_64/acpi.h>
#include <arch/x86_64/smp.h>
#include <kernel/block_device.h>
#include <kernel/mm_boot.h>
#include <kernel/kmem.h>
#include <kernel/handle.h>
#include <kernel/mutex.h>
#include <kernel/object.h>
#include <kernel/audio.h>
#include <kernel/hda.h>
#include <kernel/bluetooth.h>
#include <kernel/package.h>
#include <kernel/firmware.h>
#include <kernel/rcu.h>
#include <kernel/telemetry.h>
#include <kernel/perf.h>
#include <kernel/dma.h>
#include <kernel/gpu.h>
#include <kernel/display.h>
#include <kernel/iommu.h>
#include <kernel/device.h>
#include <kernel/io.h>
#include <kernel/completion_port.h>
#include <kernel/input.h>
#include <kernel/window_server.h>
#include <kernel/message_port.h>
#include <kernel/timer.h>
#include <kernel/deferred.h>
#include <kernel/service.h>
#include <kernel/power.h>
#include <kernel/xhci.h>
#include <kernel/block.h>
#include <kernel/pci.h>
#include <kernel/process.h>
#include <kernel/resource.h>
#include <kernel/elf_loader.h>
#include <kernel/init_user_services.h>
#include <kernel/wait.h>
#include <kernel/vm.h>
#include <kernel/vfs.h>
#include <kernel/journal.h>
#include <kernel/litefs.h>
#include <kernel/e1000.h>
#include <kernel/net_core.h>
#include <kernel/net_manager.h>
#include <kernel/socket.h>
#include <kernel/console.h>
#include <kernel/irq.h>
#include <kernel/init_early.h>
#include <kernel/init_memory.h>
#include <kernel/init_devices.h>
#include <kernel/init_core.h>
#include <kernel/init_storage.h>
#include <kernel/init_scheduler.h>
#include <kernel/init_network.h>
#include <kernel/init_filesystem.h>
#include <kernel/init_graphics.h>
#include <kernel/init_userspace.h>
#include <kernel/nvme_core.h>
#include <ascii_font.h>
#include <arch/x86_64/syscall_internal.h>
#include <usb/storage.h>
#include <kernel/sched.h>
#include <kernel/debug_stage.h>
#include <kernel/realtest.h>

#include <kernel/init_self_tests.h>
#include <kernel/init_runtime.h>

/* REFACTOR_P3_BOOT_SELF_TEST_OWNER：启动自测夹具和所有权检查。 */

#define halt_forever() kernel_halt_forever_at(__FILE__, __LINE__)

static UINT32 g_canonical_started_devices;
static UINT32 g_canonical_device_power_state;
static UINT32 g_canonical_async_power_state;
static UINT32 g_canonical_async_power_polls;
static device_t *g_canonical_pending_device;
static io_request_t *g_canonical_pending_request;
static atomic_uint g_canonical_cancel_calls;
static bool g_canonical_completion_bench_emitted;

static kstatus_t canonical_test_start(device_t *device) {
    if (device == 0) return K_EINVAL;
    ++g_canonical_started_devices;
    return K_OK;
}

static void canonical_test_stop(device_t *device) {
    (void)device;
    if (g_canonical_started_devices != 0U) --g_canonical_started_devices;
}

static kstatus_t canonical_test_submit(device_t *device, io_request_t *request) {
    if (device == 0 || request == 0 || g_canonical_started_devices == 0U) return K_EIO;
    if (request->opcode == IO_IOCTL) return K_OK; /* 閻ｆ瑧绮伴崣鏍ㄧХ鐠侯垰绶炴穱婵囧瘮閹稿倽鎹ｉ妴?*/
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < request->vec_count; ++i) {
        io_vec_t *vec = &request->vecs[i];
        for (size_t j = 0; j < vec->length; ++j) {
            ((uint8_t *)vec->base)[j] = 0x5AU;
        }
        bytes += vec->length;
    }
    uint64_t benchmark_start = telemetry_timestamp();
    io_complete(request, K_OK, bytes);
    if (!g_canonical_completion_bench_emitted) {
        kernel_perf_emit_scope("io.completion", benchmark_start);
        g_canonical_completion_bench_emitted = true;
    }
    return K_OK;
}

static kstatus_t canonical_pending_submit(device_t *device,
                                          io_request_t *request) {
    if (device == 0 || request == 0 || g_canonical_pending_request != 0) {
        return K_EBUSY;
    }
    g_canonical_pending_device = device;
    g_canonical_pending_request = request;
    /* 濡剝瀚欑拋鎯ь槵闂冪喎鍨稉顓炵毣閺堫亜鐣幋鎰畱鐠囬攱鐪伴敍宀€鏁?remove 鐠侯垰绶炵拹鐔荤煑閸欐牗绉烽妴?*/
    return K_OK;
}

static kstatus_t canonical_test_reset(device_t *device, uint32_t level) {
    if (device == 0 || level >= 3U) return K_EINVAL;
    g_canonical_device_power_state = DEVICE_POWER_ACTIVE;
    return K_OK;
}

static kstatus_t canonical_test_set_power(device_t *device, uint32_t state) {
    if (device == 0 || state > DEVICE_POWER_SUSPENDED) return K_EINVAL;
    g_canonical_device_power_state = state;
    return K_OK;
}

static kstatus_t canonical_test_begin_power(device_t *device, uint32_t state) {
    if (device == 0 || state > DEVICE_POWER_SUSPENDED) return K_EINVAL;
    g_canonical_async_power_state = state;
    g_canonical_async_power_polls = 0U;
    return K_EAGAIN;
}

static kstatus_t canonical_test_poll_power(device_t *device, uint32_t state) {
    if (device == 0 || state != g_canonical_async_power_state) return K_EINVAL;
    if (++g_canonical_async_power_polls < 2U) return K_EAGAIN;
    g_canonical_device_power_state = state;
    return K_OK;
}

static kstatus_t canonical_test_probe(device_t *device) {
    if (device == 0) return K_EINVAL;
    return device->device_id == 0xD00DULL ||
           device->device_id == 0xD00EULL ||
           device->device_id == 0xD00FULL ? K_OK : K_ENOENT;
}

static void canonical_test_remove(device_t *device) {
    if (device == g_canonical_pending_device &&
        g_canonical_pending_request != 0) {
        io_request_t *request = g_canonical_pending_request;
        g_canonical_pending_request = 0;
        g_canonical_pending_device = 0;
        io_complete(request, K_EDEVREMOVED, 0U);
    }
    if (device != 0 && device->ops != 0 && device->ops->stop != 0) {
        device->ops->stop(device);
    }
}

static void canonical_test_cancel(io_request_t *request) {
    atomic_fetch_add_explicit(&g_canonical_cancel_calls, 1U, memory_order_relaxed);
    io_complete(request, K_ECANCELED, 0);
}

static const device_ops_t g_canonical_device_ops = {
    .start = canonical_test_start,
    .stop = canonical_test_stop,
    .submit_io = canonical_test_submit,
    .reset = canonical_test_reset,
    .set_power = canonical_test_set_power,
};

static const device_ops_t g_canonical_async_device_ops = {
    .start = canonical_test_start,
    .stop = canonical_test_stop,
    .reset = canonical_test_reset,
    .begin_power = canonical_test_begin_power,
    .poll_power = canonical_test_poll_power,
};

static const device_ops_t g_canonical_pending_device_ops = {
    .start = canonical_test_start,
    .stop = canonical_test_stop,
    .submit_io = canonical_pending_submit,
    .reset = canonical_test_reset,
    .set_power = canonical_test_set_power,
};

bool kernel_init_canonical_device_dma_io_self_test(void) {
    static device_t device;
    static device_t async_device;
    static device_t pending_device;
    static driver_t driver;
    page_t *page = 0;
    page_t *pages[1] = {0};
    dma_mapping_t *mapping = 0;
    dma_mapping_t duplicate = {0};
    page_t *duplicate_pages[2] = {0};
    uint8_t buffer[64] = {0};
    io_vec_t io_vector = {buffer, sizeof(buffer)};
    bio_vec_t bio_vector;
    bio_t bio = {0};
    bio_vec_t batch_vectors[2] = {{0}};
    bio_t batch_bios[2] = {{0}};
    io_request_t request;
    io_request_t batch_requests[2];
    io_request_t cancel_request;
    BOOLEAN driver_registered = 0;
    BOOLEAN device_registered = 0;
    BOOLEAN async_device_registered = 0;
    BOOLEAN pending_device_registered = 0;
    BOOLEAN mapped = 0;
    BOOLEAN success = 0;
    kstatus_t submit_status;
    uint64_t benchmark_start;

    mapping = (dma_mapping_t *)kzalloc(sizeof(*mapping), 0);
    if (mapping == 0) goto cleanup;

    g_canonical_started_devices = 0U;
    g_canonical_device_power_state = DEVICE_POWER_ACTIVE;
    g_canonical_async_power_state = DEVICE_POWER_ACTIVE;
    g_canonical_async_power_polls = 0U;
    g_canonical_pending_device = 0;
    g_canonical_pending_request = 0;
    g_canonical_completion_bench_emitted = false;
    atomic_store_explicit(&g_canonical_cancel_calls, 0U, memory_order_relaxed);
    device_object_init(&device, 0xD00DULL, 0x0100U,
                       &g_canonical_device_ops, 0);
    if (iommu_hardware_enabled() &&
        iommu_attach_pci_device(&device, 0, 0, 31U, 7U) != K_OK) goto cleanup;
    driver_object_init(&driver, "canonical-loopback",
                       canonical_test_probe, canonical_test_remove);
    if (driver.api_version != OS_DRIVER_API_VERSION ||
        driver.struct_size != sizeof(driver)) goto cleanup;
    if (driver_register(&driver) != K_OK) goto cleanup;
    driver_registered = 1;
    if (device_register(&device) != K_OK) goto cleanup;
    device_registered = 1;
    if (atomic_load_explicit(&device.state, memory_order_acquire) != DEVICE_ACTIVE ||
        device.power_device == 0) goto cleanup;

    if (device_suspend_timeout(&device, 1U) != K_ETIMEDOUT ||
        atomic_load_explicit(&device.state, memory_order_acquire) != DEVICE_FAILED ||
        device_reset(&device, 1U) != K_OK ||
        atomic_load_explicit(&device.state, memory_order_acquire) != DEVICE_ACTIVE) goto cleanup;
    if (device_suspend(&device) != K_OK ||
        atomic_load_explicit(&device.state, memory_order_acquire) != DEVICE_SUSPENDED ||
        g_canonical_device_power_state != DEVICE_POWER_SUSPENDED ||
        device_resume(&device) != K_OK ||
        atomic_load_explicit(&device.state, memory_order_acquire) != DEVICE_ACTIVE ||
        g_canonical_device_power_state != DEVICE_POWER_ACTIVE) goto cleanup;
    if (device_reset(&device, 1U) != K_OK ||
        atomic_load_explicit(&device.state, memory_order_acquire) != DEVICE_ACTIVE ||
        device_reset(&device, 3U) != K_EINVAL ||
        atomic_load_explicit(&device.state, memory_order_acquire) != DEVICE_FAILED ||
        device_reset(&device, 1U) != K_OK ||
        atomic_load_explicit(&device.state, memory_order_acquire) != DEVICE_ACTIVE) goto cleanup;
    if (power_system_suspend() != K_OK ||
        power_get_system_state() != POWER_SYSTEM_SUSPENDED ||
        atomic_load_explicit(&device.state, memory_order_acquire) != DEVICE_SUSPENDED ||
        power_system_resume() != K_OK ||
        power_get_system_state() != POWER_SYSTEM_RUNNING ||
        atomic_load_explicit(&device.state, memory_order_acquire) != DEVICE_ACTIVE) goto cleanup;

    device_object_init(&async_device, 0xD00EULL, 0x0100U,
                       &g_canonical_async_device_ops, 0);
    if (device_register(&async_device) != K_OK) goto cleanup;
    async_device_registered = 1;
    if (atomic_load_explicit(&async_device.state, memory_order_acquire) != DEVICE_ACTIVE ||
        async_device.power_device == 0 ||
        device_suspend_timeout(&async_device, 1U) != K_ETIMEDOUT ||
        atomic_load_explicit(&async_device.state, memory_order_acquire) != DEVICE_FAILED ||
        device_reset(&async_device, 1U) != K_OK ||
        device_suspend(&async_device) != K_OK ||
        g_canonical_async_power_polls < 2U ||
        atomic_load_explicit(&async_device.state, memory_order_acquire) != DEVICE_SUSPENDED ||
        device_resume(&async_device) != K_OK ||
        atomic_load_explicit(&async_device.state, memory_order_acquire) != DEVICE_ACTIVE) {
        goto cleanup;
    }

    device_object_init(&pending_device, 0xD00FULL, 0x0100U,
                       &g_canonical_pending_device_ops, 0);
    if (device_register(&pending_device) != K_OK) goto cleanup;
    pending_device_registered = 1;
    io_request_t pending_request;
    io_request_init(&pending_request, IO_IOCTL, &pending_device, 0, 0, 0);
    pending_request.cancel = canonical_test_cancel;
    atomic_store_explicit(&g_canonical_cancel_calls, 0U, memory_order_relaxed);
    if (io_submit(&pending_request) != K_OK ||
        atomic_load_explicit(&pending_request.state, memory_order_acquire) !=
            IOREQ_SUBMITTED ||
        atomic_load_explicit(&pending_device.io_inflight, memory_order_acquire) != 1U) {
        goto cleanup;
    }
    if (device_reset(&pending_device, 1U) != K_EBUSY ||
        atomic_load_explicit(&pending_device.state, memory_order_acquire) !=
            DEVICE_ACTIVE) goto cleanup;
    device_unregister(&pending_device);
    pending_device_registered = 0;
    if (atomic_load_explicit(&pending_request.state, memory_order_acquire) !=
            IOREQ_COMPLETED || pending_request.status != K_EDEVREMOVED ||
        atomic_load_explicit(&g_canonical_cancel_calls, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&pending_device.io_inflight, memory_order_acquire) != 0U ||
        pending_request.device_ref_held != 0U) goto cleanup;

    page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (page == 0) goto cleanup;
    pages[0] = page;
    if (dma_map_pages(&device, pages, 1, DMA_BIDIRECTIONAL, mapping) != K_OK) {
        goto cleanup;
    }
    mapped = 1;
    if (!dma_mapping_active(mapping) || mapping->segment_count != 1U ||
        mapping->segments[0].addr.value == 0 || mapping->mapped_length != PAGE_SIZE) {
        goto cleanup;
    }
    if (dma_validate_access(&device, mapping->segments[0].addr, sizeof(buffer),
                            DMA_DEVICE_READ) != K_OK ||
        dma_validate_access(&device,
                            iova_make(mapping->segments[0].addr.value + PAGE_SIZE),
                            sizeof(buffer), DMA_DEVICE_READ) != K_EACCES) {
        goto cleanup;
    }
    if (dma_validate_access(&device, iova_make(UINT64_MAX - 3U), 8U,
                            DMA_DEVICE_READ) != K_EINVAL) goto cleanup;
    dma_sync_for_device(mapping);
    dma_sync_for_cpu(mapping);
    duplicate_pages[0] = page;
    duplicate_pages[1] = page;
    if (dma_map_pages(&device, duplicate_pages, 2, DMA_TO_DEVICE, &duplicate) != K_EINVAL) {
        goto cleanup;
    }

    io_request_init(&request, IO_READ, &device, 0, &io_vector, 1U);
    bio_vector.page = page;
    bio_vector.offset = 0;
    bio_vector.length = sizeof(buffer);
    bio.lba = 0;
    bio.op = BIO_OP_READ;
    bio.vecs = &bio_vector;
    bio.vec_count = 1U;
    bio.io = &request;
    list_init(&bio.node);
    benchmark_start = telemetry_timestamp();
    submit_status = block_submit_bio(&bio);
    kernel_perf_emit_scope("io.request_submit", benchmark_start);
    if (submit_status != K_OK ||
        atomic_load_explicit(&request.state, memory_order_acquire) != IOREQ_COMPLETED ||
        request.status != K_OK || request.bytes_done != sizeof(buffer) ||
        buffer[0] != 0x5AU) goto cleanup;

    for (uint32_t i = 0; i < 2U; ++i) {
        io_request_init(&batch_requests[i], IO_READ, &device, 0,
                        &io_vector, 1U);
        batch_vectors[i] = bio_vector;
        batch_bios[i].lba = i + 1U;
        batch_bios[i].op = BIO_OP_READ;
        batch_bios[i].vecs = &batch_vectors[i];
        batch_bios[i].vec_count = 1U;
        batch_bios[i].io = &batch_requests[i];
        list_init(&batch_bios[i].node);
    }
    uint32_t batch_submitted = 0U;
    if (block_submit_bio_batch(batch_bios, 2U, &batch_submitted) != K_OK ||
        batch_submitted != 2U ||
        atomic_load_explicit(&batch_requests[0].state, memory_order_acquire) !=
            IOREQ_COMPLETED ||
        atomic_load_explicit(&batch_requests[1].state, memory_order_acquire) !=
            IOREQ_COMPLETED) goto cleanup;

    io_request_init(&cancel_request, IO_IOCTL, &device, 0, 0, 0);
    cancel_request.cancel = canonical_test_cancel;
    atomic_store_explicit(&g_canonical_cancel_calls, 0U, memory_order_relaxed);
    if (io_submit(&cancel_request) != K_OK || io_cancel(&cancel_request) != K_OK ||
        atomic_load_explicit(&cancel_request.state, memory_order_acquire) != IOREQ_COMPLETED ||
        cancel_request.status != K_ECANCELED ||
        atomic_load_explicit(&g_canonical_cancel_calls, memory_order_relaxed) != 1U) goto cleanup;

    if (device.ops == 0 || device.ops->reset == 0 ||
        device.ops->reset(&device, 1U) != K_OK) goto cleanup;
    device_unregister(&device);
    device_registered = 0;
    device_unregister(&async_device);
    async_device_registered = 0;
    if (dma_validate_access(&device, mapping->segments[0].addr, sizeof(buffer),
                            DMA_DEVICE_READ) != K_EDEVREMOVED) goto cleanup;
    if (dma_unmap_checked(mapping) == K_OK) {
        mapped = 0;
        kfree(mapping);
        mapping = 0;
    }
    io_request_t removed_request;
    io_request_init(&removed_request, IO_IOCTL, &device, 0, 0, 0);
    if (io_submit(&removed_request) != K_EDEVREMOVED ||
        atomic_load_explicit(&removed_request.state, memory_order_acquire) != IOREQ_COMPLETED ||
        removed_request.status != K_EDEVREMOVED) goto cleanup;
    success = true;

cleanup:
    if (mapped && mapping != 0 && dma_unmap_checked(mapping) == K_OK) {
        kfree(mapping);
        mapping = 0;
    }
    if (!mapped && mapping != 0) {
        kfree(mapping);
        mapping = 0;
    }
    if (page != 0) page_free(page);
    if (async_device_registered) device_unregister(&async_device);
    if (pending_device_registered) device_unregister(&pending_device);
    if (device_registered) device_unregister(&device);
    if (driver_registered) driver_unregister(&driver);
    return success;
}

bool kernel_init_pci_self_test(void) {
    const pci_host_t *host = pci_current_host();
    if (host == 0 || host->device_count == 0U) return 0;
    return pci_find_class(host, 0x01U, 0x06U, 0xFFU) != 0 ||
           pci_find_class(host, 0x03U, 0x00U, 0xFFU) != 0;
}

bool kernel_init_nvme_self_test(void) {
    /* 规范 NVMe 所有者已在 init_storage 阶段验证命令提交和控制器状态。
     * 此处保留低成本、可重复的所有权检查，不恢复旧版环模型。 */
    return nvme_driver_self_test();
}

BOOLEAN kernel_init_post_scheduler(const OS_BOOT_INFO *info,
                                   const nvme_controller_t *active_controller) {
    UINT64 benchmark_start;

    realtest_mark("POST_NVME_IO_BEGIN");
    benchmark_start = telemetry_timestamp();
    if (!nvme_hardware_io_self_test()) {
        realtest_mark("POST_NVME_IO_FAIL");
        kernel_serial_write("OS_NVME_IO_FAIL STATUS=");
        kernel_serial_write_u32((UINT32)(-nvme_last_error()));
        kernel_serial_write(" STAGE=");
        kernel_serial_write_u32(nvme_last_stage());
        kernel_serial_write(" CQ=");
        kernel_serial_write_u32(nvme_last_completion_status());
        kernel_serial_write("\r\n");
        halt_forever();
    }
    kernel_perf_emit_scope("io.nvme", benchmark_start);
    if (nvme_hardware_present()) kernel_serial_write("OS_NVME_IO_OK\r\n");
    realtest_mark("POST_NVME_IO_OK");

    realtest_mark("POST_NVME_RESET_BEGIN");
    benchmark_start = telemetry_timestamp();
    if (!nvme_hardware_reset_self_test()) {
        realtest_mark("POST_NVME_RESET_FAIL");
        kernel_serial_write("OS_NVME_RESET_FAIL\r\n");
        halt_forever();
    }
    kernel_perf_emit_scope("io.nvme_reset", benchmark_start);
    if (nvme_hardware_present()) kernel_serial_write("OS_NVME_RESET_OK\r\n");
    realtest_mark("POST_NVME_RESET_OK");

    kernel_serial_write("OS_VFS_CORE_OK\r\n");
    realtest_mark("POST_IO_CORE_BEGIN");
    benchmark_start = telemetry_timestamp();
    if (!kernel_init_canonical_device_dma_io_self_test()) {
        realtest_mark("POST_IO_CORE_FAIL");
        kernel_serial_write("OS_IO_TEST_FAIL\r\n");
        halt_forever();
    }
    kernel_perf_emit_scope("io.core", benchmark_start);
    kernel_serial_write("OS_IO_OK\r\n");
    kernel_serial_write("OS_DMA_IO_CORE_OK\r\n");
    kernel_serial_write("OS_DRIVER_OK\r\n");
    realtest_mark("POST_IO_CORE_OK");
    kernel_debug_stage(OS_DEBUG_PHASE_DRIVER,
                       OS_DEBUG_STEP_READY, 1U);

    realtest_mark("POST_COMPLETION_BEGIN");
    if (!completion_port_self_test()) {
        realtest_mark("POST_COMPLETION_FAIL");
        kernel_serial_write("OS_COMPLETION_PORT_FAIL\r\n");
        halt_forever();
    }
    kernel_serial_write("OS_COMPLETION_PORT_OK\r\n");
    realtest_mark("POST_COMPLETION_OK");
    realtest_mark("POST_MESSAGE_BEGIN");
    if (!message_port_self_test()) {
        realtest_mark("POST_MESSAGE_FAIL");
        kernel_serial_write("OS_MESSAGE_PORT_FAIL\r\n");
        halt_forever();
    }
    kernel_serial_write("OS_MESSAGE_PORT_OK\r\n");
    realtest_mark("POST_MESSAGE_OK");
    realtest_mark("POST_TIMER_BEGIN");
    if (!timer_self_test()) {
        realtest_mark("POST_TIMER_FAIL");
        kernel_serial_write("OS_TIMER_FAIL\r\n");
        halt_forever();
    }
    kernel_serial_write("OS_TIMER_OK\r\n");
    realtest_mark("POST_TIMER_OK");
    realtest_mark("POST_PCI_BEGIN");
    if (!kernel_init_pci_self_test()) {
        realtest_mark("POST_PCI_FAIL");
        kernel_serial_write("OS_PCI_TEST_FAIL\r\n");
        halt_forever();
    }
    kernel_serial_write("OS_PCI_OK\r\n");
    realtest_mark("POST_PCI_OK");

    realtest_mark("POST_NVME_CORE_BEGIN");
    benchmark_start = telemetry_timestamp();
    if (!kernel_init_nvme_self_test()) {
        realtest_mark("POST_NVME_CORE_FAIL");
        kernel_serial_write("OS_NVME_TEST_FAIL\r\n");
        halt_forever();
    }
    kernel_perf_emit_scope("io.nvme_core", benchmark_start);
    kernel_serial_write("OS_NVME_OK\r\n");
    realtest_mark("POST_NVME_CORE_OK");
    /* NVMe 完成 IRQ/CQ/延迟处理的所有权现在可独立定位。 */
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 19U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 2U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 3U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 4U);
    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_9);
    kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_15);

    realtest_mark("POST_AUDIO_BEGIN");
    if (!audio_core_init() || !audio_core_self_test()) {
        realtest_mark("POST_AUDIO_FAIL");
        kernel_serial_write("OS_AUDIO_CORE_FAIL\r\n");
        halt_forever();
    }
    kernel_serial_write("OS_AUDIO_CORE_OK\r\n");
    realtest_mark("POST_AUDIO_OK");
    realtest_mark("POST_HDA_BEGIN");
    if (!hda_hardware_self_test()) {
        realtest_mark("POST_HDA_FAIL");
        kernel_serial_write("OS_HDA_FAIL=");
        kernel_serial_write_u32(hda_last_error());
        kernel_serial_write("\r\n");
        kernel_serial_write("OS_HDA_UNAVAILABLE\r\n");
        realtest_mark("POST_HDA_UNAVAILABLE");
    } else if (hda_hardware_present()) {
        kernel_serial_write("OS_HDA_HW_OK OUT=");
        kernel_serial_write_u32(hda_output_stream_count());
        kernel_serial_write(" IN=");
        kernel_serial_write_u32(hda_input_stream_count());
        kernel_serial_write("\r\n");
        if (!hda_pcm_self_test()) {
            realtest_mark("POST_HDA_PCM_FAIL");
            kernel_serial_write("OS_HDA_PCM_FAIL=");
            kernel_serial_write_u32(hda_last_error());
            kernel_serial_write("\r\nOS_HDA_UNAVAILABLE\r\n");
            realtest_mark("POST_HDA_UNAVAILABLE");
        } else {
            kernel_serial_write("OS_HDA_PCM_OK\r\n");
            realtest_mark("POST_HDA_OK");
        }
    } else {
        kernel_serial_write("OS_HDA_ABSENT\r\n");
        realtest_mark("POST_HDA_OK");
    }
    realtest_mark("POST_BLUETOOTH_BEGIN");
    if (!bluetooth_core_self_test()) {
        realtest_mark("POST_BLUETOOTH_FAIL");
        kernel_serial_write("OS_BLUETOOTH_CORE_FAIL\r\n");
        kernel_serial_write("OS_BLUETOOTH_UNAVAILABLE\r\n");
        realtest_mark("POST_BLUETOOTH_UNAVAILABLE");
    } else {
        kernel_serial_write("OS_BLUETOOTH_CORE_OK\r\n");
        realtest_mark("POST_BLUETOOTH_OK");
    }
    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_15);

    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_8);
    realtest_mark("POST_PACKAGE_BEGIN");
    if (!package_core_self_test()) {
        realtest_mark("POST_PACKAGE_FAIL");
        kernel_serial_write("OS_PACKAGE_CORE_FAIL\r\n");
        halt_forever();
    }
    kernel_serial_write("OS_PACKAGE_CORE_OK\r\n");
    realtest_mark("POST_PACKAGE_OK");
    realtest_mark("POST_FIRMWARE_BEGIN");
    if (!firmware_core_self_test()) {
        realtest_mark("POST_FIRMWARE_FAIL");
        kernel_serial_write("OS_FIRMWARE_CORE_FAIL\r\n");
        halt_forever();
    }
    kernel_serial_write("OS_FIRMWARE_CORE_OK\r\n");
    realtest_mark("POST_FIRMWARE_OK");
    realtest_mark("POST_RCU_BEGIN");
    if (!rcu_self_test()) {
        realtest_mark("POST_RCU_FAIL");
        kernel_serial_write("OS_RCU_FAIL\r\n");
        halt_forever();
    }
    kernel_serial_write("OS_RCU_OK\r\n");
    realtest_mark("POST_RCU_OK");
    realtest_mark("POST_TELEMETRY_BEGIN");
    if (!telemetry_self_test()) {
        realtest_mark("POST_TELEMETRY_FAIL");
        kernel_serial_write("OS_TELEMETRY_FAIL\r\n");
        halt_forever();
    }
    kernel_serial_write("OS_TELEMETRY_OK\r\n");
    realtest_mark("POST_TELEMETRY_OK");

#if OS_REALTEST || OS_DEBUG_SERIAL
    realtest_mark("POST_PERF_BEGIN");
    kernel_perf_report_t perf_report;
    if (!kernel_perf_benchmark(&perf_report)) {
        realtest_mark("POST_PERF_FAIL");
        kernel_serial_write("OS_PERF_KMALLOC_FAIL\r\n");
        halt_forever();
    }
    kernel_perf_emit_report(&perf_report);
    realtest_mark("POST_PERF_OK");
#endif
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_0,
                       OS_DEBUG_STEP_PROGRESS, 1U);
    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_18);

    realtest_mark("POST_RESOURCE_BEGIN");
    if (!resource_core_self_test()) {
        realtest_mark("POST_RESOURCE_FAIL");
        kernel_serial_write("OS_RESOURCE_CORE_FAIL\r\n");
        halt_forever();
    }
    kernel_serial_write("OS_RESOURCE_CORE_OK\r\n");
    realtest_mark("POST_RESOURCE_OK");
    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_16);

    kernel_init_filesystem_hooks_t filesystem_hooks = {
        .write = kernel_serial_write,
        .write_u32 = kernel_serial_write_u32,
        .halt = kernel_halt_forever,
    };
    realtest_mark("POST_FILESYSTEM_BEGIN");
    kernel_boot_begin(BOOT_PROFILE_FILESYSTEM);
    if (!kernel_init_filesystem(info, active_controller, &filesystem_hooks)) {
        realtest_mark("POST_FILESYSTEM_FAIL");
        halt_forever();
    }
    realtest_mark("POST_FILESYSTEM_OK");
    kernel_boot_end(BOOT_PROFILE_FILESYSTEM);
    kernel_init_network_hooks_t network_hooks = {
        .write = kernel_serial_write,
        .write_u32 = kernel_serial_write_u32,
        .halt = kernel_halt_forever,
    };
    realtest_mark("POST_NETWORK_BEGIN");
    kernel_boot_begin(BOOT_PROFILE_NETWORK);
    if (!kernel_init_network(&network_hooks)) {
        realtest_mark("POST_NETWORK_FAIL");
        kernel_serial_write("OS_NETWORK_UNAVAILABLE\r\n");
        realtest_mark("POST_NETWORK_UNAVAILABLE");
    } else {
        realtest_mark("POST_NETWORK_OK");
    }
    kernel_boot_end(BOOT_PROFILE_NETWORK);
    kernel_init_graphics_hooks_t graphics_hooks = {
        .write = kernel_serial_write,
        .write_u32 = kernel_serial_write_u32,
        .halt = kernel_halt_forever,
    };
    realtest_mark("POST_GRAPHICS_BEGIN");
    kernel_boot_begin(BOOT_PROFILE_GRAPHICS);
    if (!kernel_init_graphics(info, &graphics_hooks)) {
        realtest_mark("POST_GRAPHICS_FAIL");
        kernel_serial_write("OS_GRAPHICS_UNAVAILABLE\r\n");
        realtest_mark("POST_GRAPHICS_UNAVAILABLE");
    } else {
        realtest_mark("POST_GRAPHICS_OK");
    }
    kernel_init_userspace_hooks_t userspace_hooks = {
        .write = kernel_serial_write,
        .write_u32 = kernel_serial_write_u32,
        .halt = kernel_halt_forever,
    };
    realtest_mark("POST_USERSPACE_BEGIN");
    kernel_boot_end(BOOT_PROFILE_GRAPHICS);
    kernel_boot_begin(BOOT_PROFILE_USERSPACE);
    if (!kernel_init_userspace(&userspace_hooks)) {
        realtest_mark("POST_USERSPACE_FAIL");
        halt_forever();
    }
    realtest_mark("POST_USERSPACE_OK");
    kernel_boot_end(BOOT_PROFILE_USERSPACE);
    return 1;
}
