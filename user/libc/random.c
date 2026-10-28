#include "libc/libc.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

static bool random_rdrand_supported(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(1U), "c"(0U));
    (void)eax;
    (void)ebx;
    (void)edx;
    return (ecx & (1U << 30)) != 0U;
}

static bool random_rdrand64(uint64_t *value) {
    unsigned char success;
    __asm__ volatile ("rdrand %0; setc %1"
                      : "=r"(*value), "=qm"(success) : : "cc");
    return success != 0U;
}

/* OpenSSL 在 System 上使用的种子钩子。硬件随机源不可用时应报告失败，
 * 不能将 TSC 抖动伪装成熵。 */
int getentropy(void *buffer, size_t length) {
    uint8_t *output = (uint8_t *)buffer;
    if (buffer == 0 && length != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (length == 0U) return 0;
    if (length > 256U) {
        errno = EIO;
        return -1;
    }

    int64_t status = __libc_native_syscall6(OS_SYS_RANDOM_GET,
                                     (uint64_t)(uintptr_t)buffer, length,
                                     0U, 0U, 0U, 0U);
    if (status == (int64_t)length) return 0;
    if (status >= 0) {
        errno = EIO;
        return -1;
    }
    if (status != -ENOSYS) {
        errno = (int)-status;
        return -1;
    }
    if (!random_rdrand_supported()) {
        errno = ENOSYS;
        return -1;
    }
    while (length != 0U) {
        uint64_t value = 0U;
        bool generated = false;
        for (uint32_t attempt = 0U; attempt < 16U; ++attempt) {
            if (random_rdrand64(&value)) {
                generated = true;
                break;
            }
        }
        if (!generated) {
            errno = EAGAIN;
            return -1;
        }
        size_t count = length < sizeof(value) ? length : sizeof(value);
        for (size_t index = 0U; index < count; ++index) {
            output[index] = (uint8_t)(value >> (index * 8U));
        }
        output += count;
        length -= count;
    }
    return 0;
}
