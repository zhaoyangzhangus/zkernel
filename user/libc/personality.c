#include "libc/libc.h"

#include <uapi/linux_abi.h>

bool __libc_linux_personality(void) {
    /* ELF 解释器会在进入程序前启用 Linux 系统调用编号。此进程级属性在整个生命周期内
     * 保持稳定
     * the libc image, so cache the probe shared by malloc, futex, mmap, and
     * socket hot paths instead of issuing a kernel call at every operation. */
    static int cached_personality = -1;
    int personality = __atomic_load_n(&cached_personality, __ATOMIC_ACQUIRE);
    if (personality < 0) {
        int detected = __libc_linux_syscall6(OS_SYS_DEBUG_QUERY,
                                             0U, 0U, 0U, 0U, 0U, 0U) == -38LL;
        int expected = -1;
        if (__atomic_compare_exchange_n(&cached_personality, &expected,
                                        detected, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            personality = detected;
        } else {
            personality = expected;
        }
    }
    return personality != 0;
}
