#include <kernel/message_port.h>
#include <kernel/mm.h>
#include <kernel/string.h>

static void message_port_lock(message_port_t *port) {
    spinlock_lock(&port->lock);
}

static void message_port_unlock(message_port_t *port) {
    spinlock_unlock(&port->lock);
}

typedef struct message_port_receive_context {
    message_port_t *port;
    void *data;
    size_t capacity;
    size_t *size;
    kstatus_t status;
} message_port_receive_context_t;

/* 等待层在队列锁外检查谓词。检查与取走条目使用同一把端口锁，
 * 返回就绪即表示本次接收已完成，不能再被其他消费者抢走并重启超时。 */
static bool message_port_try_receive(void *context) {
    message_port_receive_context_t *receive = context;
    message_port_t *port = receive->port;
    bool finished = true;
    message_port_lock(port);
    if (port->count != 0U) {
        message_port_message_t *message = &port->messages[port->head];
        if (receive->capacity < message->size) {
            receive->status = K_EINVAL;
        } else {
            memmove(receive->data, message->data, message->size);
            *receive->size = message->size;
            if (++port->head == port->capacity) port->head = 0U;
            --port->count;
            receive->status = K_OK;
        }
    } else if (atomic_load_explicit(&port->closed, memory_order_acquire)) {
        receive->status = K_ECANCELED;
    } else {
        finished = false;
    }
    message_port_unlock(port);
    return finished;
}

static bool message_port_is_signaled(const void *object) {
    const message_port_t *port = (const message_port_t *)object;
    message_port_t *mutable_port = (message_port_t *)(uintptr_t)port;
    bool signaled;
    message_port_lock(mutable_port);
    signaled = mutable_port->count != 0U ||
               atomic_load_explicit(&mutable_port->closed, memory_order_acquire);
    message_port_unlock(mutable_port);
    return signaled;
}

static int64_t message_port_wait_value(const void *object) {
    const message_port_t *port = (const message_port_t *)object;
    message_port_t *mutable_port = (message_port_t *)(uintptr_t)port;
    int64_t value;
    message_port_lock(mutable_port);
    value = mutable_port->count;
    if (value == 0 && atomic_load_explicit(&mutable_port->closed, memory_order_acquire)) {
        value = K_ECANCELED;
    }
    message_port_unlock(mutable_port);
    return value;
}

static void message_port_destroy(void *object) {
    message_port_t *port = (message_port_t *)object;
    (void)message_port_close(port);
    kfree(port);
}

static void message_port_handle_close(void *object) {
    (void)message_port_close((message_port_t *)object);
}

static const object_ops_t g_message_port_ops = {
    .destroy = message_port_destroy,
    .handle_close = message_port_handle_close,
    .type_name = "MessagePort",
    .is_signaled = message_port_is_signaled,
    .wait_value = message_port_wait_value,
};

kstatus_t message_port_create(uint32_t capacity, message_port_t **out) {
    if (out == 0) return K_EINVAL;
    if (capacity == 0U) capacity = OS_PORT_DEFAULT_CAPACITY;
    if (capacity > OS_PORT_MAX_CAPACITY) return K_EINVAL;
    /* 有界容量不会导致此大小溢出。空槽保持未读状态；发送方会先初始化大小和负载，
     * 再发布计数。 */
    message_port_t *port = (message_port_t *)kmalloc(
        sizeof(*port) + (size_t)capacity * sizeof(port->messages[0]), 0);
    if (port == 0) return K_ENOMEM;
    object_header_init(&port->object, KOBJECT_TYPE_MESSAGE_PORT,
                       &g_message_port_ops);
    spinlock_init(&port->lock);
    wait_queue_init(&port->receive_waitq);
    atomic_init(&port->closed, false);
    port->capacity = capacity;
    port->head = 0U;
    port->tail = 0U;
    port->count = 0U;
    *out = port;
    return K_OK;
}

kstatus_t message_port_send(message_port_t *port, const void *data, size_t size) {
    if (port == 0 || data == 0 || size == 0 || size > OS_PORT_MAX_MESSAGE_SIZE) {
        return K_EINVAL;
    }
    message_port_lock(port);
    if (atomic_load_explicit(&port->closed, memory_order_acquire)) {
        message_port_unlock(port);
        return K_ECANCELED;
    }
    if (port->count >= port->capacity) {
        message_port_unlock(port);
        return K_EAGAIN;
    }
    message_port_message_t *message = &port->messages[port->tail];
    message->size = (uint32_t)size;
    memmove(message->data, data, size);
    if (++port->tail == port->capacity) port->tail = 0U;
    ++port->count;
    message_port_unlock(port);
    (void)wake_one(&port->receive_waitq);
    object_notify_signaled(port);
    return K_OK;
}

kstatus_t message_port_receive(message_port_t *port, void *data, size_t capacity,
                               size_t *size, uint64_t timeout_ns) {
    if (port == 0 || data == 0 || size == 0 || capacity == 0) return K_EINVAL;
    message_port_receive_context_t receive = {
        .port = port, .data = data, .capacity = capacity, .size = size,
        .status = K_ETIMEDOUT,
    };
    if (message_port_try_receive(&receive)) return receive.status;
    if (timeout_ns == 0U) return K_ETIMEDOUT;
    kstatus_t status = wait_on_queue(&port->receive_waitq,
        message_port_try_receive, &receive, timeout_ns);
    return status == K_OK ? receive.status : status;
}

kstatus_t message_port_close(message_port_t *port) {
    if (port == 0) return K_EINVAL;
    bool was_closed = atomic_exchange_explicit(&port->closed, true,
                                               memory_order_acq_rel);
    /* 关闭状态会一直保持。后续等待者会在订阅时重新检查谓词；重复关闭时无需重新
     * 扫描任一等待队列。 */
    if (!was_closed) {
        (void)wake_all(&port->receive_waitq);
        object_notify_signaled(port);
    }
    return K_OK;
}

bool message_port_self_test(void) {
    message_port_t *port = 0;
    const uint8_t sent[] = {'p', 'o', 'r', 't'};
    uint8_t received[sizeof(sent)] = {0};
    size_t size = 0;
    if (message_port_create(1U, &port) != K_OK || port == 0) return false;
    bool success = message_port_send(port, sent, sizeof(sent)) == K_OK &&
                   message_port_send(port, sent, sizeof(sent)) == K_EAGAIN &&
                   object_is_signaled(port) &&
                   message_port_receive(port, received, sizeof(received), &size, 0U) == K_OK &&
                   size == sizeof(sent) && received[0] == sent[0] &&
                   message_port_receive(port, received, sizeof(received), &size, 0U) == K_ETIMEDOUT &&
                   message_port_close(port) == K_OK &&
                   message_port_receive(port, received, sizeof(received), &size, 0U) == K_ECANCELED;
    uint32_t sequence = atomic_load_explicit(&port->receive_waitq.sequence,
                                              memory_order_acquire);
    for (uint32_t i = 0U; success && i < 64U; ++i) {
        success = message_port_close(port) == K_OK &&
                  atomic_load_explicit(&port->receive_waitq.sequence,
                                       memory_order_acquire) == sequence;
    }
    object_put(port);
    return success;
}
