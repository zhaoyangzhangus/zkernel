#include <kernel/init_early.h>

#include "arch/x86_64/apic.h"
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupt.h>
#include <kernel/crash_dump.h>
#include <kernel/console.h>
#include <kernel/debug_stage.h>
#include <kernel/deferred.h>
#include <kernel/irq.h>
#include <kernel/mm_boot.h>
#include <kernel/power.h>
#include <kernel/random.h>
#include <kernel/service.h>
#include <kernel/watchdog.h>

static BOOLEAN early_fail_at(const kernel_init_early_hooks_t *hooks,
                             const CHAR8 *message, const char *file,
                             uint32_t line) {
    kernel_debug_stage_fail_at(OS_DEBUG_PHASE_BOOT,
                               OS_DEBUG_STEP_FAIL, K_EIO, file, line);
    hooks->write(message);
    hooks->halt();
    return 0;
}

#define early_fail(hooks, message) \
    early_fail_at((hooks), (message), __FILE__, __LINE__)

static BOOLEAN apic_self_test(void) {
    return lapic_init(32U, 1000000U) &&
           lapic_tick_count() == 0;
}

static void publish_refactor_boot_snapshot(void) {
    /* 在可重复的基准基线提交前，刻意不宣称 Phase 0 已完成。其余记录会显示当前进度，
     * 即使后续阶段尚未开始。 */
    kernel_debug_stage_pending(OS_DEBUG_PHASE_REFACTOR_0);
    kernel_debug_stage_ready(OS_DEBUG_PHASE_REFACTOR_1);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_2,
                       OS_DEBUG_STEP_ENTER, 0U);
    kernel_debug_stage_pending(OS_DEBUG_PHASE_REFACTOR_3);
    kernel_debug_stage_pending(OS_DEBUG_PHASE_REFACTOR_4);
    kernel_debug_stage_pending(OS_DEBUG_PHASE_REFACTOR_5);
    kernel_debug_stage_pending(OS_DEBUG_PHASE_REFACTOR_6);
    kernel_debug_stage_pending(OS_DEBUG_PHASE_REFACTOR_7A);
    kernel_debug_stage_pending(OS_DEBUG_PHASE_REFACTOR_7B);
    kernel_debug_stage_pending(OS_DEBUG_PHASE_REFACTOR_8);
    kernel_debug_stage_pending(OS_DEBUG_PHASE_REFACTOR_9);
}

BOOLEAN kernel_init_early(OS_BOOT_INFO *info,
                          const kernel_init_early_hooks_t *hooks) {
    if (info == 0 || hooks == 0 || hooks->write == 0 || hooks->halt == 0) {
        return 0;
    }
    if (info->Magic != OS_BOOTINFO_MAGIC ||
        info->Version < OS_BOOTINFO_VERSION ||
        info->Size < sizeof(*info)) {
        return early_fail(hooks, "OS_KERNEL_BAD_BOOTINFO\r\n");
    }

    UINT64 kernel_end = info->KernelPhysicalBase + info->KernelSize;
    UINT64 kernel_virtual_end = info->KernelVirtualBase + info->KernelSize;
    if (info->KernelPhysicalBase == 0 || info->KernelSize == 0 ||
        kernel_end < info->KernelPhysicalBase || info->KernelVirtualBase == 0 ||
        kernel_virtual_end < info->KernelVirtualBase ||
        info->KernelEntry < info->KernelVirtualBase ||
        info->KernelEntry >= kernel_virtual_end) {
        return early_fail(hooks, "OS_KERNEL_BAD_RANGE\r\n");
    }
    if ((info->Flags & OS_BOOTINFO_HAS_BOOT_DEVICE) == 0 ||
        info->BootDevicePathSize < sizeof(EFI_DEVICE_PATH_PROTOCOL)) {
        return early_fail(hooks, "OS_BOOT_DEVICE_FAIL\r\n");
    }
    hooks->write("OS_KERNEL_OK\r\n");
    hooks->write("OS_BOOT_DEVICE_OK\r\n");
    hooks->write("Hello World!\r\n");

    if (!arch_cpu_init()) {
        return early_fail(hooks, "OS_CPU_INIT_FAIL\r\n");
    }
    random_init(info);
    kernel_serial_enable_concurrency();
    hooks->write("OS_CPU_INIT_OK\r\n");
    kernel_debug_stage(OS_DEBUG_PHASE_BOOT,
                       OS_DEBUG_STEP_READY, 1U);
    kernel_debug_stage(OS_DEBUG_PHASE_CPU,
                       OS_DEBUG_STEP_READY, 1U);
    publish_refactor_boot_snapshot();
    kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_0);
    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_0);
    kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_1);
    kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_16);
    kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_17);
    kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_18);

    if (!x86_cpu_hardening_self_test()) {
        return early_fail(hooks, "OS_HARDENING_FAIL\r\n");
    }
    hooks->write("OS_HARDENING_OK\r\n");
    if (!arch_set_kernel_stack(info->BootstrapStackBase,
                                      info->BootstrapStackSize)) {
        return early_fail(hooks, "OS_TSS_STACK_FAIL\r\n");
    }
    hooks->write("OS_TSS_OK\r\n");
    if (!x86_exception_self_test()) {
        return early_fail(hooks, "OS_EXCEPTION_TEST_FAIL\r\n");
    }
    hooks->write("OS_EXCEPTION_OK\r\n");
    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_1);
    if (!apic_self_test()) {
        return early_fail(hooks, "OS_APIC_TEST_FAIL\r\n");
    }
    hooks->write("OS_APIC_OK\r\n");
    if (!deferred_init() || !deferred_self_test()) {
        return early_fail(hooks, "OS_DEFERRED_WORK_FAIL\r\n");
    }
    hooks->write("OS_DEFERRED_WORK_OK\r\n");
    if (!service_manager_init() || !service_manager_self_test()) {
        return early_fail(hooks, "OS_SERVICE_CORE_FAIL\r\n");
    }
    hooks->write("OS_SERVICE_CORE_OK\r\n");
    if (!watchdog_manager_init() || !watchdog_self_test()) {
        return early_fail(hooks, "OS_WATCHDOG_FAIL\r\n");
    }
    hooks->write("OS_WATCHDOG_OK\r\n");
    if (!power_manager_init() || !power_self_test()) {
        return early_fail(hooks, "OS_POWER_CORE_FAIL\r\n");
    }
    hooks->write("OS_POWER_CORE_OK\r\n");
    if (!crash_dump_self_test()) {
        return early_fail(hooks, "OS_CRASH_DUMP_FAIL\r\n");
    }
    hooks->write("OS_CRASH_DUMP_OK\r\n");
    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_17);
    if (!irq_core_self_test()) {
        return early_fail(hooks, "OS_IRQ_CORE_FAIL\r\n");
    }
    hooks->write("OS_IRQ_CORE_OK\r\n");
    if (!arch_enable_kernel_paging(info)) {
        return early_fail(hooks, "OS_PAGING_INIT_FAIL\r\n");
    }
    /* 身份映射别名会一直保持有效，直到下一个 init 阶段建立最终直接映射和帧缓冲别名。 */
    hooks->write("OS_PAGING_OK\r\n");
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_3,
                       OS_DEBUG_STEP_PROGRESS, 1U);
    return 1;
}
