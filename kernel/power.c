#include <kernel/power.h>
#include <kernel/spinlock.h>
#include <kernel/sched.h>
#include <kernel/device.h>
#include <arch/x86_64/acpi.h>
#include <arch/x86_64/reboot.h>
#include <kernel/vfs.h>
#include <kernel/e1000.h>

kstatus_t power_system_request(uint32_t action) {
    static atomic_bool busy;
    if (action > 1U) return K_EINVAL;
    if (atomic_exchange_explicit(&busy, true, memory_order_acq_rel)) return K_EBUSY;
    kstatus_t status = vfs_sync_all();
    if (status == K_OK) status = e1000_shutdown();
    if (status == K_OK) {
        if (action == 0U) x86_system_reboot();
        status = x86_acpi_poweroff();
    }
    atomic_store_explicit(&busy, false, memory_order_release);
    return status == K_OK ? K_EIO : status; /* 固件返回时设备仍未关机。 */
}

static struct {
    spinlock_t lock;
    atomic_uint init_state;
    atomic_uint system_state;
    power_device_t devices[POWER_MAX_DEVICES];
} g_power;

static void power_lock(void) {
    /* Pin only the registry/state update, never the device callbacks. */
    sched_preempt_disable();
    spinlock_lock(&g_power.lock);
}

static void power_unlock(void) {
    spinlock_unlock(&g_power.lock);
    sched_preempt_enable();
}

