#include <arch/x86_64/cpu.h>
#include <kernel/gpu.h>
#include <kernel/deferred.h>
#include <kernel/kmem.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/telemetry.h>

static atomic_uint_fast64_t g_next_gpu_va;
static volatile uint32_t g_gpu_context_progress[MAX_CPUS];
static volatile uint64_t g_gpu_context_address[MAX_CPUS];

static void gpu_context_debug_set(uint32_t value, gpu_context_t *context) {
    uint32_t cpu_index = x86_current_cpu_index();
    if (cpu_index >= MAX_CPUS) return;
    __atomic_store_n(&g_gpu_context_address[cpu_index],
                     (uint64_t)(uintptr_t)context, __ATOMIC_RELEASE);
    __atomic_store_n(&g_gpu_context_progress[cpu_index], value,
                     __ATOMIC_RELEASE);
}

uint32_t gpu_context_debug_progress(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_gpu_context_progress[cpu_index],
                           __ATOMIC_ACQUIRE);
}

uint64_t gpu_context_debug_address(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_gpu_context_address[cpu_index],
                           __ATOMIC_ACQUIRE);
}

/*
 * GPU 后端完成前的提交记录。
 *
 * 记录本身持有 context、allocation 和 fence 的对象引用。这样即使用户
 * 先关闭句柄，异步完成回调仍然可以安全地访问这些对象；allocation 的
 * pin_count 则覆盖“提交成功到 fence 发出”的整个生命周期。
 */
typedef struct gpu_pending_submission {
    list_head_t node;
    gpu_context_t *context;
    gpu_allocation_t *allocation;
    gpu_fence_t *fence;
    uint64_t signal_value;
    uint64_t start_tsc;
    atomic_bool canceled;
} gpu_pending_submission_t;

static void gpu_submission_link(gpu_context_t *context,
                                 gpu_pending_submission_t *submission) {
    submission->node.next = &context->submissions;
    submission->node.prev = context->submissions.prev;
    context->submissions.prev->next = &submission->node;
    context->submissions.prev = &submission->node;
}

static void gpu_submission_unlink(gpu_pending_submission_t *submission) {
    submission->node.prev->next = submission->node.next;
    submission->node.next->prev = submission->node.prev;
    submission->node.next = &submission->node;
    submission->node.prev = &submission->node;
}

/* 在此处内联加锁边界，让调用方访问直接体现 acquire/release 语义，
 * 而不是依赖调试钩子的过程间读写摘要。 */
static inline __attribute__((always_inline)) void gpu_context_lock(gpu_context_t *context) {
    gpu_context_debug_set(1U, context);
    sched_preempt_disable();
    spinlock_lock(&context->lock);
    gpu_context_debug_set(2U, context);
}

static inline __attribute__((always_inline)) void gpu_context_unlock(gpu_context_t *context) {
    spinlock_unlock(&context->lock);
    gpu_context_debug_set(0U, 0);
    sched_preempt_enable();
}

static inline __attribute__((always_inline)) void gpu_fence_lock(gpu_fence_t *fence) {
    sched_preempt_disable();
    spinlock_lock(&fence->lock);
}

static inline __attribute__((always_inline)) void gpu_fence_unlock(gpu_fence_t *fence) {
    spinlock_unlock(&fence->lock);
    sched_preempt_enable();
}

static bool gpu_fence_is_signaled(const void *object) {
    const gpu_fence_t *fence = (const gpu_fence_t *)object;
    return atomic_load_explicit(&fence->completed_value, memory_order_acquire) != 0U ||
           atomic_load_explicit(&fence->status, memory_order_acquire) != K_OK;
}

static int64_t gpu_fence_wait_value(const void *object) {
    const gpu_fence_t *fence = (const gpu_fence_t *)object;
    return (int64_t)atomic_load_explicit(&fence->completed_value,
                                         memory_order_acquire);
}

static void gpu_fence_destroy(void *object) {
    gpu_fence_t *fence = (gpu_fence_t *)object;
    (void)wake_all(&fence->waitq);
    kfree(fence);
}

