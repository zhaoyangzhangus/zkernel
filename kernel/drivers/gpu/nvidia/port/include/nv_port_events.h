#pragma once
#include "nv_port.h"

/* 由 OS 拥有的通知状态。厂商负载转换逻辑保留在此边界之上。调用方在整个操作期间持有队列所有者。 */
typedef struct nv_port_event_record {
    uint32_t parent, object, index, info32;
    uint16_t info16;
} nv_port_event_record_t;
struct nv_port_event_node;
typedef struct nv_port_event_queue {
    nv_port_spinlock_t lock;
    struct nv_port_event_node *head, *tail;
    bool dataless_pending, closed;
} nv_port_event_queue_t;

#define NV_PORT_EVENT_READ 1U
#define NV_PORT_EVENT_CLOSED 2U
void nv_port_event_queue_init(nv_port_event_queue_t *queue);
kstatus_t nv_port_event_post(nv_port_event_queue_t *queue,
                             const nv_port_event_record_t *record);
/* IRQ 路径只使用现有 slab 容量，竞争时可能失败。无数据发布仍不需要分配内存。队列所有者保持固定。 */
kstatus_t nv_port_event_post_irq(nv_port_event_queue_t *queue,
                                 const nv_port_event_record_t *record);
kstatus_t nv_port_event_take(nv_port_event_queue_t *queue,
                             nv_port_event_record_t *record, bool *more);
uint32_t nv_port_event_poll(nv_port_event_queue_t *queue, bool consume_dataless);
bool nv_port_event_queue_close(nv_port_event_queue_t *queue);

/* 为 poll/epoll 提供带引用计数、可单独固定的等待源。创建者拥有一个对象引用。释放此引用前先关闭
 * 逻辑状态；等待者可以继续持有已关闭的源，直到取消订阅。 */
typedef struct nv_port_event_source nv_port_event_source_t;
nv_port_event_source_t *nv_port_event_source_create(void);
void *nv_port_event_source_wait_object(nv_port_event_source_t *source);
kstatus_t nv_port_event_source_post(nv_port_event_source_t *source,
                                    const nv_port_event_record_t *record);
kstatus_t nv_port_event_source_post_irq(nv_port_event_source_t *source,
                                        const nv_port_event_record_t *record);
kstatus_t nv_port_event_source_take(nv_port_event_source_t *source,
                                    nv_port_event_record_t *record, bool *more);
uint32_t nv_port_event_source_poll(nv_port_event_source_t *source,
                                   bool consume_dataless);
void nv_port_event_source_close(nv_port_event_source_t *source);
