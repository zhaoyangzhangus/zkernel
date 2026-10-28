#include <kernel/init_graphics.h>

#include <kernel/debug_stage.h>
#include <kernel/gpu.h>
#include <kernel/perf.h>
#include <kernel/telemetry.h>
#include <kernel/window_server.h>
#include <kernel/xhci.h>
#include <arch/x86_64/syscall_internal.h>

static BOOLEAN usb_self_test(void) {
    uint64_t benchmark_start = telemetry_timestamp();
    BOOLEAN event_queue_ok = xhci_event_queue_self_test();

    kernel_perf_emit_scope("io.xhci_events", benchmark_start);
    /* 硬件探测由 init_devices/xhci_hardware_self_test 所有。
     * 此边界仅确保与协议无关的 xHCI 原语行为确定，因此无需再建一套 PCI/环模型。 */
    if (!event_queue_ok ||
        !xhci_transfer_encode_self_test() ||
        !xhci_device_context_self_test() ||
        !xhci_endpoint_context_self_test()) return 0;
    return 1;
}

static BOOLEAN syscall_self_test(const OS_BOOT_INFO *info) {
    syscall_frame_t frame;
    for (UINTN i = 0; i < sizeof(frame); ++i) ((UINT8 *)&frame)[i] = 0;
    frame.rip = 0x10000ULL;
    frame.rsp = 0x20000ULL;
    frame.cs = 0x23ULL;
    frame.ss = 0x1BULL;
    frame.rflags = 0x202ULL;
    frame.rax = OS_SYS_DEBUG_QUERY;
    return info != 0 && syscall_init(info->BootstrapStackTop) &&
           syscall_dispatch(&frame) == OS_SYSCALL_ABI_VERSION &&
           x86_syscall_return_mode(&frame) == 1;
}

static BOOLEAN graphics_fail_at(const kernel_init_graphics_hooks_t *hooks,
                                const CHAR8 *message, const char *file,
                                uint32_t line) {
    kernel_debug_stage_fail_at(OS_DEBUG_PHASE_DISPLAY,
                               OS_DEBUG_STEP_FAIL, K_EIO, file, line);
    hooks->write(message);
    /* GOP 和 GPU 后端均为可选项。记录失败后继续启动，使无头机器仍可进入 shell 和服务。 */
    return 0;
}

#define graphics_fail(hooks, message) \
    graphics_fail_at((hooks), (message), __FILE__, __LINE__)

BOOLEAN kernel_init_graphics(const OS_BOOT_INFO *info,
                             const kernel_init_graphics_hooks_t *hooks) {
    uint64_t benchmark_start;
    if (info == 0 || hooks == 0 || hooks->write == 0 ||
        hooks->write_u32 == 0 || hooks->halt == 0) return 0;

    kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_13);
    kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_14);
    benchmark_start = telemetry_timestamp();
    if (!gpu_core_self_test()) {
        return graphics_fail(hooks, "OS_GPU_CORE_FAIL\r\n");
    }
    kernel_perf_emit_scope("graphics.gpu", benchmark_start);
    hooks->write("OS_GPU_OK\r\n");
    hooks->write("OS_GPU_CORE_OK\r\n");
    benchmark_start = telemetry_timestamp();
    if (!usb_self_test()) {
        return graphics_fail(hooks, "OS_USB_TEST_FAIL\r\n");
    }
    kernel_perf_emit_scope("io.xhci", benchmark_start);
    hooks->write("OS_USB_OK\r\n");
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 1U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 5U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 6U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 8U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 9U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 10U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 11U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 12U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 13U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 14U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 15U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 16U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_8,
                       OS_DEBUG_STEP_PROGRESS, 17U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7B,
                       OS_DEBUG_STEP_PROGRESS, 1U);
    /* 规范合成器负责不可变快照捕获和规划。 */
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7B,
                       OS_DEBUG_STEP_PROGRESS, 2U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7B,
                       OS_DEBUG_STEP_PROGRESS, 3U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7B,
                       OS_DEBUG_STEP_PROGRESS, 4U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7B,
                       OS_DEBUG_STEP_PROGRESS, 5U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7B,
                       OS_DEBUG_STEP_PROGRESS, 6U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7B,
                       OS_DEBUG_STEP_PROGRESS, 7U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7B,
                       OS_DEBUG_STEP_PROGRESS, 8U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7B,
                       OS_DEBUG_STEP_PROGRESS, 9U);
    if (!compositor_tile_self_test()) {
        return graphics_fail(hooks, "OS_TILE_METADATA_TEST_FAIL\r\n");
    }
    if (!desktop_alpha_self_test()) {
        return graphics_fail(hooks, "OS_ALPHA_COMPOSITION_TEST_FAIL\r\n");
    }
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7B,
                       OS_DEBUG_STEP_PROGRESS, 10U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7A,
                       OS_DEBUG_STEP_PROGRESS, 1U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7A,
                       OS_DEBUG_STEP_PROGRESS, 2U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7A,
                       OS_DEBUG_STEP_PROGRESS, 3U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7A,
                       OS_DEBUG_STEP_PROGRESS, 4U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7A,
                       OS_DEBUG_STEP_PROGRESS, 5U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7A,
                       OS_DEBUG_STEP_PROGRESS, 6U);
    if (!window_input_router_self_test()) {
        return graphics_fail(hooks, "OS_INPUT_ROUTER_TEST_FAIL\r\n");
    }
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7A,
                       OS_DEBUG_STEP_PROGRESS, 7U);
    if (!window_present_self_test()) {
        return graphics_fail(hooks, "OS_PRESENT_TEST_FAIL\r\n");
    }
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7A,
                       OS_DEBUG_STEP_PROGRESS, 8U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7A,
                       OS_DEBUG_STEP_PROGRESS, 9U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7A,
                       OS_DEBUG_STEP_PROGRESS, 10U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7A,
                       OS_DEBUG_STEP_PROGRESS, 11U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7A,
                       OS_DEBUG_STEP_PROGRESS, 12U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7A,
                       OS_DEBUG_STEP_PROGRESS, 13U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7A,
                       OS_DEBUG_STEP_PROGRESS, 14U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7A,
                       OS_DEBUG_STEP_PROGRESS, 15U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_7A,
                       OS_DEBUG_STEP_PROGRESS, 16U);
    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_14);
    benchmark_start = telemetry_timestamp();
    if (!syscall_frame_self_test() || !syscall_self_test(info)) {
        return graphics_fail(hooks, "OS_SYSCALL_TEST_FAIL\r\n");
    }
    kernel_perf_emit_scope("io.syscall", benchmark_start);
    hooks->write("OS_SYSCALL_OK\r\n");
    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_7);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_3,
                       OS_DEBUG_STEP_PROGRESS, 9U);
    return 1;
}