static void gpu_allocation_destroy(void *object) {
    gpu_allocation_t *allocation = (gpu_allocation_t *)object;
    if (allocation->backend_ops != 0 && allocation->backend_ops->release != 0) {
        allocation->backend_ops->release(allocation,
                                         allocation->backend_private);
        allocation->backend_private = 0;
    } else if (allocation->backing_page != 0) {
        page_free((page_t *)allocation->backing_page);
        allocation->backing_page = 0;
    }
    kfree(allocation);
}

static void gpu_context_destroy(void *object) {
    gpu_context_t *context = (gpu_context_t *)object;
    /* 异步提交持有额外引用，因此销毁时队列必须已经为空。 */
    kfree(context);
}

static const object_ops_t g_gpu_fence_ops = {
    .destroy = gpu_fence_destroy,
    .type_name = "GpuFence",
    .is_signaled = gpu_fence_is_signaled,
    .wait_value = gpu_fence_wait_value,
};

static const object_ops_t g_gpu_allocation_ops = {
    .destroy = gpu_allocation_destroy,
    .type_name = "GpuAllocation",
    .is_signaled = 0,
    .wait_value = 0,
};

static const object_ops_t g_gpu_context_ops = {
    .destroy = gpu_context_destroy,
    .type_name = "GpuContext",
    .is_signaled = 0,
    .wait_value = 0,
};

static void gpu_object_init(object_header_t *object, object_type_id_t type,
                            const object_ops_t *ops) {
    object_header_init(object, type, ops);
}

kstatus_t gpu_allocation_create(uint64_t size, gpu_allocation_t **out) {
    gpu_allocation_t *allocation;
    page_t *backing_page;
    uint64_t page_count;
    uint64_t capacity_pages = 1U;
    uint8_t backing_order = 0U;
    uint64_t va;
    uint64_t expected = 0U;
    if (out == 0 || size == 0U || size > UINT64_MAX - (PAGE_SIZE - 1U)) {
        return K_EINVAL;
    }
    allocation = (gpu_allocation_t *)kzalloc(sizeof(*allocation), 0);
    if (allocation == 0) return K_ENOMEM;
    (void)atomic_compare_exchange_strong_explicit(&g_next_gpu_va, &expected,
                                                  0x100000000ULL,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire);
    gpu_object_init(&allocation->object, KOBJECT_TYPE_GPU_ALLOCATION,
                    &g_gpu_allocation_ops);
    allocation->size = (size + PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U);
    allocation->domain = 0U;
    allocation->flags = 0U;
    va = atomic_fetch_add_explicit(&g_next_gpu_va, allocation->size,
                                   memory_order_relaxed);
    allocation->gpu_va = gpu_va_make(va);
    allocation->backend_kind = GPU_ALLOCATION_BACKEND_CPU_WB;
    allocation->backend_ops = 0;
    allocation->backend_private = 0;
    page_count = allocation->size >> PAGE_SHIFT;
    while (capacity_pages < page_count) {
        if (backing_order == BUDDY_MAX_ORDER) {
            kfree(allocation);
            return K_ENOMEM;
        }
        ++backing_order;
        capacity_pages <<= 1U;
    }
    backing_page = page_alloc(backing_order, PAGE_ALLOC_ZERO);
    if (backing_page == 0) {
        kfree(allocation);
        return K_ENOMEM;
    }
    backing_page->owner = PAGE_OWNER_DEVICE;
    allocation->backing_page = backing_page;
    allocation->backing_phys = page_to_phys(backing_page);
    allocation->backing_size = capacity_pages * PAGE_SIZE;
    allocation->backing = phys_to_direct(allocation->backing_phys);
    allocation->backing_order = backing_order;
    atomic_init(&allocation->pin_count, 0U);
    atomic_init(&allocation->residency_state, 1U);
    *out = allocation;
    return K_OK;
}

