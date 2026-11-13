#include <kernel/mm.h>
#include <kernel/object.h>
#include <kernel/wait.h>

typedef struct {
    void *const *objects;
    size_t count;
    bool wait_all;
    uint32_t signaled_index;
} object_wait_context_t;

void object_header_init(object_header_t *header, object_type_id_t type,
                        const object_ops_t *ops) {
    if (header == 0) return;
    refcount_init(&header->refs, 1U);
    wait_queue_init(&header->signal_waiters);
    header->type = type;
    header->flags = 0U;
    header->ops = ops;
}

void object_get(void *object) {
    if (object == 0) return;
    object_header_t *header = (object_header_t *)object;
    atomic_fetch_add_explicit(&header->refs.value, 1U, memory_order_relaxed);
}

bool object_try_get(void *object) {
    if (object == 0) return false;
    object_header_t *header = (object_header_t *)object;
    unsigned count = atomic_load_explicit(&header->refs.value,
                                          memory_order_acquire);
    while (count != 0U) {
        if (atomic_compare_exchange_weak_explicit(&header->refs.value, &count,
                                                   count + 1U,
                                                   memory_order_acquire,
                                                   memory_order_relaxed)) return true;
    }
    return false;
}

void object_put(void *object) {
    if (object == 0) return;
    object_header_t *header = (object_header_t *)object;
    if (atomic_fetch_sub_explicit(&header->refs.value, 1U,
                                  memory_order_acq_rel) == 1U &&
        header->ops != 0 && header->ops->destroy != 0) {
        header->ops->destroy(object);
    }
}

bool object_is_signaled(const void *object) {
    if (object == 0) return false;
    const object_header_t *header = (const object_header_t *)object;
    return header->ops != 0 && header->ops->is_signaled != 0 &&
           header->ops->is_signaled(object);
}

int64_t object_wait_value(const void *object) {
    if (object == 0) return K_EINVAL;
    const object_header_t *header = (const object_header_t *)object;
    return header->ops != 0 && header->ops->wait_value != 0 ?
           header->ops->wait_value(object) : 0;
}

void object_notify_changed(void *object) {
    if (object == 0) return;
    object_header_t *header = (object_header_t *)object;
    (void)wake_all(&header->signal_waiters);
}

void object_notify_signaled(void *object) {
    if (object == 0 || !object_is_signaled(object)) return;
    object_notify_changed(object);
}

static bool object_pin_wait_set(void *const *objects, size_t count,
                                bool require_signaled) {
    size_t pinned = 0U;
    for (; pinned < count; ++pinned) {
        void *object = objects[pinned];
        if (!object_try_get(object)) break;
        const object_header_t *header = (const object_header_t *)object;
        if (header->ops == 0 ||
            (require_signaled && header->ops->is_signaled == 0)) {
            object_put(object);
            break;
        }
    }
    if (pinned == count) return true;
    while (pinned != 0U) object_put(objects[--pinned]);
    return false;
}

static void object_unpin_wait_set(void *const *objects, size_t count) {
    for (size_t index = 0U; index < count; ++index) object_put(objects[index]);
}

static kstatus_t object_wait_on_pinned_signals(
    void *const *objects, size_t count, bool (*predicate)(void *),
    void *context, uint64_t timeout_ns) {
    /* 对象引用已固定；就绪和零超时查询不需要构建、排序订阅队列。
     * 阻塞路径仍会在订阅前复查谓词，避免检查与注册之间丢失通知。 */
    if (predicate(context)) return K_OK;
    if (timeout_ns == 0U) return K_ETIMEDOUT;
    wait_queue_t *inline_queues[WAIT_INLINE_QUEUE_COUNT];
    wait_queue_t **queues = inline_queues;
    if (count > WAIT_INLINE_QUEUE_COUNT) {
        if (count > SIZE_MAX / sizeof(*queues)) return K_EOVERFLOW;
        queues = (wait_queue_t **)kmalloc(count * sizeof(*queues), 0U);
        if (queues == 0) return K_ENOMEM;
    }
    for (size_t index = 0U; index < count; ++index) {
        queues[index] = &((object_header_t *)objects[index])->signal_waiters;
    }
    kstatus_t status = wait_on_queues_in_place(queues, count, predicate, context,
                                      timeout_ns);
    if (queues != inline_queues) kfree(queues);
    return status;
}

kstatus_t object_wait_on_signals(void *const *objects, size_t count,
                                 bool (*predicate)(void *), void *context,
                                 uint64_t timeout_ns) {
    if (objects == 0 || count == 0U || predicate == 0 ||
        count > SIZE_MAX / sizeof(void *)) return K_EINVAL;
    if (!object_pin_wait_set(objects, count, false)) return K_EINVAL;
    kstatus_t status = object_wait_on_pinned_signals(objects, count, predicate,
                                                     context, timeout_ns);
    object_unpin_wait_set(objects, count);
    return status;
}

kstatus_t object_wait_on_signal(bool (*predicate)(void *), void *context,
                                uint64_t timeout_ns) {
    if (predicate == 0 || context == 0) return K_EINVAL;
    void *objects[] = { context };
    return object_wait_on_signals(objects, 1U, predicate, context, timeout_ns);
}

static bool object_wait_predicate(void *opaque) {
    object_wait_context_t *context = (object_wait_context_t *)opaque;
    if (context->wait_all) {
        for (size_t index = 0U; index < context->count; ++index) {
            if (!object_is_signaled(context->objects[index])) return false;
        }
        context->signaled_index = UINT32_MAX;
        return true;
    }
    for (size_t index = 0U; index < context->count; ++index) {
        if (object_is_signaled(context->objects[index])) {
            context->signaled_index = (uint32_t)index;
            return true;
        }
    }
    return false;
}

kstatus_t object_wait_many(void *const *objects, size_t count, bool wait_all,
                           uint64_t timeout_ns, uint32_t *signaled_index,
                           int64_t *wait_value) {
    if (objects == 0 || count == 0U || count > UINT32_MAX ||
        count > SIZE_MAX / sizeof(void *) || signaled_index == 0 ||
        wait_value == 0) return K_EINVAL;
    if (!object_pin_wait_set(objects, count, true)) return K_EINVAL;

    object_wait_context_t context = {
        .objects = objects,
        .count = count,
        .wait_all = wait_all,
        .signaled_index = UINT32_MAX,
    };
    kstatus_t status = object_wait_on_pinned_signals(
        objects, count, object_wait_predicate, &context, timeout_ns);
    if (status == K_OK) {
        *signaled_index = context.signaled_index;
        *wait_value = wait_all ? 0 :
                      object_wait_value(objects[context.signaled_index]);
    }
    object_unpin_wait_set(objects, count);
    return status;
}
