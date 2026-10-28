#include <arch/x86_64/cpu.h>
#include <kernel/mm.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/wait.h>

static spinlock_t g_timeout_lock;
static list_head_t g_timeout_waiters;
static atomic_uint g_wait_init_state;
enum { WAIT_WAKE_BATCH_COUNT = 16U };

typedef struct wait_delay_diagnostic {
    uint64_t tid, queue, total_ns, before_claim_ns, after_claim_ns;
} wait_delay_diagnostic_t;
static volatile uint64_t g_wait_delay_tid;
static volatile wait_delay_diagnostic_t g_wait_delays[32];
/* 在超时锁保护下调用：唤醒路径不额外加锁或分配内存。 */
static void wait_record_delay(const waiter_t *waiter, uint64_t resumed) {
    if (waiter->thread->tid != g_wait_delay_tid ||
        waiter->diagnostic_claimed_tsc == 0U) return;
    uint64_t total = x86_tsc_to_ns(resumed - waiter->diagnostic_started_tsc);
    if (total < 10000000ULL) return;
    uint64_t queue = (uint64_t)(uintptr_t)waiter->queue;
    for (size_t index = 0U; index < 32U; ++index) {
        if (g_wait_delays[index].tid != 0U &&
            (g_wait_delays[index].tid != waiter->thread->tid ||
             g_wait_delays[index].queue != queue)) continue;
        if (total > g_wait_delays[index].total_ns)
            g_wait_delays[index] = (wait_delay_diagnostic_t){
                waiter->thread->tid, queue, total,
                x86_tsc_to_ns(waiter->diagnostic_claimed_tsc -
                              waiter->diagnostic_started_tsc),
                x86_tsc_to_ns(resumed - waiter->diagnostic_claimed_tsc)};
        break;
    }
}

static void wait_start_diagnostic(waiter_t *waiter) {
    uint64_t target = g_wait_delay_tid;
    waiter->diagnostic_started_tsc = target != 0U && waiter->thread->tid == target ?
                                      x86_read_tsc() : 0U;
    waiter->diagnostic_claimed_tsc = 0U;
}

static uint64_t wait_resume_timestamp(const waiter_t *waiter) {
    /* 只有所有者会写 started_tsc。无关调度唤醒后，claimed_tsc 仍可能在超时锁下变化。 */
    return waiter->diagnostic_started_tsc != 0U ? x86_read_tsc() : 0U;
}

static uint64_t wait_lock(spinlock_t *lock) {
    uint64_t flags = 0U;
    sched_preempt_disable();
#if !defined(OS_WAIT_HOST_TEST)
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
#endif
    spinlock_lock(lock);
    return flags;
}

static void wait_unlock(spinlock_t *lock, uint64_t flags) {
    spinlock_unlock(lock);
    sched_preempt_enable();
#if !defined(OS_WAIT_HOST_TEST)
    if ((flags & (1ULL << 9)) != 0U) {
        __asm__ volatile ("sti" : : : "memory");
    } else {
        __asm__ volatile ("cli" : : : "memory");
    }
#else
    (void)flags;
#endif
}

static void wait_insert_tail(list_head_t *head, list_head_t *node) {
    node->next = head;
    node->prev = head->prev;
    head->prev->next = node;
    head->prev = node;
}

static void wait_remove(list_head_t *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    list_init(node);
}

static void waiter_unlink_locked(waiter_t *waiter) {
    if (waiter == 0) return;
    for (size_t index = 0U; index < waiter->subscription_count; ++index) {
        list_head_t *node = &waiter->subscriptions[index].queue_node;
        if (node->next != node) wait_remove(node);
    }
    if (waiter->timeout_node.next != &waiter->timeout_node) {
        wait_remove(&waiter->timeout_node);
    }
}

/* 仅由所有者执行清理，同时持有超时锁和所有已订阅队列锁。
 * 先解除链接，再清除取消路径可观察到的栈关联。 */
static void waiter_detach_owner_locked(waiter_t *waiter) {
    waiter_unlink_locked(waiter);
    atomic_store_explicit(&waiter->thread->blocked_waiter, 0,
                          memory_order_release);
}

static wait_subscription_t *subscription_from_node(list_head_t *node) {
    return (wait_subscription_t *)((uint8_t *)node -
                                   __builtin_offsetof(wait_subscription_t,
                                                      queue_node));
}

