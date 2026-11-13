#include <arch/x86_64/cpu.h>
#include <kernel/perf.h>
#include <kernel/telemetry.h>
#include "internal.h"

/* REFACTOR_P8_NVME_SELF_TEST_OWNER：NVMe 数据路径和复位校验。 */

bool nvme_driver_self_test(void) {
    if (nvme_driver_register() != K_OK) return false;
    if (nvme_hardware_present() && nvme_active_controller() == 0) return false;
    return nvme_all_controllers_ready();
}

bool nvme_hardware_io_self_test(void) {
    const nvme_controller_t *controller = nvme_active_controller();
    if (controller == 0) return !nvme_hardware_present();
    return nvme_read_lba0_self_test(controller, UINT32_MAX);
}

bool nvme_hardware_reset_self_test(void) {
    const nvme_controller_t *before = nvme_active_controller();
    uint64_t benchmark_start;
    uint64_t io_operations = 0U;
    if (before == 0) return !nvme_hardware_present();
    /* 连续提交超过队列深度，验证 SQ/CQ 头尾指针和 phase 的环回。 */
    if (before->io_queue_count == 0U ||
        before->io_queue_count > NVME_MAX_IO_QUEUES) {
        return false;
    }
    benchmark_start = telemetry_timestamp();
    for (uint16_t queue = 0; queue < before->io_queue_count; ++queue) {
        const nvme_queue_t *state = &before->io_queues[queue];
        if (!state->active || state->queue_id != queue + 1U ||
            state->depth < 2U || state->depth > NVME_IO_QUEUE_DEPTH) {
            return false;
        }
        for (uint32_t i = 0; i <= state->depth; ++i) {
            if (!nvme_read_lba0_self_test(before, queue)) return false;
            ++io_operations;
        }
    }
    uint64_t elapsed_ns = x86_tsc_to_ns(
        telemetry_timestamp() - benchmark_start);
    uint64_t iops = 0U;
    if (elapsed_ns != 0U && elapsed_ns != UINT64_MAX &&
        io_operations <= UINT64_MAX / 1000000000ULL) {
        iops = (io_operations * 1000000000ULL) / elapsed_ns;
    }
    kernel_perf_emit_value("io.nvme_iops", iops);
    if (before->device == 0 || device_reset(before->device, 1U) != K_OK) {
        return false;
    }

    /* Reset 后保持同一控制器、命名空间和队列拓扑，再验证 MSI-X 回绑。 */
    const nvme_controller_t *after = nvme_active_controller();
    if (after != before || !after->started || !after->identified ||
        after->namespace_count == 0 || after->io_queue_count == 0) {
        return false;
    }
    if (after->io_queue_count != before->io_queue_count) return false;
    if (!nvme_msix_rebind_self_test(after)) return false;
    for (uint16_t queue = 0; queue < after->io_queue_count; ++queue) {
        const nvme_queue_t *state = &after->io_queues[queue];
        if (!state->active || state->queue_id != queue + 1U ||
            state->depth < 2U || state->depth > NVME_IO_QUEUE_DEPTH) {
            return false;
        }
        for (uint32_t i = 0; i <= state->depth; ++i) {
            if (!nvme_read_lba0_self_test(after, queue)) return false;
        }
    }
    return nvme_read_lba0_self_test(after, UINT32_MAX);
}
