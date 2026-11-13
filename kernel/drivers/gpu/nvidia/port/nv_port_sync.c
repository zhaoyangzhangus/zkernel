#include "nv_port.h"

#ifdef NV_PORT_HOST_TEST
#include <threads.h>
#endif

void nv_port_spin_init(nv_port_spinlock_t *lock) {
    if (lock != 0) spinlock_init(&lock->core);
}

void nv_port_spin_lock(nv_port_spinlock_t *lock) {
    if (lock != 0) spinlock_lock(&lock->core);
}

bool nv_port_spin_try_lock(nv_port_spinlock_t *lock) {
    return lock != 0 && spinlock_try_lock(&lock->core);
}

void nv_port_spin_unlock(nv_port_spinlock_t *lock) {
    if (lock != 0) spinlock_unlock(&lock->core);
}

uint64_t nv_port_spin_lock_irqsave(nv_port_spinlock_t *lock) {
#ifdef NV_PORT_HOST_TEST
    nv_port_spin_lock(lock);
    return 0U;
#else
    uint64_t flags;
    if (lock == 0) return 0U;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    sched_preempt_disable();
    nv_port_spin_lock(lock);
    return flags;
#endif
}

void nv_port_spin_unlock_irqrestore(nv_port_spinlock_t *lock,
                                    uint64_t flags) {
    if (lock == 0) return;
    nv_port_spin_unlock(lock);
#ifdef NV_PORT_HOST_TEST
    (void)flags;
#else
    sched_preempt_enable();
    if ((flags & (1ULL << 9)) != 0U) {
        __asm__ volatile ("sti" : : : "memory");
    } else {
        __asm__ volatile ("cli" : : : "memory");
    }
#endif
}

void nv_port_mutex_init(nv_port_mutex_t *mutex) {
    if (mutex == 0) return;
#ifdef NV_PORT_HOST_TEST
    atomic_init(&mutex->state, 0U);
#else
    kmutex_init(&mutex->core, true);
#endif
}

kstatus_t nv_port_mutex_try_lock(nv_port_mutex_t *mutex) {
    if (mutex == 0) return K_EINVAL;
#ifdef NV_PORT_HOST_TEST
    unsigned expected = 0U;
    return atomic_compare_exchange_strong_explicit(
               &mutex->state, &expected, 1U, memory_order_acquire,
               memory_order_relaxed) ? K_OK : K_EBUSY;
#else
    return kmutex_try_lock(&mutex->core);
#endif
}

kstatus_t nv_port_mutex_lock(nv_port_mutex_t *mutex, uint64_t timeout_ns) {
    if (mutex == 0) return K_EINVAL;
#ifdef NV_PORT_HOST_TEST
    uint64_t deadline = nv_port_deadline_after(timeout_ns);
    for (;;) {
        if (nv_port_mutex_try_lock(mutex) == K_OK) return K_OK;
        if (timeout_ns == 0U || nv_port_deadline_expired(deadline)) {
            return K_ETIMEDOUT;
        }
        thrd_yield();
    }
#else
    return kmutex_lock(&mutex->core, timeout_ns);
#endif
}

kstatus_t nv_port_mutex_unlock(nv_port_mutex_t *mutex) {
    if (mutex == 0) return K_EINVAL;
#ifdef NV_PORT_HOST_TEST
    unsigned expected = 1U;
    return atomic_compare_exchange_strong_explicit(
               &mutex->state, &expected, 0U, memory_order_release,
               memory_order_relaxed) ? K_OK : K_EPERM;
#else
    return kmutex_unlock(&mutex->core);
#endif
}

void nv_port_rwlock_init(nv_port_rwlock_t *lock) {
    if (lock == 0) return;
    atomic_init(&lock->state, 0U);
    atomic_init(&lock->writers_waiting, 0U);
#ifndef NV_PORT_HOST_TEST
    wait_queue_init(&lock->readers_waitq);
    wait_queue_init(&lock->writers_waitq);
#endif
}

