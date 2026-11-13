#include <arch/x86_64/cpu.h>

#include "internal.h"

/*
 * REFACTOR_P8_NVME_TIMING_OWNER：截止时间计算和命令 ID 分配与控制器生命周期及 I/O
 * 完成处理相互独立。
 */

bool nvme_deadline_reached(uint64_t deadline) {
    return (int64_t)(x86_read_tsc() - deadline) >= 0;
}

uint64_t nvme_timeout_deadline(const nvme_controller_t *controller) {
    uint64_t units = (controller->capabilities >> 24) & 0xFFU;
    uint64_t timeout_ns = units == 0 ? 5000000000ULL : units * 500000000ULL;
    uint64_t ticks = x86_timeout_ns_to_tsc(timeout_ns);
    uint64_t now = x86_read_tsc();
    return ticks > UINT64_MAX - now ? UINT64_MAX : now + ticks;
}

uint16_t nvme_next_command_id(nvme_controller_t *controller) {
    unsigned value = atomic_fetch_add_explicit(&controller->next_command_id, 1U,
                                               memory_order_relaxed);
    uint16_t command_id = (uint16_t)value;
    if (command_id == 0U) command_id = 1U;
    return command_id;
}