kstatus_t gpu_allocation_create_with_backend(
    uint64_t size, uint32_t domain, uint32_t flags,
    gpu_allocation_backend_t backend_kind, gpu_va_t gpu_va,
    const gpu_allocation_backend_ops_t *backend_ops,
    void *backend_private, gpu_allocation_t **out) {
    gpu_allocation_t *allocation;

    if (out == 0 || size == 0U || size > UINT64_MAX - (PAGE_SIZE - 1U) ||
        backend_kind == GPU_ALLOCATION_BACKEND_CPU_WB ||
        backend_kind > GPU_ALLOCATION_BACKEND_NVIDIA_GPU ||
        backend_ops == 0 || backend_ops->release == 0) {
        return K_EINVAL;
    }

    allocation = (gpu_allocation_t *)kzalloc(sizeof(*allocation), 0);
    if (allocation == 0) return K_ENOMEM;
    gpu_object_init(&allocation->object, KOBJECT_TYPE_GPU_ALLOCATION,
                    &g_gpu_allocation_ops);
    allocation->size = (size + PAGE_SIZE - 1U) &
                       ~(uint64_t)(PAGE_SIZE - 1U);
    allocation->domain = domain;
    allocation->flags = flags;
    allocation->gpu_va = gpu_va;
    allocation->backend_kind = backend_kind;
    allocation->backend_ops = backend_ops;
    allocation->backend_private = backend_private;
    allocation->backing = 0;
    allocation->backing_phys = paddr_make(0U);
    allocation->backing_size = 0U;
    allocation->backing_page = 0;
    allocation->backing_order = 0U;
    atomic_init(&allocation->pin_count, 0U);
    atomic_init(&allocation->residency_state, 1U);
    *out = allocation;
    return K_OK;
}

gpu_allocation_backend_t gpu_allocation_backend(
    const gpu_allocation_t *allocation) {
    if (allocation == 0 ||
        allocation->object.type != KOBJECT_TYPE_GPU_ALLOCATION) {
        return GPU_ALLOCATION_BACKEND_CPU_WB;
    }
    return allocation->backend_kind;
}

kstatus_t gpu_allocation_pin(gpu_allocation_t *allocation) {
    kstatus_t status;
    unsigned previous;

    if (allocation == 0 ||
        allocation->object.type != KOBJECT_TYPE_GPU_ALLOCATION) {
        return K_EINVAL;
    }
    if (allocation->backend_ops != 0 && allocation->backend_ops->pin != 0) {
        status = allocation->backend_ops->pin(
            allocation, allocation->backend_private);
        if (status != K_OK) return status;
    }
    previous = atomic_fetch_add_explicit(&allocation->pin_count, 1U,
                                         memory_order_acq_rel);
    if (previous == UINT32_MAX) {
        atomic_fetch_sub_explicit(&allocation->pin_count, 1U,
                                  memory_order_acq_rel);
        if (allocation->backend_ops != 0 &&
            allocation->backend_ops->unpin != 0) {
            allocation->backend_ops->unpin(
                allocation, allocation->backend_private);
        }
        return K_EOVERFLOW;
    }
    return K_OK;
}

kstatus_t gpu_allocation_unpin(gpu_allocation_t *allocation) {
    unsigned current;

    if (allocation == 0 ||
        allocation->object.type != KOBJECT_TYPE_GPU_ALLOCATION) {
        return K_EINVAL;
    }
    current = atomic_load_explicit(&allocation->pin_count,
                                   memory_order_acquire);
    for (;;) {
        if (current == 0U) return K_EINVAL;
        if (atomic_compare_exchange_weak_explicit(
                &allocation->pin_count, &current, current - 1U,
                memory_order_acq_rel, memory_order_acquire)) {
            break;
        }
    }
    if (allocation->backend_ops != 0 && allocation->backend_ops->unpin != 0) {
        allocation->backend_ops->unpin(allocation,
                                       allocation->backend_private);
    }
    return K_OK;
}

static bool gpu_allocation_range_valid(const gpu_allocation_t *allocation,
                                       uint64_t offset, uint64_t length) {
    return allocation != 0 &&
           allocation->object.type == KOBJECT_TYPE_GPU_ALLOCATION &&
           offset <= allocation->size && length <= allocation->size - offset;
}