bool power_manager_init(void) {
    if (atomic_load_explicit(&g_power.init_state, memory_order_acquire) == 2U) return true;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_power.init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        spinlock_init(&g_power.lock);
        atomic_init(&g_power.system_state, POWER_SYSTEM_RUNNING);
        for (uint32_t i = 0; i < POWER_MAX_DEVICES; ++i) {
            g_power.devices[i].registered = false;
            atomic_init(&g_power.devices[i].state, POWER_DEVICE_RUNNING);
        }
        atomic_store_explicit(&g_power.init_state, 2U, memory_order_release);
        return true;
    }
    while (atomic_load_explicit(&g_power.init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

static bool power_known_locked(const power_device_t *device) {
    uintptr_t address = (uintptr_t)device;
    uintptr_t base = (uintptr_t)&g_power.devices[0];
    if (address < base || address - base >= sizeof(g_power.devices)) return false;
    uintptr_t offset = address - base;
    if (offset % sizeof(g_power.devices[0]) != 0U) return false;
    return g_power.devices[offset / sizeof(g_power.devices[0])].registered;
}

static uint32_t power_snapshot_locked(power_device_t **devices) {
    uint32_t count = 0U;
    for (uint32_t i = 0; i < POWER_MAX_DEVICES; ++i) {
        if (g_power.devices[i].registered) {
            devices[count++] = &g_power.devices[i];
        }
    }
    return count;
}

kstatus_t power_register_device(const char *name, power_suspend_fn suspend,
                                power_resume_fn resume, void *context,
                                power_device_t **out) {
    if (name == 0 || suspend == 0 || resume == 0 || out == 0 ||
        !power_manager_init()) return K_EINVAL;
    power_lock();
    if (atomic_load_explicit(&g_power.system_state, memory_order_acquire) !=
            POWER_SYSTEM_RUNNING) {
        power_unlock();
        return K_EBUSY;
    }
    for (uint32_t i = 0; i < POWER_MAX_DEVICES; ++i) {
        power_device_t *device = &g_power.devices[i];
        if (device->registered) continue;
        device->name = name;
        device->suspend = suspend;
        device->resume = resume;
        device->context = context;
        atomic_store_explicit(&device->state, POWER_DEVICE_RUNNING, memory_order_release);
        device->registered = true;
        *out = device;
        power_unlock();
        return K_OK;
    }
    power_unlock();
    return K_ENOMEM;
}

kstatus_t power_unregister_device(power_device_t *device) {
    if (!power_manager_init()) return K_EIO;
    power_lock();
    if (!power_known_locked(device)) {
        power_unlock();
        return K_ENOENT;
    }
    power_device_state_t device_state = (power_device_state_t)
        atomic_load_explicit(&device->state, memory_order_acquire);
    if (atomic_load_explicit(&g_power.system_state, memory_order_acquire) !=
            POWER_SYSTEM_RUNNING ||
        (device_state != POWER_DEVICE_RUNNING &&
         device_state != POWER_DEVICE_FAILED)) {
        power_unlock();
        return K_EBUSY;
    }
    device->registered = false;
    device->name = 0;
    device->suspend = 0;
    device->resume = 0;
    device->context = 0;
    power_unlock();
    return K_OK;
}

static kstatus_t power_suspend_device_object(void *context) {
    return device_suspend((device_t *)context);
}

static kstatus_t power_resume_device_object(void *context) {
    return device_resume((device_t *)context);
}

kstatus_t power_register_device_object(struct device *device,
                                       power_device_t **out) {
    const char *name = "device";
    if (device == 0 || out == 0 || device->ops == 0 ||
        (device->ops->set_power == 0 && device->ops->begin_power == 0) ||
        (device->ops->begin_power != 0 && device->ops->poll_power == 0) ||
        device->power_device != 0) {
        return K_EINVAL;
    }
    if (device->driver != 0 && device->driver->name != 0) {
        name = device->driver->name;
    }
    return power_register_device(name, power_suspend_device_object,
                                 power_resume_device_object, device, out);
}

kstatus_t power_unregister_device_object(struct device *device) {
    kstatus_t status;
    if (device == 0 || device->power_device == 0) return K_ENOENT;
    status = power_unregister_device(device->power_device);
    if (status == K_OK) device->power_device = 0;
    return status;
}

kstatus_t power_system_suspend(void) {
    power_device_t *devices[POWER_MAX_DEVICES];
    power_device_state_t previous_states[POWER_MAX_DEVICES];
    uint32_t count;
    if (!power_manager_init()) return K_EIO;

    power_lock();
    if (atomic_load_explicit(&g_power.system_state, memory_order_acquire) !=
            POWER_SYSTEM_RUNNING) {
        power_unlock();
        return K_EBUSY;
    }
    count = power_snapshot_locked(devices);
    atomic_store_explicit(&g_power.system_state, POWER_SYSTEM_SUSPENDING,
                          memory_order_release);
    for (uint32_t i = 0; i < count; ++i) {
        previous_states[i] = (power_device_state_t)atomic_load_explicit(
            &devices[i]->state, memory_order_acquire);
        atomic_store_explicit(&devices[i]->state, POWER_DEVICE_SUSPENDING,
                              memory_order_release);
    }
    power_unlock();

    for (uint32_t i = 0; i < count; ++i) {
        kstatus_t status = devices[i]->suspend(devices[i]->context);
        power_lock();
        atomic_store_explicit(&devices[i]->state,
                              status == K_OK ? POWER_DEVICE_SUSPENDED :
                                               POWER_DEVICE_FAILED,
                              memory_order_release);
        power_unlock();
        if (status != K_OK) {
            bool rollback_failed = false;
            for (uint32_t j = i; j > 0; --j) {
                power_device_t *device = devices[j - 1U];
                power_device_state_t state = (power_device_state_t)
                    atomic_load_explicit(&device->state, memory_order_acquire);
                if (state != POWER_DEVICE_SUSPENDED) continue;
                kstatus_t resume_status = device->resume(device->context);
                power_lock();
                atomic_store_explicit(&device->state,
                                      resume_status == K_OK ? POWER_DEVICE_RUNNING :
                                                              POWER_DEVICE_FAILED,
                                      memory_order_release);
                power_unlock();
                if (resume_status != K_OK) rollback_failed = true;
            }
            power_lock();
            /* No suspend callback ran for the remaining devices. */
            for (uint32_t j = i + 1U; j < count; ++j) {
                atomic_store_explicit(&devices[j]->state, previous_states[j],
                                      memory_order_release);
            }
            atomic_store_explicit(&g_power.system_state,
                                  rollback_failed ? POWER_SYSTEM_FAILED :
                                                    POWER_SYSTEM_RUNNING,
                                  memory_order_release);
            power_unlock();
            return status;
        }
    }
    power_lock();
    atomic_store_explicit(&g_power.system_state, POWER_SYSTEM_SUSPENDED,
                          memory_order_release);
    power_unlock();
    return K_OK;
}

kstatus_t power_system_resume(void) {
    power_device_t *devices[POWER_MAX_DEVICES];
    uint32_t count;
    if (!power_manager_init()) return K_EIO;

    power_lock();
    if (atomic_load_explicit(&g_power.system_state, memory_order_acquire) !=
            POWER_SYSTEM_SUSPENDED) {
        power_unlock();
        return K_EBUSY;
    }
    count = power_snapshot_locked(devices);
    atomic_store_explicit(&g_power.system_state, POWER_SYSTEM_RESUMING,
                          memory_order_release);
    for (uint32_t i = 0; i < count; ++i) {
        atomic_store_explicit(&devices[i]->state, POWER_DEVICE_RESUMING,
                              memory_order_release);
    }
    power_unlock();

    for (uint32_t i = count; i > 0; --i) {
        power_device_t *device = devices[i - 1U];
        kstatus_t status = device->resume(device->context);
        power_lock();
        atomic_store_explicit(&device->state,
                              status == K_OK ? POWER_DEVICE_RUNNING :
                                               POWER_DEVICE_FAILED,
                              memory_order_release);
        if (status != K_OK) {
            /* Reverse-order resume stopped here: untouched devices remain
             * suspended, not in a transition that can no longer complete. */
            for (uint32_t j = 0U; j + 1U < i; ++j) {
                atomic_store_explicit(&devices[j]->state, POWER_DEVICE_SUSPENDED,
                                      memory_order_release);
            }
            atomic_store_explicit(&g_power.system_state, POWER_SYSTEM_FAILED,
                                  memory_order_release);
            power_unlock();
            return status;
        }
        power_unlock();
    }
    power_lock();
    atomic_store_explicit(&g_power.system_state, POWER_SYSTEM_RUNNING,
                          memory_order_release);
    power_unlock();
    return K_OK;
}

kstatus_t power_system_enter_acpi_sleep(uint8_t sleep_state) {
    kstatus_t status;
    if (sleep_state != 3U && sleep_state != 4U) return K_EINVAL;
    if (!x86_acpi_sleep_supported()) return K_ENOSYS;
    status = power_system_suspend();
    if (status != K_OK) return status;
    status = x86_acpi_enter_sleep(sleep_state);
    if (status != K_OK) {
        (void)power_system_resume();
        return status;
    }
    /* 正常硬件会在这里进入 S3/S4；若固件立即返回，系统仍保持 SUSPENDED，
     * 由电源键/平台事件处理器调用 power_system_resume。 */
    return K_OK;
}

power_system_state_t power_get_system_state(void) {
    if (!power_manager_init()) return POWER_SYSTEM_FAILED;
    return (power_system_state_t)atomic_load_explicit(&g_power.system_state,
                                                       memory_order_acquire);
}

power_device_state_t power_get_device_state(const power_device_t *device) {
    if (!power_manager_init()) return POWER_DEVICE_FAILED;
    power_lock();
    power_device_state_t state = power_known_locked(device) ?
        (power_device_state_t)atomic_load_explicit(&device->state, memory_order_acquire) :
        POWER_DEVICE_FAILED;
    power_unlock();
    return state;
}

typedef struct power_test_context {
    uint8_t id;
    uint8_t *events;
    uint32_t *event_count;
    bool fail_suspend;
} power_test_context_t;

static kstatus_t power_test_suspend(void *context) {
    power_test_context_t *test = (power_test_context_t *)context;
    if (test == 0 || test->events == 0 || test->event_count == 0 ||
        *test->event_count >= 8U) return K_EINVAL;
    test->events[(*test->event_count)++] = test->id;
    if (test->fail_suspend) return K_EIO;
    return K_OK;
}

static kstatus_t power_test_resume(void *context) {
    return power_test_suspend(context);
}

bool power_self_test(void) {
    uint8_t events[16] = {0};
    uint32_t event_count = 0U;
    power_test_context_t first = {1U, events, &event_count, false};
    power_test_context_t second = {2U, events, &event_count, false};
    power_device_t *first_device = 0;
    power_device_t *second_device = 0;
    if (!power_manager_init() ||
        power_register_device("power-test-a", power_test_suspend, power_test_resume,
                             &first, &first_device) != K_OK ||
        power_register_device("power-test-b", power_test_suspend, power_test_resume,
                             &second, &second_device) != K_OK ||
        power_system_suspend() != K_OK ||
        power_get_system_state() != POWER_SYSTEM_SUSPENDED ||
        power_get_device_state(first_device) != POWER_DEVICE_SUSPENDED ||
        power_system_resume() != K_OK ||
        power_get_system_state() != POWER_SYSTEM_RUNNING ||
        power_get_device_state(second_device) != POWER_DEVICE_RUNNING ||
         event_count != 4U || events[0] != 1U || events[1] != 2U ||
         events[2] != 2U || events[3] != 1U) return false;

    /* 重复挂起/恢复，检查状态机在多轮使用后仍保持顺序和可重入性。 */
    for (uint32_t cycle = 0; cycle < 4U; ++cycle) {
        event_count = 0U;
        if (power_system_suspend() != K_OK ||
            power_get_system_state() != POWER_SYSTEM_SUSPENDED ||
            power_system_resume() != K_OK ||
            power_get_system_state() != POWER_SYSTEM_RUNNING ||
            event_count != 4U || events[0] != 1U || events[1] != 2U ||
            events[2] != 2U || events[3] != 1U) return false;
    }
    if (power_unregister_device(first_device) != K_OK ||
        power_unregister_device(second_device) != K_OK) return false;

    /* 第二个设备挂起失败时，已经挂起的第一个设备必须按逆序回滚。 */
    event_count = 0U;
    first.fail_suspend = false;
    second.fail_suspend = true;
    first_device = 0;
    second_device = 0;
    if (power_register_device("power-test-a", power_test_suspend, power_test_resume,
                             &first, &first_device) != K_OK ||
        power_register_device("power-test-b", power_test_suspend, power_test_resume,
                              &second, &second_device) != K_OK ||
        power_system_suspend() != K_EIO ||
        power_get_system_state() != POWER_SYSTEM_RUNNING ||
        power_get_device_state(first_device) != POWER_DEVICE_RUNNING ||
        power_get_device_state(second_device) != POWER_DEVICE_FAILED ||
        event_count != 3U || events[0] != 1U || events[1] != 2U || events[2] != 1U) {
        return false;
    }
    second.fail_suspend = false;
    event_count = 0U;
    if (power_system_suspend() != K_OK || power_system_resume() != K_OK ||
        power_unregister_device(first_device) != K_OK ||
        power_unregister_device(second_device) != K_OK) return false;
    return true;
}