bool nv_port_rwlock_read_try_lock(nv_port_rwlock_t *lock) {
    unsigned state;

    if (lock == 0) return false;
    state = atomic_load_explicit(&lock->state, memory_order_acquire);
    if ((state & NV_PORT_RW_WRITER) != 0U ||
        atomic_load_explicit(&lock->writers_waiting,
                             memory_order_acquire) != 0U ||
        state >= NV_PORT_RW_WRITER - 1U)
        return false;
    return atomic_compare_exchange_strong_explicit(
        &lock->state, &state, state + 1U,
        memory_order_acquire, memory_order_relaxed);
}

static bool nv_port_rwlock_read_ready(void *context) {
    nv_port_rwlock_t *lock = (nv_port_rwlock_t *)context;
    unsigned state = atomic_load_explicit(&lock->state, memory_order_acquire);
    for (;;) {
        if ((state & NV_PORT_RW_WRITER) != 0U ||
            atomic_load_explicit(&lock->writers_waiting,
                                 memory_order_acquire) != 0U ||
            state >= NV_PORT_RW_WRITER - 1U) return false;
        /* 其他读取者的 CAS 竞争不是可休眠的条件：后续可能没有写入者来唤醒当前读取者。 */
        if (atomic_compare_exchange_weak_explicit(
                &lock->state, &state, state + 1U,
                memory_order_acquire, memory_order_relaxed)) return true;
    }
}

static bool nv_port_rwlock_write_ready(void *context) {
    nv_port_rwlock_t *lock = (nv_port_rwlock_t *)context;
    unsigned expected = 0U;
    return atomic_compare_exchange_strong_explicit(
        &lock->state, &expected, NV_PORT_RW_WRITER,
        memory_order_acquire, memory_order_relaxed);
}

void nv_port_rwlock_read_lock(nv_port_rwlock_t *lock) {
    if (lock == 0 || nv_port_rwlock_read_ready(lock)) return;
#ifdef NV_PORT_HOST_TEST
    while (!nv_port_rwlock_read_ready(lock)) thrd_yield();
#else
    /* 谓词会在等待发布保护下获取锁，也能处理释放操作与订阅并发的情况。绝不能因自旋而抢占锁持有者。 */
    while (wait_on_queue(&lock->readers_waitq, nv_port_rwlock_read_ready,
                          lock, UINT64_MAX) != K_OK) {}
#endif
}

void nv_port_rwlock_read_unlock(nv_port_rwlock_t *lock) {
    if (lock == 0) return;
    unsigned previous = atomic_fetch_sub_explicit(&lock->state, 1U,
                                                   memory_order_release);
#ifndef NV_PORT_HOST_TEST
    if (previous == 1U) wake_one(&lock->writers_waitq);
    if (previous == NV_PORT_RW_WRITER - 1U) wake_all(&lock->readers_waitq);
#else
    (void)previous;
#endif
}

void nv_port_rwlock_write_lock(nv_port_rwlock_t *lock) {
    if (lock == 0) return;
    atomic_fetch_add_explicit(&lock->writers_waiting, 1U, memory_order_acq_rel);
    if (!nv_port_rwlock_write_ready(lock)) {
#ifdef NV_PORT_HOST_TEST
        while (!nv_port_rwlock_write_ready(lock)) thrd_yield();
#else
        while (wait_on_queue(&lock->writers_waitq, nv_port_rwlock_write_ready,
                              lock, UINT64_MAX) != K_OK) {}
#endif
    }
    atomic_fetch_sub_explicit(&lock->writers_waiting, 1U,
                              memory_order_release);
}

void nv_port_rwlock_write_unlock(nv_port_rwlock_t *lock) {
    if (lock == 0) return;
    atomic_store_explicit(&lock->state, 0U, memory_order_release);
#ifndef NV_PORT_HOST_TEST
    /* 始终通知这两类等待者。等待中的写入者可能处于计数递增与订阅之间；若根据该计数跳过读取者，
     * 写入者已经获取并释放锁时，读取者可能会一直无法唤醒。 */
    wake_one(&lock->writers_waitq);
    wake_all(&lock->readers_waitq);
#endif
}

#ifndef NV_PORT_HOST_TEST
static bool nv_port_semaphore_try(void *context) {
    nv_port_semaphore_t *semaphore = (nv_port_semaphore_t *)context;
    unsigned count;

    if (semaphore == 0) return false;
    count = atomic_load_explicit(&semaphore->count, memory_order_acquire);
    while (count != 0U) {
        if (atomic_compare_exchange_weak_explicit(
                &semaphore->count, &count, count - 1U,
                memory_order_acq_rel, memory_order_acquire)) return true;
    }
    return false;
}
#endif