kstatus_t gpu_allocation_cpu_map(gpu_allocation_t *allocation,
                                 uint64_t offset, uint64_t length,
                                 void **address) {
    if (address == 0 || length == 0U ||
        !gpu_allocation_range_valid(allocation, offset, length)) {
        return K_EINVAL;
    }
    *address = 0;
    if (allocation->backend_ops != 0 &&
        allocation->backend_ops->map_cpu != 0) {
        return allocation->backend_ops->map_cpu(
            allocation, allocation->backend_private, offset, length, address);
    }
    if ((allocation->backend_kind != GPU_ALLOCATION_BACKEND_CPU_WB &&
         allocation->backend_kind != GPU_ALLOCATION_BACKEND_CPU_WC) ||
        allocation->backing == 0) {
        return K_ENOSYS;
    }
    *address = (void *)((uintptr_t)allocation->backing + (uintptr_t)offset);
    return K_OK;
}

void gpu_allocation_cpu_unmap(gpu_allocation_t *allocation, void *address,
                              uint64_t length) {
    if (allocation == 0 || allocation->object.type != KOBJECT_TYPE_GPU_ALLOCATION ||
        allocation->backend_ops == 0 ||
        allocation->backend_ops->unmap_cpu == 0) {
        return;
    }
    allocation->backend_ops->unmap_cpu(allocation,
                                       allocation->backend_private,
                                       address, length);
}

kstatus_t gpu_allocation_get_physical(gpu_allocation_t *allocation,
                                      uint64_t offset, uint64_t length,
                                      paddr_t *physical) {
    if (physical == 0 || length == 0U ||
        !gpu_allocation_range_valid(allocation, offset, length)) {
        return K_EINVAL;
    }
    if (allocation->backend_ops != 0 &&
        allocation->backend_ops->get_physical != 0) {
        return allocation->backend_ops->get_physical(
            allocation, allocation->backend_private, offset, length, physical);
    }
    if ((allocation->backend_kind != GPU_ALLOCATION_BACKEND_CPU_WB &&
         allocation->backend_kind != GPU_ALLOCATION_BACKEND_CPU_WC) ||
        allocation->backing_page == 0 || offset > allocation->backing_size ||
        length > allocation->backing_size - offset ||
        allocation->backing_phys.value > UINT64_MAX - offset) {
        return K_ENOSYS;
    }
    *physical = paddr_make(allocation->backing_phys.value + offset);
    return K_OK;
}

kstatus_t gpu_context_create(struct device *device, struct process *process,
                             gpu_context_t **out) {
    gpu_context_t *context;
    if (out == 0) return K_EINVAL;
    context = (gpu_context_t *)kzalloc(sizeof(*context), 0);
    if (context == 0) return K_ENOMEM;
    gpu_object_init(&context->object, KOBJECT_TYPE_GPU_CONTEXT,
                    &g_gpu_context_ops);
    context->device = device;
    context->process = process;
    context->gpu_vm = 0;
    context->backend = 0;
    spinlock_init(&context->lock);
    list_init(&context->submissions);
    atomic_init(&context->state, GPU_CONTEXT_RUNNING);
    *out = context;
    return K_OK;
}

kstatus_t gpu_fence_create(gpu_fence_t **out) {
    gpu_fence_t *fence;
    if (out == 0) return K_EINVAL;
    fence = (gpu_fence_t *)kzalloc(sizeof(*fence), 0);
    if (fence == 0) return K_ENOMEM;
    gpu_object_init(&fence->object, KOBJECT_TYPE_GPU_FENCE, &g_gpu_fence_ops);
    atomic_init(&fence->completed_value, 0U);
    atomic_init(&fence->status, K_OK);
    spinlock_init(&fence->lock);
    wait_queue_init(&fence->waitq);
    fence->backend = 0;
    *out = fence;
    return K_OK;
}

