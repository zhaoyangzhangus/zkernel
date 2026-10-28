#include "libc/libc.h"
#include <uapi/linux_abi.h>

#define LIBC_ATEXIT_MAX 128U
#define LIBC_AT_QUICK_EXIT_MAX 32U

static void (*g_exit_handlers[LIBC_ATEXIT_MAX])(void);
static void (*g_quick_exit_handlers[LIBC_AT_QUICK_EXIT_MAX])(void);
static size_t g_exit_handler_count;
static size_t g_quick_exit_handler_count;
static bool g_exit_handlers_running;

int atexit(void (*function)(void)) {
    if (function == 0 || g_exit_handler_count >= LIBC_ATEXIT_MAX) {
        errno = EINVAL;
        return -1;
    }
    g_exit_handlers[g_exit_handler_count++] = function;
    return 0;
}

int at_quick_exit(void (*function)(void)) {
    if (function == 0 || g_quick_exit_handler_count >= LIBC_AT_QUICK_EXIT_MAX) {
        errno = EINVAL;
        return -1;
    }
    g_quick_exit_handlers[g_quick_exit_handler_count++] = function;
    return 0;
}

void __libc_run_exit_handlers(void) {
    if (g_exit_handlers_running) return;
    g_exit_handlers_running = true;
    while (g_exit_handler_count != 0U) {
        void (*function)(void) = g_exit_handlers[--g_exit_handler_count];
        if (function != 0) function();
    }
    (void)fflush(0);
    __libc_close_temporary_files();
}

void quick_exit(int status) {
    while (g_quick_exit_handler_count != 0U) {
        void (*function)(void) =
            g_quick_exit_handlers[--g_quick_exit_handler_count];
        if (function != 0) function();
    }
    _exit(status);
}

void _exit(int status) {
    /* 同一个 DSO 同时服务 System 原生服务和 Linux 兼容模式下的 Chromium。
     * OS_SYS_PROCESS_EXIT 在原生 ABI 中是系统调用 1，而 Linux 系统调用 1 是 write；
     * 发出终止调用前，先通过原生专用的高位系统调用探测并选择正确的退出 ABI。 */
    int64_t native_probe = __libc_linux_syscall6(
        OS_SYS_DEBUG_QUERY, 0U, 0U, 0U, 0U, 0U, 0U);
    if (native_probe == -38LL) {
        (void)__libc_linux_syscall6(
            LINUX_SYS_EXIT_GROUP, (uint64_t)(uint32_t)status,
                          0U, 0U, 0U, 0U, 0U);
    } else {
        (void)__libc_native_syscall6(
            OS_SYS_PROCESS_EXIT, (uint64_t)(uint32_t)status,
            0U, 0U, 0U, 0U, 0U);
    }
    for (;;) __asm__ volatile("pause");
}

void exit(int status) {
    __libc_run_exit_handlers();
    _exit(status);
}

void abort(void) {
    _exit(134);
}

void __assert_fail(const char *expression, const char *file,
                   int line, const char *function) {
    (void)fprintf(stderr, "assertion failed: %s (%s:%d in %s)\n",
                  expression != 0 ? expression : "(null)",
                  file != 0 ? file : "(unknown)", line,
                  function != 0 ? function : "(unknown)");
    _exit(134);
}
