#include <kernel/input.h>
#include <kernel/sched.h>
#include <kernel/string.h>

#define INPUT_COALESCE_THRESHOLD (INPUT_CORE_CAPACITY * 3U / 4U)

static struct {
    spinlock_t lock;
    input_event_t events[INPUT_CORE_CAPACITY];
    uint32_t read;
    uint32_t write;
    uint32_t count;
    atomic_uint pending;
    atomic_uint_fast64_t dropped;
    atomic_uintptr_t wake_queue;
    wait_queue_t waitq;
    bool initialized;
} g_input;

static void input_lock(void) {
    /* input_core_push runs from device deferred work while input_core_read
     * runs in the user syscall path.  Never let the owner be preempted on a
     * one-vCPU guest while the other path spins on this lock. */
    sched_preempt_disable();
    spinlock_lock(&g_input.lock);
}

static void input_unlock(void) {
    spinlock_unlock(&g_input.lock);
    sched_preempt_enable();
}

static uint64_t input_timestamp(void) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static void input_publish_pending(void) {
    /* The ring lock serializes writers; publish its authoritative count
     * without another locked read-modify-write on the mirror cache line. */
    atomic_store_explicit(&g_input.pending, g_input.count, memory_order_release);
}

static int32_t input_add_delta(int32_t left, int32_t right) {
    int64_t sum = (int64_t)left + right;
    if (sum > INT32_MAX) return INT32_MAX;
    if (sum < INT32_MIN) return INT32_MIN;
    return (int32_t)sum;
}

static bool input_delta_can_merge(int32_t left, int32_t right) {
    /* 反向移动必须保留边缘截断的顺序；溢出也不能靠饱和加法丢失位移。 */
    int64_t sum = (int64_t)left + right;
    return !(left < 0 && right > 0) && !(left > 0 && right < 0) &&
           sum >= INT32_MIN && sum <= INT32_MAX;
}

static bool input_event_is_motion(const input_event_t *event) {
    return event != 0 &&
           (event->type == INPUT_EVENT_RELATIVE ||
            (event->type == INPUT_EVENT_POINTER && event->code == 0U));
}

static bool input_coalesce_motion(const input_event_t *incoming) {
    if (incoming == 0 || g_input.count == 0U) return false;
    bool relative = incoming->type == INPUT_EVENT_RELATIVE;
    if (!relative && (incoming->type != INPUT_EVENT_POINTER ||
                      incoming->code != 0U)) return false;
    /* X and Y are emitted as separate events by boot-mouse HID.  Under queue
     * pressure, coalesce within the trailing motion run so a busy pointer
     * cannot fill the ring merely because the axes alternate.  Never cross a
     * key/button event. */
    for (uint32_t offset = 0U; offset < g_input.count; ++offset) {
        uint32_t index = (g_input.write + INPUT_CORE_CAPACITY - 1U - offset) %
                         INPUT_CORE_CAPACITY;
        input_event_t *queued = &g_input.events[index];
        if (queued->type != incoming->type ||
            (!relative && queued->code != 0U)) break;
        if (queued->device_id != incoming->device_id ||
            queued->flags != incoming->flags ||
            (relative && queued->code != incoming->code)) continue;
        if (relative) {
            if (!input_delta_can_merge(queued->value, incoming->value)) return false;
            queued->value = input_add_delta(queued->value, incoming->value);
        } else {
            if (!input_delta_can_merge(queued->value2, incoming->value2) ||
                !input_delta_can_merge(queued->value3, incoming->value3) ||
                !input_delta_can_merge(queued->value4, incoming->value4)) return false;
            queued->value2 = input_add_delta(queued->value2, incoming->value2);
            queued->value3 = input_add_delta(queued->value3, incoming->value3);
            queued->value4 = input_add_delta(queued->value4, incoming->value4);
        }
        queued->timestamp = incoming->timestamp;
        return true;
    }
    return false;
}