kstatus_t gpu_fence_signal(gpu_fence_t *fence, uint64_t value) {
    uint64_t current;
    bool notify = false;
    if (fence == 0 || fence->object.type != KOBJECT_TYPE_GPU_FENCE) return K_EINVAL;
    gpu_fence_lock(fence);
    kstatus_t failure = atomic_load_explicit(&fence->status, memory_order_relaxed);
    if (failure != K_OK) {
        gpu_fence_unlock(fence);
        return failure;
    }
    current = atomic_load_explicit(&fence->completed_value, memory_order_acquire);
    if (value > current) {
        atomic_store_explicit(&fence->completed_value, value, memory_order_release);
        notify = true;
    }
    gpu_fence_unlock(fence);
    if (notify) {
        (void)wake_all(&fence->waitq);
        object_notify_signaled(fence);
    }
    return K_OK;
}

kstatus_t gpu_fence_fail(gpu_fence_t *fence, kstatus_t status) {
    bool notify = false;
    kstatus_t result;
    if (fence == 0 || fence->object.type != KOBJECT_TYPE_GPU_FENCE ||
        status == K_OK) return K_EINVAL;
    gpu_fence_lock(fence);
    result = (kstatus_t)atomic_load_explicit(&fence->status, memory_order_relaxed);
    if (result == K_OK) {
        atomic_store_explicit(&fence->status, (int)status, memory_order_release);
        result = status;
        notify = true;
    }
    gpu_fence_unlock(fence);
    if (notify) {
        (void)wake_all(&fence->waitq);
        object_notify_signaled(fence);
    }
    return result;
}

typedef struct gpu_fence_wait_context {
    gpu_fence_t *fence;
    uint64_t value;
} gpu_fence_wait_context_t;

static bool gpu_fence_ready(void *context) {
    gpu_fence_wait_context_t *wait = (gpu_fence_wait_context_t *)context;
    return atomic_load_explicit(&wait->fence->completed_value,
                                memory_order_acquire) >= wait->value ||
           atomic_load_explicit(&wait->fence->status, memory_order_acquire) != K_OK;
}

static void gpu_complete_submission(void *argument) {
    gpu_pending_submission_t *submission =
        (gpu_pending_submission_t *)argument;
    gpu_context_t *context;
    gpu_allocation_t *allocation;
    gpu_fence_t *fence;
    uint64_t signal_value;
    uint64_t start_tsc;

    if (submission == 0) return;
    context = submission->context;
    allocation = submission->allocation;
    fence = submission->fence;
    signal_value = submission->signal_value;
    start_tsc = submission->start_tsc;

    /* 先从 context 的 pending 队列摘除，再释放对象引用。 */
    gpu_context_lock(context);
    gpu_submission_unlink(submission);
    if (!atomic_load_explicit(&submission->canceled, memory_order_acquire)) {
        (void)gpu_fence_signal(fence, signal_value);
    }
    (void)gpu_allocation_unpin(allocation);
    gpu_context_unlock(context);

    (void)telemetry_record_latency(TELEMETRY_CATEGORY_GPU_SUBMIT,
                                   allocation->gpu_va.value, start_tsc);
    object_put(fence);
    object_put(allocation);
    object_put(context);
    kfree(submission);
}

kstatus_t gpu_fence_wait(gpu_fence_t *fence, uint64_t value,
                         uint64_t timeout_ns) {
    gpu_fence_wait_context_t context;
    uint64_t completed_value;
    if (fence == 0 || fence->object.type != KOBJECT_TYPE_GPU_FENCE || value == 0U) {
        return K_EINVAL;
    }
    context.fence = fence;
    context.value = value;
    completed_value = atomic_load_explicit(&fence->completed_value,
                                           memory_order_acquire);
    if (completed_value >= value) return K_OK;
    if (atomic_load_explicit(&fence->status, memory_order_acquire) != K_OK) {
        return (kstatus_t)atomic_load_explicit(&fence->status,
                                               memory_order_acquire);
    }
    kstatus_t wait_status = wait_on_queue(&fence->waitq, gpu_fence_ready,
                                          &context, timeout_ns);
    if (wait_status != K_OK) return wait_status;
    /* 设备丢失可能与完成通知并发；目标值已完成时，历史等待仍成功。 */
    completed_value = atomic_load_explicit(&fence->completed_value,
                                           memory_order_acquire);
    if (completed_value >= value) return K_OK;
    return (kstatus_t)atomic_load_explicit(&fence->status, memory_order_acquire);
}

