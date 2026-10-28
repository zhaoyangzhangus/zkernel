#include "internal.h"
#include <kernel/string.h>

static thread_t *rt_thread_from_node(list_head_t *node) {
    return (thread_t *)((uint8_t *)node -
                        __builtin_offsetof(thread_t, sched.rt_node));
}

static thread_t *fair_thread_from_node(rb_node_t *node) {
    return (thread_t *)((uint8_t *)node -
                        __builtin_offsetof(thread_t, sched.fair_node));
}

static bool fair_before(const thread_t *left, const thread_t *right) {
    if (left->sched.vruntime != right->sched.vruntime) {
        return left->sched.vruntime < right->sched.vruntime;
    }
    if (left->tid != right->tid) return left->tid < right->tid;
    return (uintptr_t)left < (uintptr_t)right;
}

bool scheduler_fair_current_wins(const run_queue_t *queue) {
    const thread_t *current = queue->current;
    /* 在记入当前线程执行尾段后调用。必须保留与 RB 树完全相同的比较规则，
     * 包括 TID/地址平局处理；RT 始终优先。已入队、阻塞或正在迁移的当前线程
     * 必须走完整路径。 */
    return current != 0 && current != queue->idle &&
           current->sched_class == SCHED_CLASS_FAIR &&
           !current->migration_pending &&
           (current->sched.flags & SCHED_ENTITY_ENQUEUED) == 0U &&
           atomic_load_explicit(&current->state, memory_order_relaxed) == THREAD_RUNNING &&
           queue->rt_bitmap == 0U && queue->fair_leftmost != 0 &&
           fair_before(current, queue->fair_leftmost);
}

void scheduler_update_min_vruntime(run_queue_t *queue) {
    bool found = queue->fair_leftmost != 0;
    uint64_t candidate = found ? queue->fair_leftmost->sched.vruntime : 0U;
    thread_t *current = queue->current;
    if (current != 0 && current != queue->idle &&
        current->sched_class == SCHED_CLASS_FAIR &&
        atomic_load_explicit(&current->state, memory_order_relaxed) == THREAD_RUNNING) {
        if (!found || current->sched.vruntime < candidate)
            candidate = current->sched.vruntime;
        found = true;
    }
    /* 空闲和出队操作不得重置公平时钟。否则每个新渲染线程都会从零开始运行，
     * 直到追上浏览器线程为止。 */
    if (found && candidate > queue->min_vruntime)
        queue->min_vruntime = candidate;
}

void scheduler_prepare_fair_migration(const run_queue_t *source, thread_t *thread) {
    if (thread->sched_class != SCHED_CLASS_FAIR ||
        (thread->sched.flags & SCHED_ENTITY_VRUNTIME_RELATIVE) != 0U) return;
    /* 每个 CPU 的公平时钟起点各不相同。只携带未偿还的服务负债，不传递旧 CPU 的
     * 绝对时钟值。休眠线程不会获得额外额度，与常规插入时的下限策略一致。 */
    uint64_t floor = source->min_vruntime;
    thread->sched.vruntime = thread->sched.vruntime > floor ?
                            thread->sched.vruntime - floor : 0U;
    thread->sched.flags |= SCHED_ENTITY_VRUNTIME_RELATIVE;
}

void scheduler_restore_fair_migration(run_queue_t *target, thread_t *thread) {
    if ((thread->sched.flags & SCHED_ENTITY_VRUNTIME_RELATIVE) == 0U) return;
    scheduler_update_min_vruntime(target);
    uint64_t floor = target->min_vruntime;
    uint64_t debt = thread->sched.vruntime;
    thread->sched.vruntime = debt > UINT64_MAX - floor ? UINT64_MAX : floor + debt;
    thread->sched.flags &= (uint16_t)~SCHED_ENTITY_VRUNTIME_RELATIVE;
}

static void fair_insert(run_queue_t *queue, thread_t *thread) {
    scheduler_update_min_vruntime(queue);
    /* 限制新线程和长期休眠线程的优势。不能免除已记入且领先于下限的可运行线程
     * CPU 时间。 */
    if (thread->sched.vruntime < queue->min_vruntime)
        thread->sched.vruntime = queue->min_vruntime;
    rb_node_t *parent = 0;
    rb_node_t **link = &queue->fair_root.root;
    while (*link != 0) {
        parent = *link;
        thread_t *candidate = fair_thread_from_node(parent);
        link = fair_before(thread, candidate) ? &parent->left : &parent->right;
    }
    rb_node_t *node = &thread->sched.fair_node;
    node->left = 0;
    node->right = 0;
    node->parent_color = (uintptr_t)parent | RB_TREE_RED;
    *link = node;
    rb_tree_insert_rebalance(&queue->fair_root, node);
    ++queue->fair_count;
    if (queue->fair_leftmost == 0 ||
        fair_before(thread, queue->fair_leftmost)) {
        queue->fair_leftmost = thread;
    }
    scheduler_update_min_vruntime(queue);
}