void nv_port_semaphore_init(nv_port_semaphore_t *semaphore, uint32_t count) {
    if (semaphore == 0) return;
    atomic_init(&semaphore->count, count);
#ifndef NV_PORT_HOST_TEST
    wait_queue_init(&semaphore->waitq);
#endif
}

kstatus_t nv_port_semaphore_down(nv_port_semaphore_t *semaphore,
                                 uint64_t timeout_ns) {
    if (semaphore == 0) return K_EINVAL;
#ifdef NV_PORT_HOST_TEST
    uint64_t deadline = nv_port_deadline_after(timeout_ns);
    for (;;) {
        unsigned count = atomic_load_explicit(&semaphore->count,
                                              memory_order_acquire);
        while (count != 0U) {
            if (atomic_compare_exchange_weak_explicit(
                    &semaphore->count, &count, count - 1U,
                    memory_order_acq_rel, memory_order_acquire)) return K_OK;
        }
        if (timeout_ns == 0U || nv_port_deadline_expired(deadline)) {
            return K_ETIMEDOUT;
        }
        thrd_yield();
    }
#else
    return wait_on_queue(&semaphore->waitq, nv_port_semaphore_try, semaphore,
                         timeout_ns);
#endif
}

void nv_port_semaphore_up(nv_port_semaphore_t *semaphore) {
    if (semaphore == 0) return;
    unsigned count = atomic_load_explicit(&semaphore->count,
                                          memory_order_relaxed);
    while (count != UINT32_MAX &&
           !atomic_compare_exchange_weak_explicit(
               &semaphore->count, &count, count + 1U,
               memory_order_release, memory_order_relaxed)) {
    }
#ifndef NV_PORT_HOST_TEST
    if (count != UINT32_MAX) (void)wake_one(&semaphore->waitq);
#endif
}

void nv_port_completion_init(nv_port_completion_t *completion) {
    if (completion == 0) return;
#ifdef NV_PORT_HOST_TEST
    atomic_init(&completion->count, 0U);
#else
    completion_init(&completion->core, 0U);
#endif
}

void nv_port_completion_signal(nv_port_completion_t *completion) {
    if (completion == 0) return;
#ifdef NV_PORT_HOST_TEST
    unsigned count = atomic_load_explicit(&completion->count,
                                          memory_order_relaxed);
    while (count != UINT32_MAX &&
           !atomic_compare_exchange_weak_explicit(
               &completion->count, &count, count + 1U,
               memory_order_release, memory_order_relaxed)) {
    }
#else
    complete(&completion->core);
#endif
}

kstatus_t nv_port_completion_wait(nv_port_completion_t *completion,
                                  uint64_t timeout_ns) {
    if (completion == 0) return K_EINVAL;
#ifdef NV_PORT_HOST_TEST
    uint64_t deadline = nv_port_deadline_after(timeout_ns);
    for (;;) {
        unsigned count = atomic_load_explicit(&completion->count,
                                              memory_order_acquire);
        while (count != 0U) {
            if (atomic_compare_exchange_weak_explicit(
                    &completion->count, &count, count - 1U,
                    memory_order_acq_rel, memory_order_acquire)) return K_OK;
        }
        if (timeout_ns == 0U || nv_port_deadline_expired(deadline)) {
            return K_ETIMEDOUT;
        }
        thrd_yield();
    }
#else
    return completion_wait(&completion->core, timeout_ns);
#endif
}

void nv_port_once_init(nv_port_once_t *once) {
    if (once != 0) atomic_init(&once->state, 0U);
}

kstatus_t nv_port_once_run(nv_port_once_t *once, void (*function)(void *),
                           void *argument) {
    if (once == 0 || function == 0) return K_EINVAL;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(
            &once->state, &expected, 1U, memory_order_acq_rel,
            memory_order_acquire)) {
        function(argument);
        atomic_store_explicit(&once->state, 2U, memory_order_release);
        return K_OK;
    }
    while (atomic_load_explicit(&once->state, memory_order_acquire) == 1U) {
        __asm__ volatile ("pause");
    }
    return atomic_load_explicit(&once->state, memory_order_acquire) == 2U ?
           K_OK : K_EIO;
}