static kstatus_t gpu_submit_locked(gpu_context_t *context,
                                   const gpu_submission_t *submission) {
    gpu_allocation_t *allocation = submission != 0 ? submission->allocation : 0;
    uint64_t command_offset = submission != 0 ? submission->command_offset : 0U;
    uint64_t command_length = submission != 0 ? submission->command_length : 0U;
    gpu_fence_t *fence = submission != 0 ? submission->fence : 0;
    uint64_t signal_value = submission != 0 ? submission->signal_value : 0U;
    unsigned state;
    uint64_t start_tsc;
    gpu_pending_submission_t *pending;
    kstatus_t pin_status;
    if (context == 0 || allocation == 0 || fence == 0 || command_length == 0U ||
        signal_value == 0U || context->object.type != KOBJECT_TYPE_GPU_CONTEXT ||
        allocation->object.type != KOBJECT_TYPE_GPU_ALLOCATION ||
        fence->object.type != KOBJECT_TYPE_GPU_FENCE || command_offset > allocation->size ||
        command_length > allocation->size - command_offset) return K_EINVAL;
    state = atomic_load_explicit(&context->state, memory_order_acquire);
    if (state == GPU_CONTEXT_LOST) return K_EDEVLOST;
    if (state != GPU_CONTEXT_RUNNING) return K_EBUSY;
    gpu_context_debug_set(4U, context);
    start_tsc = telemetry_timestamp();
    pending = (gpu_pending_submission_t *)kzalloc(sizeof(*pending), 0);
    if (pending == 0) return K_ENOMEM;
    gpu_context_debug_set(5U, context);
    list_init(&pending->node);
    pending->context = context;
    pending->allocation = allocation;
    pending->fence = fence;
    pending->signal_value = signal_value;
    pending->start_tsc = start_tsc;
    atomic_init(&pending->canceled, false);

    /* 提交建立异步完成所需的三条对象引用。 */
    object_get(context);
    object_get(allocation);
    object_get(fence);
    pin_status = gpu_allocation_pin(allocation);
    if (pin_status != K_OK) {
        object_put(fence);
        object_put(allocation);
        object_put(context);
        kfree(pending);
        return pin_status;
    }
    gpu_submission_link(context, pending);
    gpu_context_debug_set(6U, context);
    gpu_context_debug_set(7U, context);
    if (!deferred_schedule(gpu_complete_submission, pending)) {
        gpu_submission_unlink(pending);
        (void)gpu_allocation_unpin(allocation);
        object_put(fence);
        object_put(allocation);
        object_put(context);
        kfree(pending);
        return K_EBUSY;
    }
    gpu_context_debug_set(8U, context);
    return K_OK;
}

kstatus_t gpu_submit(gpu_context_t *context, gpu_allocation_t *allocation,
                     uint64_t command_offset, uint64_t command_length,
                     gpu_fence_t *fence, uint64_t signal_value) {
    gpu_submission_t submission = {
        allocation, command_offset, command_length, fence, signal_value
    };
    kstatus_t status;
    if (context == 0) return K_EINVAL;
    gpu_context_lock(context);
    gpu_context_debug_set(3U, context);
    status = gpu_submit_locked(context, &submission);
    gpu_context_debug_set(9U, context);
    gpu_context_unlock(context);
    return status;
}

