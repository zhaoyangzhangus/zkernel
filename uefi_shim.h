#ifndef ZKERNEL_UEFI_SHIM_H
#define ZKERNEL_UEFI_SHIM_H

/* Freestanding, single-core, non-reentrant UEFI test harness.
 * The allocator is called only from the UEFI main execution path;
 * no interrupt handler, timer callback, AP, or firmware event callback
 * may call into it concurrently.  Therefore locking is unnecessary.
 *
 * These functions intentionally compile to NO operations.  The native
 * POSIX build continues to use pthread_mutex_t through frame_pool.h.
 * NOT suitable for a preemptive or SMP zkernel kernel without synchronization.
 */
#include <stddef.h>
#include <stdint.h>
typedef struct { unsigned unused; } fm_mutex_t;
static inline int fm_mutex_init(fm_mutex_t *m, const void *attr) {
    (void)m; (void)attr;
    return 0;
}
static inline void fm_mutex_lock(fm_mutex_t *m) { (void)m; }
static inline void fm_mutex_unlock(fm_mutex_t *m) { (void)m; }
static inline int fm_mutex_destroy(fm_mutex_t *m) { (void)m; return 0; }

/* Freestanding runtime provided by uefi_main.c */
void *memset(void *dst, int val, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
#endif
