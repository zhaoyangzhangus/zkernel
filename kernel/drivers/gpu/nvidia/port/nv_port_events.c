#include "include/nv_port_events.h"
#include <kernel/kmem.h>
#include <kernel/object.h>

typedef struct nv_port_event_node {
    struct nv_port_event_node *next;
    nv_port_event_record_t record;
} nv_port_event_node_t;

void nv_port_event_queue_init(nv_port_event_queue_t *queue) {
    nv_port_spin_init(&queue->lock);
    queue->head = queue->tail = NULL;
    queue->dataless_pending = queue->closed = false;
}

static void nv_port_event_append_locked(nv_port_event_queue_t *queue,
                                         nv_port_event_node_t *node) {
    if (node == NULL) queue->dataless_pending = true;
    else {
        if (queue->tail != NULL) queue->tail->next = node;
        else queue->head = node;
        queue->tail = node;
    }
}

/* 普通和中断发布共享入队事务；IRQ 只使用现有缓存容量，不进行阻塞分配。 */
static kstatus_t nv_port_event_post_internal(nv_port_event_queue_t *queue,
    const nv_port_event_record_t *record, bool irq, bool *notify) {
    if (notify != NULL) *notify = false;
    if (queue == NULL) return K_EINVAL;
    nv_port_event_node_t *node = NULL;
    if (record != NULL && !irq) {
        node = kmalloc(sizeof(*node), 0U);
        if (node == NULL) return K_ENOMEM;
        node->record = *record;
        node->next = NULL;
    }
    uint64_t flags = nv_port_spin_lock_irqsave(&queue->lock);
    if (queue->closed) {
        nv_port_spin_unlock_irqrestore(&queue->lock, flags);
        if (node != NULL) kfree(node);
        return K_EBADF;
    }
    if (record != NULL && irq) {
        /* 先检查关闭状态，保证中断路径不需要释放新分配的节点。 */
        node = kmalloc_try_cached(sizeof(*node), 0U);
        if (node == NULL) {
            nv_port_spin_unlock_irqrestore(&queue->lock, flags);
            return K_ENOMEM;
        }
        node->record = *record;
        node->next = NULL;
    }
    /* 无载荷事件只有一个待消费标志，重复发布无需重复唤醒。
     * 带载荷事件仍逐条入队和通知，不改变记录顺序或数量。 */
    if (notify != NULL) *notify = record != NULL || !queue->dataless_pending;
    nv_port_event_append_locked(queue, node);
    nv_port_spin_unlock_irqrestore(&queue->lock, flags);
    return K_OK;
}

kstatus_t nv_port_event_post(nv_port_event_queue_t *queue,
                             const nv_port_event_record_t *record) {
    return nv_port_event_post_internal(queue, record, false, NULL);
}

kstatus_t nv_port_event_post_irq(nv_port_event_queue_t *queue,
                                 const nv_port_event_record_t *record) {
    return nv_port_event_post_internal(queue, record, true, NULL);
}

kstatus_t nv_port_event_take(nv_port_event_queue_t *queue,
                             nv_port_event_record_t *record, bool *more) {
    if (queue == NULL || record == NULL || more == NULL) return K_EINVAL;
    *more = false;
    uint64_t flags = nv_port_spin_lock_irqsave(&queue->lock);
    nv_port_event_node_t *node = queue->head;
    if (node == NULL) {
        kstatus_t status = queue->closed ? K_EBADF : K_ENOENT;
        nv_port_spin_unlock_irqrestore(&queue->lock, flags);
        return status;
    }
    queue->head = node->next;
    if (queue->head == NULL) queue->tail = NULL;
    *record = node->record;
    *more = queue->head != NULL;
    nv_port_spin_unlock_irqrestore(&queue->lock, flags);
    kfree(node);
    return K_OK;
}

uint32_t nv_port_event_poll(nv_port_event_queue_t *queue, bool consume_dataless) {
    if (queue == NULL) return NV_PORT_EVENT_CLOSED;
    uint64_t flags = nv_port_spin_lock_irqsave(&queue->lock);
    uint32_t state = queue->closed ? NV_PORT_EVENT_CLOSED : 0U;
    if (queue->head != NULL || queue->dataless_pending) state |= NV_PORT_EVENT_READ;
    if (consume_dataless) queue->dataless_pending = false;
    nv_port_spin_unlock_irqrestore(&queue->lock, flags);
    return state;
}