kstatus_t gpu_submit_batch(gpu_context_t *context, const gpu_submission_t *submissions,
                           uint32_t count, uint32_t *submitted) {
    uint32_t completed = 0U;
    kstatus_t status = K_OK;
    uint64_t start_tsc = 0U;
    if (submitted == 0 || submissions == 0 || count == 0U ||
        count > GPU_SUBMIT_BATCH_MAX || context == 0) return K_EINVAL;
    *submitted = 0U;
    gpu_context_lock(context);
    /* 先完整校验，避免参数错误导致批处理只提交一半。 */
    for (uint32_t i = 0; i < count; ++i) {
        const gpu_submission_t *item = &submissions[i];
        if (item->allocation == 0 || item->fence == 0 || item->command_length == 0U ||
            item->signal_value == 0U ||
            context->object.type != KOBJECT_TYPE_GPU_CONTEXT ||
            item->allocation->object.type != KOBJECT_TYPE_GPU_ALLOCATION ||
            item->fence->object.type != KOBJECT_TYPE_GPU_FENCE ||
            item->command_offset > item->allocation->size ||
            item->command_length > item->allocation->size - item->command_offset) {
            status = K_EINVAL;
            goto done;
        }
    }
    if (atomic_load_explicit(&context->state, memory_order_acquire) == GPU_CONTEXT_LOST) {
        status = K_EDEVLOST;
        goto done;
    }
    if (atomic_load_explicit(&context->state, memory_order_acquire) != GPU_CONTEXT_RUNNING) {
        status = K_EBUSY;
        goto done;
    }
    start_tsc = telemetry_timestamp();
    for (; completed < count; ++completed) {
        status = gpu_submit_locked(context, &submissions[completed]);
        if (status != K_OK) break;
    }
    (void)telemetry_record(TELEMETRY_CATEGORY_GPU_BATCH,
                           context->object.type, count, completed);
    (void)telemetry_record_latency(TELEMETRY_CATEGORY_GPU_BATCH,
                                   context->object.type, start_tsc);
done:
    *submitted = completed;
    gpu_context_unlock(context);
    return status;
}

kstatus_t gpu_context_mark_lost(gpu_context_t *context) {
    if (context == 0 || context->object.type != KOBJECT_TYPE_GPU_CONTEXT) return K_EINVAL;
    gpu_context_lock(context);
    atomic_store_explicit(&context->state, GPU_CONTEXT_LOST, memory_order_release);
    /* 已经提交到硬件的工作无法再保证结果，统一以设备丢失结束。 */
    for (list_head_t *cursor = context->submissions.next;
         cursor != &context->submissions; cursor = cursor->next) {
        gpu_pending_submission_t *submission =
            (gpu_pending_submission_t *)cursor;
        atomic_store_explicit(&submission->canceled, true, memory_order_release);
        (void)gpu_fence_fail(submission->fence, K_EDEVLOST);
    }
    gpu_context_unlock(context);
    return K_OK;
}

kstatus_t gpu_context_reset(gpu_context_t *context) {
    if (context == 0 || context->object.type != KOBJECT_TYPE_GPU_CONTEXT) return K_EINVAL;
    gpu_context_lock(context);
    atomic_store_explicit(&context->state, GPU_CONTEXT_RESETTING, memory_order_release);
    atomic_store_explicit(&context->state, GPU_CONTEXT_RUNNING, memory_order_release);
    gpu_context_unlock(context);
    return K_OK;
}

