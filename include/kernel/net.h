#pragma once
#pragma once
#include "base.h"
#include "object.h"
#include "list.h"
#include "wait.h"
#include "refcount.h"

typedef kstatus_t (*net_device_transmit_fn)(void *context,
                                            const void *frame,
                                            size_t length);

/* NIC 后端向网络栈暴露的最小二层设备能力。 */
typedef struct net_device {
    char name[16];
    uint8_t mac[6];
    uint16_t mtu;
    uint16_t max_frame_size;
    bool link_up;
    void *context;
    net_device_transmit_fn transmit;
} net_device_t;

typedef struct net_buffer {
    refcount_t refs;
    uint8_t *head;
    uint8_t *data;
    uint8_t *tail;
    uint8_t *end;
    uint32_t length;
    uint32_t protocol;
    net_device_t *device;
    uint32_t checksum_state;
    uint32_t flags;
    void *fragments;
} net_buffer_t;

void net_device_init(net_device_t *device, const char *name,
                     const uint8_t mac[6], uint16_t mtu,
                     net_device_transmit_fn transmit, void *context);
kstatus_t net_device_send(net_device_t *device, const void *frame, size_t length);

/* 运行时自检在失败时使用的系统调用进度快照。 */
uint32_t syscall_socket_create_debug_progress(uint32_t cpu_index);

typedef struct socket {
    object_header_t object;
    uint16_t family;
    uint16_t type;
    uint16_t protocol;
    uint16_t state;
    uint32_t flags;

    wait_queue_t waitq;
    spinlock_t lock;

    void *rx_queue;
    void *tx_queue;
    void *protocol_state;
} socket_t;
