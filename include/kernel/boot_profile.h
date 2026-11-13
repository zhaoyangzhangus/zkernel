#pragma once

#include <kernel/telemetry.h>

/* 每个片段只由一个写入者写入，且每次引导仅写入一次。调试器停止时读取；启动关键路径上不分配内存、
 * 不输出 UART 日志，也不写入文件系统。结束值为零表示阶段尚未完成。片段可能重叠，记录的是包含式
 * 墙上时间，因此不能相加。 */
enum kernel_boot_phase {
    BOOT_PROFILE_TOTAL,
    BOOT_PROFILE_EARLY,
    BOOT_PROFILE_MEMORY,
    BOOT_PROFILE_DEVICES,
    BOOT_PROFILE_CORE,
    BOOT_PROFILE_STORAGE,
    BOOT_PROFILE_SCHEDULER,
    BOOT_PROFILE_POST_GATES,
    BOOT_PROFILE_FILESYSTEM,
    BOOT_PROFILE_GPU,
    BOOT_PROFILE_NETWORK,
    BOOT_PROFILE_GRAPHICS,
    BOOT_PROFILE_USERSPACE,
    BOOT_PROFILE_RUNTIME,
    BOOT_PROFILE_COUNT
};

typedef struct kernel_boot_span {
    volatile uint64_t begin_tsc;
    volatile uint64_t end_tsc;
} kernel_boot_span_t;

extern kernel_boot_span_t g_kernel_boot_profile[BOOT_PROFILE_COUNT];

static inline void kernel_boot_begin(enum kernel_boot_phase phase) {
    g_kernel_boot_profile[phase].begin_tsc = telemetry_timestamp();
}

static inline void kernel_boot_end(enum kernel_boot_phase phase) {
    g_kernel_boot_profile[phase].end_tsc = telemetry_timestamp();
}