bool gpu_core_self_test(void) {
    gpu_allocation_t *allocation = 0;
    gpu_context_t *context = 0;
    gpu_fence_t *fence = 0;
    gpu_fence_t *lost_fence = 0;
    gpu_fence_t *batch_fence = 0;
    gpu_fence_t *timeline_fence = 0;
    gpu_context_t *orphan_context = 0;
    gpu_allocation_t *orphan_allocation = 0;
    gpu_fence_t *orphan_fence = 0;
    gpu_submission_t batch[2];
    uint32_t submitted = 0U;
    bool success = gpu_allocation_create(4097U, &allocation) == K_OK &&
                   gpu_context_create(0, 0, &context) == K_OK &&
                   gpu_fence_create(&fence) == K_OK && allocation != 0 &&
                   context != 0 && fence != 0 && allocation->size == 8192U &&
                   gpu_submit(context, allocation, 0U, 64U, fence, 1U) == K_OK &&
                   atomic_load_explicit(&allocation->pin_count,
                                        memory_order_acquire) == 1U &&
                   gpu_fence_wait(fence, 1U, 0U) == K_ETIMEDOUT &&
                   deferred_run(64U) >= 1U &&
                   gpu_fence_wait(fence, 1U, 0U) == K_OK &&
                   gpu_fence_create(&lost_fence) == K_OK &&
                   lost_fence != 0 &&
                   gpu_submit(context, allocation, 0U, 64U, lost_fence, 1U) == K_OK &&
                   atomic_load_explicit(&allocation->pin_count,
                                        memory_order_acquire) == 1U &&
                   gpu_context_mark_lost(context) == K_OK &&
                   gpu_fence_wait(lost_fence, 1U, 0U) == K_EDEVLOST &&
                   deferred_run(64U) >= 1U &&
                   atomic_load_explicit(&allocation->pin_count,
                                        memory_order_acquire) == 0U &&
                   gpu_submit(context, allocation, 0U, 64U, fence, 2U) == K_EDEVLOST &&
                   gpu_context_reset(context) == K_OK &&
                   gpu_fence_create(&batch_fence) == K_OK && batch_fence != 0;
    if (success) {
        batch[0] = (gpu_submission_t){allocation, 0U, 64U, fence, 2U};
        batch[1] = (gpu_submission_t){allocation, 64U, 64U, batch_fence, 1U};
        success = gpu_submit_batch(context, batch, 2U, &submitted) == K_OK &&
                  submitted == 2U &&
                  atomic_load_explicit(&allocation->pin_count,
                                       memory_order_acquire) == 2U &&
                  gpu_fence_wait(fence, 2U, 0U) == K_ETIMEDOUT &&
                  gpu_fence_wait(batch_fence, 1U, 0U) == K_ETIMEDOUT &&
                  deferred_run(64U) >= 2U &&
                  gpu_fence_wait(fence, 2U, 0U) == K_OK &&
                   gpu_fence_wait(batch_fence, 1U, 0U) == K_OK &&
                   atomic_load_explicit(&allocation->pin_count, memory_order_acquire) == 0U;
    }
    if (success) {
        /* 已完成的 timeline 值不能被后续设备错误改写为失败。 */
        success = gpu_fence_create(&timeline_fence) == K_OK &&
                  timeline_fence != 0 &&
                  gpu_fence_signal(timeline_fence, 1U) == K_OK &&
                  gpu_fence_fail(timeline_fence, K_EDEVLOST) == K_EDEVLOST &&
                  gpu_fence_wait(timeline_fence, 1U, 0U) == K_OK &&
                  gpu_fence_wait(timeline_fence, 2U, 0U) == K_EDEVLOST;
    }
    if (success) {
        bool orphan_success =
            gpu_context_create(0, 0, &orphan_context) == K_OK &&
            gpu_allocation_create(4096U, &orphan_allocation) == K_OK &&
            gpu_fence_create(&orphan_fence) == K_OK &&
            gpu_submit(orphan_context, orphan_allocation, 0U, 64U,
                       orphan_fence, 1U) == K_OK;
        if (orphan_success) {
            /* 模拟进程退出：外部句柄引用全部释放，pending 引用必须兜底。 */
            object_put(orphan_fence);
            object_put(orphan_allocation);
            object_put(orphan_context);
            orphan_fence = 0;
            orphan_allocation = 0;
            orphan_context = 0;
            orphan_success = deferred_run(64U) >= 1U;
        }
        success = orphan_success;
    }
    if (orphan_fence != 0) object_put(orphan_fence);
    if (orphan_context != 0) object_put(orphan_context);
    if (orphan_allocation != 0) object_put(orphan_allocation);
    if (lost_fence != 0) object_put(lost_fence);
    if (batch_fence != 0) object_put(batch_fence);
    if (timeline_fence != 0) object_put(timeline_fence);
    if (fence != 0) object_put(fence);
    if (context != 0) object_put(context);
    if (allocation != 0) object_put(allocation);
    return success;
}
