#include "internal.h"
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/string.h>

static void e1000_queue_lock(e1000_software_queue_t *queue) {
    sched_preempt_disable();
    spinlock_lock(&queue->lock);
}

static void e1000_queue_unlock(e1000_software_queue_t *queue) {
    spinlock_unlock(&queue->lock);
    sched_preempt_enable();
}

void e1000_queue_init(e1000_software_queue_t *queue) {
    if (queue == 0) return;
    spinlock_init(&queue->lock);
    queue->head = 0U;
    queue->tail = 0U;
    queue->count = 0U;
    queue->borrowed = false;
    queue->dropped = 0U;
}

bool e1000_queue_push(e1000_software_queue_t *queue,
                      const e1000_rx_packet_t *packet) {
    return e1000_queue_push_payload(queue, packet,
                                     packet != 0 ? packet->payload : 0);
}

/* Copy the parsed DMA payload straight into its owned queue slot. No DMA
 * pointer escapes this call: the driver may recycle the descriptor on return. */
bool e1000_queue_push_payload(e1000_software_queue_t *queue,
                              const e1000_rx_packet_t *packet,
                              const uint8_t *payload) {
    if (queue == 0 || packet == 0 || packet->payload_length > SOCKET_MAX_PAYLOAD) {
        return false;
    }
    if (packet->payload_length != 0U && payload == 0) return false;
    e1000_queue_lock(queue);
    if (queue->count >= E1000_SOFTWARE_QUEUE_DEPTH) {
        ++queue->dropped;
        e1000_queue_unlock(queue);
        return false;
    }
    e1000_rx_packet_t *slot = &queue->packets[queue->tail];
    memcpy(slot, packet, offsetof(e1000_rx_packet_t, payload));
    if (packet->payload_length != 0U)
        memcpy(slot->payload, payload, packet->payload_length);
    queue->tail = (queue->tail + 1U) % E1000_SOFTWARE_QUEUE_DEPTH;
    ++queue->count;
    e1000_queue_unlock(queue);
    return true;
}

/* 借用队首期间不持锁，也不允许生产者复用该槽位；同队列的其他消费者
 * 等待归还后再取，保证处理顺序。网卡生命周期锁保护队列销毁和重新初始化。 */
const e1000_rx_packet_t *e1000_queue_borrow(e1000_software_queue_t *queue) {
    if (queue == 0) return 0;
    e1000_queue_lock(queue);
    const e1000_rx_packet_t *packet = 0;
    if (queue->count != 0U && !queue->borrowed) {
        queue->borrowed = true;
        packet = &queue->packets[queue->head];
    }
    e1000_queue_unlock(queue);
    return packet;
}

void e1000_queue_release(e1000_software_queue_t *queue,
                         const e1000_rx_packet_t *packet) {
    if (queue == 0 || packet == 0) return;
    e1000_queue_lock(queue);
    if (queue->borrowed && queue->count != 0U &&
        packet == &queue->packets[queue->head]) {
        queue->head = (queue->head + 1U) % E1000_SOFTWARE_QUEUE_DEPTH;
        --queue->count;
        queue->borrowed = false;
    }
    e1000_queue_unlock(queue);
}

bool e1000_packet_queue_self_test(void) {
    e1000_software_queue_t *queue = kmalloc(sizeof(*queue), 0U);
    e1000_rx_packet_t expected;
    bool success = false;

    _Static_assert(E1000_SOFTWARE_QUEUE_DEPTH <= SOCKET_MAX_PAYLOAD,
                   "queue self-test payload capacity");
    if (queue == 0) return false;
    e1000_queue_init(queue);
    /* Initialize every byte the bounded queue copy will read, but not the
     * unused payload capacity. Verify data as well as FIFO counters below. */
    memset(&expected, 0, offsetof(e1000_rx_packet_t, payload) +
                         E1000_SOFTWARE_QUEUE_DEPTH);
    for (uint32_t index = 0U; index < E1000_SOFTWARE_QUEUE_DEPTH; ++index) {
        expected.payload[index] = (uint8_t)(index + 1U);
    }
    for (uint32_t index = 0U; index < E1000_SOFTWARE_QUEUE_DEPTH; ++index) {
        expected.sequence = index + 1U;
        expected.payload_length = (uint16_t)(index + 1U);
        if (!e1000_queue_push(queue, &expected)) goto out;
    }
    if (e1000_queue_push(queue, &expected) || queue->dropped != 1U) {
        goto out;
    }
    for (uint32_t index = 0U; index < E1000_SOFTWARE_QUEUE_DEPTH; ++index) {
        expected.sequence = index + 1U;
        expected.payload_length = (uint16_t)(index + 1U);
        const e1000_rx_packet_t *actual = e1000_queue_borrow(queue);
        if (actual == 0 ||
            memcmp(actual, &expected,
                   offsetof(e1000_rx_packet_t, payload) +
                   expected.payload_length) != 0) {
            goto out;
        }
        e1000_queue_release(queue, actual);
    }
    success = e1000_queue_borrow(queue) == 0 && queue->count == 0U;
out:
    kfree(queue);
    return success;
}
