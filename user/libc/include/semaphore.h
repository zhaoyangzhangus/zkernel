#pragma once

#include <errno.h>
#include <stdint.h>
#include <time.h>

/* 由 System futex 原语支持的进程本地 POSIX 信号量。 */
typedef struct {
    volatile uint32_t value;
} sem_t;

#ifndef SEM_VALUE_MAX
#define SEM_VALUE_MAX 0x7fffffffU
#endif

#ifdef __cplusplus
extern "C" {
#endif
int sem_init(sem_t *semaphore, int pshared, unsigned int value);
int sem_destroy(sem_t *semaphore);
int sem_wait(sem_t *semaphore);
int sem_trywait(sem_t *semaphore);
int sem_timedwait(sem_t *semaphore, const struct timespec *absolute_timeout);
int sem_post(sem_t *semaphore);
int sem_getvalue(const sem_t *semaphore, int *value);
#ifdef __cplusplus
}
#endif
