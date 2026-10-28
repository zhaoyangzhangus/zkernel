#include <kernel/mutex.h>
#include <kernel/sched.h>

#define PI_PRIORITY_NONE RT_PRIORITY_LEVELS
#define PI_CHAIN_MAX_DEPTH 32U

typedef struct {
    list_head_t node;
    kmutex_t *mutex;
    thread_t *thread;
    bool linked;
} kmutex_waiter_t;

typedef struct {
    kmutex_t *mutex;
    kmutex_waiter_t *waiter;
    thread_t *thread;
} kmutex_wait_context_t;

static spinlock_t g_pi_lock;
static atomic_uint g_pi_init_state;

static void pi_initialize(void) {
    if (atomic_load_explicit(&g_pi_init_state, memory_order_acquire) == 2U) return;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_pi_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        spinlock_init(&g_pi_lock);
        atomic_store_explicit(&g_pi_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_pi_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static void pi_lock(void) {
    sched_preempt_disable();
    spinlock_lock(&g_pi_lock);
}

static void pi_unlock(void) {
    spinlock_unlock(&g_pi_lock);
    sched_preempt_enable();
}

static kmutex_t *mutex_from_owner_node(list_head_t *node) {
    return (kmutex_t *)((uint8_t *)node - __builtin_offsetof(kmutex_t, owner_node));
}

static kmutex_waiter_t *waiter_from_node(list_head_t *node) {
    return (kmutex_waiter_t *)((uint8_t *)node -
                              __builtin_offsetof(kmutex_waiter_t, node));
}

static uint8_t donation_priority(const thread_t *thread) {
    /* 远端运行队列尚未消费邮箱时，PI 链也必须使用最新的逻辑优先级。 */
    if (atomic_load_explicit(&thread->priority_queued, memory_order_acquire)) {
        uint32_t request = atomic_load_explicit(&thread->priority_request,
                                                memory_order_acquire);
        return (request >> 8U) == SCHED_CLASS_RT ? (uint8_t)request :
               (RT_PRIORITY_LEVELS - 1U);
    }
    return thread->sched_class == SCHED_CLASS_RT ? thread->rt_priority :
           (RT_PRIORITY_LEVELS - 1U);
}

static uint8_t mutex_best_waiter(const kmutex_t *mutex) {
    uint8_t best = PI_PRIORITY_NONE;
    for (list_head_t *node = mutex->pi_waiters.next;
         node != &mutex->pi_waiters; node = node->next) {
        const kmutex_waiter_t *waiter = waiter_from_node(node);
        uint8_t priority = donation_priority(waiter->thread);
        if (priority < best) best = priority;
    }
    return best;
}

static void recompute_thread_priority(thread_t *thread, uint32_t depth) {
    if (thread == 0 || depth >= PI_CHAIN_MAX_DEPTH) return;
    uint8_t target_class = thread->base_sched_class;
    uint8_t target_priority = thread->base_rt_priority;
    for (list_head_t *node = thread->owned_mutexes.next;
         node != &thread->owned_mutexes; node = node->next) {
        kmutex_t *mutex = mutex_from_owner_node(node);
        uint8_t donated = mutex_best_waiter(mutex);
        if (donated != PI_PRIORITY_NONE &&
            (target_class != SCHED_CLASS_RT || donated < target_priority)) {
            target_class = SCHED_CLASS_RT;
            target_priority = donated;
        }
    }
    if (atomic_load_explicit(&thread->priority_queued, memory_order_acquire) ||
        thread->sched_class != target_class ||
        (target_class == SCHED_CLASS_RT && thread->rt_priority != target_priority)) {
        sched_set_effective_priority(thread, target_class, target_priority);
    }
    kmutex_t *blocked_on = (kmutex_t *)thread->pi_blocked_on;
    if (blocked_on != 0 && blocked_on->owner != 0 && blocked_on->owner != thread) {
        recompute_thread_priority(blocked_on->owner, depth + 1U);
    }
}

static bool would_deadlock(thread_t *thread, kmutex_t *mutex) {
    thread_t *owner = mutex->owner;
    for (uint32_t depth = 0; owner != 0 && depth < PI_CHAIN_MAX_DEPTH; ++depth) {
        if (owner == thread) return true;
        kmutex_t *blocked_on = (kmutex_t *)owner->pi_blocked_on;
        owner = blocked_on != 0 ? blocked_on->owner : 0;
    }
    return owner != 0;
}

static void assign_owner(kmutex_t *mutex, thread_t *thread) {
    mutex->owner = thread;
    mutex->exit_deferred = false;
    if (mutex->pi_enabled) {
        list_add_tail(&thread->owned_mutexes, &mutex->owner_node);
    }
}

void kmutex_init(kmutex_t *mutex, bool priority_inheritance) {
    if (mutex == 0) return;
    pi_initialize();
    wait_queue_init(&mutex->waitq);
    mutex->owner = 0;
    list_init(&mutex->owner_node);
    list_init(&mutex->pi_waiters);
    mutex->pi_enabled = priority_inheritance;
    mutex->initialized = true;
    mutex->exit_deferred = false;
}

kstatus_t kmutex_try_lock(kmutex_t *mutex) {
    if (mutex == 0 || !mutex->initialized) return K_EINVAL;
    thread_t *current = sched_current_thread();
    if (current == 0) return K_EPERM;
    pi_lock();
    kstatus_t status;
    if (mutex->owner == 0) {
        assign_owner(mutex, current);
        status = K_OK;
    } else {
        status = mutex->owner == current ? K_EBUSY : K_EAGAIN;
    }
    pi_unlock();
    return status;
}

kstatus_t kmutex_try_lock_bootstrap(kmutex_t *mutex) {
    if (mutex == 0 || !mutex->initialized) return K_EINVAL;
    thread_t *current = sched_current_thread();
    if (current == 0 || current->sched_class != SCHED_CLASS_IDLE) return K_EPERM;
    pi_lock();
    kstatus_t status;
    if (mutex->owner == 0) {
        assign_owner(mutex, current);
        status = K_OK;
    } else {
        status = mutex->owner == current ||
                 mutex->owner->owner_cpu == current->owner_cpu ? K_EBUSY : K_EAGAIN;
    }
    pi_unlock();
    return status;
}

static bool kmutex_try_acquire_predicate(void *opaque) {
    kmutex_wait_context_t *context = (kmutex_wait_context_t *)opaque;
    bool acquired = false;
    pi_lock();
    if (context->mutex->owner == 0) {
        if (context->waiter->linked) {
            list_del(&context->waiter->node);
            context->waiter->linked = false;
        }
        context->thread->pi_blocked_on = 0;
        assign_owner(context->mutex, context->thread);
        recompute_thread_priority(context->thread, 0);
        acquired = true;
    } else if (context->mutex->pi_enabled) {
        recompute_thread_priority(context->mutex->owner, 0);
    }
    pi_unlock();
    return acquired;
}

static kstatus_t kmutex_lock_internal(kmutex_t *mutex, uint64_t timeout_ns,
                                      bool uninterruptible) {
    if (mutex == 0 || !mutex->initialized) return K_EINVAL;
    thread_t *current = sched_current_thread();
    if (current == 0 || current->sched_class == SCHED_CLASS_IDLE) return K_EPERM;
    kmutex_waiter_t waiter;
    list_init(&waiter.node);
    waiter.mutex = mutex;
    waiter.thread = current;
    waiter.linked = false;

    pi_lock();
    if (mutex->owner == 0) {
        assign_owner(mutex, current);
        pi_unlock();
        return K_OK;
    }
    if (mutex->owner == current || would_deadlock(current, mutex)) {
        pi_unlock();
        return K_EBUSY;
    }
    list_add_tail(&mutex->pi_waiters, &waiter.node);
    waiter.linked = true;
    current->pi_blocked_on = mutex;
    if (mutex->pi_enabled) recompute_thread_priority(mutex->owner, 0);
    pi_unlock();

    kmutex_wait_context_t context = {mutex, &waiter, current};
    kstatus_t status = uninterruptible ?
        wait_on_queue_uninterruptible(&mutex->waitq, kmutex_try_acquire_predicate,
                                       &context, timeout_ns) :
        wait_on_queue(&mutex->waitq, kmutex_try_acquire_predicate,
                       &context, timeout_ns);
    pi_lock();
    if (waiter.linked) {
        list_del(&waiter.node);
        waiter.linked = false;
    }
    if (current->pi_blocked_on == mutex) current->pi_blocked_on = 0;
    if (mutex->pi_enabled && mutex->owner != 0) {
        recompute_thread_priority(mutex->owner, 0);
    }
    pi_unlock();
    return status;
}

kstatus_t kmutex_lock(kmutex_t *mutex, uint64_t timeout_ns) {
    return kmutex_lock_internal(mutex, timeout_ns, false);
}

kstatus_t kmutex_lock_uninterruptible(kmutex_t *mutex, uint64_t timeout_ns) {
    if (mutex == 0 || !mutex->initialized) return K_EINVAL;
    thread_t *current = sched_current_thread();
    if (current == 0 || current->sched_class == SCHED_CLASS_IDLE) return K_EPERM;
    /* 保护锁获取和所有等待栈清理，而不只是锁所有权。
     * 否则 READY 等待者/所有者的续执行可能因 EXIT 而丢失。 */
    sched_exit_defer_begin(current);
    kstatus_t status = kmutex_lock_internal(mutex, timeout_ns, true);
    if (status == K_OK) {
        mutex->exit_deferred = true;
    } else {
        sched_exit_defer_end(current);
    }
    return status;
}

kstatus_t kmutex_unlock(kmutex_t *mutex) {
    if (mutex == 0 || !mutex->initialized) return K_EINVAL;
    thread_t *current = sched_current_thread();
    if (current == 0) return K_EPERM;
    pi_lock();
    if (mutex->owner != current) {
        pi_unlock();
        return K_EPERM;
    }
    if (mutex->pi_enabled && mutex->owner_node.next != &mutex->owner_node) {
        list_del(&mutex->owner_node);
    }
    bool exit_deferred = mutex->exit_deferred;
    mutex->exit_deferred = false;
    mutex->owner = 0;
    if (mutex->pi_enabled) recompute_thread_priority(current, 0);
    pi_unlock();
    /* 唤醒全部竞争者，让仍在等待的线程重新向新所有者传播优先级。 */
    (void)wake_all(&mutex->waitq);
    if (exit_deferred) sched_exit_defer_end(current);
    return K_OK;
}

bool kmutex_pi_self_test(void) {
    kmutex_t first;
    kmutex_t second;
    thread_t low;
    thread_t middle;
    thread_t high;
    kmutex_waiter_t high_waiter;
    kmutex_waiter_t low_waiter;
    sched_initialize_test_thread(&low, 1U, SCHED_CLASS_FAIR, 0U);
    sched_initialize_test_thread(&middle, 2U, SCHED_CLASS_FAIR, 0U);
    sched_initialize_test_thread(&high, 3U, SCHED_CLASS_RT, 2U);
    kmutex_init(&first, true);
    kmutex_init(&second, true);
    list_init(&high_waiter.node);
    high_waiter.mutex = &first;
    high_waiter.thread = &high;
    high_waiter.linked = true;
    list_init(&low_waiter.node);
    low_waiter.mutex = &second;
    low_waiter.thread = &low;
    low_waiter.linked = true;

    pi_lock();
    assign_owner(&first, &low);
    assign_owner(&second, &middle);
    low.pi_blocked_on = &second;
    list_add_tail(&first.pi_waiters, &high_waiter.node);
    list_add_tail(&second.pi_waiters, &low_waiter.node);
    recompute_thread_priority(&low, 0);
    bool donated = low.sched_class == SCHED_CLASS_RT && low.rt_priority == 2U &&
                   middle.sched_class == SCHED_CLASS_RT && middle.rt_priority == 2U;
    list_del(&high_waiter.node);
    list_del(&low_waiter.node);
    low.pi_blocked_on = 0;
    if (first.owner_node.next != &first.owner_node) list_del(&first.owner_node);
    if (second.owner_node.next != &second.owner_node) list_del(&second.owner_node);
    first.owner = 0;
    second.owner = 0;
    recompute_thread_priority(&low, 0);
    recompute_thread_priority(&middle, 0);
    bool restored = low.sched_class == SCHED_CLASS_FAIR &&
                    middle.sched_class == SCHED_CLASS_FAIR;
    pi_unlock();
    return donated && restored;
}
