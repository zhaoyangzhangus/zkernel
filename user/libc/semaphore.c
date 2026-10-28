#include "libc/libc.h"

#include <semaphore.h>

#include <uapi/linux_abi.h>

static bool semaphore_linux_personality(void) {
    return __libc_linux_personality();
}

static uint64_t semaphore_linux_timeout_pointer(uint64_t timeout_ns,
                                                struct timespec *timeout) {
    if (timeout_ns == OS_WAIT_INFINITE || timeout == 0) return 0U;
    timeout->tv_sec = (time_t)(timeout_ns / 1000000000ULL);
    timeout->tv_nsec = (long)(timeout_ns % 1000000000ULL);
    return (uint64_t)(uintptr_t)timeout;
}

/* 有意保持信号量 ABI 精简：semaphore.h 只公开一个进程内计数器，
 * 阻塞操作由 System futex 原语提供。 */
static int semaphore_futex_wait(volatile uint32_t *address,
                                uint32_t expected,
                                uint64_t timeout_ns) {
    int64_t status;
    if (semaphore_linux_personality()) {
        struct timespec timeout;
        uint64_t timeout_pointer = semaphore_linux_timeout_pointer(
            timeout_ns, &timeout);
        status = __libc_linux_syscall6(
            LINUX_SYS_FUTEX, (uint64_t)(uintptr_t)address,
            LINUX_FUTEX_WAIT | LINUX_FUTEX_PRIVATE_FLAG, expected,
            timeout_pointer, 0U, 0U);
    } else {
        status = __libc_native_syscall6(
            OS_SYS_FUTEX_WAIT, (uint64_t)(uintptr_t)address, expected,
            timeout_ns, 0U, 0U, 0U);
    }
    if (status >= 0) return 0;
    (void)__libc_status_result(status);
    return errno;
}

static int semaphore_futex_wake(volatile uint32_t *address) {
    int64_t status = semaphore_linux_personality() ?
        __libc_linux_syscall6(LINUX_SYS_FUTEX,
                        (uint64_t)(uintptr_t)address,
                        LINUX_FUTEX_WAKE | LINUX_FUTEX_PRIVATE_FLAG, 1U,
                        0U, 0U, 0U) :
        __libc_native_syscall6(OS_SYS_FUTEX_WAKE, (uint64_t)(uintptr_t)address, 1U,
                        0U, 0U, 0U, 0U);
    if (status >= 0) return 0;
    (void)__libc_status_result(status);
    return errno;
}

static int semaphore_absolute_timeout(const struct timespec *absolute,
                                      uint64_t *timeout_ns) {
    struct timespec now;
    uint64_t seconds;
    uint64_t nanoseconds;

    if (absolute == 0 || timeout_ns == 0 || absolute->tv_sec < 0 ||
        absolute->tv_nsec < 0 || absolute->tv_nsec >= 1000000000L) {
        return EINVAL;
    }
    if (clock_gettime(CLOCK_REALTIME, &now) < 0) return errno;
    if (absolute->tv_sec < now.tv_sec ||
        (absolute->tv_sec == now.tv_sec &&
         absolute->tv_nsec <= now.tv_nsec)) {
        *timeout_ns = 0U;
        return 0;
    }

    seconds = (uint64_t)(absolute->tv_sec - now.tv_sec);
    if (absolute->tv_nsec < now.tv_nsec) {
        --seconds;
        nanoseconds = (uint64_t)absolute->tv_nsec + 1000000000ULL -
                      (uint64_t)now.tv_nsec;
    } else {
        nanoseconds = (uint64_t)absolute->tv_nsec -
                      (uint64_t)now.tv_nsec;
    }
    if (seconds > (UINT64_MAX - nanoseconds) / 1000000000ULL) {
        *timeout_ns = UINT64_MAX;
    } else {
        *timeout_ns = seconds * 1000000000ULL + nanoseconds;
    }
    return 0;
}

int sem_init(sem_t *semaphore, int pshared, unsigned int value) {
    if (semaphore == 0 || pshared != 0 || value > SEM_VALUE_MAX) {
        errno = EINVAL;
        return -1;
    }
    __atomic_store_n(&semaphore->value, (uint32_t)value, __ATOMIC_RELAXED);
    return 0;
}

int sem_destroy(sem_t *semaphore) {
    if (semaphore == 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int sem_wait(sem_t *semaphore) {
    if (semaphore == 0) {
        errno = EINVAL;
        return -1;
    }
    for (;;) {
        uint32_t value = __atomic_load_n(&semaphore->value, __ATOMIC_ACQUIRE);
        while (value != 0U) {
            if (__atomic_compare_exchange_n(&semaphore->value, &value,
                                            value - 1U, false,
                                            __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED)) {
                return 0;
            }
        }
        int error = semaphore_futex_wait(&semaphore->value, 0U,
                                         OS_WAIT_INFINITE);
        if (error == 0 || error == EAGAIN) continue;
        errno = error;
        return -1;
    }
}

int sem_trywait(sem_t *semaphore) {
    uint32_t value;
    if (semaphore == 0) {
        errno = EINVAL;
        return -1;
    }
    value = __atomic_load_n(&semaphore->value, __ATOMIC_ACQUIRE);
    while (value != 0U) {
        if (__atomic_compare_exchange_n(&semaphore->value, &value,
                                        value - 1U, false,
                                        __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            return 0;
        }
    }
    errno = EAGAIN;
    return -1;
}

int sem_timedwait(sem_t *semaphore, const struct timespec *absolute_timeout) {
    if (semaphore == 0 || absolute_timeout == 0) {
        errno = EINVAL;
        return -1;
    }
    for (;;) {
        uint64_t timeout_ns = 0U;
        uint32_t value = __atomic_load_n(&semaphore->value, __ATOMIC_ACQUIRE);
        while (value != 0U) {
            if (__atomic_compare_exchange_n(&semaphore->value, &value,
                                            value - 1U, false,
                                            __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED)) {
                return 0;
            }
        }
        int error = semaphore_absolute_timeout(absolute_timeout, &timeout_ns);
        if (error != 0) {
            errno = error;
            return -1;
        }
        if (timeout_ns == 0U) {
            errno = ETIMEDOUT;
            return -1;
        }
        error = semaphore_futex_wait(&semaphore->value, 0U, timeout_ns);
        if (error == 0 || error == EAGAIN) continue;
        errno = error;
        return -1;
    }
}

int sem_post(sem_t *semaphore) {
    uint32_t value;
    if (semaphore == 0) {
        errno = EINVAL;
        return -1;
    }
    value = __atomic_load_n(&semaphore->value, __ATOMIC_RELAXED);
    for (;;) {
        if (value >= SEM_VALUE_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
        if (__atomic_compare_exchange_n(&semaphore->value, &value, value + 1U,
                                        false, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED)) {
            int error = semaphore_futex_wake(&semaphore->value);
            if (error != 0) {
                errno = error;
                return -1;
            }
            return 0;
        }
    }
}

int sem_getvalue(const sem_t *semaphore, int *value) {
    if (semaphore == 0 || value == 0) {
        errno = EINVAL;
        return -1;
    }
    *value = (int)__atomic_load_n(&semaphore->value, __ATOMIC_ACQUIRE);
    return 0;
}
