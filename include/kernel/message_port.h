#pragma once

#include <kernel/base.h>
#include <kernel/object.h>
#include <kernel/spinlock.h>
#include <kernel/wait.h>

#include <uapi/ipc.h>

#define KOBJECT_TYPE_MESSAGE_PORT 0x0109U

#define MESSAGE_PORT_RIGHT_READ  (1U << 0)
#define MESSAGE_PORT_RIGHT_WRITE (1U << 1)
#define MESSAGE_PORT_RIGHT_WAIT  (1U << 31)
#define MESSAGE_PORT_RIGHT_ALL   (MESSAGE_PORT_RIGHT_READ | \
                                  MESSAGE_PORT_RIGHT_WRITE | \
                                  MESSAGE_PORT_RIGHT_WAIT)

typedef struct message_port_message {
    uint32_t size;
    uint8_t data[OS_PORT_MAX_MESSAGE_SIZE];
} message_port_message_t;

typedef struct message_port {
    object_header_t object;
    spinlock_t lock;
    wait_queue_t receive_waitq;
    atomic_bool closed;
    uint32_t capacity;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    /* 一次性分配 capacity 个槽位；只读取已发布消息中的有效字节。 */
    message_port_message_t messages[];
} message_port_t;

kstatus_t message_port_create(uint32_t capacity, message_port_t **out);
kstatus_t message_port_send(message_port_t *port, const void *data, size_t size);
kstatus_t message_port_receive(message_port_t *port, void *data, size_t capacity,
                               size_t *size, uint64_t timeout_ns);
kstatus_t message_port_close(message_port_t *port);

bool message_port_self_test(void);
