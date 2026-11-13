#include <kernel/service.h>

static struct {
    spinlock_t lock;
    atomic_uint init_state;
    service_t services[SERVICE_MANAGER_MAX_SERVICES];
} g_services;

static void service_lock(void) {
    spinlock_lock(&g_services.lock);
}

static void service_unlock(void) {
    spinlock_unlock(&g_services.lock);
}

bool service_manager_init(void) {
    if (atomic_load_explicit(&g_services.init_state, memory_order_acquire) == 2U) return true;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_services.init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        spinlock_init(&g_services.lock);
        for (uint32_t i = 0; i < SERVICE_MANAGER_MAX_SERVICES; ++i) {
            g_services.services[i].registered = false;
            atomic_init(&g_services.services[i].state, SERVICE_STOPPED);
        }
        atomic_store_explicit(&g_services.init_state, 2U, memory_order_release);
        return true;
    }
    while (atomic_load_explicit(&g_services.init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

static bool service_known_locked(const service_t *service) {
    uintptr_t address = (uintptr_t)service;
    uintptr_t base = (uintptr_t)g_services.services;
    if (address < base) return false;
    uintptr_t offset = address - base;
    /* 解引用前先拒绝外部指针和对象内部指针。 */
    if (offset >= sizeof(g_services.services) || offset % sizeof(service_t) != 0U) {
        return false;
    }
    return g_services.services[offset / sizeof(service_t)].registered;
}

kstatus_t service_register(const char *name, service_start_fn start,
                           service_stop_fn stop, void *context,
                           uint32_t restart_limit, service_t **out) {
    if (name == 0 || start == 0 || out == 0 || !service_manager_init()) return K_EINVAL;
    service_lock();
    for (uint32_t i = 0; i < SERVICE_MANAGER_MAX_SERVICES; ++i) {
        service_t *service = &g_services.services[i];
        if (service->registered) continue;
        service->name = name;
        service->start = start;
        service->stop = stop;
        service->context = context;
        service->restart_limit = restart_limit;
        service->restart_count = 0U;
        atomic_store_explicit(&service->state, SERVICE_STOPPED, memory_order_release);
        service->registered = true;
        *out = service;
        service_unlock();
        return K_OK;
    }
    service_unlock();
    return K_ENOMEM;
}

kstatus_t service_unregister(service_t *service) {
    if (!service_manager_init()) return K_EIO;
    service_lock();
    if (!service_known_locked(service) ||
        atomic_load_explicit(&service->state, memory_order_acquire) != SERVICE_STOPPED) {
        service_unlock();
        return K_EBUSY;
    }
    service->registered = false;
    service_unlock();
    return K_OK;
}

kstatus_t service_start(service_t *service) {
    service_start_fn start;
    void *context;
    if (!service_manager_init()) return K_EIO;
    service_lock();
    if (!service_known_locked(service)) {
        service_unlock();
        return K_ENOENT;
    }
    unsigned expected = SERVICE_STOPPED;
    if (!atomic_compare_exchange_strong_explicit(&service->state, &expected,
                                                 SERVICE_STARTING,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        service_unlock();
        return expected == SERVICE_ACTIVE ? K_EBUSY : K_EAGAIN;
    }
    start = service->start;
    context = service->context;
    service_unlock();

    kstatus_t status = start(context);
    service_lock();
    if (!service->registered) {
        service_unlock();
        return K_ECANCELED;
    }
    atomic_store_explicit(&service->state,
                          status == K_OK ? SERVICE_ACTIVE : SERVICE_FAILED,
                          memory_order_release);
    if (status == K_OK) service->restart_count = 0U;
    service_unlock();
    return status;
}

kstatus_t service_stop(service_t *service) {
    service_stop_fn stop;
    void *context;
    if (!service_manager_init()) return K_EIO;
    service_lock();
    if (!service_known_locked(service)) {
        service_unlock();
        return K_ENOENT;
    }
    unsigned state = atomic_load_explicit(&service->state, memory_order_acquire);
    if (state == SERVICE_STOPPED) {
        service_unlock();
        return K_OK;
    }
    if (state == SERVICE_STARTING || state == SERVICE_STOPPING) {
        service_unlock();
        return K_EBUSY;
    }
    atomic_store_explicit(&service->state, SERVICE_STOPPING, memory_order_release);
    stop = service->stop;
    context = service->context;
    service_unlock();

    if (stop != 0) stop(context);
    service_lock();
    atomic_store_explicit(&service->state, SERVICE_STOPPED, memory_order_release);
    service_unlock();
    return K_OK;
}

kstatus_t service_mark_failed(service_t *service) {
    if (!service_manager_init()) return K_EIO;
    service_lock();
    if (!service_known_locked(service)) {
        service_unlock();
        return K_ENOENT;
    }
    unsigned state = atomic_load_explicit(&service->state, memory_order_acquire);
    if (state != SERVICE_ACTIVE) {
        service_unlock();
        return K_EBUSY;
    }
    atomic_store_explicit(&service->state, SERVICE_FAILED, memory_order_release);
    service_unlock();
    return K_OK;
}

uint32_t service_recover_failed(uint32_t budget) {
    uint32_t recovered = 0U;
    if (budget == 0U || !service_manager_init()) return 0U;
    for (uint32_t i = 0; i < SERVICE_MANAGER_MAX_SERVICES && recovered < budget; ++i) {
        service_t *service = &g_services.services[i];
        service_stop_fn stop = 0;
        void *context = 0;
        service_lock();
        bool retry = service->registered &&
                     atomic_load_explicit(&service->state, memory_order_acquire) ==
                         SERVICE_FAILED && service->restart_count < service->restart_limit;
        if (retry) {
            ++service->restart_count;
            stop = service->stop;
            context = service->context;
            /* 先占有恢复槽位，防止并发 stop/recover 重复消费同一失败。 */
            atomic_store_explicit(&service->state, SERVICE_STOPPING,
                                  memory_order_release);
        }
        service_unlock();
        if (!retry) continue;

        if (stop != 0) stop(context);
        service_lock();
        if (service->registered) {
            atomic_store_explicit(&service->state, SERVICE_STOPPED, memory_order_release);
        }
        service_unlock();
        if (service_start(service) == K_OK) ++recovered;
    }
    return recovered;
}

service_state_t service_get_state(const service_t *service) {
    if (!service_manager_init()) return SERVICE_FAILED;
    service_lock();
    bool known = service_known_locked(service);
    service_state_t state = known ? (service_state_t)atomic_load_explicit(
        &service->state, memory_order_acquire) : SERVICE_FAILED;
    service_unlock();
    return state;
}

typedef struct service_test_context {
    atomic_uint starts;
    atomic_uint stops;
} service_test_context_t;

static kstatus_t service_test_start(void *context) {
    service_test_context_t *test = (service_test_context_t *)context;
    if (test != 0) atomic_fetch_add_explicit(&test->starts, 1U, memory_order_relaxed);
    return K_OK;
}

static void service_test_stop(void *context) {
    service_test_context_t *test = (service_test_context_t *)context;
    if (test != 0) atomic_fetch_add_explicit(&test->stops, 1U, memory_order_relaxed);
}

bool service_manager_self_test(void) {
    service_test_context_t test;
    service_t *service = 0;
    atomic_init(&test.starts, 0U);
    atomic_init(&test.stops, 0U);
    if (service_register("self-test", service_test_start, service_test_stop,
                         &test, 2U, &service) != K_OK || service == 0 ||
        service_start(service) != K_OK ||
        service_get_state(service) != SERVICE_ACTIVE ||
        service_mark_failed(service) != K_OK || service_recover_failed(1U) != 1U ||
        service_get_state(service) != SERVICE_ACTIVE ||
        service_stop(service) != K_OK ||
        atomic_load_explicit(&test.starts, memory_order_relaxed) != 2U ||
        service_unregister(service) != K_OK) return false;
    return atomic_load_explicit(&test.stops, memory_order_relaxed) == 2U;
}
