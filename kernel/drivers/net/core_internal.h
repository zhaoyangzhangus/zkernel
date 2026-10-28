#pragma once

/* REFACTOR_P8_E1000_CORE_INTERNAL_MODEL */

#include <kernel/dma.h>
#include <kernel/net_core.h>
#include <kernel/pci.h>

#include "internal.h"

#define E1000_RING_COUNT 16U
#define E1000_MII_BMCR 0U
#define E1000_REG_RDT 0x2818U
#define E1000_RX_STATUS_DD (1U << 0)
#define E1000_RX_BUDGET 32U

/* Cover a full 256KiB TCP window (~180 Ethernet frames) while the bounded
 * 32-packet poll batch dispatches ACKs. 256 descriptors still fit one page;
 * do not increase the per-callback software queue/latency budget with it. */
#define RTL8126_RING_COUNT 256U
#define RTL8126_TX_BUFFER_SIZE 2048U
#define RTL8126_RX_BUFFER_SIZE 16384U
#define RTL8126_RX_DESCRIPTOR_LENGTH (RTL8126_RX_BUFFER_SIZE - 1U)

typedef enum net_backend {
    NET_BACKEND_NONE = 0,
    NET_BACKEND_E1000,
    NET_BACKEND_RTL8126,
} net_backend_t;

typedef struct __attribute__((packed)) e1000_descriptor {
    uint64_t address;
    uint16_t length;
    uint8_t checksum_offset;
    uint8_t command;
    uint8_t status;
    uint8_t checksum_status;
    uint16_t special;
} e1000_descriptor_t;

_Static_assert(sizeof(e1000_descriptor_t) == 16U, "e1000 descriptor ABI");

typedef struct __attribute__((packed)) rtl8126_descriptor {
    uint32_t options1;
    uint32_t options2;
    uint64_t address;
} rtl8126_descriptor_t;

_Static_assert(sizeof(rtl8126_descriptor_t) == 16U,
               "RTL8126 descriptor ABI");

typedef struct e1000_state {
    const pci_device_t *pci;
    net_backend_t backend;
    spinlock_t tx_lock;
    uint8_t mmio_bar;
    bool io_mode;
    uint16_t io_base;
    volatile uint8_t *mmio;
    uint64_t mmio_span;
    page_t *tx_ring_page;
    page_t *rx_ring_page;
    dma_mapping_t tx_ring_dma;
    dma_mapping_t rx_ring_dma;
    e1000_descriptor_t *tx_ring;
    e1000_descriptor_t *rx_ring;
    page_t *tx_pages[E1000_RING_COUNT];
    page_t *rx_pages[E1000_RING_COUNT];
    dma_mapping_t tx_dma[E1000_RING_COUNT];
    dma_mapping_t rx_dma[E1000_RING_COUNT];
    uint8_t mac[6];
    uint32_t ipv4_address;
    uint8_t ipv4_prefix_length;
    uint32_t ipv4_gateway;
    uint8_t ipv6_address[16];
    bool ipv6_address_configured;
    uint16_t phy_control;
    bool phy_loopback;
    uint32_t tx_tail;
    uint32_t rx_clean;
    uint32_t hardware_queue_count;
    uint32_t software_queue_count;
    e1000_software_queue_t software_queues[E1000_SOFTWARE_QUEUE_COUNT];
    net_device_t device;
    net_arp_cache_t arp_cache;
    net_ipv6_neighbor_cache_t ndp_cache;
    bool tcp_loopback_peer_enabled;
    bool tcp_loopback_synack_seen;
    bool tcp_loopback_data_ack_seen;
    bool link_up;
    bool initialized;
    e1000_recovery_context_t recovery;

    /* RTL8126 keeps its own descriptor format and ring lifetime. */
    volatile uint8_t *rtl_mmio;
    uint64_t rtl_mmio_span;
    uint8_t rtl_mmio_bar;
    page_t *rtl_tx_ring_page;
    page_t *rtl_rx_ring_page;
    dma_mapping_t rtl_tx_ring_dma;
    dma_mapping_t rtl_rx_ring_dma;
    rtl8126_descriptor_t *rtl_tx_ring;
    rtl8126_descriptor_t *rtl_rx_ring;
    page_t *rtl_tx_buffer_page;
    page_t *rtl_rx_buffer_page;
    dma_mapping_t rtl_tx_buffer_dma;
    dma_mapping_t rtl_rx_buffer_dma;
    uint8_t *rtl_tx_buffers;
    uint8_t *rtl_rx_buffers;
    uint16_t rtl_tx_next;
    uint16_t rtl_tx_clean;
    uint16_t rtl_tx_pending;
    uint16_t rtl_rx_next;
    uint16_t rtl_xid;
    uint32_t rtl_phy_ocp_base;
    uint8_t rtl_irq_vector;
    bool rtl_irq_msix;
    bool rtl_irq_bound;
    bool rtl_dma_quarantined;
    bool rtl_firmware_required;
    volatile uint32_t rtl_irq_count;
    volatile uint32_t rtl_irq_status_count;
    volatile uint32_t rtl_rx_descriptor_count;
    volatile uint32_t rtl_rx_frame_count;
    volatile uint32_t rtl_tx_count;
    uint32_t rtl_diagnostic_reports;
    uint64_t rtl_diagnostic_tsc;
    uint8_t rtl_cmd_after_start;
    uint8_t rtl_cmd_before_irq;
    uint8_t rtl_cmd_after_irq;
    uint8_t rtl_cmd_last_before_enable;
    uint8_t rtl_cmd_last_after_enable;
    uint32_t rtl_enable_attempts;
    uint32_t rtl_enable_failures;
} e1000_state_t;