static void fair_remove(run_queue_t *queue, thread_t *thread) {
    rb_tree_erase(&queue->fair_root, &thread->sched.fair_node);
    if (queue->fair_count != 0) --queue->fair_count;
    if (thread == queue->fair_leftmost) {
        rb_node_t *first = rb_tree_first(&queue->fair_root);
        queue->fair_leftmost = first != 0 ? fair_thread_from_node(first) : 0;
    }
    scheduler_update_min_vruntime(queue);
}

bool validate_fair_node(rb_node_t *node, rb_node_t *parent,
                        thread_t **previous, uint32_t black_depth,
                        uint32_t *expected_black_depth, uint32_t *count) {
    if (node == 0) {
        if (*expected_black_depth == UINT32_MAX) *expected_black_depth = black_depth;
        return black_depth == *expected_black_depth;
    }
    if (rb_tree_parent(node) != parent) return false;
    if (rb_tree_color(node) == RB_TREE_BLACK) {
        ++black_depth;
    } else if (rb_tree_color(node->left) == RB_TREE_RED ||
               rb_tree_color(node->right) == RB_TREE_RED) {
        return false;
    }
    if (!validate_fair_node(node->left, node, previous, black_depth,
                            expected_black_depth, count)) return false;
    thread_t *thread = fair_thread_from_node(node);
    if (*previous != 0 && !fair_before(*previous, thread)) return false;
    *previous = thread;
    ++*count;
    return validate_fair_node(node->right, node, previous, black_depth,
                              expected_black_depth, count);
}

void initialize_thread(thread_t *thread, uint64_t tid, uint8_t class_id,
                       uint8_t priority) {
    memset(thread, 0, sizeof(*thread));
    refcount_init(&thread->object.refs, 1U);
    atomic_init(&thread->state, THREAD_READY);
    atomic_init(&thread->remote_wake_queued, false);
    atomic_init(&thread->priority_queued, false);
    thread->priority_next = 0;
    atomic_init(&thread->priority_request, 0U);
    atomic_init(&thread->remote_wake_lock, 0U);
    atomic_init(&thread->block_epoch, 0U);
    atomic_init(&thread->start_pending, false);
    atomic_init(&thread->exit_defer_count, 0U);
    atomic_init(&thread->blocked_waiter, 0);
    atomic_init(&thread->command_ack, 0U);
    thread->tid = tid;
    thread->sched_class = class_id;
    thread->rt_priority = priority;
    thread->base_sched_class = class_id;
    thread->base_rt_priority = priority;
    thread->sched.weight = 1024U;
    thread->sched.vruntime = 0;
    list_init(&thread->sched.rt_node);
    list_init(&thread->process_node);
    list_init(&thread->owned_mutexes);
    for (uint32_t word = 0; word < MAX_CPUS / 64U; ++word) {
        thread->affinity.bits[word] = UINT64_MAX;
    }
}

void sched_initialize_new_thread(thread_t *thread, uint32_t cpu_id,
                                 bool pin_to_cpu) {
    if (thread == 0) return;

    atomic_init(&thread->state, THREAD_NEW);
    atomic_init(&thread->remote_wake_queued, false);
    atomic_init(&thread->priority_queued, false);
    thread->priority_next = 0;
    atomic_init(&thread->priority_request, 0U);
    atomic_init(&thread->remote_wake_lock, 0U);
    atomic_init(&thread->block_epoch, 0U);
    atomic_init(&thread->start_pending, false);
    atomic_init(&thread->exit_defer_count, 0U);
    atomic_init(&thread->blocked_waiter, 0);
    atomic_init(&thread->command_ack, 0U);
    thread->sched_class = SCHED_CLASS_FAIR;
    thread->rt_priority = 0U;
    thread->base_sched_class = SCHED_CLASS_FAIR;
    thread->base_rt_priority = 0U;
    thread->sched.runtime_ns = 0U;
    thread->sched.vruntime = 0U;
    thread->sched.exec_start_ns = 0U;
    thread->sched.slice_runtime_ns = 0U;
    thread->sched.weight = 1024U;
    thread->sched.nice = 0;
    thread->sched.latency_hint = 0;
    thread->sched.flags = 0U;
    list_init(&thread->sched.rt_node);

    thread->current_cpu = (uint16_t)(cpu_id < MAX_CPUS ? cpu_id : 0U);
    thread->owner_cpu = thread->current_cpu;
    thread->migration_target_cpu = UINT16_MAX;
    thread->migration_pending = false;
    thread->command_release_next = 0;
    thread->command_release_count = 0U;
    bool pinned = pin_to_cpu && cpu_id < MAX_CPUS;
    for (uint32_t word = 0U; word < MAX_CPUS / 64U; ++word) {
        thread->affinity.bits[word] = pinned ? 0U : UINT64_MAX;
    }
    if (pinned) {
        thread->affinity.bits[cpu_id >> 6] = 1ULL << (cpu_id & 63U);
    }
    sched_mark_initial_placement(thread);
}