void nv_port_ref_init(nv_port_refcount_t *refcount, uint32_t initial) {
    if (refcount != 0) atomic_init(&refcount->value, initial);
}

bool nv_port_ref_get(nv_port_refcount_t *refcount) {
    if (refcount == 0) return false;
    unsigned value = atomic_load_explicit(&refcount->value,
                                          memory_order_acquire);
    while (value != 0U) {
        if (atomic_compare_exchange_weak_explicit(
                &refcount->value, &value, value + 1U,
                memory_order_acq_rel, memory_order_acquire)) return true;
    }
    return false;
}

bool nv_port_ref_put(nv_port_refcount_t *refcount) {
    if (refcount == 0) return false;
    unsigned value = atomic_load_explicit(&refcount->value,
                                          memory_order_acquire);
    while (value != 0U) {
        if (atomic_compare_exchange_weak_explicit(
                &refcount->value, &value, value - 1U,
                memory_order_acq_rel, memory_order_acquire)) return value == 1U;
    }
    return false;
}

uint32_t nv_port_ref_value(const nv_port_refcount_t *refcount) {
    return refcount == 0 ? 0U : atomic_load_explicit(&refcount->value,
                                                     memory_order_acquire);
}

static void nv_port_once_test_function(void *argument) {
    uint32_t *counter = (uint32_t *)argument;
    if (counter != 0) ++*counter;
}

bool nv_port_sync_self_test(void) {
    nv_port_spinlock_t spin;
    nv_port_mutex_t mutex;
    nv_port_rwlock_t rwlock;
    nv_port_semaphore_t semaphore;
    nv_port_completion_t completion;
    nv_port_once_t once;
    nv_port_refcount_t refcount;
    uint32_t once_counter = 0U;
    uint64_t flags;

    nv_port_spin_init(&spin);
    if (!nv_port_spin_try_lock(&spin)) return false;
    nv_port_spin_unlock(&spin);
    flags = nv_port_spin_lock_irqsave(&spin);
    nv_port_spin_unlock_irqrestore(&spin, flags);

    nv_port_mutex_init(&mutex);
    if (nv_port_mutex_try_lock(&mutex) != K_OK ||
        nv_port_mutex_try_lock(&mutex) != K_EBUSY ||
        nv_port_mutex_unlock(&mutex) != K_OK ||
        nv_port_mutex_unlock(&mutex) != K_EPERM) return false;

    nv_port_rwlock_init(&rwlock);
    nv_port_rwlock_read_lock(&rwlock);
    nv_port_rwlock_read_unlock(&rwlock);
    nv_port_rwlock_write_lock(&rwlock);
    nv_port_rwlock_write_unlock(&rwlock);

    nv_port_semaphore_init(&semaphore, 1U);
    if (nv_port_semaphore_down(&semaphore, 0U) != K_OK ||
        nv_port_semaphore_down(&semaphore, 0U) != K_ETIMEDOUT) return false;
    nv_port_semaphore_up(&semaphore);
    if (nv_port_semaphore_down(&semaphore, 0U) != K_OK) return false;

    nv_port_completion_init(&completion);
    if (nv_port_completion_wait(&completion, 0U) != K_ETIMEDOUT) return false;
    nv_port_completion_signal(&completion);
    if (nv_port_completion_wait(&completion, 0U) != K_OK) return false;

    nv_port_once_init(&once);
    if (nv_port_once_run(&once, nv_port_once_test_function, &once_counter) != K_OK ||
        nv_port_once_run(&once, nv_port_once_test_function, &once_counter) != K_OK ||
        once_counter != 1U) return false;

    nv_port_ref_init(&refcount, 1U);
    if (!nv_port_ref_get(&refcount) || nv_port_ref_value(&refcount) != 2U ||
        nv_port_ref_put(&refcount) || !nv_port_ref_put(&refcount) ||
        nv_port_ref_value(&refcount) != 0U || nv_port_ref_get(&refcount)) {
        return false;
    }
    return true;
}