bool nv_port_event_queue_close(nv_port_event_queue_t *queue) {
    if (queue == NULL) return false;
    uint64_t flags = nv_port_spin_lock_irqsave(&queue->lock);
    if (queue->closed) {
        nv_port_spin_unlock_irqrestore(&queue->lock, flags);
        return false;
    }
    queue->closed = true;
    queue->dataless_pending = false;
    nv_port_event_node_t *node = queue->head;
    queue->head = queue->tail = NULL;
    nv_port_spin_unlock_irqrestore(&queue->lock, flags);
    while (node != NULL) {
        nv_port_event_node_t *next = node->next;
        kfree(node);
        node = next;
    }
    return true;
}

struct nv_port_event_source {
    object_header_t header;
    nv_port_event_queue_t queue;
};

#define KOBJECT_TYPE_NV_EVENT_SOURCE 0x0124U

static bool nv_port_event_source_signaled(const void *object) {
    nv_port_event_source_t *source = (nv_port_event_source_t *)object;
    /* 就绪谓词不得消费通知。只有描述符适配器实际报告就绪状态时，才会消费无数据状态。 */
    return nv_port_event_poll(&source->queue, false) != 0U;
}

static int64_t nv_port_event_source_value(const void *object) {
    nv_port_event_source_t *source = (nv_port_event_source_t *)object;
    return nv_port_event_poll(&source->queue, false);
}

static void nv_port_event_source_destroy(void *object) {
    nv_port_event_source_t *source = object;
    nv_port_event_queue_close(&source->queue);
    kfree(source);
}

static const object_ops_t nv_port_event_source_ops = {
    .destroy = nv_port_event_source_destroy,
    .handle_close = NULL,
    .type_name = "gpu-event-source",
    .is_signaled = nv_port_event_source_signaled,
    .wait_value = nv_port_event_source_value,
};

nv_port_event_source_t *nv_port_event_source_create(void) {
    nv_port_event_source_t *source = kmalloc(sizeof(*source), 0U);
    if (source == NULL) return NULL;
    object_header_init(&source->header, KOBJECT_TYPE_NV_EVENT_SOURCE,
                       &nv_port_event_source_ops);
    nv_port_event_queue_init(&source->queue);
    return source;
}

void *nv_port_event_source_wait_object(nv_port_event_source_t *source) {
    return source;
}

static kstatus_t nv_port_event_source_post_internal(nv_port_event_source_t *source,
    const nv_port_event_record_t *record, bool irq) {
    if (source == NULL) return K_EINVAL;
    bool notify;
    kstatus_t status = nv_port_event_post_internal(&source->queue, record, irq, &notify);
    /* 调用方持有 source 引用；解锁后仅通知这个对象的订阅者。 */
    if (status == K_OK && notify) object_notify_changed(source);
    return status;
}

kstatus_t nv_port_event_source_post(nv_port_event_source_t *source,
                                    const nv_port_event_record_t *record) {
    return nv_port_event_source_post_internal(source, record, false);
}

kstatus_t nv_port_event_source_take(nv_port_event_source_t *source,
                                    nv_port_event_record_t *record, bool *more) {
    if (source == NULL) return K_EINVAL;
    return nv_port_event_take(&source->queue, record, more);
}

kstatus_t nv_port_event_source_post_irq(nv_port_event_source_t *source,
                                        const nv_port_event_record_t *record) {
    return nv_port_event_source_post_internal(source, record, true);
}

uint32_t nv_port_event_source_poll(nv_port_event_source_t *source,
                                   bool consume_dataless) {
    return source == NULL ? NV_PORT_EVENT_CLOSED :
           nv_port_event_poll(&source->queue, consume_dataless);
}

void nv_port_event_source_close(nv_port_event_source_t *source) {
    if (source == NULL) return;
    if (nv_port_event_queue_close(&source->queue))
        object_notify_changed(source);
}