void sched_initialize_test_thread(thread_t *thread, uint64_t tid,
                                  uint8_t class_id, uint8_t priority) {
    initialize_thread(thread, tid, class_id, priority);
}

void enqueue_local_raw(scheduler_cpu_t *cpu, thread_t *thread) {
    if ((thread->sched.flags & SCHED_ENTITY_ENQUEUED) != 0 ||
        atomic_load_explicit(&thread->state, memory_order_relaxed) != THREAD_READY) return;
    scheduler_restore_fair_migration(&cpu->queue, thread);
    if (thread->sched_class == SCHED_CLASS_RT) {
        list_add_tail(&cpu->queue.rt_queues[thread->rt_priority],
                      &thread->sched.rt_node);
        cpu->queue.rt_bitmap |= 1U << thread->rt_priority;
    } else if (thread->sched_class == SCHED_CLASS_FAIR) {
        fair_insert(&cpu->queue, thread);
    } else {
        return;
    }
    thread->sched.flags |= SCHED_ENTITY_ENQUEUED;
    ++cpu->queue.nr_running;
    cpu->queue_snapshot_dirty = true;
}

void enqueue_local(scheduler_cpu_t *cpu, thread_t *thread) {
    enqueue_local_raw(cpu, thread);
    scheduler_publish_queue_snapshot(cpu);
}

void dequeue_local_raw(scheduler_cpu_t *cpu, thread_t *thread) {
    if ((thread->sched.flags & SCHED_ENTITY_ENQUEUED) == 0) return;
    if (thread->sched_class == SCHED_CLASS_RT) {
        list_del(&thread->sched.rt_node);
        if (list_empty(&cpu->queue.rt_queues[thread->rt_priority])) {
            cpu->queue.rt_bitmap &= ~(1U << thread->rt_priority);
        }
    } else if (thread->sched_class == SCHED_CLASS_FAIR) {
        fair_remove(&cpu->queue, thread);
    }
    thread->sched.flags &= (uint16_t)~SCHED_ENTITY_ENQUEUED;
    if (cpu->queue.nr_running != 0) --cpu->queue.nr_running;
    cpu->queue_snapshot_dirty = true;
}

void dequeue_local(scheduler_cpu_t *cpu, thread_t *thread) {
    dequeue_local_raw(cpu, thread);
    scheduler_publish_queue_snapshot(cpu);
}

thread_t *pick_next_local(scheduler_cpu_t *cpu) {
    if (cpu->queue.rt_bitmap != 0) {
        uint32_t priority = (uint32_t)__builtin_ctz(cpu->queue.rt_bitmap);
        return rt_thread_from_node(cpu->queue.rt_queues[priority].next);
    }
    return cpu->queue.fair_leftmost != 0 ?
           cpu->queue.fair_leftmost : cpu->queue.idle;
}

bool sched_rt_policy_self_test(void) {
    scheduler_cpu_t cpu = {0};
    thread_t first = {0};
    thread_t second = {0};
    thread_t higher = {0};
    for (uint32_t priority = 0U; priority < RT_PRIORITY_LEVELS; ++priority) {
        list_init(&cpu.queue.rt_queues[priority]);
    }
    atomic_init(&cpu.queue_snapshot, 0U);
    initialize_thread(&first, 1U, SCHED_CLASS_RT, 7U);
    initialize_thread(&second, 2U, SCHED_CLASS_RT, 7U);
    initialize_thread(&higher, 3U, SCHED_CLASS_RT, 3U);

    enqueue_local(&cpu, &first);
    enqueue_local(&cpu, &second);
    if (pick_next_local(&cpu) != &first) return false;
    dequeue_local(&cpu, &first);
    if (pick_next_local(&cpu) != &second) return false;
    enqueue_local(&cpu, &higher);
    if (pick_next_local(&cpu) != &higher) return false;
    dequeue_local(&cpu, &second);
    dequeue_local(&cpu, &higher);
    return cpu.queue.nr_running == 0U && cpu.queue.rt_bitmap == 0U;
}
