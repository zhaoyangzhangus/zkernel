#include "libc/libc.h"

#include <uapi/linux_abi.h>

static unsigned int g_probe_count;
static int g_bad_probe;
static int64_t g_probe_result;

int64_t __libc_linux_syscall6(uint64_t number, uint64_t argument0,
                              uint64_t argument1, uint64_t argument2,
                              uint64_t argument3, uint64_t argument4,
                              uint64_t argument5) {
    ++g_probe_count;
    if (number != OS_SYS_DEBUG_QUERY || argument0 != 0U || argument1 != 0U ||
        argument2 != 0U || argument3 != 0U || argument4 != 0U ||
        argument5 != 0U) {
        g_bad_probe = 1;
    }
    return g_probe_result;
}

int main(int argc, char **argv) {
    bool expected_linux = argc > 1 && argv[1] != 0 && argv[1][0] == 'l';
    g_probe_result = expected_linux ? -38LL : 0LL;

    for (unsigned int iteration = 0U; iteration < 1024U; ++iteration) {
        if (__libc_linux_personality() != expected_linux) return 1;
    }
    if (g_probe_count != 1U || g_bad_probe) return 2;

    /* 加载器激活后不能更改进程 personality。改变模拟结果可以捕获缓存预热后意外
     * 再次探测的情况。 */
    g_probe_result = expected_linux ? 0LL : -38LL;
    if (__libc_linux_personality() != expected_linux || g_probe_count != 1U)
        return 3;
    return 0;
}
