#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <kernel/socket.h>
#include <kernel/spinlock.h>

struct pci_device;

const struct pci_device *e1000_pci_find(void);

typedef uint32_t (*e1000_recovery_read_t)(const void *owner,
                                          uint32_t offset);
typedef void (*e1000_recovery_write_t)(const void *owner,
                                       uint32_t offset,
                                       uint32_t value);

/* The recovery unit owns interrupt registration and device interrupt state.
 * The controller keeps its hardware representation private through callbacks. */
typedef struct e1000_recovery_context {
    void *owner;
    e1000_recovery_read_t read;
    e1000_recovery_write_t write;
    uint8_t irq_vector;
    bool irq_bound;
} e1000_recovery_context_t;

bool e1000_recovery_bind(e1000_recovery_context_t *context,
                         const struct pci_device *pci,
                         void *owner,
                         e1000_recovery_read_t read,
                         e1000_recovery_write_t write);
void e1000_recovery_unbind(e1000_recovery_context_t *context);

#define E1000_SOFTWARE_QUEUE_COUNT 4U
#define E1000_SOFTWARE_QUEUE_DEPTH 32U

typedef struct e1000_rx_packet {
    uint16_t family;
    uint8_t protocol;
    uint8_t flags;
    uint8_t source_mac[6];
    uint8_t destination_mac[6];
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t payload_length;
    uint32_t source_address;
    uint32_t destination_address;
    uint32_t sequence;
    uint32_t acknowledgement;
    uint16_t window;
    uint16_t peer_mss;
    uint8_t peer_window_scale;
    bool peer_window_scale_present;
    uint8_t source_address6[16];
    uint8_t destination_address6[16];
    /* Bytes beyond payload_length are unspecified and must not be read. */
    uint8_t payload[SOCKET_MAX_PAYLOAD];
} e1000_rx_packet_t;

typedef struct e1000_software_queue {
    e1000_rx_packet_t packets[E1000_SOFTWARE_QUEUE_DEPTH];
    spinlock_t lock;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    bool borrowed;
    uint64_t dropped;
} e1000_software_queue_t;

void e1000_queue_init(e1000_software_queue_t *queue);
bool e1000_queue_push(e1000_software_queue_t *queue,
                      const e1000_rx_packet_t *packet);
bool e1000_queue_push_payload(e1000_software_queue_t *queue,
                              const e1000_rx_packet_t *packet,
                              const uint8_t *payload);
/* 借用期间槽位仍由队列持有，处理结束后必须归还。初始化只能在无使用者时进行。 */
const e1000_rx_packet_t *e1000_queue_borrow(e1000_software_queue_t *queue);
void e1000_queue_release(e1000_software_queue_t *queue,
                         const e1000_rx_packet_t *packet);
bool e1000_packet_queue_self_test(void);

uint32_t e1000_flow_hash_ipv4(uint32_t source_address,
                              uint32_t destination_address,
                              uint16_t source_port,
                              uint16_t destination_port,
                              uint8_t protocol);
uint32_t e1000_flow_hash_ipv6(const uint8_t source_address[16],
                              const uint8_t destination_address[16],
                              uint16_t source_port,
                              uint16_t destination_port);
uint32_t e1000_select_software_queue(uint32_t software_queue_count,
                                     uint32_t flow_hash);
bool e1000_rss_self_test_state(uint32_t software_queue_count);
