/* REFACTOR_SYSCALL_DEBUG_OWNER：有界用户诊断仅写入串口。 */

#include <arch/x86_64/uaccess.h>
#include <kernel/console.h>
#include <kernel/process.h>
#include <kernel/power.h>

#include "internal.h"

#define DEBUG_WRITE_MAX_BYTES 240U

int64_t syscall_system_power(uint64_t action, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    if (action > 1U) return K_EINVAL;
    if (current_process() == 0) return K_EPERM;
    return power_system_request((uint32_t)action);
}

#ifndef OS_DEBUG_SERIAL
#define OS_DEBUG_SERIAL 0
#endif

/* 临时捕获仅供 Chrome 使用的 stderr，以诊断来宾中止。 */
#define OS_CHROME_DEBUG_SERIAL 1

static bool debug_write_from_chrome(void) {
    static const char chrome_path[] = "/usr/apps/chrome/chrome";
    process_t *process = current_process();
    if (process == 0) return false;
    for (size_t index = 0U; index < sizeof(chrome_path); ++index) {
        if (process->linux_exe[index] != chrome_path[index]) return false;
    }
    return true;
}

int64_t syscall_debug_write(uint64_t buffer, uint64_t length,
                            uint64_t unused2, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5) {
    char text[DEBUG_WRITE_MAX_BYTES + 1U];
    kstatus_t status;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (length > DEBUG_WRITE_MAX_BYTES) return K_EINVAL;
    if (length == 0U) return 0;
    status = copy_from_user(text, (const void __user *)(uintptr_t)buffer,
                            (size_t)length);
    if (status != K_OK) return status;
    text[length] = '\0';
#if OS_DEBUG_SERIAL
    kernel_serial_write_serial_only(text);
#elif OS_CHROME_DEBUG_SERIAL
    if (debug_write_from_chrome()) {
        /* serial_only itself is gated off in normal builds. Use the same
         * bounded capture as Linux write(2), without enabling global UART
         * logging or drawing diagnostics into the framebuffer. */
        linux_chrome_stderr_write(current_process(), text, (size_t)length);
    }
#else
    (void)text;
#endif
    return (int64_t)length;
}
