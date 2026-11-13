#include <kernel/device.h>
#include <kernel/iommu.h>
#include <kernel/kmem.h>
#include <kernel/power.h>
#include <kernel/telemetry.h>
#include <kernel/string.h>
#include <kernel/sched.h>
#include <arch/x86_64/cpu.h>

#define DRIVER_OBJECT_TYPE 0x0107U
#define DEVICE_REGISTRY_LIMIT 256U
#define DRIVER_REGISTRY_LIMIT 128U

static spinlock_t g_device_lock;
static atomic_uint g_device_init_state;
static device_t *g_devices[DEVICE_REGISTRY_LIMIT];
static driver_t *g_drivers[DRIVER_REGISTRY_LIMIT];

static void device_lock(void) {
    /* 注册表操作会嵌套获取状态锁，因此要让锁持有者留在当前 CPU 上。
     * 驱动回调和 I/O 等待仍在此临界区之外执行。 */
    sched_preempt_disable();
    spinlock_lock(&g_device_lock);
}

static void device_unlock(void) {
    spinlock_unlock(&g_device_lock);
    sched_preempt_enable();
}

static void device_state_lock(device_t *dev) {
    /* 回调执行后也会单独使用此操作，并非只在持有注册表锁时使用。 */
    sched_preempt_disable();
    spinlock_lock(&dev->state_lock);
}

static void device_state_unlock(device_t *dev) {
    spinlock_unlock(&dev->state_lock);
    sched_preempt_enable();
}

static bool device_wait_io_idle(device_t *dev, uint64_t timeout_ns) {
    uint64_t start;
    uint64_t limit;
    if (dev == 0) return false;
    start = x86_read_tsc();
    limit = timeout_ns == 0U || timeout_ns == UINT64_MAX ? 0U :
            x86_timeout_ns_to_tsc(timeout_ns);
    for (;;) {
        if (atomic_load_explicit(&dev->io_inflight, memory_order_acquire) == 0U) {
            return true;
        }
        if (limit != 0U && x86_read_tsc() - start > limit) return false;
        __asm__ volatile ("pause");
    }
}

static void object_nop_destroy(void *object) {
    (void)object;
}

static const object_ops_t g_device_object_ops = {
    .destroy = object_nop_destroy,
    .type_name = "Device",
    .is_signaled = 0,
    .wait_value = 0,
};

static const object_ops_t g_driver_object_ops = {
    .destroy = object_nop_destroy,
    .type_name = "Driver",
    .is_signaled = 0,
    .wait_value = 0,
};