/* Remove one queued pointer-motion event without disturbing key/button order. */
static bool input_drop_oldest_motion(void) {
    uint32_t offset;
    uint32_t target = 0U;
    bool found = false;
    if (g_input.count == 0U) return false;
    for (offset = 0U; offset < g_input.count; ++offset) {
        uint32_t index = (g_input.read + offset) % INPUT_CORE_CAPACITY;
        if (input_event_is_motion(&g_input.events[index])) {
            target = index;
            found = true;
            break;
        }
    }
    if (!found) return false;
    /* A full ring has read==write; use the event count, not that sentinel.
     * Shift the shorter side so deleting the oldest motion needs no copy. */
    uint32_t suffix = g_input.count - offset - 1U;
    if (offset < suffix) {
        while (offset-- != 0U) {
            uint32_t previous = (target + INPUT_CORE_CAPACITY - 1U) %
                                INPUT_CORE_CAPACITY;
            g_input.events[target] = g_input.events[previous];
            target = previous;
        }
        g_input.read = (g_input.read + 1U) % INPUT_CORE_CAPACITY;
    } else {
        while (suffix-- != 0U) {
            uint32_t next = (target + 1U) % INPUT_CORE_CAPACITY;
            g_input.events[target] = g_input.events[next];
            target = next;
        }
        g_input.write = (g_input.write + INPUT_CORE_CAPACITY - 1U) %
                        INPUT_CORE_CAPACITY;
    }
    --g_input.count;
    input_publish_pending();
    atomic_fetch_add_explicit(&g_input.dropped, 1U, memory_order_relaxed);
    return true;
}

static void input_notify_waiters(void) {
    (void)wake_one(&g_input.waitq);
    wait_queue_t *wake_queue = (wait_queue_t *)(uintptr_t)
        atomic_load_explicit(&g_input.wake_queue, memory_order_acquire);
    if (wake_queue != 0) (void)wake_one(wake_queue);
}

bool input_core_init(void) {
    spinlock_init(&g_input.lock);
    g_input.read = 0U;
    g_input.write = 0U;
    g_input.count = 0U;
    atomic_init(&g_input.pending, 0U);
    atomic_init(&g_input.dropped, 0U);
    atomic_init(&g_input.wake_queue, (uintptr_t)0U);
    wait_queue_init(&g_input.waitq);
    g_input.initialized = true;
    return true;
}

void input_core_bind_wakeup(wait_queue_t *wait_queue) {
    atomic_store_explicit(&g_input.wake_queue, (uintptr_t)wait_queue,
                          memory_order_release);
}

kstatus_t input_core_push(const input_event_t *event) {
    input_event_t incoming;
    if (event == 0 || !g_input.initialized) return K_EINVAL;
    incoming = *event;
    if (incoming.timestamp == 0U) incoming.timestamp = input_timestamp();
    input_lock();
    if (g_input.count >= INPUT_COALESCE_THRESHOLD &&
        input_coalesce_motion(&incoming)) {
        input_unlock();
        input_notify_waiters();
        return K_OK;
    }
    if (g_input.count >= INPUT_CORE_CAPACITY) {
        /*
         * 输入设备在中断/延迟轮询上下文中不能等待用户态消费者。
         * 旧实现直接返回 EAGAIN，xHCI 又只能丢弃这个返回值；一旦
         * 图形帧提交暂时变慢，队列就会一直满着，后续按键全部消失。
         * 丢弃最旧事件并接收最新事件可使队列恢复流动；键盘状态的
         * 最新变化比过期的重复/移动事件更有价值。
         */
        if (!input_drop_oldest_motion()) {
            if (input_event_is_motion(&incoming) ||
                incoming.type == INPUT_EVENT_POINTER) {
                atomic_fetch_add_explicit(&g_input.dropped, 1U,
                                          memory_order_relaxed);
                input_unlock();
                return K_EAGAIN;
            }
            g_input.read = (g_input.read + 1U) % INPUT_CORE_CAPACITY;
            --g_input.count;
            input_publish_pending();
            atomic_fetch_add_explicit(&g_input.dropped, 1U, memory_order_relaxed);
        }
    }
    g_input.events[g_input.write] = incoming;
    g_input.write = (g_input.write + 1U) % INPUT_CORE_CAPACITY;
    ++g_input.count;
    input_publish_pending();
    input_unlock();
    input_notify_waiters();
    return K_OK;
}

kstatus_t input_core_push_pointer(const input_pointer_motion_t *motion) {
    input_event_t event = {0};
    if (motion == 0) return K_EINVAL;
    event.timestamp = motion->timestamp;
    event.device_id = motion->device_id;
    event.type = INPUT_EVENT_POINTER;
    event.flags = motion->flags;
    event.code = motion->buttons_changed;
    event.value = motion->buttons;
    event.value2 = motion->dx;
    event.value3 = motion->dy;
    event.value4 = motion->wheel;
    return input_core_push(&event);
}

kstatus_t input_core_pop(input_event_t *event) {
    if (event == 0 || !g_input.initialized) return K_EINVAL;
    input_lock();
    if (g_input.count == 0U) {
        input_unlock();
        return K_EAGAIN;
    }
    *event = g_input.events[g_input.read];
    g_input.read = (g_input.read + 1U) % INPUT_CORE_CAPACITY;
    --g_input.count;
    input_publish_pending();
    input_unlock();
    return K_OK;
}

