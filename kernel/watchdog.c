#include <arch/x86_64/cpu.h>
#include <kernel/deferred.h>
#include <kernel/spinlock.h>
#include <kernel/sched.h>
#include <kernel/watchdog.h>

static struct {
    spinlock_t lock;
    atomic_uint init_state;
    watchdog_client_t clients[WATCHDOG_MAX_CLIENTS];
} g_watchdog;

static void watchdog_lock(void) {
    /* 在受保护状态释放之前，确保所有者在此 CPU 上始终可运行。 */
    sched_preempt_disable();
    spinlock_lock(&g_watchdog.lock);
}

static bool watchdog_try_lock(void) {
    sched_preempt_disable();
    if (spinlock_try_lock(&g_watchdog.lock)) return true;
    sched_preempt_enable();
    return false;
}

static void watchdog_unlock(void) {
    spinlock_unlock(&g_watchdog.lock);
    sched_preempt_enable();
}

bool watchdog_manager_init(void) {
    if (atomic_load_explicit(&g_watchdog.init_state, memory_order_acquire) == 2U) return true;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_watchdog.init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        spinlock_init(&g_watchdog.lock);
        for (uint32_t i = 0; i < WATCHDOG_MAX_CLIENTS; ++i) {
            g_watchdog.clients[i].registered = false;
            g_watchdog.clients[i].queued = false;
            g_watchdog.clients[i].notified = false;
            atomic_init(&g_watchdog.clients[i].state, WATCHDOG_STOPPED);
        }
        atomic_store_explicit(&g_watchdog.init_state, 2U, memory_order_release);
        return true;
    }
    while (atomic_load_explicit(&g_watchdog.init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

static bool watchdog_known_locked(const watchdog_client_t *client) {
    uintptr_t address = (uintptr_t)client;
    uintptr_t base = (uintptr_t)&g_watchdog.clients[0];
    if (address < base || address - base >= sizeof(g_watchdog.clients)) return false;
    uintptr_t offset = address - base;
    if (offset % sizeof(g_watchdog.clients[0]) != 0U) return false;
    return g_watchdog.clients[offset / sizeof(g_watchdog.clients[0])].registered;
}

static uint64_t watchdog_deadline(uint64_t now, uint64_t timeout) {
    return timeout > UINT64_MAX - now ? UINT64_MAX : now + timeout;
}

static bool watchdog_deadline_reached(uint64_t now, uint64_t deadline) {
    return (int64_t)(now - deadline) >= 0;
}

static void watchdog_expire_work(void *argument) {
    watchdog_client_t *client = (watchdog_client_t *)argument;
    watchdog_expire_fn expire = 0;
    void *context = 0;

    if (!watchdog_manager_init()) return;
    watchdog_lock();
    if (client != 0 && watchdog_known_locked(client)) {
        client->queued = false;
        if (atomic_load_explicit(&client->state, memory_order_acquire) ==
                WATCHDOG_EXPIRED && !client->notified) {
            client->notified = true;
            expire = client->expire;
            context = client->context;
        }
    }
    watchdog_unlock();
    if (expire != 0) expire(context);
}

kstatus_t watchdog_register(const char *name, uint64_t timeout_ns,
                            watchdog_expire_fn expire, void *context,
                            watchdog_client_t **out) {
    if (name == 0 || timeout_ns == 0 || timeout_ns == UINT64_MAX ||
        expire == 0 || out == 0 || !watchdog_manager_init()) return K_EINVAL;
    uint64_t timeout_tsc = x86_timeout_ns_to_tsc(timeout_ns);
    if (timeout_tsc == 0) timeout_tsc = 1U;

    watchdog_lock();
    for (uint32_t i = 0; i < WATCHDOG_MAX_CLIENTS; ++i) {
        watchdog_client_t *client = &g_watchdog.clients[i];
        if (client->registered) continue;
        client->name = name;
        client->timeout_tsc = timeout_tsc;
        client->deadline_tsc = 0;
        client->expire = expire;
        client->context = context;
        client->expiration_count = 0;
        client->queued = false;
        client->notified = false;
        atomic_store_explicit(&client->state, WATCHDOG_STOPPED, memory_order_release);
        client->registered = true;
        *out = client;
        watchdog_unlock();
        return K_OK;
    }
    watchdog_unlock();
    return K_ENOMEM;
}

kstatus_t watchdog_unregister(watchdog_client_t *client) {
    if (!watchdog_manager_init()) return K_EIO;
    watchdog_lock();
    if (!watchdog_known_locked(client)) {
        watchdog_unlock();
        return K_ENOENT;
    }
    if (atomic_load_explicit(&client->state, memory_order_acquire) != WATCHDOG_STOPPED) {
        watchdog_unlock();
        return K_EBUSY;
    }
    client->registered = false;
    client->name = 0;
    client->expire = 0;
    client->context = 0;
    watchdog_unlock();
    return K_OK;
}

kstatus_t watchdog_start(watchdog_client_t *client) {
    if (!watchdog_manager_init()) return K_EIO;
    watchdog_lock();
    if (!watchdog_known_locked(client)) {
        watchdog_unlock();
        return K_ENOENT;
    }
    watchdog_state_t state = (watchdog_state_t)atomic_load_explicit(
        &client->state, memory_order_acquire);
    if (state == WATCHDOG_RUNNING) {
        watchdog_unlock();
        return K_EBUSY;
    }
    client->deadline_tsc = watchdog_deadline(x86_read_tsc(), client->timeout_tsc);
    client->notified = false;
    atomic_store_explicit(&client->state, WATCHDOG_RUNNING, memory_order_release);
    watchdog_unlock();
    return K_OK;
}

kstatus_t watchdog_stop(watchdog_client_t *client) {
    if (!watchdog_manager_init()) return K_EIO;
    watchdog_lock();
    if (!watchdog_known_locked(client)) {
        watchdog_unlock();
        return K_ENOENT;
    }
    atomic_store_explicit(&client->state, WATCHDOG_STOPPED, memory_order_release);
    client->notified = true;
    watchdog_unlock();
    return K_OK;
}

kstatus_t watchdog_kick(watchdog_client_t *client) {
    if (!watchdog_manager_init()) return K_EIO;
    watchdog_lock();
    if (!watchdog_known_locked(client)) {
        watchdog_unlock();
        return K_ENOENT;
    }
    if (atomic_load_explicit(&client->state, memory_order_acquire) != WATCHDOG_RUNNING) {
        watchdog_unlock();
        return K_EBUSY;
    }
    client->deadline_tsc = watchdog_deadline(x86_read_tsc(), client->timeout_tsc);
    watchdog_unlock();
    return K_OK;
}

uint32_t watchdog_poll(uint64_t now_tsc) {
    uint32_t expired = 0U;
    /* 定时器中断不能等待初始化锁；启动阶段尚未就绪时直接跳过本次轮询。 */
    if (atomic_load_explicit(&g_watchdog.init_state, memory_order_acquire) != 2U ||
        !watchdog_try_lock()) return 0U;
    for (uint32_t i = 0; i < WATCHDOG_MAX_CLIENTS; ++i) {
        watchdog_client_t *client = &g_watchdog.clients[i];
        if (!client->registered) continue;
        watchdog_state_t state = (watchdog_state_t)atomic_load_explicit(
            &client->state, memory_order_acquire);
        if (state == WATCHDOG_RUNNING &&
            watchdog_deadline_reached(now_tsc, client->deadline_tsc)) {
            atomic_store_explicit(&client->state, WATCHDOG_EXPIRED, memory_order_release);
            ++client->expiration_count;
            client->notified = false;
            ++expired;
            state = WATCHDOG_EXPIRED;
        }
        if (state == WATCHDOG_EXPIRED && !client->notified && !client->queued &&
            deferred_try_schedule(watchdog_expire_work, client)) {
            client->queued = true;
        }
    }
    watchdog_unlock();
    return expired;
}

watchdog_state_t watchdog_get_state(const watchdog_client_t *client) {
    if (!watchdog_manager_init()) return WATCHDOG_EXPIRED;
    watchdog_lock();
    watchdog_state_t state = watchdog_known_locked(client) ?
        (watchdog_state_t)atomic_load_explicit(&client->state, memory_order_acquire) :
        WATCHDOG_EXPIRED;
    watchdog_unlock();
    return state;
}

uint32_t watchdog_expiration_count(const watchdog_client_t *client) {
    if (!watchdog_manager_init()) return 0U;
    watchdog_lock();
    uint32_t count = watchdog_known_locked(client) ? client->expiration_count : 0U;
    watchdog_unlock();
    return count;
}

static void watchdog_test_expired(void *context) {
    atomic_uint *count = (atomic_uint *)context;
    if (count != 0) atomic_fetch_add_explicit(count, 1U, memory_order_relaxed);
}

bool watchdog_self_test(void) {
    atomic_uint callback_count;
    watchdog_client_t *client = 0;
    atomic_init(&callback_count, 0U);
    if (watchdog_register("self-test", 1000000ULL, watchdog_test_expired,
                         &callback_count, &client) != K_OK || client == 0 ||
        watchdog_start(client) != K_OK ||
        watchdog_poll(watchdog_deadline(x86_read_tsc(), client->timeout_tsc)) != 1U ||
        deferred_run(4U) != 1U ||
        watchdog_get_state(client) != WATCHDOG_EXPIRED ||
        watchdog_expiration_count(client) != 1U ||
        atomic_load_explicit(&callback_count, memory_order_relaxed) != 1U ||
        watchdog_start(client) != K_OK || watchdog_kick(client) != K_OK ||
        watchdog_stop(client) != K_OK || watchdog_unregister(client) != K_OK) {
        if (client != 0) (void)watchdog_stop(client);
        return false;
    }
    return true;
}
