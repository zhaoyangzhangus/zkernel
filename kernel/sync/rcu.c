#include <kernel/rcu.h>
#include <kernel/sched.h>
#include <kernel/string.h>

typedef struct rcu_callback {
    rcu_callback_fn callback;
    void *argument;
} rcu_callback_t;

static struct {
    spinlock_t gate;
    spinlock_t lock;
    atomic_uint init_state;
    atomic_uint reader_count;
    rcu_callback_t callbacks[RCU_CALLBACK_CAPACITY];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} g_rcu;

static void rcu_lock(void) {
    sched_preempt_disable();
    spinlock_lock(&g_rcu.lock);
}

static void rcu_gate_lock(void) {
    sched_preempt_disable();
    spinlock_lock(&g_rcu.gate);
}

static void rcu_gate_unlock(void) {
    spinlock_unlock(&g_rcu.gate);
    sched_preempt_enable();
}

static void rcu_unlock(void) {
    spinlock_unlock(&g_rcu.lock);
    sched_preempt_enable();
}

bool rcu_init(void) {
    if (atomic_load_explicit(&g_rcu.init_state, memory_order_acquire) == 2U) return true;
    unsigned expected = 0U;
    sched_preempt_disable();
    if (atomic_compare_exchange_strong_explicit(&g_rcu.init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        spinlock_init(&g_rcu.gate);
        spinlock_init(&g_rcu.lock);
        atomic_init(&g_rcu.reader_count, 0U);
        g_rcu.head = 0U;
        g_rcu.tail = 0U;
        g_rcu.count = 0U;
        atomic_store_explicit(&g_rcu.init_state, 2U, memory_order_release);
        sched_preempt_enable();
        return true;
    }
    sched_preempt_enable();
    while (atomic_load_explicit(&g_rcu.init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

void rcu_read_lock(void) {
    if (!rcu_init()) return;
    rcu_gate_lock();
    atomic_fetch_add_explicit(&g_rcu.reader_count, 1U, memory_order_acquire);
    rcu_gate_unlock();
}

void rcu_read_unlock(void) {
    if (atomic_load_explicit(&g_rcu.init_state, memory_order_acquire) != 2U) return;
    rcu_gate_lock();
    unsigned count = atomic_load_explicit(&g_rcu.reader_count, memory_order_relaxed);
    if (count != 0U) atomic_store_explicit(&g_rcu.reader_count, count - 1U,
                                           memory_order_release);
    rcu_gate_unlock();
}

bool rcu_read_held(void) {
    if (atomic_load_explicit(&g_rcu.init_state, memory_order_acquire) != 2U) return false;
    return atomic_load_explicit(&g_rcu.reader_count, memory_order_acquire) != 0U;
}

kstatus_t rcu_call(rcu_callback_fn callback, void *argument) {
    if (callback == 0 || !rcu_init()) return K_EINVAL;
    rcu_lock();
    if (g_rcu.count == RCU_CALLBACK_CAPACITY) {
        rcu_unlock();
        return K_ENOMEM;
    }
    g_rcu.callbacks[g_rcu.tail].callback = callback;
    g_rcu.callbacks[g_rcu.tail].argument = argument;
    g_rcu.tail = (g_rcu.tail + 1U) % RCU_CALLBACK_CAPACITY;
    ++g_rcu.count;
    rcu_unlock();
    return K_OK;
}

/* 调用方持有回调队列锁，并提供与队列不别名且容量足以容纳所选批次的缓冲区。
 * 最多复制两个连续区间，并在最后统一发布索引，而不是每个回调后都更新。 */
static uint32_t rcu_take_callbacks_locked(rcu_callback_t *out, uint32_t budget) {
    uint32_t count = g_rcu.count < budget ? g_rcu.count : budget;
    if (count == 0U) return 0U;
    uint32_t first = RCU_CALLBACK_CAPACITY - g_rcu.head;
    if (first > count) first = count;
    memcpy(out, g_rcu.callbacks + g_rcu.head, first * sizeof(*out));
    if (count > first)
        memcpy(out + first, g_rcu.callbacks, (count - first) * sizeof(*out));
    g_rcu.head = (g_rcu.head + count) % RCU_CALLBACK_CAPACITY;
    g_rcu.count -= count;
    return count;
}

uint32_t rcu_poll(uint32_t budget) {
    rcu_callback_t callbacks[RCU_CALLBACK_CAPACITY];
    uint32_t count = 0U;
    if (budget == 0U || !rcu_init()) return 0U;
    if (budget > RCU_CALLBACK_CAPACITY) budget = RCU_CALLBACK_CAPACITY;
    /* 闸门保证检查期间不会有新的读侧进入。 */
    rcu_gate_lock();
    if (atomic_load_explicit(&g_rcu.reader_count, memory_order_acquire) != 0U) {
        rcu_gate_unlock();
        return 0U;
    }
    rcu_lock();
    count = rcu_take_callbacks_locked(callbacks, budget);
    rcu_unlock();
    rcu_gate_unlock();
    for (uint32_t i = 0; i < count; ++i) {
        if (callbacks[i].callback != 0) callbacks[i].callback(callbacks[i].argument);
    }
    return count;
}

kstatus_t rcu_synchronize(void) {
    rcu_callback_t callbacks[RCU_CALLBACK_CAPACITY];
    uint32_t count = 0U;
    if (!rcu_init() || rcu_read_held()) return K_EBUSY;
    for (;;) {
        rcu_gate_lock();
        if (atomic_load_explicit(&g_rcu.reader_count, memory_order_acquire) != 0U) {
            rcu_gate_unlock();
            __asm__ volatile ("pause");
            continue;
        }
        /* gate 锁住新读者后摘下队列，确保本次 grace period 不会被新读者
         * 插入窗口打断；回调在释放 gate 后执行，避免回调重入 RCU 锁。 */
        rcu_lock();
        count = rcu_take_callbacks_locked(callbacks, RCU_CALLBACK_CAPACITY);
        rcu_unlock();
        rcu_gate_unlock();
        break;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (callbacks[i].callback != 0) callbacks[i].callback(callbacks[i].argument);
    }
    return K_OK;
}

static void rcu_test_callback(void *argument) {
    atomic_uint *count = (atomic_uint *)argument;
    if (count != 0) atomic_fetch_add_explicit(count, 1U, memory_order_relaxed);
}

bool rcu_self_test(void) {
    atomic_uint callback_count;
    atomic_init(&callback_count, 0U);
    if (!rcu_init()) return false;
    rcu_read_lock();
    if (!rcu_read_held() || rcu_call(rcu_test_callback, &callback_count) != K_OK ||
        rcu_poll(1U) != 0U) {
        rcu_read_unlock();
        return false;
    }
    rcu_read_unlock();
    return rcu_poll(1U) == 1U &&
           atomic_load_explicit(&callback_count, memory_order_relaxed) == 1U &&
           rcu_synchronize() == K_OK;
}
