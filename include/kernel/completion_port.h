#pragma once

#include <kernel/base.h>
#include <kernel/object.h>
#include <kernel/spinlock.h>
#include <kernel/wait.h>
#include <uapi/io.h>

/* completion port 是异步 I/O、网络和设备事件共用的内核对象。 */
#define KOBJECT_TYPE_COMPLETION_PORT 0x0108U
#define COMPLETION_PORT_DEFAULT_CAPACITY 64U
#define COMPLETION_PORT_MAX_CAPACITY 256U

#define COMPLETION_PORT_RIGHT_READ  (1U << 0)
#define COMPLETION_PORT_RIGHT_WRITE (1U << 1)
#define COMPLETION_PORT_RIGHT_WAIT  (1U << 31)
#define COMPLETION_PORT_RIGHT_ALL   (COMPLETION_PORT_RIGHT_READ | \
                                     COMPLETION_PORT_RIGHT_WRITE | \
                                     COMPLETION_PORT_RIGHT_WAIT)

typedef struct completion_port {
    object_header_t object;
    spinlock_t lock;
    wait_queue_t waitq;
    atomic_bool closed;
    uint32_t capacity;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    /* 一次性分配恰好 capacity 个槽位；尚未发布的槽位不可读取。 */
    os_completion_entry_t entries[];
} completion_port_t;

kstatus_t completion_port_create(uint32_t capacity, completion_port_t **out);
kstatus_t completion_port_post(completion_port_t *port,
                               const os_completion_entry_t *entry);
kstatus_t completion_port_wait(completion_port_t *port, uint64_t timeout_ns,
                               os_completion_entry_t *entry);
kstatus_t completion_port_close(completion_port_t *port);

bool completion_port_self_test(void);