static void device_initialize(void) {
    if (atomic_load_explicit(&g_device_init_state, memory_order_acquire) == 2U) return;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_device_init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        spinlock_init(&g_device_lock);
        for (uint32_t i = 0; i < DEVICE_REGISTRY_LIMIT; ++i) g_devices[i] = 0;
        for (uint32_t i = 0; i < DRIVER_REGISTRY_LIMIT; ++i) g_drivers[i] = 0;
        atomic_store_explicit(&g_device_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_device_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static void initialize_object(object_header_t *header, object_type_id_t type,
                              const object_ops_t *ops) {
    refcount_init(&header->refs, 1U);
    header->type = type;
    header->flags = 0;
    header->ops = ops;
}

void device_object_init(device_t *dev, uint64_t device_id, uint32_t class_id,
                        const device_ops_t *ops, void *bus_data) {
    if (dev == 0) return;
    memset(dev, 0, sizeof(*dev));
    initialize_object(&dev->object, KOBJECT_TYPE_DEVICE, &g_device_object_ops);
    dev->device_id = device_id;
    atomic_init(&dev->state, DEVICE_DISCOVERED);
    dev->class_id = class_id;
    dev->ops = ops;
    dev->bus_data = bus_data;
    list_init(&dev->sibling_node);
    list_init(&dev->children);
    list_init(&dev->io_requests);
    spinlock_init(&dev->state_lock);
    atomic_init(&dev->io_inflight, 0U);
    dev->power_timeout_ns = DEVICE_DEFAULT_POWER_TIMEOUT_NS;
}

void driver_object_init(driver_t *drv, const char *name,
                        kstatus_t (*probe)(device_t *),
                        void (*remove)(device_t *)) {
    if (drv == 0) return;
    memset(drv, 0, sizeof(*drv));
    initialize_object(&drv->object, DRIVER_OBJECT_TYPE, &g_driver_object_ops);
    drv->name = name;
    drv->probe = probe;
    drv->remove = remove;
    drv->api_version = OS_DRIVER_API_VERSION;
    drv->struct_size = sizeof(*drv);
}

static int32_t find_device_slot_with_free(device_t *dev, int32_t *first_free) {
    if (first_free != 0) *first_free = -1;
    for (uint32_t i = 0; i < DEVICE_REGISTRY_LIMIT; ++i) {
        if (g_devices[i] == dev) return (int32_t)i;
        if (first_free != 0 && *first_free < 0 && g_devices[i] == 0) {
            *first_free = (int32_t)i;
        }
    }
    return -1;
}

static int32_t find_device_slot(device_t *dev) {
    return find_device_slot_with_free(dev, 0);
}

static int32_t find_driver_slot_with_free(driver_t *drv, int32_t *first_free) {
    if (first_free != 0) *first_free = -1;
    for (uint32_t i = 0; i < DRIVER_REGISTRY_LIMIT; ++i) {
        if (g_drivers[i] == drv) return (int32_t)i;
        if (first_free != 0 && *first_free < 0 && g_drivers[i] == 0) {
            *first_free = (int32_t)i;
        }
    }
    return -1;
}

static int32_t find_driver_slot(driver_t *drv) {
    return find_driver_slot_with_free(drv, 0);
}

static bool bind_driver(device_t *dev, driver_t *drv) {
    if (dev == 0 || drv == 0 || drv->probe == 0) return false;
    if (drv->probe(dev) != K_OK) return false;
    device_lock();
    if (find_device_slot(dev) < 0 || atomic_load_explicit(&dev->state,
        memory_order_acquire) >= DEVICE_REMOVING || dev->driver != 0) {
        device_unlock();
        /* 探测已经分配了驱动私有资源，绑定失败时必须立即回收。 */
        if (drv->remove != 0) drv->remove(dev);
        return false;
    }
    dev->driver = drv;
    atomic_store_explicit(&dev->state, DEVICE_BOUND, memory_order_release);
    device_unlock();
    if (dev->ops != 0 && dev->ops->start != 0) {
        if (dev->ops->start(dev) != K_OK) {
            /* 启动失败也走统一 remove，释放 BAR 映射、队列页等资源。 */
            if (drv->remove != 0) drv->remove(dev);
            device_lock();
            if (dev->driver == drv) dev->driver = 0;
            atomic_store_explicit(&dev->state, DEVICE_FAILED, memory_order_release);
            device_unlock();
            return false;
        }
    }
    atomic_store_explicit(&dev->state, DEVICE_ACTIVE, memory_order_release);
    return true;
}

kstatus_t device_register(device_t *dev) {
    if (dev == 0 || dev->object.type != KOBJECT_TYPE_DEVICE) return K_EINVAL;
    device_initialize();
    device_lock();
    int32_t slot;
    if (find_device_slot_with_free(dev, &slot) >= 0) {
        device_unlock();
        return K_EBUSY;
    }
    if (slot < 0) {
        device_unlock();
        return K_ENOMEM;
    }
    list_init(&dev->sibling_node);
    if (dev->parent != 0) list_add_tail(&dev->parent->children,
                                        &dev->sibling_node);
    g_devices[slot] = dev;
    object_get(dev);
    atomic_store_explicit(&dev->state, DEVICE_ENUMERATED, memory_order_release);
    device_unlock();

    for (uint32_t i = 0; i < DRIVER_REGISTRY_LIMIT; ++i) {
        driver_t *drv;
        device_lock();
        drv = g_drivers[i];
        if (drv == 0 || !object_try_get(drv)) {
            device_unlock();
            continue;
        }
        device_unlock();
        bool bound = bind_driver(dev, drv);
        object_put(drv);
        if (bound) break;
    }
    if (dev->ops != 0 && (dev->ops->set_power != 0 ||
                          dev->ops->begin_power != 0)) {
        kstatus_t status = power_register_device_object(dev, &dev->power_device);
        if (status != K_OK) {
            /* 电源生命周期无法建立时，不把半初始化设备留在注册表中。 */
            device_unregister(dev);
            return status;
        }
    }
    return K_OK;
}

kstatus_t device_get_by_id(uint64_t device_id, device_t **out) {
    if (out == 0) return K_EINVAL;
    *out = 0;
    device_initialize();
    device_lock();
    for (uint32_t i = 0; i < DEVICE_REGISTRY_LIMIT; ++i) {
        device_t *dev = g_devices[i];
        if (dev == 0 || dev->device_id != device_id) continue;
        uint32_t state = atomic_load_explicit(&dev->state, memory_order_acquire);
        if (state >= DEVICE_REMOVING || !object_try_get(dev)) {
            device_unlock();
            return K_ENOENT;
        }
        *out = dev;
        device_unlock();
        return K_OK;
    }
    device_unlock();
    return K_ENOENT;
}

kstatus_t device_get_by_index(uint32_t index, device_t **out) {
    uint32_t visible = 0U;
    if (out == 0) return K_EINVAL;
    *out = 0;
    device_initialize();
    device_lock();
    for (uint32_t slot = 0; slot < DEVICE_REGISTRY_LIMIT; ++slot) {
        device_t *dev = g_devices[slot];
        if (dev == 0 || atomic_load_explicit(&dev->state, memory_order_acquire) >=
                         DEVICE_REMOVING) continue;
        if (visible++ != index) continue;
        if (!object_try_get(dev)) {
            device_unlock();
            return K_ENOENT;
        }
        *out = dev;
        device_unlock();
        return K_OK;
    }
    device_unlock();
    return K_ENOENT;
}

static void device_cancel_pending_io(device_t *dev) {
    for (;;) {
        io_request_t *request = 0;
        device_lock();
        if (list_empty(&dev->io_requests)) {
            device_unlock();
            return;
        }
        /* 解锁前先认领：仅转移引用无法阻止其他终结者在分发前释放或复用请求。
         * 成功认领的一方会一直持有执行权，直到在锁外完成处理。 */
        for (list_head_t *node = dev->io_requests.next;
             node != &dev->io_requests; node = node->next) {
            io_request_t *candidate = list_entry(node, io_request_t, device_node);
            if (io_request_claim_device_cancel(candidate)) {
                request = candidate;
                break;
            }
        }
        /* 速度较慢的终结者不能阻塞后续请求的取消操作。只有所有剩余节点都已有
         * 所有者时才等待。 */
        if (request == 0) {
            device_unlock();
            __asm__ volatile ("pause");
            continue;
        }
        device_unlock();
        /*
         * 设备移除和用户 IO_CANCEL 可能同时观察到同一个请求。
         * 上方的 CAS 已选出唯一所有者。驱动回调和设备引用释放必须在设备列表锁之外执行。
         */
        io_request_finish_device_cancel(request, K_EDEVREMOVED);
    }
}

void device_unregister(device_t *dev) {
    if (dev == 0) return;
    device_initialize();
    driver_t *drv = 0;
    power_device_t *power_device = 0;
    uint32_t previous_state = DEVICE_ENUMERATED;
    device_lock();
    int32_t slot = find_device_slot(dev);
    if (slot < 0) {
        device_unlock();
        return;
    }
    device_state_lock(dev);
    uint32_t state = atomic_load_explicit(&dev->state, memory_order_acquire);
    if (state == DEVICE_REMOVING || state == DEVICE_REMOVED ||
        state == DEVICE_RECOVERING) {
        device_state_unlock(dev);
        device_unlock();
        return;
    }
    previous_state = state;
    atomic_store_explicit(&dev->state, DEVICE_REMOVING, memory_order_release);
    device_state_unlock(dev);
    power_device = (power_device_t *)dev->power_device;
    device_unlock();

    if (power_device != 0 && power_unregister_device(power_device) != K_OK) {
        device_lock();
        if (find_device_slot(dev) >= 0 &&
            atomic_load_explicit(&dev->state, memory_order_acquire) == DEVICE_REMOVING) {
            atomic_store_explicit(&dev->state, previous_state, memory_order_release);
        }
        device_unlock();
        return;
    }

    device_lock();
    slot = find_device_slot(dev);
    if (slot < 0) {
        device_unlock();
        return;
    }
    dev->power_device = 0;
    drv = dev->driver;
    dev->driver = 0;
    g_devices[slot] = 0;
    if (dev->sibling_node.next != &dev->sibling_node) {
        list_del(&dev->sibling_node);
    }
    atomic_store_explicit(&dev->state, DEVICE_REMOVED, memory_order_release);
    device_unlock();
    /* 先完成仍挂在设备上的请求，再释放驱动资源，避免调用者永久等待。 */
    device_cancel_pending_io(dev);
    if (drv != 0 && drv->remove != 0) drv->remove(dev);
    /* PCI 设备的 DMA 已停止后再拆除 VT-d domain，避免重插时泄漏页表。 */
    (void)iommu_detach_device(dev);
    object_put(dev);
}

static kstatus_t device_call_power(device_t *dev, const device_ops_t *ops,
                                    uint32_t state, uint64_t timeout_ns) {
    uint64_t start_tsc = x86_read_tsc();
    uint64_t limit = timeout_ns == 0U || timeout_ns == UINT64_MAX ? 0U :
                     x86_timeout_ns_to_tsc(timeout_ns);
    kstatus_t status;

    if (ops->begin_power != 0) {
        if (ops->poll_power == 0) return K_EINVAL;
        status = ops->begin_power(dev, state);
        while (status == K_EAGAIN) {
            if (limit != 0U && x86_read_tsc() - start_tsc > limit) {
                return K_ETIMEDOUT;
            }
            status = ops->poll_power(dev, state);
            __asm__ volatile ("pause");
        }
    } else {
        if (ops->set_power == 0) return K_ENOSYS;
        status = ops->set_power(dev, state);
    }
    if (status == K_OK && limit != 0U && x86_read_tsc() - start_tsc > limit) {
        status = K_ETIMEDOUT;
    }
    (void)telemetry_record_latency(TELEMETRY_CATEGORY_POWER_TRANSACTION,
                                   dev->device_id, start_tsc);
    return status;
}

kstatus_t device_suspend_timeout(device_t *dev, uint64_t timeout_ns) {
    kstatus_t status;
    const device_ops_t *ops;

    if (dev == 0) return K_EINVAL;
    device_initialize();
    device_lock();
    if (find_device_slot(dev) < 0) {
        device_unlock();
        return K_ENOENT;
    }
    device_state_lock(dev);
    if (atomic_load_explicit(&dev->state, memory_order_acquire) != DEVICE_ACTIVE) {
        device_state_unlock(dev);
        device_unlock();
        return K_EBUSY;
    }
    ops = dev->ops;
    if (ops == 0 || (ops->set_power == 0 && ops->begin_power == 0) ||
        (ops->begin_power != 0 && ops->poll_power == 0)) {
        device_state_unlock(dev);
        device_unlock();
        return K_ENOSYS;
    }
    atomic_store_explicit(&dev->state, DEVICE_RECOVERING, memory_order_release);
    device_state_unlock(dev);
    device_unlock();

    if (!device_wait_io_idle(dev, timeout_ns)) {
        device_state_lock(dev);
        atomic_store_explicit(&dev->state, DEVICE_FAILED, memory_order_release);
        device_state_unlock(dev);
        return K_ETIMEDOUT;
    }
    status = device_call_power(dev, ops, DEVICE_POWER_SUSPENDED, timeout_ns);
    device_state_lock(dev);
    atomic_store_explicit(&dev->state,
                          status == K_OK ? DEVICE_SUSPENDED : DEVICE_FAILED,
                          memory_order_release);
    device_state_unlock(dev);
    return status;
}

kstatus_t device_suspend(device_t *dev) {
    if (dev == 0) return K_EINVAL;
    return device_suspend_timeout(dev, dev->power_timeout_ns);
}

kstatus_t device_resume_timeout(device_t *dev, uint64_t timeout_ns) {
    kstatus_t status;
    const device_ops_t *ops;

    if (dev == 0) return K_EINVAL;
    device_initialize();
    device_lock();
    if (find_device_slot(dev) < 0) {
        device_unlock();
        return K_ENOENT;
    }
    device_state_lock(dev);
    if (atomic_load_explicit(&dev->state, memory_order_acquire) != DEVICE_SUSPENDED) {
        device_state_unlock(dev);
        device_unlock();
        return K_EBUSY;
    }
    ops = dev->ops;
    if (ops == 0 || (ops->set_power == 0 && ops->begin_power == 0) ||
        (ops->begin_power != 0 && ops->poll_power == 0)) {
        device_state_unlock(dev);
        device_unlock();
        return K_ENOSYS;
    }
    atomic_store_explicit(&dev->state, DEVICE_RECOVERING, memory_order_release);
    device_state_unlock(dev);
    device_unlock();

    if (!device_wait_io_idle(dev, timeout_ns)) {
        device_state_lock(dev);
        atomic_store_explicit(&dev->state, DEVICE_FAILED, memory_order_release);
        device_state_unlock(dev);
        return K_ETIMEDOUT;
    }
    status = device_call_power(dev, ops, DEVICE_POWER_ACTIVE, timeout_ns);
    device_state_lock(dev);
    atomic_store_explicit(&dev->state,
                          status == K_OK ? DEVICE_ACTIVE : DEVICE_FAILED,
                          memory_order_release);
    device_state_unlock(dev);
    return status;
}

kstatus_t device_resume(device_t *dev) {
    if (dev == 0) return K_EINVAL;
    return device_resume_timeout(dev, dev->power_timeout_ns);
}

kstatus_t device_set_power_timeout(device_t *dev, uint64_t timeout_ns) {
    if (dev == 0 || timeout_ns == 0U) return K_EINVAL;
    device_initialize();
    device_lock();
    if (find_device_slot(dev) < 0) {
        device_unlock();
        return K_ENOENT;
    }
    device_state_lock(dev);
    uint32_t state = atomic_load_explicit(&dev->state, memory_order_acquire);
    if (state == DEVICE_REMOVED || state == DEVICE_REMOVING ||
        state == DEVICE_RECOVERING) {
        device_state_unlock(dev);
        device_unlock();
        return K_EBUSY;
    }
    dev->power_timeout_ns = timeout_ns;
    device_state_unlock(dev);
    device_unlock();
    return K_OK;
}

bool device_io_begin(device_t *dev, io_request_t *req) {
    bool accepted = false;
    if (dev == 0 || req == 0) return false;
    device_initialize();
    device_lock();
    if (find_device_slot(dev) < 0) {
        device_unlock();
        return false;
    }
    device_state_lock(dev);
    uint32_t state = atomic_load_explicit(&dev->state, memory_order_acquire);
    if (state == DEVICE_ACTIVE &&
        atomic_load_explicit(&req->state, memory_order_acquire) == IOREQ_QUEUED &&
        list_empty(&req->device_node) &&
        object_try_get(dev)) {
        list_add_tail(&dev->io_requests, &req->device_node);
        atomic_fetch_add_explicit(&dev->io_inflight, 1U, memory_order_acq_rel);
        req->device_ref_held = 1U;
        accepted = true;
    }
    device_state_unlock(dev);
    device_unlock();
    return accepted;
}

bool device_io_end(device_t *dev, io_request_t *req) {
    bool linked;
    bool reference_held;
    if (dev == 0 || req == 0) return false;
    device_initialize();
    device_lock();
    reference_held = req->device_ref_held != 0U;
    req->device_ref_held = 0U;
    linked = req->device_node.next != 0 && req->device_node.prev != 0 &&
             req->device_node.next != &req->device_node;
    if (linked) {
        list_del(&req->device_node);
        uint32_t current = atomic_load_explicit(&dev->io_inflight,
                                                memory_order_acquire);
        while (current != 0U &&
               !atomic_compare_exchange_weak_explicit(&dev->io_inflight, &current,
                                                      current - 1U,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire)) { }
    }
    device_unlock();
    return reference_held;
}

kstatus_t device_reset(device_t *dev, uint32_t level) {
    kstatus_t status;
    const device_ops_t *ops;

    if (dev == 0) return K_EINVAL;
    device_initialize();
    device_lock();
    if (find_device_slot(dev) < 0) {
        device_unlock();
        return K_ENOENT;
    }
    device_state_lock(dev);
    uint32_t state = atomic_load_explicit(&dev->state, memory_order_acquire);
    if (state == DEVICE_REMOVING || state == DEVICE_REMOVED ||
        state == DEVICE_RECOVERING) {
        device_state_unlock(dev);
        device_unlock();
        return K_EBUSY;
    }
    if (atomic_load_explicit(&dev->io_inflight, memory_order_acquire) != 0U) {
        device_state_unlock(dev);
        device_unlock();
        return K_EBUSY;
    }
    ops = dev->ops;
    if (ops == 0 || ops->reset == 0) {
        device_state_unlock(dev);
        device_unlock();
        return K_ENOSYS;
    }
    atomic_store_explicit(&dev->state, DEVICE_RECOVERING, memory_order_release);
    device_state_unlock(dev);
    device_unlock();

    status = ops->reset(dev, level);
    device_state_lock(dev);
    atomic_store_explicit(&dev->state,
                          status == K_OK ? DEVICE_ACTIVE : DEVICE_FAILED,
                          memory_order_release);
    device_state_unlock(dev);
    return status;
}

kstatus_t driver_register(driver_t *drv) {
    if (drv == 0 || drv->object.type != DRIVER_OBJECT_TYPE || drv->probe == 0 ||
        drv->api_version != OS_DRIVER_API_VERSION ||
        drv->struct_size < sizeof(driver_t)) {
        return K_EINVAL;
    }
    device_initialize();
    device_lock();
    int32_t slot;
    if (find_driver_slot_with_free(drv, &slot) >= 0) {
        device_unlock();
        return K_EBUSY;
    }
    if (slot < 0) {
        device_unlock();
        return K_ENOMEM;
    }
    g_drivers[slot] = drv;
    object_get(drv);
    device_unlock();

    for (uint32_t i = 0; i < DEVICE_REGISTRY_LIMIT; ++i) {
        device_t *dev;
        device_lock();
        dev = g_devices[i];
        if (dev == 0 || !object_try_get(dev)) {
            device_unlock();
            continue;
        }
        device_unlock();
        (void)bind_driver(dev, drv);
        object_put(dev);
    }
    return K_OK;
}

void driver_unregister(driver_t *drv) {
    if (drv == 0) return;
    device_initialize();
    device_t *bound_devices[DEVICE_REGISTRY_LIMIT];
    uint32_t bound_count = 0;
    device_lock();
    int32_t slot = find_driver_slot(drv);
    if (slot < 0) {
        device_unlock();
        return;
    }
    g_drivers[slot] = 0;
    for (uint32_t i = 0; i < DEVICE_REGISTRY_LIMIT; ++i) {
        device_t *dev = g_devices[i];
        if (dev == 0 || dev->driver != drv) continue;
        /* 设备可能在解锁后被热拔出，先持有对象引用再调用 remove。 */
        if (!object_try_get(dev)) continue;
        dev->driver = 0;
        atomic_store_explicit(&dev->state, DEVICE_ENUMERATED, memory_order_release);
        if (bound_count < DEVICE_REGISTRY_LIMIT) bound_devices[bound_count++] = dev;
        else object_put(dev);
    }
    device_unlock();
    if (drv->remove != 0) {
        for (uint32_t i = 0; i < bound_count; ++i) {
            drv->remove(bound_devices[i]);
            object_put(bound_devices[i]);
        }
    } else {
        for (uint32_t i = 0; i < bound_count; ++i) object_put(bound_devices[i]);
    }
    object_put(drv);
}