static waiter_t *waiter_from_timeout_node(list_head_t *node) {
    return (waiter_t *)((uint8_t *)node -
                        __builtin_offsetof(waiter_t, timeout_node));
}

static void wait_global_init(void) {
    if (atomic_load_explicit(&g_wait_init_state, memory_order_acquire) == 2U) return;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_wait_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        spinlock_init(&g_timeout_lock);
        list_init(&g_timeout_waiters);
        atomic_store_explicit(&g_wait_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_wait_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static bool deadline_reached(uint64_t now, uint64_t deadline) {
    return deadline != UINT64_MAX && (int64_t)(now - deadline) >= 0;
}

static uint64_t make_deadline(uint64_t timeout_ns) {
    if (timeout_ns == UINT64_MAX) return UINT64_MAX;
    uint64_t now = x86_read_tsc();
    uint64_t ticks = x86_timeout_ns_to_tsc(timeout_ns);
    return ticks > UINT64_MAX - now ? UINT64_MAX : now + ticks;
}

static bool wait_deadline_reached(uint64_t deadline) {
    return deadline != UINT64_MAX && deadline_reached(x86_read_tsc(), deadline);
}

void wait_queue_init(wait_queue_t *queue) {
    if (queue == 0) return;
    wait_global_init();
    spinlock_init(&queue->lock);
    list_init(&queue->waiters);
    atomic_init(&queue->sequence, 0U);
}

static bool wait_without_scheduler(bool (*predicate)(void *), void *context,
                                   uint64_t deadline) {
    for (;;) {
        if (predicate(context)) return true;
        if (wait_deadline_reached(deadline)) return false;
        __asm__ volatile ("pause");
    }
}

static uint64_t wait_state_and_queues_lock(wait_queue_t *const *queues,
                                           size_t count) {
    uint64_t timeout_flags = wait_lock(&g_timeout_lock);
    for (size_t index = 0U; index < count; ++index) {
        (void)wait_lock(&queues[index]->lock);
    }
    return timeout_flags;
}

static void wait_state_and_queues_unlock(wait_queue_t *const *queues,
                                         size_t count, uint64_t timeout_flags) {
    for (size_t index = count; index != 0U; --index) {
        wait_unlock(&queues[index - 1U]->lock, 0U);
    }
    wait_unlock(&g_timeout_lock, timeout_flags);
}

static void queue_pointer_sift(wait_queue_t **queues, size_t root, size_t count) {
    wait_queue_t *queue = queues[root];
    /* root < count/2 可在不使 size_t 溢出的前提下保证 2*root+1 有界。 */
    while (root < count / 2U) {
        size_t child = 2U * root + 1U;
        if (child + 1U < count &&
            (uintptr_t)queues[child] < (uintptr_t)queues[child + 1U]) ++child;
        if ((uintptr_t)queue >= (uintptr_t)queues[child]) break;
        queues[root] = queues[child];
        root = child;
    }
    queues[root] = queue;
}

static void queue_pointer_sort(wait_queue_t **queues, size_t count) {
    if (count > 16U) {
        /* 保留对已排序数组的线性路径。较大的任意/逆序集合使用无需分配、非递归的 O(n log n) 堆排序。 */
        size_t ordered = 1U;
        while (ordered < count &&
               (uintptr_t)queues[ordered - 1U] <= (uintptr_t)queues[ordered]) ++ordered;
        if (ordered == count) return;
        for (size_t root = count / 2U; root != 0U; --root)
            queue_pointer_sift(queues, root - 1U, count);
        for (size_t end = count; end > 1U;) {
            --end;
            wait_queue_t *queue = queues[0];
            queues[0] = queues[end];
            queues[end] = queue;
            queue_pointer_sift(queues, 0U, end);
        }
        return;
    }
    for (size_t index = 1U; index < count; ++index) {
        wait_queue_t *queue = queues[index];
        size_t position = index;
        while (position != 0U &&
               (uintptr_t)queues[position - 1U] > (uintptr_t)queue) {
            queues[position] = queues[position - 1U];
            --position;
        }
        queues[position] = queue;
    }
}

static kstatus_t wait_on_sorted_queues(wait_queue_t *const *queues,
                                       size_t queue_count,
                                       bool (*predicate)(void *), void *context,
                                       uint64_t deadline, uint32_t flags) {
    thread_t *thread = sched_current_thread();
    bool can_schedule = thread != 0 &&
                        thread->object.type == KOBJECT_TYPE_THREAD &&
                        thread->sched_class != SCHED_CLASS_IDLE;
    if (!can_schedule) {
        return wait_without_scheduler(predicate, context, deadline) ?
               K_OK : K_ETIMEDOUT;
    }

    /* 就绪回调可能获取子系统锁（socket、GPU 等）。绝不能在超时/队列锁下调用：
     * 子系统锁持有者的 IRQ 可能需要同一等待锁才能发布通知。 */
    if (queue_count > WAIT_INLINE_QUEUE_COUNT) {
        bool ready = predicate(context);
        bool expired = !ready && wait_deadline_reached(deadline);
        if (ready || expired) return ready ? K_OK : K_ETIMEDOUT;
    }

    wait_subscription_t inline_subscriptions[WAIT_INLINE_QUEUE_COUNT];
    unsigned inline_sequences[WAIT_INLINE_QUEUE_COUNT];
    wait_subscription_t *subscriptions = inline_subscriptions;
    unsigned *sequences = inline_sequences;
    if (queue_count > WAIT_INLINE_QUEUE_COUNT) {
        const size_t entry_bytes = sizeof(*subscriptions) + sizeof(*sequences);
        if (queue_count > SIZE_MAX / entry_bytes) return K_EOVERFLOW;
        subscriptions = (wait_subscription_t *)kmalloc(
            queue_count * entry_bytes, 0U);
        if (subscriptions == 0) return K_ENOMEM;
        sequences = (unsigned *)(subscriptions + queue_count);
    }

    waiter_t waiter;
    list_init(&waiter.timeout_node);
    waiter.thread = thread;
    waiter.queue = queues[0];
    waiter.subscriptions = subscriptions;
    waiter.subscription_count = queue_count;
    waiter.flags = flags;
    waiter.deadline_tsc = deadline;
    atomic_init(&waiter.state, WAITER_WAITING);
    for (size_t index = 0U; index < queue_count; ++index) {
        list_init(&subscriptions[index].queue_node);
        subscriptions[index].waiter = &waiter;
        subscriptions[index].queue = queues[index];
    }

    kstatus_t result = K_EIO;
    for (;;) {
        for (size_t index = 0U; index < queue_count; ++index) {
            sequences[index] = atomic_load_explicit(
                &queues[index]->sequence, memory_order_acquire);
        }
        if (predicate(context)) {
            result = K_OK;
            break;
        }
        if (wait_deadline_reached(deadline)) {
            result = K_ETIMEDOUT;
            break;
        }

        uint64_t timeout_flags = wait_state_and_queues_lock(queues, queue_count);
        bool changed = false;
        for (size_t index = 0U; index < queue_count; ++index) {
            if (sequences[index] != atomic_load_explicit(
                    &queues[index]->sequence, memory_order_acquire)) {
                changed = true;
                break;
            }
        }
        if (changed) {
            wait_state_and_queues_unlock(queues, queue_count, timeout_flags);
            continue;
        }

        atomic_store_explicit(&waiter.state, WAITER_WAITING,
                              memory_order_relaxed);
        wait_start_diagnostic(&waiter);
        atomic_store_explicit(&thread->blocked_waiter, &waiter,
                              memory_order_release);
        if (!sched_publish_blocked(thread)) {
            atomic_store_explicit(&thread->blocked_waiter, 0,
                                  memory_order_release);
            wait_state_and_queues_unlock(queues, queue_count, timeout_flags);
            result = K_EBUSY;
            break;
        }
        const uint64_t kill_bit = 1ULL << (OS_SIGKILL - 1U);
        if ((flags & WAIT_FLAG_UNINTERRUPTIBLE) == 0U &&
            ((__atomic_load_n(&thread->signal_pending, __ATOMIC_ACQUIRE) &
             kill_bit) != 0U ||
            (__atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE) &
             THREAD_FLAG_TERMINATE_REQUEST) != 0U)) {
            /*
             * 进程 kill 或任务管理器终止线程可能与 RUNNING -> BLOCKED 转换竞争：
             * 终止方可能观察到 RUNNING，并将信号递送延迟到下一个系统调用边界。
             * 发布 BLOCKED 后，在等待锁仍将取消操作串行化时重新检查两个递送标记，
             * 避免信号滞留在尚未开始调度的等待者上。
             */
            waiter_detach_owner_locked(&waiter);
            (void)sched_publish_running(thread);
            wait_state_and_queues_unlock(queues, queue_count, timeout_flags);
            result = K_ECANCELED;
            break;
        }
        for (size_t index = 0U; index < queue_count; ++index) {
            wait_insert_tail(&queues[index]->waiters,
                             &subscriptions[index].queue_node);
        }
        if (deadline != UINT64_MAX) {
            wait_insert_tail(&g_timeout_waiters, &waiter.timeout_node);
        }

        /* 已在每个队列锁下验证采样到的序列。解锁谓词检查后到达的通知会强制重试；
         * 发布订阅后到达的通知则能看到该订阅，即使它在 schedule() 前已唤醒。
         * 不会在这些锁下执行外部谓词。 */
        if (wait_deadline_reached(deadline)) {
            waiter_detach_owner_locked(&waiter);
            (void)sched_publish_running(thread);
            wait_state_and_queues_unlock(queues, queue_count, timeout_flags);
            result = K_ETIMEDOUT;
            break;
        }
        wait_state_and_queues_unlock(queues, queue_count, timeout_flags);

        schedule();
        uint64_t resumed = wait_resume_timestamp(&waiter);
        /* 调用方已恢复运行，即使唤醒/schedule 竞争导致调度器状态过期也一样。
         * 释放栈等待者前先完成此处理。 */
        (void)sched_publish_running(thread);

        timeout_flags = wait_state_and_queues_lock(queues, queue_count);
        unsigned state = atomic_load_explicit(&waiter.state, memory_order_acquire);
        if (state == WAITER_WOKEN) wait_record_delay(&waiter, resumed);
        /* 每个恢复运行的等待者都会在判断唤醒原因或重试谓词前，释放订阅和栈关联。 */
        waiter_detach_owner_locked(&waiter);
        wait_state_and_queues_unlock(queues, queue_count, timeout_flags);

        /* 无关的调度器唤醒不会认领或取消此等待。 */
        if (state == WAITER_WAITING) continue;
        if (state == WAITER_TIMED_OUT) {
            result = K_ETIMEDOUT;
            break;
        }
        if (state == WAITER_CANCELLED) {
            result = K_ECANCELED;
            break;
        }
        if (state != WAITER_WOKEN) {
            result = K_EIO;
            break;
        }
        /* 唤醒仅是提示：释放所有订阅并重新检查谓词。 */
    }

    if (subscriptions != inline_subscriptions) kfree(subscriptions);
    return result;
}

kstatus_t wait_on_queues_in_place(wait_queue_t **queues, size_t count,
                         bool (*predicate)(void *), void *context,
                         uint64_t timeout_ns) {
    if (queues == 0 || count == 0U || predicate == 0) return K_EINVAL;
    if (count > SIZE_MAX / sizeof(wait_queue_t *)) return K_EOVERFLOW;
    if (count == 1U) return wait_on_queue(queues[0], predicate, context,
                                         timeout_ns);
    wait_global_init();
    for (size_t index = 0U; index < count; ++index) {
        if (queues[index] == 0) return K_EINVAL;
    }
    queue_pointer_sort(queues, count);
    size_t unique_count = 0U;
    for (size_t index = 0U; index < count; ++index) {
        if (unique_count == 0U || queues[unique_count - 1U] != queues[index]) {
            queues[unique_count++] = queues[index];
        }
    }

    uint64_t deadline = make_deadline(timeout_ns);
    return wait_on_sorted_queues(queues, unique_count, predicate, context, deadline, 0U);
}

kstatus_t wait_on_queues(wait_queue_t *const *input_queues, size_t count,
                         bool (*predicate)(void *), void *context,
                         uint64_t timeout_ns) {
    if (input_queues == 0 || count == 0U || predicate == 0) return K_EINVAL;
    if (count > SIZE_MAX / sizeof(wait_queue_t *)) return K_EOVERFLOW;
    if (count == 1U) return wait_on_queue(input_queues[0], predicate, context,
                                         timeout_ns);
    wait_queue_t *inline_queues[WAIT_INLINE_QUEUE_COUNT];
    wait_queue_t **queues = inline_queues;
    if (count > WAIT_INLINE_QUEUE_COUNT) {
        queues = (wait_queue_t **)kmalloc(count * sizeof(*queues), 0U);
        if (queues == 0) return K_ENOMEM;
    }
    for (size_t index = 0U; index < count; ++index) queues[index] = input_queues[index];
    kstatus_t result = wait_on_queues_in_place(queues, count, predicate, context, timeout_ns);
    if (queues != inline_queues) kfree(queues);
    return result;
}

kstatus_t wait_on_queue(wait_queue_t *queue, bool (*predicate)(void *),
                        void *context, uint64_t timeout_ns) {
    if (queue == 0 || predicate == 0) return K_EINVAL;
    /* 单队列已排序且唯一。复用相同的截止时间、订阅和取消逻辑，不执行多队列初始化。 */
    wait_global_init();
    return wait_on_sorted_queues(&queue, 1U, predicate, context,
                                 make_deadline(timeout_ns), 0U);
}

kstatus_t wait_on_queue_uninterruptible(wait_queue_t *queue,
                                       bool (*predicate)(void *), void *context,
                                       uint64_t timeout_ns) {
    if (queue == 0 || predicate == 0) return K_EINVAL;
    wait_global_init();
    return wait_on_sorted_queues(&queue, 1U, predicate, context,
                                 make_deadline(timeout_ns),
                                 WAIT_FLAG_UNINTERRUPTIBLE);
}

static thread_t *waiter_claim_locked(waiter_t *waiter, unsigned terminal_state) {
    unsigned expected = WAITER_WAITING;
    if (!atomic_compare_exchange_strong_explicit(&waiter->state, &expected,
                                                  terminal_state,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) return 0;
    waiter->diagnostic_claimed_tsc = waiter->diagnostic_started_tsc != 0U ?
                                      x86_read_tsc() : 0U;
    return waiter->thread;
}

static thread_t *waiter_claim_pinned_locked(waiter_t *waiter, unsigned state) {
    thread_t *thread = waiter_claim_locked(waiter, state);
    /* 所有者清理由超时锁串行化。在所有者恢复运行、退出并释放最后一个线程引用前，先在此固定线程。 */
    if (thread != 0) object_get(thread);
    return thread;
}

static void wait_wake_pinned(thread_t *thread) {
    if (thread == 0) return;
    sched_wake(thread);
    object_put(thread);
}

uint32_t wake_one(wait_queue_t *queue) {
    if (queue == 0) return 0U;
    wait_global_init();
    uint64_t timeout_flags = wait_lock(&g_timeout_lock);
    uint64_t queue_flags = wait_lock(&queue->lock);
    atomic_fetch_add_explicit(&queue->sequence, 1U, memory_order_release);
    thread_t *thread = 0;
    for (list_head_t *node = queue->waiters.next; node != &queue->waiters;) {
        wait_subscription_t *subscription = subscription_from_node(node);
        node = node->next;
        thread = waiter_claim_pinned_locked(subscription->waiter, WAITER_WOKEN);
        /* 成功表示认领此等待者；失败表示其已处于终态。持有当前队列锁时仅分离此队列的订阅，
         * 使 wake_all 不必反复扫描已认领的前缀。所有者仍负责清理其他队列/超时项及阻塞关联。 */
        wait_remove(&subscription->queue_node);
        if (thread != 0) break;
    }
    wait_unlock(&queue->lock, queue_flags);
    wait_unlock(&g_timeout_lock, timeout_flags);
    if (thread != 0) {
        wait_wake_pinned(thread);
        return 1U;
    }
    return 0U;
}

uint32_t wake_all(wait_queue_t *queue) {
    if (queue == 0) return 0U;
    wait_global_init();
    uint32_t count = 0U;
    for (;;) {
        thread_t *threads[WAIT_WAKE_BATCH_COUNT];
        size_t collected = 0U;
        uint64_t timeout_flags = wait_lock(&g_timeout_lock);
        uint64_t queue_flags = wait_lock(&queue->lock);
        for (list_head_t *node = queue->waiters.next;
             node != &queue->waiters && collected < WAIT_WAKE_BATCH_COUNT;) {
            wait_subscription_t *subscription = subscription_from_node(node);
            node = node->next;
            thread_t *thread = waiter_claim_pinned_locked(subscription->waiter, WAITER_WOKEN);
            wait_remove(&subscription->queue_node);
            if (thread != 0) {
                threads[collected++] = thread;
            }
        }
        /* 保留 wake_one 的序列记账，包括最后一次未成功探测，同时避免为每个线程获取锁。 */
        atomic_fetch_add_explicit(&queue->sequence,
                                  collected + (collected < WAIT_WAKE_BATCH_COUNT ? 1U : 0U),
                                  memory_order_release);
        wait_unlock(&queue->lock, queue_flags);
        wait_unlock(&g_timeout_lock, timeout_flags);
        for (size_t index = 0U; index < collected; ++index) {
            wait_wake_pinned(threads[index]);
        }
        count += (uint32_t)collected;
        if (collected < WAIT_WAKE_BATCH_COUNT) break;
    }
    return count;
}

bool wait_cancel_thread(thread_t *target) {
    if (target == 0) return false;
    wait_global_init();
    uint64_t timeout_flags = wait_lock(&g_timeout_lock);
    waiter_t *waiter = atomic_load_explicit(&target->blocked_waiter,
                                           memory_order_acquire);
    /* 所有终态认领都使用超时锁；因此在所有者替换关联前，已认领的等待者不会重新变为 WAITING。 */
    if (waiter == 0 ||
        (waiter->flags & WAIT_FLAG_UNINTERRUPTIBLE) != 0U ||
        atomic_load_explicit(&waiter->state, memory_order_acquire) != WAITER_WAITING ||
        waiter->subscriptions == 0 ||
        waiter->subscription_count == 0U) {
        wait_unlock(&g_timeout_lock, timeout_flags);
        return false;
    }
    for (size_t index = 0U; index < waiter->subscription_count; ++index) {
        (void)wait_lock(&waiter->subscriptions[index].queue->lock);
    }
    thread_t *thread = waiter_claim_pinned_locked(waiter, WAITER_CANCELLED);
    if (thread != 0) waiter_unlink_locked(waiter);
    for (size_t index = waiter->subscription_count; index != 0U; --index) {
        wait_unlock(&waiter->subscriptions[index - 1U].queue->lock, 0U);
    }
    wait_unlock(&g_timeout_lock, timeout_flags);
    wait_wake_pinned(thread);
    return thread != 0;
}

void wait_poll_timeouts(uint64_t now_tsc) {
    if (atomic_load_explicit(&g_wait_init_state, memory_order_acquire) != 2U) return;
    for (;;) {
        thread_t *threads[WAIT_WAKE_BATCH_COUNT];
        size_t collected = 0U;
        uint64_t timeout_flags = wait_lock(&g_timeout_lock);
        for (list_head_t *node = g_timeout_waiters.next;
             node != &g_timeout_waiters && collected < WAIT_WAKE_BATCH_COUNT;) {
            waiter_t *waiter = waiter_from_timeout_node(node);
            node = node->next;
            if (atomic_load_explicit(&waiter->state, memory_order_acquire) !=
                WAITER_WAITING) {
                wait_remove(&waiter->timeout_node);
                continue;
            }
            if (!deadline_reached(now_tsc, waiter->deadline_tsc)) continue;
            thread_t *thread = waiter_claim_pinned_locked(waiter, WAITER_TIMED_OUT);
            /* 超时链表仅由 g_timeout_lock 管理。保留队列订阅供所有者清理，
             * 但线程等待调度期间不要重复扫描已过期条目。 */
            wait_remove(&waiter->timeout_node);
            if (thread != 0) threads[collected++] = thread;
        }
        wait_unlock(&g_timeout_lock, timeout_flags);
        for (size_t index = 0U; index < collected; ++index) {
            wait_wake_pinned(threads[index]);
        }
        if (collected < WAIT_WAKE_BATCH_COUNT) return;
    }
}

static bool completion_try_consume(void *context) {
    completion_t *completion = (completion_t *)context;
    unsigned count = atomic_load_explicit(&completion->count, memory_order_acquire);
    while (count != 0U) {
        if (atomic_compare_exchange_weak_explicit(&completion->count, &count,
                                                  count - 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) return true;
    }
    return false;
}

void completion_init(completion_t *completion, uint32_t initial_count) {
    if (completion == 0) return;
    atomic_init(&completion->count, initial_count);
    wait_queue_init(&completion->waitq);
}

void complete(completion_t *completion) {
    if (completion == 0) return;
    atomic_fetch_add_explicit(&completion->count, 1U, memory_order_release);
    (void)wake_one(&completion->waitq);
}

kstatus_t completion_wait(completion_t *completion, uint64_t timeout_ns) {
    if (completion == 0) return K_EINVAL;
    /* 已发布 token 无需获取超时/队列锁。消费失败仅表示探测未命中：
     * 未加锁等待谓词前后的序列验证会关闭发布到休眠之间的竞争窗口。 */
    if (completion_try_consume(completion)) return K_OK;
    if (timeout_ns == 0U) return K_ETIMEDOUT;
    return wait_on_queue(&completion->waitq, completion_try_consume,
                         completion, timeout_ns);
}