size_t input_core_pop_batch(input_event_t *events, size_t capacity) {
    if (events == 0 || capacity == 0U || !g_input.initialized) return 0U;
    input_lock();
    size_t count = g_input.count < capacity ? g_input.count : capacity;
    if (count != 0U) {
        size_t first = INPUT_CORE_CAPACITY - g_input.read;
        if (first > count) first = count;
        memcpy(events, &g_input.events[g_input.read], first * sizeof(*events));
        if (first < count)
            memcpy(events + first, g_input.events,
                   (count - first) * sizeof(*events));
        g_input.read = (g_input.read + (uint32_t)count) % INPUT_CORE_CAPACITY;
        g_input.count -= (uint32_t)count;
        input_publish_pending();
    }
    input_unlock();
    return count;
}

size_t input_core_pop_cursor_motion(input_event_t *events, size_t capacity) {
    if (events == 0 || capacity == 0U || !g_input.initialized) return 0U;
    size_t count = 0U;
    input_lock();
    while (count < capacity && g_input.count != 0U) {
        const input_event_t *event = &g_input.events[g_input.read];
        bool relative = event->type == INPUT_EVENT_RELATIVE &&
            (event->code == INPUT_REL_X || event->code == INPUT_REL_Y);
        bool pointer = event->type == INPUT_EVENT_POINTER &&
            event->code == 0U && event->value4 == 0;
        if (!relative && !pointer) break;
        events[count++] = *event;
        g_input.read = (g_input.read + 1U) % INPUT_CORE_CAPACITY;
        --g_input.count;
    }
    if (count != 0U) input_publish_pending();
    input_unlock();
    return count;
}

static bool input_has_event(void *context) {
    bool has_event;
    (void)context;
    /* 等待条件必须读取真实队列状态，避免原子计数与环形队列短暂不一致。 */
    input_lock();
    has_event = g_input.count != 0U;
    input_unlock();
    return has_event;
}

kstatus_t input_core_read(input_event_t *event, uint64_t timeout_ns) {
    kstatus_t status;
    if (event == 0 || !g_input.initialized) return K_EINVAL;
    for (;;) {
        status = input_core_pop(event);
        if (status == K_OK || timeout_ns == 0U) return status;
        status = wait_on_queue(&g_input.waitq, input_has_event, 0, timeout_ns);
        if (status != K_OK) return status;
        /* 唤醒可能是提示性的；重新从队列取出，避免把 EAGAIN 传播给用户态。 */
    }
}

uint32_t input_core_pending(void) {
    if (!g_input.initialized) return 0U;
    return atomic_load_explicit(&g_input.pending, memory_order_acquire);
}

kstatus_t input_core_wait(uint64_t timeout_ns) {
    if (!g_input.initialized) return K_EINVAL;
    if (input_core_pending() != 0U) return K_OK;
    return wait_on_queue(&g_input.waitq, input_has_event, 0, timeout_ns);
}

uint64_t input_core_dropped(void) {
    if (!g_input.initialized) return 0U;
    return atomic_load_explicit(&g_input.dropped, memory_order_acquire);
}

bool input_core_self_test(void) {
    input_event_t sent = {
        .timestamp = 0U,
        .device_id = 1U,
        .type = INPUT_EVENT_KEY,
        .flags = 0U,
        .code = 0x04U,
        .value = INPUT_VALUE_PRESS,
    };
    input_pointer_motion_t pointer = {
        .timestamp = 13U,
        .device_id = 2U,
        .flags = 0U,
        .buttons_changed = 1U,
        .buttons = 1U,
        .dx = 3,
        .dy = -2,
        .wheel = 1,
    };
    input_event_t received = {0};
    if (!g_input.initialized || input_core_push(&sent) != K_OK ||
        input_core_pop(&received) != K_OK || received.device_id != sent.device_id ||
        received.type != INPUT_EVENT_KEY || received.code != sent.code ||
        received.value != sent.value || received.timestamp == 0U) {
        return false;
    }
    if (input_core_push_pointer(&pointer) != K_OK ||
        input_core_pop(&received) != K_OK ||
        received.type != INPUT_EVENT_POINTER ||
        received.code != pointer.buttons_changed ||
        received.value != pointer.buttons ||
        received.value2 != pointer.dx || received.value3 != pointer.dy ||
        received.value4 != pointer.wheel) {
        return false;
    }
    return input_core_pending() == 0U && input_core_dropped() == 0U;
}