static inline uint32_t e1000_read(const e1000_state_t *state, uint32_t offset) {
    if (state->io_mode) {
        __asm__ volatile ("outw %w0, %1" : : "a"((uint16_t)offset),
                          "Nd"(state->io_base));
        uint32_t value;
        __asm__ volatile ("inl %1, %0" : "=a"(value) :
                          "Nd"((uint16_t)(state->io_base + 4U)));
        return value;
    }
    return *(volatile const uint32_t *)(state->mmio + offset);
}

static inline void e1000_write(const e1000_state_t *state, uint32_t offset,
                               uint32_t value) {
    if (state->io_mode) {
        __asm__ volatile ("outw %w0, %1" : : "a"((uint16_t)offset),
                          "Nd"(state->io_base));
        __asm__ volatile ("outl %0, %1" : : "a"(value),
                          "Nd"((uint16_t)(state->io_base + 4U)));
        return;
    }
    *(volatile uint32_t *)(state->mmio + offset) = value;
    __asm__ volatile ("mfence" : : : "memory");
}

bool e1000_transmit(e1000_state_t *state, const uint8_t *frame, size_t length);
kstatus_t e1000_transmit_status(e1000_state_t *state, const uint8_t *frame,
                               size_t length);
void e1000_record_error(uint32_t error);
e1000_state_t *e1000_controller_state(void);
void e1000_state_prepare(e1000_state_t *state, const pci_device_t *pci,
                         net_backend_t backend);
void e1000_configure_link_local(e1000_state_t *state);
void e1000_install_socket_outputs(e1000_state_t *state);
void e1000_remove_socket_outputs(void);
void e1000_runtime_set_ready(bool ready);
void e1000_self_test_begin(void);
bool e1000_self_test_initialize(e1000_state_t *state, const pci_device_t *pci);
bool e1000_self_test_destroy(e1000_state_t *state);
bool e1000_self_test_enable_phy_loopback(e1000_state_t *state);
bool e1000_self_test_restore_phy(e1000_state_t *state, uint16_t value);
bool e1000_self_test_poll_receive(e1000_state_t *state);
uint32_t e1000_self_test_recovery_read(const void *owner, uint32_t offset);
void e1000_self_test_recovery_write(const void *owner, uint32_t offset,
                                    uint32_t value);
bool e1000_poll_receive(e1000_state_t *state);
bool e1000_process_rx_frame(e1000_state_t *state, const uint8_t *buffer,
                            uint16_t length);
bool e1000_dispatch_software_queues(e1000_state_t *state);

const pci_device_t *rtl8126_pci_find(void);
bool rtl8126_initialize_state(e1000_state_t *state, const pci_device_t *pci);
bool rtl8126_destroy_state(e1000_state_t *state);
kstatus_t rtl8126_transmit_status(e1000_state_t *state, const uint8_t *frame,
                                  size_t length);
bool rtl8126_poll_receive_state(e1000_state_t *state);
void rtl8126_rearm_interrupts(e1000_state_t *state);
bool rtl8126_link_up_state(e1000_state_t *state);
void rtl8126_emit_diagnostic(void);
bool rtl8126_bind_interrupt_state(e1000_state_t *state);
void rtl8126_unbind_interrupt_state(e1000_state_t *state);

kstatus_t e1000_device_transmit(void *context, const void *frame, size_t length);
kstatus_t e1000_tcp_ipv4_output(
    void *context, uint32_t source_address, uint16_t source_port,
    uint32_t destination_address, uint16_t destination_port,
    uint32_t sequence, uint32_t acknowledgement, uint8_t flags,
    uint16_t window, const void *payload, size_t payload_length);
kstatus_t e1000_tcp_ipv6_output(
    void *context, const uint8_t source_address[16], uint16_t source_port,
    const uint8_t destination_address[16], uint16_t destination_port,
    uint32_t sequence, uint32_t acknowledgement, uint8_t flags,
    uint16_t window, const void *payload, size_t payload_length);
kstatus_t e1000_udp_ipv4_output(
    void *context, uint32_t source_address, uint16_t source_port,
    uint32_t destination_address, uint16_t destination_port,
    const void *payload, size_t payload_length);
kstatus_t e1000_udp_ipv6_output(
    void *context, const uint8_t source_address[16], uint16_t source_port,
    const uint8_t destination_address[16], uint16_t destination_port,
    const void *payload, size_t payload_length);

bool e1000_address6_equal(const uint8_t left[16], const uint8_t right[16]);
bool e1000_address6_zero(const uint8_t address[16]);
bool e1000_send_neighbor_advertisement(e1000_state_t *state,
                                       const net_ndp_view_t *request);
bool e1000_arp_reply(e1000_state_t *state, const net_arp_view_t *request);
bool e1000_tcp_loopback_peer_ack(e1000_state_t *state,
                                 const e1000_rx_packet_t *packet,
                                 const socket_tcp_reply_t *reply);
bool e1000_tcp_loopback_peer_ack6(e1000_state_t *state,
                                  const e1000_rx_packet_t *packet,
                                  const socket_tcp_reply_t *reply);
bool e1000_unmatched_tcp_reset_self_test(void);
