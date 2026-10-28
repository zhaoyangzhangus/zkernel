#include <arch/x86_64/paging.h>
#include <kernel/console.h>
#include <kernel/realtest.h>
#include <kernel/kmem.h>
#include <kernel/xhci.h>
#include <usb/storage.h>
#include "internal.h"

/* REFACTOR_P8_XHCI_MSC_OWNER：bulk-only 传输和会话所有权。 */

#ifndef OS_XHCI_DIAGNOSTIC_BOT
#define OS_XHCI_DIAGNOSTIC_BOT 0
#endif

#define XHCI_RING_TRB_COUNT 256U
#define XHCI_RING_SEGMENT_COUNT 2U
#define XHCI_RING_TOTAL_TRB_COUNT \
    (XHCI_RING_SEGMENT_COUNT * XHCI_RING_TRB_COUNT)
#define XHCI_CONFIGURE_ENDPOINT_TYPE 12U
#define XHCI_STOP_ENDPOINT_TYPE 15U
#define XHCI_TRB_CYCLE (1U << 0)
#define XHCI_TRB_INTERRUPT_ON_SHORT_PACKET (1U << 2)
#define XHCI_TRB_INTERRUPT_ON_COMPLETION (1U << 5)
#define XHCI_TRB_CHAIN (1U << 4)
#define XHCI_TRB_LINK_TOGGLE_CYCLE (1U << 1)
#define XHCI_TRB_TYPE_SHIFT 10U
#define XHCI_NORMAL_TRB_TYPE 1U
#define XHCI_LINK_TRB_TYPE 6U
#define XHCI_TRB_SLOT_SHIFT 24U
#define XHCI_TRB_ENDPOINT_SHIFT 16U
#define XHCI_TRANSFER_EVENT_TYPE 32U
#define XHCI_COMPLETION_SHIFT 24U
#define XHCI_COMPLETION_SUCCESS 1U
#define XHCI_COMPLETION_SHORT_PACKET 13U
#define XHCI_PORT_REGISTER_BASE 0x400U
#define XHCI_PORT_REGISTER_STRIDE 0x10U
#define XHCI_PORTSC_READY 0x03U
#define XHCI_UAS_STREAM_ID 1U
#define XHCI_UAS_TASK_TAG XHCI_UAS_STREAM_ID
#define XHCI_UAS_COMMAND_IU 0x01U
#define XHCI_UAS_STATUS_IU 0x03U
#define XHCI_UAS_RESPONSE_IU 0x04U
#define XHCI_UAS_READ_READY_IU 0x06U
#define XHCI_UAS_WRITE_READY_IU 0x07U
#define XHCI_UAS_COMMAND_LENGTH 32U
/* Linux 的 sense_iu 为 16 字节，另加 96 字节 SCSI sense 缓冲区。 */
#define XHCI_UAS_STATUS_LENGTH 112U
#define XHCI_UAS_REQUESTED_STREAM_IDS 256U
#define XHCI_UAS_STREAM_CONTEXT_BYTES 16U

typedef struct xhci_msc_transport {
    bool configured;
    bool uas;
    bool uas_streams;
    uint8_t slot;
    uint8_t interface_number;
    uint8_t bulk_in_endpoint;
    uint8_t bulk_out_endpoint;
    uint16_t bulk_in_max_packet;
    uint16_t bulk_out_max_packet;
    xhci_dma_region_t bulk_in_ring;
    xhci_dma_region_t bulk_out_ring;
    xhci_dma_region_t data_buffer;
    dma_mapping_t direct_mapping;
    void *direct_cpu;
    uint32_t direct_offset;
    uint32_t bulk_in_enqueue;
    uint32_t bulk_out_enqueue;
    uint8_t bulk_in_cycle;
    uint8_t bulk_out_cycle;
    xhci_dma_region_t uas_command_ring;
    xhci_dma_region_t uas_status_ring;
    xhci_dma_region_t uas_data_in_ring;
    xhci_dma_region_t uas_data_out_ring;
    /* Stream 1 使用独立的双段环；Stream 0 保留不用。 */
    xhci_dma_region_t uas_status_stream_ring;
    xhci_dma_region_t uas_data_in_stream_ring;
    xhci_dma_region_t uas_data_out_stream_ring;
    xhci_dma_region_t uas_command_buffer;
    xhci_dma_region_t uas_status_buffer;
    xhci_dma_region_t uas_data_buffer;
    xhci_dma_region_t uas_status_streams;
    xhci_dma_region_t uas_data_in_streams;
    xhci_dma_region_t uas_data_out_streams;
    uint8_t uas_command_endpoint;
    uint8_t uas_status_endpoint;
    uint8_t uas_data_in_endpoint;
    uint8_t uas_data_out_endpoint;
    uint16_t uas_command_max_packet;
    uint16_t uas_status_max_packet;
    uint16_t uas_data_in_max_packet;
    uint16_t uas_data_out_max_packet;
    uint8_t uas_command_max_burst;
    uint8_t uas_status_max_burst;
    uint8_t uas_data_in_max_burst;
    uint8_t uas_data_out_max_burst;
    uint8_t uas_status_max_streams;
    uint8_t uas_data_in_max_streams;
    uint8_t uas_data_out_max_streams;
    uint32_t uas_command_enqueue;
    uint32_t uas_status_enqueue;
    uint32_t uas_data_in_enqueue;
    uint32_t uas_data_out_enqueue;
    uint8_t uas_command_cycle;
    uint8_t uas_status_cycle;
    uint8_t uas_data_in_cycle;
    uint8_t uas_data_out_cycle;
    bool uas_diagnostic_emitted;
} xhci_msc_transport_t;

static xhci_msc_transport_t
g_xhci_msc_transports[XHCI_MAX_SLOT_TABLE];

static bool g_xhci_usb_msc;
/* 仅供调试器读取的计数器；不按传输逐次写串口或磁盘日志。 */
static volatile uint64_t g_xhci_msc_direct_bytes;
static volatile uint64_t g_xhci_msc_bounce_bytes;

/* 仅在命令之间扩容。绝不复制正在使用的 dma_mapping_t：DMA Core 按地址注册映射，
 * 替换时必须显式转移其所有权。 */
static bool xhci_msc_reserve(xhci_state_t *state, xhci_dma_region_t *buffer,
                              uint32_t length) {
    if (length == 0U || length > XHCI_MSC_MAX_TRANSFER) return false;
    if (buffer->cpu != 0 && (uint64_t)buffer->page_count * PAGE_SIZE >= length)
        return true;
    xhci_dma_region_t replacement = {0};
    uint32_t pages = (length + PAGE_SIZE - 1U) / PAGE_SIZE;
    if (!xhci_alloc_dma_region(state, &replacement, pages)) return false;
    if (!xhci_free_dma_region(buffer)) {
        (void)xhci_free_dma_region(&replacement);
        return false;
    }
    if (dma_mapping_move(&buffer->mapping, &replacement.mapping) != K_OK) {
        (void)xhci_free_dma_region(&replacement);
        return false;
    }
    buffer->head = replacement.head;
    buffer->pages = replacement.pages;
    buffer->page_count = replacement.page_count;
    buffer->allocation_order = replacement.allocation_order;
    buffer->cpu = replacement.cpu;
    return true;
}

static bool xhci_msc_control_no_data(xhci_state_t *state,
                                     xhci_device_context_t *device,
                                     uint8_t request_type,
                                     uint8_t request,
                                     uint16_t value,
                                     uint16_t index) {
    uint8_t setup[8] = {
        request_type, request,
        (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)index, (uint8_t)(index >> 8),
        0U, 0U
    };
    return xhci_submit_control_transfer_device(
        state, device, setup, 0U, false);
}

static bool xhci_msc_get_interface(xhci_state_t *state,
                                   xhci_device_context_t *device,
                                   uint8_t interface_number,
                                   uint8_t *alternate) {
    static const uint8_t request[8] = {
        0x81U, 0x0AU, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U
    };
    uint8_t setup[8];

    if (state == 0 || device == 0 || alternate == 0) return false;
    for (uint32_t index = 0U; index < sizeof(setup); ++index)
        setup[index] = request[index];
    setup[4] = interface_number;
    if (!xhci_submit_control_transfer_device(state, device, setup, 1U, true))
        return false;
    *alternate = ((const uint8_t *)device->descriptor_buffer.cpu)[0];
    return true;
}

void xhci_msc_transport_release(uint8_t slot) {
    if (slot == 0U) return;
    xhci_msc_transport_t *transport = &g_xhci_msc_transports[slot];

    if (transport->configured || usb_msc_present(slot)) {
        usb_msc_detach(slot);
    }
    if (dma_mapping_active(&transport->direct_mapping)) {
        /* 解映射失败后仍保留已注册映射及其固定引用。 */
        realtest_mark("USB_MSC_DMA_MAPPING_QUARANTINED");
        return;
    }
    (void)xhci_free_dma_region(&transport->bulk_in_ring);
    (void)xhci_free_dma_region(&transport->bulk_out_ring);
    (void)xhci_free_dma_region(&transport->data_buffer);
    (void)xhci_free_dma_region(&transport->uas_command_ring);
    (void)xhci_free_dma_region(&transport->uas_status_ring);
    (void)xhci_free_dma_region(&transport->uas_data_in_ring);
    (void)xhci_free_dma_region(&transport->uas_data_out_ring);
    (void)xhci_free_dma_region(&transport->uas_status_stream_ring);
    (void)xhci_free_dma_region(&transport->uas_data_in_stream_ring);
    (void)xhci_free_dma_region(&transport->uas_data_out_stream_ring);
    (void)xhci_free_dma_region(&transport->uas_command_buffer);
    (void)xhci_free_dma_region(&transport->uas_status_buffer);
    (void)xhci_free_dma_region(&transport->uas_data_buffer);
    (void)xhci_free_dma_region(&transport->uas_status_streams);
    (void)xhci_free_dma_region(&transport->uas_data_in_streams);
    (void)xhci_free_dma_region(&transport->uas_data_out_streams);

    for (size_t i = 0U; i < sizeof(*transport); ++i) {
        ((uint8_t *)transport)[i] = 0U;
    }

    g_xhci_usb_msc = false;
    for (uint32_t index = 1U; index < XHCI_MAX_SLOT_TABLE; ++index) {
        if (g_xhci_msc_transports[index].configured) {
            g_xhci_usb_msc = true;
            break;
        }
    }
}

static uint32_t xhci_msc_endpoint_id(uint8_t endpoint, bool direction_in) {
    return (uint32_t)endpoint * 2U + (direction_in ? 1U : 0U);
}

static bool xhci_msc_ring_valid(const xhci_dma_region_t *ring) {
    return ring != 0 && ring->cpu != 0 &&
           ring->page_count == XHCI_RING_SEGMENT_COUNT &&
           ring->mapping.segment_count == 1U &&
           ring->mapping.segments[0].length >=
               (uint64_t)XHCI_RING_SEGMENT_COUNT * PAGE_SIZE;
}

static bool xhci_msc_init_ring(xhci_dma_region_t *ring) {
    xhci_trb_t *trbs;
    uint64_t base;

    if (!xhci_msc_ring_valid(ring)) return false;
    trbs = (xhci_trb_t *)ring->cpu;
    base = xhci_dma_address(&ring->mapping);
    for (uint32_t index = 0U;
         index < XHCI_RING_TOTAL_TRB_COUNT; ++index) {
        trbs[index].parameter = 0U;
        trbs[index].status = 0U;
        trbs[index].control = 0U;
    }
    for (uint32_t segment = 0U; segment < XHCI_RING_SEGMENT_COUNT;
         ++segment) {
        xhci_trb_t *link = trbs +
            segment * XHCI_RING_TRB_COUNT + XHCI_RING_TRB_COUNT - 1U;
        uint32_t next_segment =
            (segment + 1U) % XHCI_RING_SEGMENT_COUNT;
        link->parameter = base + (uint64_t)next_segment * PAGE_SIZE;
        link->control = XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT;
        if (segment == XHCI_RING_SEGMENT_COUNT - 1U)
            link->control |= XHCI_TRB_LINK_TOGGLE_CYCLE;
    }
    dma_sync_for_device(&ring->mapping);
    return true;
}

static bool xhci_msc_ring_advance(xhci_dma_region_t *ring,
                                  uint32_t *enqueue, uint8_t *cycle) {
    xhci_trb_t *trbs;
    uint32_t segment;
    uint32_t offset;
    xhci_trb_t *link;

    if (!xhci_msc_ring_valid(ring) || enqueue == 0 || cycle == 0 ||
        *enqueue >= XHCI_RING_TOTAL_TRB_COUNT) return false;
    segment = *enqueue / XHCI_RING_TRB_COUNT;
    offset = *enqueue % XHCI_RING_TRB_COUNT;
    if (offset == XHCI_RING_TRB_COUNT - 1U) return false;
    if (offset != XHCI_RING_TRB_COUNT - 2U) {
        *enqueue += 1U;
        return true;
    }

    trbs = (xhci_trb_t *)ring->cpu;
    link = trbs + segment * XHCI_RING_TRB_COUNT +
           XHCI_RING_TRB_COUNT - 1U;
    link->control = (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) | *cycle;
    if (segment == XHCI_RING_SEGMENT_COUNT - 1U) {
        link->control |= XHCI_TRB_LINK_TOGGLE_CYCLE;
        *enqueue = 0U;
        *cycle ^= 1U;
    } else {
        *enqueue = (segment + 1U) * XHCI_RING_TRB_COUNT;
    }
    return true;
}

/* 一个 TD 可以跨越物理上不连续的页面，但 Normal TRB 不得跨越 64 KiB DMA 边界。
 * 仅最后一个 TRB 请求完成通知。 */
static bool xhci_msc_queue_data(xhci_dma_region_t *ring, uint32_t *enqueue,
                               uint8_t *cycle, dma_mapping_t *mapping,
                               uint32_t offset, uint32_t length,
                               uint16_t max_packet, bool direction_in,
                               uint64_t *last_trb) {
    if (!xhci_msc_ring_valid(ring) || enqueue == 0 || cycle == 0 ||
        mapping == 0 || mapping->segments == 0 || length == 0U ||
        length > XHCI_MSC_MAX_TRANSFER || max_packet == 0U ||
        *enqueue >= XHCI_RING_TOTAL_TRB_COUNT ||
        *enqueue % XHCI_RING_TRB_COUNT == XHCI_RING_TRB_COUNT - 1U)
        return false;
    uint32_t pieces = 0U;
    uint32_t first_index = *enqueue;
    uint8_t first_cycle = *cycle;
    uint64_t skip = offset;
    uint32_t remaining = length;
    /* 修改环之前先预检：映射字节数和槽位数必须足够。 */
    for (unsigned pass = 0U; pass < 2U; ++pass) {
        skip = offset; remaining = length;
        for (uint32_t s = 0U; s < mapping->segment_count && remaining != 0U; ++s) {
            dma_segment_t *segment = &mapping->segments[s];
            if (skip >= segment->length) { skip -= segment->length; continue; }
            uint64_t address = segment->addr.value + skip;
            uint64_t available = segment->length - skip;
            skip = 0U;
            while (available != 0U && remaining != 0U) {
                uint32_t chunk = 0x10000U - (uint32_t)(address & 0xffffU);
                if (chunk > available) chunk = (uint32_t)available;
                if (chunk > remaining) chunk = remaining;
                if (pass == 0U) {
                    if (++pieces >= XHCI_RING_SEGMENT_COUNT * (XHCI_RING_TRB_COUNT - 1U))
                        return false;
                } else {
                    xhci_trb_t *trbs = ring->cpu;
                    uint32_t index = *enqueue;
                    uint32_t packets = (remaining - chunk + max_packet - 1U) / max_packet;
                    if (packets > 31U) packets = 31U;
                    trbs[index].parameter = address;
                    trbs[index].status = chunk | (packets << 17);
                    bool more = remaining != chunk;
                    trbs[index].control = (XHCI_NORMAL_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
                        (index == first_index ? (first_cycle ^ 1U) : *cycle) |
                        (more ? XHCI_TRB_CHAIN : XHCI_TRB_INTERRUPT_ON_COMPLETION) |
                        (direction_in ? XHCI_TRB_INTERRUPT_ON_SHORT_PACKET : 0U);
                    if (last_trb != 0) *last_trb = xhci_dma_address(&ring->mapping) +
                                                  (uint64_t)index * sizeof(xhci_trb_t);
                    if (!xhci_msc_ring_advance(ring, enqueue, cycle)) return false;
                    if (index % XHCI_RING_TRB_COUNT == XHCI_RING_TRB_COUNT - 2U && more)
                        trbs[index + 1U].control |= XHCI_TRB_CHAIN;
                }
                remaining -= chunk; available -= chunk; address += chunk;
            }
        }
        if (remaining != 0U) return false;
    }
    /* 完整链构造完成后才发布第一个 cycle。 */
    dma_wmb();
    xhci_trb_t *trbs = ring->cpu;
    trbs[first_index].control = (trbs[first_index].control & ~XHCI_TRB_CYCLE) | first_cycle;
    return true;
}

static bool xhci_msc_direct_map(xhci_state_t *state,
                               xhci_msc_transport_t *transport, void *buffer,
                               uint32_t length, bool direction_in) {
    uintptr_t start = (uintptr_t)buffer;
    if (length < PAGE_SIZE || length > XHCI_MSC_MAX_TRANSFER ||
        start < X86_64_DIRECT_MAP_BASE || start > UINTPTR_MAX - length ||
        dma_mapping_active(&transport->direct_mapping)) return false;
    uint32_t offset = (uint32_t)(start & (PAGE_SIZE - 1U));
    uint32_t count = (offset + length + PAGE_SIZE - 1U) / PAGE_SIZE;
    page_t **pages = kmalloc((size_t)count * sizeof(*pages), 0U);
    if (pages == 0) return false;
    bool ok = true;
    paddr_t root = x86_current_root_table();
    for (uint32_t i = 0U; i < count; ++i) {
        paddr_t physical;
        uint64_t flags;
        if (x86_translate_page(root, (start & ~(uintptr_t)(PAGE_SIZE - 1U)) +
                               (uint64_t)i * PAGE_SIZE, &physical, &flags) != K_OK ||
            (direction_in && !x86_page_entry_writable(flags)) ||
            (pages[i] = phys_to_page(paddr_make(physical.value & ~(uint64_t)(PAGE_SIZE - 1U)))) == 0) {
            ok = false; break;
        }
    }
    if (ok) ok = dma_map_pages((device_t *)&state->pci->device, pages, count,
        direction_in ? DMA_FROM_DEVICE : DMA_TO_DEVICE, &transport->direct_mapping) == K_OK;
    kfree(pages);
    if (ok) { transport->direct_cpu = buffer; transport->direct_offset = offset; }
    return ok;
}

static bool xhci_msc_direct_finish(xhci_state_t *state,
                                  xhci_msc_transport_t *transport,
                                  uint8_t endpoint, bool complete) {
    if (!dma_mapping_active(&transport->direct_mapping)) return true;
    /* 超时 TD 不得继续 DMA 到调用方重新使用的缓冲区。先停止端点，再解除固定；
     * 若无法停止，则禁用总线主控。 */
    if (!complete && !xhci_submit_command_ex(state, XHCI_STOP_ENDPOINT_TYPE,
                                             transport->slot, endpoint, 0U, 0)) {
        uint32_t command;
        if (pci_read_config32(state->pci, 4U, &command) != K_OK ||
            pci_write_config32(state->pci, 4U, (command & 0xffffU) & ~(1U << 2)) != K_OK ||
            pci_read_config32(state->pci, 4U, &command) != K_OK || (command & (1U << 2)) != 0U) {
            /* DMA 可能仍在运行时不能安全地归还所有权。 */
            kernel_serial_write_serial_only("OS_USB_DMA_QUIESCE_FAILED\r\n");
            printf("USB DMA could not be stopped; halting to protect memory.\n");
            for (;;) __asm__ volatile ("cli; hlt" : : : "memory");
        }
        state->initialized = false;
    }
    dma_sync_for_cpu(&transport->direct_mapping);
    if (dma_unmap_checked(&transport->direct_mapping) != K_OK) return false;
    transport->direct_cpu = 0;
    return true;
}

static bool xhci_msc_init_stream_contexts(
    xhci_dma_region_t *streams, const xhci_dma_region_t *ring,
    uint32_t context_count) {
    uint64_t *entries;
    uint64_t entry_count;
    if (streams == 0 || ring == 0 || streams->cpu == 0 || ring->cpu == 0 ||
        streams->page_count == 0U || context_count < 2U ||
        (uint64_t)context_count * XHCI_UAS_STREAM_CONTEXT_BYTES >
            (uint64_t)streams->page_count * PAGE_SIZE)
        return false;
    entries = (uint64_t *)streams->cpu;
    entry_count = (uint64_t)streams->page_count * PAGE_SIZE / sizeof(uint64_t);
    for (uint64_t index = 0U; index < entry_count; ++index)
        entries[index] = 0U;
    /* Stream 0 保留不用。Stream 1 从 DCS=1 的首个 TRB 开始。 */
    entries[XHCI_UAS_STREAM_ID * 2U] =
        xhci_dma_address(&ring->mapping) | 3ULL;
    dma_sync_for_device(&streams->mapping);
    return true;
}

static bool xhci_msc_select_stream_exponent(const xhci_state_t *state,
                                            uint8_t stream_depth,
                                            uint8_t *exponent,
                                            uint32_t *context_count) {
    uint32_t device_stream_ids;
    uint32_t required_contexts;
    uint32_t selected_contexts = 2U;

    if (state == 0 || exponent == 0 || context_count == 0 ||
        stream_depth == 0U ||
        state->max_primary_stream_array_size < 4U) {
        return false;
    }
    device_stream_ids = stream_depth >= 8U ? XHCI_UAS_REQUESTED_STREAM_IDS :
                        1U << stream_depth;
    if (device_stream_ids > XHCI_UAS_REQUESTED_STREAM_IDS)
        device_stream_ids = XHCI_UAS_REQUESTED_STREAM_IDS;
    required_contexts = device_stream_ids + 1U;
    while (selected_contexts < required_contexts) selected_contexts <<= 1U;
    if (selected_contexts > state->max_primary_stream_array_size)
        return false;

    *exponent = 0U;
    while ((1U << (*exponent + 1U)) < selected_contexts) ++*exponent;
    *context_count = selected_contexts;
    return true;
}

static uint8_t xhci_msc_uas_stream_depth(const xhci_msc_transport_t *transport) {
    uint8_t depth;
    if (transport == 0) return 0U;
    depth = transport->uas_status_max_streams;
    if (transport->uas_data_in_max_streams < depth)
        depth = transport->uas_data_in_max_streams;
    if (transport->uas_data_out_max_streams < depth)
        depth = transport->uas_data_out_max_streams;
    return depth;
}

static uint16_t xhci_uas_iu_tag(const uint8_t *iu) {
    if (iu == 0) return 0U;
    return ((uint16_t)iu[2] << 8) | iu[3];
}

static void xhci_msc_set_uas_endpoint(xhci_state_t *state,
                                      uint32_t *endpoint,
                                      uint8_t type,
                                      uint16_t max_packet,
                                      uint8_t max_burst,
                                      uint8_t max_primary_streams,
                                      const xhci_dma_region_t *ring,
                                      const xhci_dma_page_t *streams) {
    xhci_init_endpoint_context(
        state, endpoint, 0U, type, max_packet,
        &ring->mapping);
    xhci_endpoint_context_set_max_burst(endpoint, max_burst);
    if (streams != 0)
        xhci_endpoint_context_set_streams(
            endpoint, xhci_dma_address(&streams->mapping),
            max_primary_streams);
}

static void xhci_msc_set_input_slot(xhci_state_t *state,
                                    uint32_t *input_slot,
                                    uint32_t last_context) {
    const uint32_t *output_slot;
    if (state == 0 || input_slot == 0 || state->output_context.cpu == 0 ||
        last_context == 0U || last_context > 31U) return;
    dma_sync_for_cpu(&state->output_context.mapping);
    output_slot = (const uint32_t *)state->output_context.cpu;
    for (uint32_t index = 0U; index < 4U; ++index)
        input_slot[index] = output_slot[index];
    input_slot[0] &= ~(0x1FU << 27);
    input_slot[0] |= last_context << 27;
}

static void xhci_msc_prepare_input_context(xhci_state_t *state,
                                           uint32_t last_context,
                                           uint32_t drop_flags,
                                           uint32_t add_flags) {
    uint32_t *input;
    uint32_t *input_slot;

    if (state == 0 || state->input_context.cpu == 0) return;
    input = (uint32_t *)state->input_context.cpu;
    xhci_device_context_clear(input, PAGE_SIZE);
    input_slot = (uint32_t *)((uint8_t *)input + state->context_size);
    xhci_msc_set_input_slot(state, input_slot, last_context);
    input[0] = drop_flags;
    input[1] = add_flags | 1U;
}

static bool xhci_msc_copy_output_endpoint(xhci_state_t *state,
                                          uint32_t *endpoint,
                                          uint32_t endpoint_id) {
    const uint32_t *output;
    uint32_t context_words;

    if (state == 0 || endpoint == 0 || state->output_context.cpu == 0 ||
        endpoint_id == 0U || endpoint_id > 31U ||
        state->context_size == 0U ||
        (state->context_size % sizeof(uint32_t)) != 0U) {
        return false;
    }
    dma_sync_for_cpu(&state->output_context.mapping);
    context_words = state->context_size / sizeof(uint32_t);
    output = (const uint32_t *)state->output_context.cpu +
             context_words * endpoint_id;
    for (uint32_t index = 0U; index < context_words; ++index)
        endpoint[index] = output[index];
    return true;
}

static uint32_t xhci_msc_collect_uas_endpoints(
    const xhci_msc_transport_t *transport,
    uint8_t endpoints[4], uint16_t stream_ids[4]) {
    const uint8_t endpoint_numbers[4] = {
        transport != 0 ? transport->uas_command_endpoint : 0U,
        transport != 0 ? transport->uas_status_endpoint : 0U,
        transport != 0 ? transport->uas_data_in_endpoint : 0U,
        transport != 0 ? transport->uas_data_out_endpoint : 0U,
    };
    const bool directions[4] = {false, true, true, false};
    uint32_t count = 0U;

    if (transport == 0 || endpoints == 0 || stream_ids == 0 ||
        !transport->configured || !transport->uas) return 0U;
    for (uint32_t index = 0U; index < 4U; ++index) {
        uint32_t endpoint = xhci_msc_endpoint_id(
            endpoint_numbers[index], directions[index]);
        if (endpoint == 0U || endpoint > 31U) return 0U;
        bool duplicate = false;
        for (uint32_t previous = 0U; previous < count; ++previous) {
            if (endpoints[previous] == endpoint) duplicate = true;
        }
        if (duplicate) continue;
        endpoints[count] = (uint8_t)endpoint;
        stream_ids[count] = transport->uas_streams && index != 0U ?
                            XHCI_UAS_STREAM_ID : 0U;
        ++count;
    }
    return count;
}

static bool xhci_stop_uas_transport(xhci_state_t *state,
                                    const xhci_msc_transport_t *transport) {
    uint8_t endpoints[4];
    uint16_t stream_ids[4];
    uint32_t count = xhci_msc_collect_uas_endpoints(
        transport, endpoints, stream_ids);
    bool ok = count != 0U;

    for (uint32_t index = 0U; index < count; ++index) {
        if (!xhci_submit_command_ex(state, XHCI_STOP_ENDPOINT_TYPE,
                                    transport->slot, endpoints[index],
                                    stream_ids[index], 0)) {
            ok = false;
        }
    }
    for (uint32_t index = 0U; index < count; ++index) {
        if (!xhci_drop_endpoint_transfer_events(
                state, transport->slot, endpoints[index])) {
            ok = false;
        }
    }
    return ok;
}

static bool xhci_drop_uas_endpoint_contexts(
    xhci_state_t *state, const xhci_msc_transport_t *transport) {
    uint8_t endpoints[4];
    uint16_t stream_ids[4];
    uint32_t count = xhci_msc_collect_uas_endpoints(
        transport, endpoints, stream_ids);
    uint32_t drop_mask = 0U;
    uint32_t max_id = 0U;
    uint32_t *input;
    uint32_t *input_slot;

    (void)stream_ids;
    if (count == 0U || state == 0 || state->input_context.cpu == 0) return false;
    for (uint32_t index = 0U; index < count; ++index) {
        drop_mask |= 1U << endpoints[index];
        if (endpoints[index] > max_id) max_id = endpoints[index];
    }
    input = (uint32_t *)state->input_context.cpu;
    input_slot = (uint32_t *)((uint8_t *)input + state->context_size);
    xhci_msc_set_input_slot(state, input_slot, max_id);
    input[0] = drop_mask;
    input[1] = 0U;
    dma_sync_for_device(&state->input_context.mapping);
    return xhci_submit_command(
        state, XHCI_CONFIGURE_ENDPOINT_TYPE, transport->slot,
        xhci_dma_address(&state->input_context.mapping), 0);
}

static bool xhci_configure_uas_endpoints(xhci_state_t *state) {
    xhci_msc_transport_t *transport;
    uint32_t command_id;
    uint32_t status_id;
    uint32_t data_in_id;
    uint32_t data_out_id;
    uint32_t ids[4];
    uint32_t max_id;
    uint32_t *input;
    uint32_t *control;
    uint32_t *status_context;
    uint32_t *data_in_context;
    uint32_t *data_out_context;
    uint8_t slot;
    uint8_t stream_depth;
    uint8_t primary_stream_exponent;
    uint8_t uas_alternate;
    uint32_t stream_flags;
    uint32_t stream_context_count = 0U;
    uint32_t stream_context_pages = 0U;

    if (state == 0 || state->device_slot == 0U ||
        !state->msc_uas_present ||
        state->msc_uas_command_endpoint == 0U ||
        state->msc_uas_status_endpoint == 0U ||
        state->msc_uas_data_in_endpoint == 0U ||
        state->msc_uas_data_out_endpoint == 0U ||
        state->msc_uas_command_max_packet == 0U ||
        state->msc_uas_status_max_packet == 0U ||
        state->msc_uas_data_in_max_packet == 0U ||
        state->msc_uas_data_out_max_packet == 0U) {
        return false;
    }
    if ((state->msc_uas_command_address & 0x80U) != 0U ||
        (state->msc_uas_status_address & 0x80U) == 0U ||
        (state->msc_uas_data_in_address & 0x80U) == 0U ||
        (state->msc_uas_data_out_address & 0x80U) != 0U) {
        realtest_mark("USB_MSC_UAS_DIRECTION_INVALID");
        return false;
    }

    command_id = xhci_msc_endpoint_id(
        state->msc_uas_command_endpoint, false);
    status_id = xhci_msc_endpoint_id(
        state->msc_uas_status_endpoint, true);
    data_in_id = xhci_msc_endpoint_id(
        state->msc_uas_data_in_endpoint, true);
    data_out_id = xhci_msc_endpoint_id(
        state->msc_uas_data_out_endpoint, false);
    ids[0] = command_id;
    ids[1] = status_id;
    ids[2] = data_in_id;
    ids[3] = data_out_id;
    max_id = command_id;
    for (uint32_t index = 0U; index < 4U; ++index) {
        if (ids[index] == 0U || ids[index] > 31U) return false;
        if (ids[index] > max_id) max_id = ids[index];
        for (uint32_t previous = 0U; previous < index; ++previous)
            if (ids[index] == ids[previous]) return false;
    }
    realtest_mark_number("USB_MSC_UAS_COMMAND_ADDRESS",
                                state->msc_uas_command_address);
    realtest_mark_number("USB_MSC_UAS_STATUS_ADDRESS",
                                state->msc_uas_status_address);
    realtest_mark_number("USB_MSC_UAS_DATA_IN_ADDRESS",
                                state->msc_uas_data_in_address);
    realtest_mark_number("USB_MSC_UAS_DATA_OUT_ADDRESS",
                                state->msc_uas_data_out_address);
    realtest_mark_number("USB_MSC_UAS_COMMAND_ID", command_id);
    realtest_mark_number("USB_MSC_UAS_STATUS_ID", status_id);
    realtest_mark_number("USB_MSC_UAS_DATA_IN_ID", data_in_id);
    realtest_mark_number("USB_MSC_UAS_DATA_OUT_ID", data_out_id);
    slot = state->device_slot;
    uas_alternate = state->msc_uas_alternate;
    kernel_serial_printf_serial_only(
        "OS_UAS_CONFIG_BEGIN SLOT=%u SPEED=%u ALT=%u\r\n",
        slot, state->device_speed, uas_alternate);
    xhci_msc_transport_release(slot);
    transport = &g_xhci_msc_transports[slot];
    transport->uas = true;
    transport->slot = slot;
    transport->interface_number = state->msc_uas_interface;
    transport->uas_command_endpoint = state->msc_uas_command_endpoint;
    transport->uas_status_endpoint = state->msc_uas_status_endpoint;
    transport->uas_data_in_endpoint = state->msc_uas_data_in_endpoint;
    transport->uas_data_out_endpoint = state->msc_uas_data_out_endpoint;
    transport->uas_command_max_packet = state->msc_uas_command_max_packet;
    transport->uas_status_max_packet = state->msc_uas_status_max_packet;
    transport->uas_data_in_max_packet = state->msc_uas_data_in_max_packet;
    transport->uas_data_out_max_packet = state->msc_uas_data_out_max_packet;
    transport->uas_command_max_burst = state->msc_uas_command_max_burst;
    transport->uas_status_max_burst = state->msc_uas_status_max_burst;
    transport->uas_data_in_max_burst = state->msc_uas_data_in_max_burst;
    transport->uas_data_out_max_burst = state->msc_uas_data_out_max_burst;
    transport->uas_status_max_streams = state->msc_uas_status_max_streams;
    transport->uas_data_in_max_streams = state->msc_uas_data_in_max_streams;
    transport->uas_data_out_max_streams = state->msc_uas_data_out_max_streams;
    realtest_mark_number("USB_MSC_UAS_COMMAND_BURST",
                                transport->uas_command_max_burst);
    realtest_mark_number("USB_MSC_UAS_STATUS_BURST",
                                transport->uas_status_max_burst);
    realtest_mark_number("USB_MSC_UAS_DATA_IN_BURST",
                                transport->uas_data_in_max_burst);
    realtest_mark_number("USB_MSC_UAS_DATA_OUT_BURST",
                                transport->uas_data_out_max_burst);
    /* USB 2.x UAS 不使用 xHCI 流。SuperSpeed UAS 会在伴随描述符声明了可用深度时使用流。 */
    stream_depth = xhci_msc_uas_stream_depth(transport);
    transport->uas_streams = state->device_speed >= 4U &&
                             state->max_primary_stream_array_size >= 4U;
    if (state->device_speed >= 4U &&
        state->max_primary_stream_array_size < 4U) {
        realtest_mark_number("USB_MSC_UAS_HOST_STREAMS_UNSUPPORTED",
                                    state->max_primary_stream_array_size);
        xhci_msc_transport_release(slot);
        return false;
    }
    if (transport->uas_streams && stream_depth == 0U) {
        realtest_mark_number("USB_MSC_UAS_STREAM_DEPTH_INVALID",
                                    stream_depth);
        xhci_msc_transport_release(slot);
        return false;
    }
    primary_stream_exponent = 0U;
    if (transport->uas_streams &&
        !xhci_msc_select_stream_exponent(state, stream_depth,
                                         &primary_stream_exponent,
                                         &stream_context_count)) {
        realtest_mark_number("USB_MSC_UAS_STREAM_DEPTH_INVALID",
                                    stream_depth);
        xhci_msc_transport_release(slot);
        return false;
    }
    if (transport->uas_streams) {
        stream_context_pages =
            (stream_context_count * XHCI_UAS_STREAM_CONTEXT_BYTES +
             PAGE_SIZE - 1U) / PAGE_SIZE;
        if (stream_context_pages == 0U) {
            xhci_msc_transport_release(slot);
            return false;
        }
    }
    realtest_mark_number("USB_MSC_UAS_STREAMS",
                                transport->uas_streams ? 1U : 0U);
    realtest_mark_number("USB_MSC_UAS_STREAM_DEPTH", stream_depth);
    realtest_mark_number("USB_MSC_UAS_PRIMARY_STREAM_EXPONENT",
                                primary_stream_exponent);
    realtest_mark_number("USB_MSC_UAS_STREAM_CONTEXT_COUNT",
                                stream_context_count);

    if (!xhci_alloc_dma_region(state, &transport->uas_command_ring,
                               XHCI_RING_SEGMENT_COUNT) ||
        !xhci_alloc_dma_region(state, &transport->uas_status_ring,
                               XHCI_RING_SEGMENT_COUNT) ||
        !xhci_alloc_dma_region(state, &transport->uas_data_in_ring,
                               XHCI_RING_SEGMENT_COUNT) ||
        !xhci_alloc_dma_region(state, &transport->uas_data_out_ring,
                               XHCI_RING_SEGMENT_COUNT) ||
        !xhci_alloc_dma_region(state, &transport->uas_command_buffer, 1U) ||
        !xhci_alloc_dma_region(state, &transport->uas_status_buffer, 1U) ||
        !xhci_alloc_dma_region(state, &transport->uas_data_buffer, 1U)) {
        xhci_msc_transport_release(slot);
        return false;
    }

    if (transport->uas_streams &&
        (!xhci_alloc_dma_region(state, &transport->uas_status_stream_ring,
                                XHCI_RING_SEGMENT_COUNT) ||
         !xhci_alloc_dma_region(state, &transport->uas_data_in_stream_ring,
                                XHCI_RING_SEGMENT_COUNT) ||
         !xhci_alloc_dma_region(state, &transport->uas_data_out_stream_ring,
                                XHCI_RING_SEGMENT_COUNT) ||
         !xhci_alloc_dma_region(state, &transport->uas_status_streams,
                                stream_context_pages) ||
         !xhci_alloc_dma_region(state, &transport->uas_data_in_streams,
                                stream_context_pages) ||
         !xhci_alloc_dma_region(state, &transport->uas_data_out_streams,
                                stream_context_pages))) {
        xhci_msc_transport_release(slot);
        return false;
    }

    if (!xhci_msc_init_ring(&transport->uas_command_ring) ||
        !xhci_msc_init_ring(&transport->uas_status_ring) ||
        !xhci_msc_init_ring(&transport->uas_data_in_ring) ||
        !xhci_msc_init_ring(&transport->uas_data_out_ring)) {
        xhci_msc_transport_release(slot);
        return false;
    }
    if (transport->uas_streams &&
         (!xhci_msc_init_ring(&transport->uas_status_stream_ring) ||
         !xhci_msc_init_ring(&transport->uas_data_in_stream_ring) ||
         !xhci_msc_init_ring(&transport->uas_data_out_stream_ring) ||
         !xhci_msc_init_stream_contexts(&transport->uas_status_streams,
                                        &transport->uas_status_stream_ring,
                                        stream_context_count) ||
         !xhci_msc_init_stream_contexts(&transport->uas_data_in_streams,
                                        &transport->uas_data_in_stream_ring,
                                        stream_context_count) ||
         !xhci_msc_init_stream_contexts(&transport->uas_data_out_streams,
                                        &transport->uas_data_out_stream_ring,
                                        stream_context_count))) {
        xhci_msc_transport_release(slot);
        return false;
    }

    input = (uint32_t *)state->input_context.cpu;
    control = input;

    /* usb_set_interface() 会先安装常规端点上下文。随后 UAS 驱动只修改状态/数据上下文来分配流。两个
     * xHCI Configure Endpoint 事务必须分开执行；流上下文不能替代新端点。 */
    xhci_msc_prepare_input_context(state, max_id, 0U, 0U);
    for (uint32_t index = 0U; index < 4U; ++index)
        control[1] |= 1U << ids[index];
    realtest_mark_number("USB_MSC_UAS_BASE_ADD_FLAGS", control[1]);
    xhci_msc_set_uas_endpoint(
        state,
        (uint32_t *)((uint8_t *)input +
                     state->context_size * (command_id + 1U)),
        2U, transport->uas_command_max_packet,
        transport->uas_command_max_burst, 0U,
        &transport->uas_command_ring, 0);
    xhci_msc_set_uas_endpoint(
        state,
        (uint32_t *)((uint8_t *)input +
                     state->context_size * (status_id + 1U)),
        6U, transport->uas_status_max_packet,
        transport->uas_status_max_burst, 0U,
        &transport->uas_status_ring, 0);
    data_in_context =
        (uint32_t *)((uint8_t *)input +
                     state->context_size * (data_in_id + 1U));
    data_out_context =
        (uint32_t *)((uint8_t *)input +
                     state->context_size * (data_out_id + 1U));
    xhci_msc_set_uas_endpoint(
        state, data_in_context, 6U,
        transport->uas_data_in_max_packet,
        transport->uas_data_in_max_burst, 0U,
        &transport->uas_data_in_ring, 0);
    xhci_msc_set_uas_endpoint(
        state, data_out_context, 2U,
        transport->uas_data_out_max_packet,
        transport->uas_data_out_max_burst, 0U,
        &transport->uas_data_out_ring, 0);

    dma_sync_for_device(&state->input_context.mapping);
    dma_sync_for_device(&transport->uas_command_ring.mapping);
    dma_sync_for_device(&transport->uas_status_ring.mapping);
    dma_sync_for_device(&transport->uas_data_in_ring.mapping);
    dma_sync_for_device(&transport->uas_data_out_ring.mapping);
    dma_sync_for_device(&transport->uas_command_buffer.mapping);
    dma_sync_for_device(&transport->uas_status_buffer.mapping);
    dma_sync_for_device(&transport->uas_data_buffer.mapping);
    kernel_serial_printf_serial_only(
        "OS_UAS_CONFIGURE SLOT=%u PHASE=BASE MAX_ID=%u FLAGS=%u\r\n",
        slot, max_id, control[1]);
    if (!xhci_submit_command(state, XHCI_CONFIGURE_ENDPOINT_TYPE, slot,
                             xhci_dma_address(&state->input_context.mapping),
                             0)) {
        realtest_mark("USB_MSC_UAS_BASE_CONFIGURE_FAIL");
        xhci_msc_transport_release(slot);
        return false;
    }
    kernel_serial_printf_serial_only(
        "OS_UAS_CONFIGURE_OK SLOT=%u PHASE=BASE\r\n", slot);
    realtest_mark("USB_MSC_UAS_BASE_CONFIGURED");
    /* 端点上下文的激活由 xHCI 管理。先配置主机端点，再选择 USB UAS 备用设置。这与 xHCI HCD 执行
     * usb_set_interface() 时的顺序一致：先建立带宽/端点状态，再向设备发送 SET_INTERFACE。 */
    if (!xhci_msc_control_no_data(state, &state->device, 0x01U, 0x0BU,
                                  uas_alternate,
                                  transport->interface_number)) {
        realtest_mark("USB_MSC_UAS_SET_INTERFACE_FAIL");
        xhci_msc_transport_release(slot);
        return false;
    }
    realtest_mark("USB_MSC_UAS_SET_INTERFACE_OK");
    kernel_serial_printf_serial_only(
        "OS_UAS_SET_INTERFACE_OK SLOT=%u ALT=%u\r\n",
        slot, uas_alternate);

    if (transport->uas_streams) {
        stream_flags = (1U << status_id) |
                       (1U << data_in_id) |
                       (1U << data_out_id);
        xhci_msc_prepare_input_context(state, max_id,
                                       stream_flags, stream_flags);
        status_context =
            (uint32_t *)((uint8_t *)input +
                         state->context_size * (status_id + 1U));
        data_in_context =
            (uint32_t *)((uint8_t *)input +
                         state->context_size * (data_in_id + 1U));
        data_out_context =
            (uint32_t *)((uint8_t *)input +
                         state->context_size * (data_out_id + 1U));
        if (!xhci_msc_copy_output_endpoint(state, status_context, status_id) ||
            !xhci_msc_copy_output_endpoint(state, data_in_context, data_in_id) ||
            !xhci_msc_copy_output_endpoint(state, data_out_context,
                                            data_out_id)) {
            realtest_mark("USB_MSC_UAS_STREAM_CONTEXT_FAIL");
            xhci_msc_transport_release(slot);
            return false;
        }
        xhci_endpoint_context_set_streams(
            status_context,
            xhci_dma_address(&transport->uas_status_streams.mapping),
            primary_stream_exponent);
        xhci_endpoint_context_set_streams(
            data_in_context,
            xhci_dma_address(&transport->uas_data_in_streams.mapping),
            primary_stream_exponent);
        xhci_endpoint_context_set_streams(
            data_out_context,
            xhci_dma_address(&transport->uas_data_out_streams.mapping),
            primary_stream_exponent);
        dma_sync_for_device(&state->input_context.mapping);
        dma_sync_for_device(&transport->uas_status_stream_ring.mapping);
        dma_sync_for_device(&transport->uas_data_in_stream_ring.mapping);
        dma_sync_for_device(&transport->uas_data_out_stream_ring.mapping);
        dma_sync_for_device(&transport->uas_status_streams.mapping);
        dma_sync_for_device(&transport->uas_data_in_streams.mapping);
        dma_sync_for_device(&transport->uas_data_out_streams.mapping);
        realtest_mark_number("USB_MSC_UAS_STREAM_DROP_FLAGS",
                                    stream_flags);
        realtest_mark_number("USB_MSC_UAS_STREAM_ADD_FLAGS",
                                    stream_flags);
        kernel_serial_printf_serial_only(
            "OS_UAS_CONFIGURE SLOT=%u PHASE=STREAMS MAX_ID=%u "
            "FLAGS=%u STATUS_CTX=%u,%u,%u\r\n",
            slot, max_id, stream_flags, status_context[0],
            status_context[1], status_context[2]);
        if (!xhci_submit_command(
                state, XHCI_CONFIGURE_ENDPOINT_TYPE, slot,
                xhci_dma_address(&state->input_context.mapping), 0)) {
            realtest_mark("USB_MSC_UAS_STREAM_CONFIGURE_FAIL");
            xhci_msc_transport_release(slot);
            return false;
        }
        kernel_serial_printf_serial_only(
            "OS_UAS_CONFIGURE_OK SLOT=%u PHASE=STREAMS\r\n", slot);
        realtest_mark("USB_MSC_UAS_STREAMS_CONFIGURED");
    }

    status_context =
        (uint32_t *)((uint8_t *)input +
                     state->context_size * (status_id + 1U));

    realtest_mark_number("USB_MSC_UAS_STATUS_CTX0", status_context[0]);
    realtest_mark_number("USB_MSC_UAS_STATUS_CTX1", status_context[1]);
    realtest_mark_number("USB_MSC_UAS_STATUS_CTX2", status_context[2]);
    realtest_mark_number("USB_MSC_UAS_STATUS_CTX3", status_context[3]);
    realtest_mark_number("USB_MSC_UAS_STATUS_CTX4", status_context[4]);
    realtest_mark("USB_MSC_UAS_ENDPOINTS_CONFIGURED");

    dma_sync_for_cpu(&state->output_context.mapping);
    uint32_t *output_status =
        (uint32_t *)state->output_context.cpu +
        (uint32_t)(state->context_size / sizeof(uint32_t)) * status_id;
    realtest_mark_number("USB_MSC_UAS_STATUS_OUT0", output_status[0]);
    realtest_mark_number("USB_MSC_UAS_STATUS_OUT1", output_status[1]);
    realtest_mark_number("USB_MSC_UAS_STATUS_OUT2", output_status[2]);
    realtest_mark_number("USB_MSC_UAS_STATUS_OUT3", output_status[3]);

    uint8_t active_alternate = 0xFFU;
    if (!xhci_msc_get_interface(state, &state->device,
                                transport->interface_number,
                                &active_alternate)) {
        realtest_mark("USB_MSC_UAS_GET_INTERFACE_FAIL");
        xhci_msc_transport_release(slot);
        return false;
    }
    realtest_mark_number("USB_MSC_UAS_ACTIVE_ALT", active_alternate);
    if (active_alternate != uas_alternate) {
        realtest_mark("USB_MSC_UAS_ALT_MISMATCH");
        xhci_msc_transport_release(slot);
        return false;
    }

    transport->configured = true;
    transport->uas_command_enqueue = 0U;
    transport->uas_status_enqueue = 0U;
    transport->uas_data_in_enqueue = 0U;
    transport->uas_data_out_enqueue = 0U;
    transport->uas_command_cycle = 1U;
    transport->uas_status_cycle = 1U;
    transport->uas_data_in_cycle = 1U;
    transport->uas_data_out_cycle = 1U;
    state->msc_configured = true;
    g_xhci_usb_msc = true;
    realtest_mark("USB_MSC_UAS_CONFIGURED");
    return true;
}

static bool xhci_configure_msc_bot_endpoints(xhci_state_t *state) {
    uint32_t in_id;
    uint32_t out_id;
    uint32_t *input;
    uint32_t *control;
    uint32_t *input_slot;
    uint32_t *output_slot;
    uint8_t slot;
    xhci_msc_transport_t *existing;
    bool recovering_uas;

    if (state == 0 || state->device_slot == 0U ||
        state->msc_bulk_in_endpoint == 0U ||
        state->msc_bulk_out_endpoint == 0U ||
        state->msc_bulk_in_max_packet == 0U ||
        state->msc_bulk_out_max_packet == 0U) return false;

    slot = state->device_slot;
    existing = &g_xhci_msc_transports[slot];
    recovering_uas = existing->configured && existing->uas;
    if (recovering_uas && !xhci_stop_uas_transport(state, existing)) {
        realtest_mark("USB_MSC_UAS_FALLBACK_STOP_FAIL");
        return false;
    }

    /* 部分 UAS 设备将 BOT 作为备用设置 0，并在加载器交接期间保留固件选定的 UAS 设置。 */
    if (!xhci_msc_control_no_data(state, &state->device, 0x01U, 0x0BU, 0U,
                                  state->msc_interface)) {
        realtest_mark("USB_MSC_SET_INTERFACE_FAIL");
        return false;
    }
    realtest_mark("USB_MSC_SET_INTERFACE_OK");
    uint8_t active_alternate = 0xFFU;
    if (!xhci_msc_get_interface(state, &state->device, state->msc_interface,
                                &active_alternate)) {
        realtest_mark("USB_MSC_GET_INTERFACE_FAIL");
        return false;
    }
    realtest_mark_number("USB_MSC_INTERFACE_ALT", active_alternate);

    if (recovering_uas && !xhci_drop_uas_endpoint_contexts(state, existing)) {
        realtest_mark("USB_MSC_UAS_FALLBACK_DROP_FAIL");
        return false;
    }

    in_id = (uint32_t)state->msc_bulk_in_endpoint * 2U + 1U;
    out_id = (uint32_t)state->msc_bulk_out_endpoint * 2U;
    if (in_id > 31U || out_id > 31U || in_id == out_id) return false;

    xhci_msc_transport_release(slot);
    xhci_msc_transport_t *transport = &g_xhci_msc_transports[slot];

    if (!xhci_alloc_dma_region(state, &transport->bulk_in_ring,
                               XHCI_RING_SEGMENT_COUNT) ||
        !xhci_alloc_dma_region(state, &transport->bulk_out_ring,
                               XHCI_RING_SEGMENT_COUNT) ||
        !xhci_alloc_dma_region(state, &transport->data_buffer, 1U)) {
        xhci_msc_transport_release(slot);
        return false;
    }

    if (!xhci_msc_init_ring(&transport->bulk_in_ring) ||
        !xhci_msc_init_ring(&transport->bulk_out_ring)) {
        xhci_msc_transport_release(slot);
        return false;
    }

    input = (uint32_t *)state->input_context.cpu;
    control = input;
    input_slot = (uint32_t *)((uint8_t *)input + state->context_size);
    output_slot = (uint32_t *)state->output_context.cpu;
    for (uint32_t i = 0U; i < 4U; ++i) input_slot[i] = output_slot[i];

    uint32_t context_entries = in_id > out_id ? in_id : out_id;
    input_slot[0] &= ~(0x1FU << 27);
    input_slot[0] |= context_entries << 27;
    control[0] = 0U;
    control[1] = 1U | (1U << in_id) | (1U << out_id);

    xhci_init_endpoint_context(
        state,
        (uint32_t *)((uint8_t *)input + state->context_size * (in_id + 1U)),
        0U, 6U, state->msc_bulk_in_max_packet, &transport->bulk_in_ring.mapping);
    xhci_endpoint_context_set_max_burst(
        (uint32_t *)((uint8_t *)input + state->context_size * (in_id + 1U)),
        state->msc_bulk_in_max_burst);
    xhci_init_endpoint_context(
        state,
        (uint32_t *)((uint8_t *)input + state->context_size * (out_id + 1U)),
        0U, 2U, state->msc_bulk_out_max_packet, &transport->bulk_out_ring.mapping);
    xhci_endpoint_context_set_max_burst(
        (uint32_t *)((uint8_t *)input + state->context_size * (out_id + 1U)),
        state->msc_bulk_out_max_burst);
    realtest_mark_number("USB_MSC_IN_ENDPOINT", state->msc_bulk_in_endpoint);
    realtest_mark_number("USB_MSC_OUT_ENDPOINT", state->msc_bulk_out_endpoint);
    realtest_mark_number("USB_MSC_IN_BURST", state->msc_bulk_in_max_burst);
    realtest_mark_number("USB_MSC_OUT_BURST", state->msc_bulk_out_max_burst);

    dma_sync_for_device(&state->input_context.mapping);
    dma_sync_for_device(&transport->bulk_in_ring.mapping);
    dma_sync_for_device(&transport->bulk_out_ring.mapping);
    dma_sync_for_device(&transport->data_buffer.mapping);

    if (!xhci_submit_command(state, XHCI_CONFIGURE_ENDPOINT_TYPE, slot,
                             xhci_dma_address(&state->input_context.mapping), 0)) {
        xhci_msc_transport_release(slot);
        return false;
    }

    transport->configured = true;
    transport->slot = slot;
    transport->interface_number = state->msc_interface;
    transport->bulk_in_endpoint = state->msc_bulk_in_endpoint;
    transport->bulk_out_endpoint = state->msc_bulk_out_endpoint;
    transport->bulk_in_max_packet = state->msc_bulk_in_max_packet;
    transport->bulk_out_max_packet = state->msc_bulk_out_max_packet;
    transport->bulk_in_enqueue = 0U;
    transport->bulk_out_enqueue = 0U;
    transport->bulk_in_cycle = 1U;
    transport->bulk_out_cycle = 1U;
    state->msc_configured = true;
    g_xhci_usb_msc = true;
    /* Linux 仅在传输失败后才进入 BOT 复位恢复流程。刚选中的备用设置拥有干净端点，可直接接收首个 CBW；
     * 此处复位会触发设备恢复等待，还可能导致首个批量事务超时。 */
    realtest_mark("USB_MSC_BOT_CONFIGURED");
    return true;
}

bool xhci_configure_msc_endpoints(xhci_state_t *state) {
    if (!OS_XHCI_DIAGNOSTIC_BOT &&
        state != 0 && state->device_speed >= 3U &&
        state->msc_uas_present &&
        state->msc_uas_command_endpoint != 0U &&
        state->msc_uas_status_endpoint != 0U &&
        state->msc_uas_data_in_endpoint != 0U &&
        state->msc_uas_data_out_endpoint != 0U) {
        if (state->device_speed >= 4U &&
            state->max_primary_stream_array_size < 4U) {
            /* Linux 要求至少有四个主流上下文项才会启用 SuperSpeed UAS。设备支持 BOT 备用设置时，优先选择
             * BOT，避免存储设备无法使用。 */
            realtest_mark_number(
                "USB_MSC_UAS_HOST_STREAMS_UNSUPPORTED",
                state->max_primary_stream_array_size);
            if (state->msc_bulk_in_endpoint != 0U &&
                state->msc_bulk_out_endpoint != 0U &&
                state->msc_bulk_in_max_packet != 0U &&
                state->msc_bulk_out_max_packet != 0U) {
                realtest_mark("USB_MSC_UAS_BOT_FALLBACK");
                return xhci_configure_msc_bot_endpoints(state);
            }
            return false;
        }
        return xhci_configure_uas_endpoints(state);
    }

    if (OS_XHCI_DIAGNOSTIC_BOT && state != 0 && state->msc_uas_present)
        realtest_mark("USB_MSC_DIAGNOSTIC_BOT");
    return xhci_configure_msc_bot_endpoints(state);
}

static void xhci_msc_log_bulk_error(
    xhci_state_t *state, xhci_msc_transport_t *transport,
    xhci_dma_region_t *ring_page, uint32_t trb_index,
    uint32_t endpoint_id, const xhci_trb_t *event) {
    xhci_slot_device_t *slot_device;
    xhci_dma_page_t *output_page;
    const uint32_t *endpoint_context = 0;
    const xhci_trb_t *trb = 0;
    const uint32_t *data = 0;
    uint32_t context_words;
    uint8_t root_port = 0U;

    if (state == 0 || transport == 0 || event == 0) return;
    slot_device = xhci_topology_slot(transport->slot);
    output_page = slot_device != 0 && slot_device->used ?
        &slot_device->context.output_context : 0;
    context_words = state->context_size / sizeof(uint32_t);
    if (ring_page != 0 && xhci_msc_ring_valid(ring_page) &&
        trb_index < XHCI_RING_TOTAL_TRB_COUNT) {
        dma_sync_for_cpu(&ring_page->mapping);
        trb = (const xhci_trb_t *)ring_page->cpu + trb_index;
    }
    if (transport->data_buffer.cpu != 0) {
        dma_sync_for_cpu(&transport->data_buffer.mapping);
        data = (const uint32_t *)transport->data_buffer.cpu;
    }
    if (output_page != 0 && output_page->cpu != 0 &&
        endpoint_id <= 31U && context_words >= 5U) {
        dma_sync_for_cpu(&output_page->mapping);
        endpoint_context = (const uint32_t *)output_page->cpu +
                           context_words * endpoint_id;
    }
    if (slot_device != 0 && slot_device->used)
        root_port = slot_device->root_port;

    realtest_mark_number("USB_MSC_BULK_EVENT_ENDPOINT", endpoint_id);
    realtest_mark_number("USB_MSC_BULK_EVENT_CONTROL", event->control);
    realtest_mark_number("USB_MSC_BULK_EVENT_PTR_LO",
                                (uint32_t)event->parameter);
    realtest_mark_number("USB_MSC_BULK_EVENT_PTR_HIGH",
                                (uint32_t)(event->parameter >> 32));
    if (endpoint_context != 0) {
        realtest_mark_number("USB_MSC_BULK_FAIL_CTX0",
                                    endpoint_context[0]);
        realtest_mark_number("USB_MSC_BULK_FAIL_CTX1",
                                    endpoint_context[1]);
        realtest_mark_number("USB_MSC_BULK_FAIL_CTX2",
                                    endpoint_context[2]);
        realtest_mark_number("USB_MSC_BULK_FAIL_CTX3",
                                    endpoint_context[3]);
        realtest_mark_number("USB_MSC_BULK_FAIL_CTX4",
                                    endpoint_context[4]);
    }
    if (trb != 0) {
        uint64_t trb_dma = xhci_dma_address(&ring_page->mapping) +
                           (uint64_t)trb_index * sizeof(*trb);
        realtest_mark_number("USB_MSC_BULK_FAIL_RING_CTRL",
                                    trb->control);
        realtest_mark_number("USB_MSC_BULK_FAIL_RING_STATUS",
                                    trb->status);
        realtest_mark_number("USB_MSC_BULK_FAIL_RING_PARAM_LO",
                                    (uint32_t)trb->parameter);
        realtest_mark_number("USB_MSC_BULK_FAIL_RING_PARAM_HIGH",
                                    (uint32_t)(trb->parameter >> 32));
        realtest_mark_number("USB_MSC_BULK_FAIL_TRB_LO",
                                    (uint32_t)trb_dma);
        realtest_mark_number("USB_MSC_BULK_FAIL_TRB_HIGH",
                                    (uint32_t)(trb_dma >> 32));
    }
    realtest_mark_number("USB_MSC_BULK_FAIL_DATA_LO",
        (uint32_t)xhci_dma_address(&transport->data_buffer.mapping));
    realtest_mark_number("USB_MSC_BULK_FAIL_DATA_HIGH",
        (uint32_t)(xhci_dma_address(&transport->data_buffer.mapping) >> 32));
    if (data != 0) {
        realtest_mark_number("USB_MSC_BULK_FAIL_DATA0", data[0]);
        realtest_mark_number("USB_MSC_BULK_FAIL_DATA1", data[1]);
        realtest_mark_number("USB_MSC_BULK_FAIL_DATA2", data[2]);
        realtest_mark_number("USB_MSC_BULK_FAIL_DATA3", data[3]);
    }
    realtest_mark_number("USB_MSC_BULK_FAIL_USBSTS",
        xhci_controller_read32(state, state->operational_offset + 0x04U));
    if (root_port != 0U) {
        realtest_mark_number("USB_MSC_BULK_FAIL_PORTSC",
            xhci_controller_read32(
                state, state->operational_offset + 0x400U +
                       (uint32_t)(root_port - 1U) * 0x10U));
    }
}

static kstatus_t xhci_msc_transfer_prepared(
    xhci_state_t *state, xhci_msc_transport_t *transport,
    uint8_t endpoint, bool direction_in, void *buffer,
    uint32_t length, uint32_t *actual) {
    xhci_dma_region_t *ring_page;
    uint32_t *enqueue;
    uint8_t *cycle;
    uint32_t endpoint_id;
    bool direct = dma_mapping_active(&transport->direct_mapping);
    dma_mapping_t *data_mapping = direct ? &transport->direct_mapping :
                                          &transport->data_buffer.mapping;

    if (state == 0 || transport == 0 || !transport->configured ||
        buffer == 0 || length == 0U || length > XHCI_MSC_MAX_TRANSFER ||
        (!direct && (transport->data_buffer.cpu == 0 ||
         length > transport->data_buffer.page_count * PAGE_SIZE))) return K_EINVAL;

    if (direction_in) {
        if (endpoint != transport->bulk_in_endpoint) return K_EINVAL;
        endpoint_id = (uint32_t)endpoint * 2U + 1U;
        ring_page = &transport->bulk_in_ring;
        enqueue = &transport->bulk_in_enqueue;
        cycle = &transport->bulk_in_cycle;
    } else {
        if (endpoint != transport->bulk_out_endpoint) return K_EINVAL;
        endpoint_id = (uint32_t)endpoint * 2U;
        ring_page = &transport->bulk_out_ring;
        enqueue = &transport->bulk_out_enqueue;
        cycle = &transport->bulk_out_cycle;
    }

    if (endpoint_id > 31U || *enqueue >= XHCI_RING_TOTAL_TRB_COUNT ||
        (*enqueue % XHCI_RING_TRB_COUNT) == XHCI_RING_TRB_COUNT - 1U ||
        !xhci_msc_ring_valid(ring_page)) return K_EIO;

    if (transport->bulk_in_enqueue == 0U &&
        transport->bulk_out_enqueue == 0U) {
        xhci_slot_device_t *slot_device =
            xhci_topology_slot(transport->slot);
        uint8_t root_port = slot_device != 0 && slot_device->used ?
                            slot_device->root_port : 0U;
        uint32_t portsc = root_port != 0U ?
            xhci_controller_read32(
                state, state->operational_offset + XHCI_PORT_REGISTER_BASE +
                       (uint32_t)(root_port - 1U) *
                           XHCI_PORT_REGISTER_STRIDE) :
            UINT32_MAX;
        realtest_mark_number("USB_MSC_BULK_PORTSC_BEFORE", portsc);
        if (portsc == UINT32_MAX ||
            (portsc & XHCI_PORTSC_READY) != XHCI_PORTSC_READY) {
            realtest_mark_number("USB_MSC_BULK_FAIL_STAGE", 9U);
            return K_EDEVREMOVED;
        }
    }

    if (!direction_in && !direct) {
        for (uint32_t i = 0U; i < length; ++i)
            ((uint8_t *)transport->data_buffer.cpu)[i] =
                ((const uint8_t *)buffer)[i];
    } else if (!direct) {
        for (uint32_t i = 0U; i < length; ++i)
            ((uint8_t *)transport->data_buffer.cpu)[i] = 0U;
    }

    uint32_t index = *enqueue;
    if (!xhci_msc_queue_data(ring_page, enqueue, cycle, data_mapping,
             direct ? transport->direct_offset : 0U, length,
             direction_in ? transport->bulk_in_max_packet : transport->bulk_out_max_packet,
             direction_in, 0)) return K_EIO;

    dma_sync_for_device(&ring_page->mapping);
    dma_sync_for_device(data_mapping);
    dma_wmb();
    *(volatile uint32_t *)(state->mmio + state->doorbell_offset +
                           (uint32_t)transport->slot * sizeof(uint32_t)) =
        endpoint_id;
    __asm__ volatile ("mfence" : : : "memory");

    /* 慢速实体 USB 存储设备的大块顺序读取，超时机制不得依赖 CPU 速度对应的自旋次数。 */
    uint64_t deadline = xhci_controller_timeout_deadline(5000000000ULL);
    for (;;) {
        if (xhci_controller_timeout_reached(deadline)) break;
        xhci_trb_t event;
        if (!xhci_next_ring_event(state, &event)) {
            __asm__ volatile ("pause");
            continue;
        }

        uint32_t type = (event.control >> XHCI_TRB_TYPE_SHIFT) & 0x3FU;
        uint8_t event_slot = (uint8_t)(event.control >> XHCI_TRB_SLOT_SHIFT);
        uint32_t event_endpoint =
            (event.control >> XHCI_TRB_ENDPOINT_SHIFT) & 0x1FU;
        if (type != XHCI_TRANSFER_EVENT_TYPE ||
            event_slot != transport->slot || event_endpoint != endpoint_id) {
            if (!xhci_handle_hid_io_event(state, &event) &&
                !xhci_defer_event(state, &event)) {
                realtest_mark_number("USB_MSC_BULK_FAIL_STAGE", 5U);
                return K_EIO;
            }
            continue;
        }

        uint32_t completion = event.status >> XHCI_COMPLETION_SHIFT;
        uint32_t residual = event.status & 0x00FFFFFFU;
        if (completion != XHCI_COMPLETION_SUCCESS &&
            completion != XHCI_COMPLETION_SHORT_PACKET) {
            realtest_mark_number("USB_MSC_BULK_FAIL_STAGE", 6U);
            realtest_mark_number("USB_MSC_BULK_FAIL_CODE", completion);
            realtest_mark_number("USB_MSC_BULK_FAIL_STATUS", event.status);
            xhci_msc_log_bulk_error(state, transport, ring_page, index,
                                    endpoint_id, &event);
            return K_EIO;
        }
        if (residual > length) {
            realtest_mark_number("USB_MSC_BULK_FAIL_STAGE", 7U);
            realtest_mark_number("USB_MSC_BULK_FAIL_RESIDUAL", residual);
            return K_EIO;
        }

        uint32_t completed = length - residual;
        if (direction_in && !direct) {
            dma_sync_for_cpu(&transport->data_buffer.mapping);
            for (uint32_t i = 0U; i < completed; ++i)
                ((uint8_t *)buffer)[i] =
                    ((const uint8_t *)transport->data_buffer.cpu)[i];
        }
        if (actual != 0) *actual = completed;
        __atomic_add_fetch(direct ? &g_xhci_msc_direct_bytes : &g_xhci_msc_bounce_bytes,
                           completed, __ATOMIC_RELAXED);
        return K_OK;
    }
    realtest_mark_number("USB_MSC_BULK_FAIL_STAGE", 8U);
    return K_ETIMEDOUT;
}

static kstatus_t xhci_msc_transfer_locked(
    xhci_state_t *state, xhci_msc_transport_t *transport,
    uint8_t endpoint, bool direction_in, void *buffer,
    uint32_t length, uint32_t *actual) {
    if (state == 0 || transport == 0 || !transport->configured || buffer == 0 ||
        length == 0U || length > XHCI_MSC_MAX_TRANSFER) return K_EINVAL;
    if (dma_mapping_active(&transport->direct_mapping)) return K_EIO;
    bool direct = xhci_msc_direct_map(state, transport, buffer, length, direction_in);
    if (!direct && !xhci_msc_reserve(state, &transport->data_buffer, length)) return K_ENOMEM;
    kstatus_t status = xhci_msc_transfer_prepared(state, transport, endpoint,
                                                direction_in, buffer, length, actual);
    uint8_t endpoint_id = (uint8_t)((uint32_t)endpoint * 2U + (direction_in ? 1U : 0U));
    if (direct && !xhci_msc_direct_finish(state, transport, endpoint_id, status == K_OK))
        return K_EIO;
    return status;
}

static bool xhci_uas_queue_transfer(
    xhci_state_t *state,
    xhci_msc_transport_t *transport,
    xhci_dma_region_t *ring_page,
    uint32_t *enqueue,
    uint8_t *cycle,
    xhci_dma_region_t *buffer_page,
    const void *source,
    uint32_t length,
    uint8_t endpoint,
    bool direction_in,
    uint16_t stream_id,
    uint64_t *trb_address) {
    uint32_t endpoint_id;

    if (state == 0 || transport == 0 || ring_page == 0 || enqueue == 0 ||
        cycle == 0 || buffer_page == 0 || buffer_page->cpu == 0 ||
        length == 0U || length > XHCI_MSC_MAX_TRANSFER ||
        length > buffer_page->page_count * PAGE_SIZE || endpoint == 0U ||
        *enqueue >= XHCI_RING_TOTAL_TRB_COUNT ||
        (*enqueue % XHCI_RING_TRB_COUNT) == XHCI_RING_TRB_COUNT - 1U ||
        !xhci_msc_ring_valid(ring_page)) {
        kernel_serial_printf_serial_only(
            "OS_UAS_QUEUE_FAIL STAGE=1 EP=%u IN=%u STREAM=%u LEN=%u ENQ=%u RING=%u CPU=%u PAGES=%u SEGS=%u\r\n",
            endpoint, direction_in ? 1U : 0U, stream_id, length,
            enqueue != 0 ? *enqueue : 0U,
            ring_page != 0 && ring_page->cpu != 0 ? 1U : 0U,
            buffer_page != 0 && buffer_page->cpu != 0 ? 1U : 0U,
            ring_page != 0 ? ring_page->page_count : 0U,
            ring_page != 0 ? ring_page->mapping.segment_count : 0U);
        return false;
    }
    endpoint_id = xhci_msc_endpoint_id(endpoint, direction_in);
    if (endpoint_id > 31U) {
        kernel_serial_printf_serial_only(
            "OS_UAS_QUEUE_FAIL STAGE=2 EP=%u IN=%u STREAM=%u LEN=%u\r\n",
            endpoint, direction_in ? 1U : 0U, stream_id, length);
        return false;
    }

    bool direct = buffer_page == &transport->uas_data_buffer &&
                  dma_mapping_active(&transport->direct_mapping);
    dma_mapping_t *data_mapping = direct ? &transport->direct_mapping : &buffer_page->mapping;
    if (direction_in && !direct) {
        for (uint32_t offset = 0U; offset < length; ++offset)
            ((uint8_t *)buffer_page->cpu)[offset] = 0U;
    } else if (!direct && source != 0 && source != buffer_page->cpu) {
        for (uint32_t offset = 0U; offset < length; ++offset)
            ((uint8_t *)buffer_page->cpu)[offset] =
                ((const uint8_t *)source)[offset];
    }

    if (!xhci_msc_queue_data(ring_page, enqueue, cycle, data_mapping,
                             direct ? transport->direct_offset : 0U,
                             length, direction_in ? transport->uas_data_in_max_packet :
                                                    transport->uas_data_out_max_packet,
                             direction_in, trb_address)) {
        kernel_serial_printf_serial_only(
            "OS_UAS_QUEUE_FAIL STAGE=3 EP=%u IN=%u STREAM=%u LEN=%u ENQ=%u\r\n",
            endpoint, direction_in ? 1U : 0U, stream_id, length, *enqueue);
        return false;
    }

    dma_sync_for_device(&ring_page->mapping);
    dma_sync_for_device(data_mapping);
    dma_wmb();
    (void)stream_id;
    return true;
}

static void xhci_uas_ring_doorbell(const xhci_state_t *state,
                                   uint8_t slot,
                                   uint8_t endpoint,
                                   bool direction_in,
                                   uint16_t stream_id) {
    volatile uint32_t *doorbell;
    if (state == 0 || state->mmio == 0 || slot == 0U) return;
    doorbell = (volatile uint32_t *)(state->mmio + state->doorbell_offset +
                                     (uint32_t)slot * sizeof(uint32_t));
    *doorbell = (uint32_t)xhci_msc_endpoint_id(endpoint, direction_in) |
                ((uint32_t)stream_id << 16);
    __asm__ volatile ("mfence" : : : "memory");
    (void)*doorbell;
}

static kstatus_t xhci_uas_scsi_command_locked(
    xhci_state_t *state,
    xhci_msc_transport_t *transport,
    const uint8_t *command,
    uint8_t command_length,
    void *buffer,
    uint32_t length,
    bool direction_in,
    uint32_t *actual) {
    uint8_t *command_iu;
    uint8_t *status_iu;
    uint32_t command_id;
    uint32_t status_id;
    uint32_t data_id;
    uint64_t command_trb = 0U;
    uint64_t status_trb = 0U;
    uint64_t data_trb = 0U;
    uint16_t stream_id;
    xhci_dma_region_t *status_ring;
    uint32_t *status_enqueue;
    uint8_t *status_cycle;
    xhci_dma_region_t *data_ring;
    uint32_t *data_enqueue;
    uint8_t *data_cycle;
    bool command_done = false;
    bool status_done = false;
    bool data_done = length == 0U;
    bool data_submitted = false;
    bool emit_diagnostic;
    kstatus_t result = K_EIO;

    if (state == 0 || transport == 0 || !transport->configured ||
        !transport->uas || command == 0 || command_length == 0U ||
        command_length > 16U || (length != 0U && buffer == 0) ||
        length > XHCI_MSC_MAX_TRANSFER) {
        return K_EINVAL;
    }

    command_id = xhci_msc_endpoint_id(transport->uas_command_endpoint, false);
    status_id = xhci_msc_endpoint_id(transport->uas_status_endpoint, true);
    data_id = xhci_msc_endpoint_id(
        direction_in ? transport->uas_data_in_endpoint :
                       transport->uas_data_out_endpoint,
        direction_in);
    if (command_id > 31U || status_id > 31U ||
        (length != 0U && data_id > 31U)) {
        return K_EINVAL;
    }
    stream_id = transport->uas_streams ? XHCI_UAS_STREAM_ID : 0U;
    status_ring = transport->uas_streams ?
                  &transport->uas_status_stream_ring :
                  &transport->uas_status_ring;
    status_enqueue = &transport->uas_status_enqueue;
    status_cycle = &transport->uas_status_cycle;
    data_ring = direction_in ?
                (transport->uas_streams ?
                 &transport->uas_data_in_stream_ring :
                 &transport->uas_data_in_ring) :
                (transport->uas_streams ?
                 &transport->uas_data_out_stream_ring :
                 &transport->uas_data_out_ring);
    data_enqueue = direction_in ? &transport->uas_data_in_enqueue :
                                   &transport->uas_data_out_enqueue;
    data_cycle = direction_in ? &transport->uas_data_in_cycle :
                                 &transport->uas_data_out_cycle;
    emit_diagnostic = !transport->uas_diagnostic_emitted;
    transport->uas_diagnostic_emitted = true;
    realtest_mark_number("USB_MSC_UAS_STREAM_ID", stream_id);
    if (emit_diagnostic) {
        kernel_serial_printf_serial_only(
            "OS_UAS_SCSI_BEGIN SLOT=%u STREAM=%u CMD=%u STATUS=%u DATA=%u LEN=%u IN=%u\r\n",
            transport->slot, stream_id, command_id, status_id, data_id, length,
            direction_in ? 1U : 0U);
    }

    command_iu = (uint8_t *)transport->uas_command_buffer.cpu;
    status_iu = (uint8_t *)transport->uas_status_buffer.cpu;
    if (command_iu == 0 || status_iu == 0 || transport->uas_data_buffer.cpu == 0)
        return K_EIO;
    for (uint32_t offset = 0U; offset < XHCI_UAS_COMMAND_LENGTH; ++offset)
        command_iu[offset] = 0U;
    command_iu[0] = XHCI_UAS_COMMAND_IU;
    /* UAS 标签是供所有 IU 共用的 16 位大端字段。 */
    command_iu[2] = (uint8_t)(XHCI_UAS_TASK_TAG >> 8);
    command_iu[3] = (uint8_t)XHCI_UAS_TASK_TAG;
    command_iu[4] = 0U; /* UAS_SIMPLE_TAG：简单标签 */
    command_iu[5] = 0U;
    command_iu[6] = 0U; /* 16 字节 CDB 字段之后没有额外 CDB 字节 */
    command_iu[7] = 0U;
    for (uint32_t offset = 0U; offset < command_length; ++offset)
        command_iu[16U + offset] = command[offset];
    for (uint32_t offset = 0U; offset < XHCI_UAS_STATUS_LENGTH; ++offset)
        status_iu[offset] = 0U;

    /* UAS 按状态、数据流、命令的顺序提交。 */
    if (!xhci_uas_queue_transfer(
            state, transport, status_ring, status_enqueue, status_cycle,
            &transport->uas_status_buffer, 0, XHCI_UAS_STATUS_LENGTH,
            transport->uas_status_endpoint, true, stream_id,
            &status_trb)) {
        kernel_serial_write_serial_only("OS_UAS_STATUS_QUEUE_FAIL\r\n");
        return K_EIO;
    }
    if (emit_diagnostic) {
        kernel_serial_printf_serial_only(
            "OS_UAS_STATUS_QUEUED TRB=%u\r\n", (uint32_t)status_trb);
    }
    realtest_mark_number("USB_MSC_UAS_STATUS_LENGTH",
                                XHCI_UAS_STATUS_LENGTH);
    realtest_mark_number("USB_MSC_UAS_STATUS_RING_LO",
                                (uint32_t)xhci_dma_address(
                                    &status_ring->mapping));
    realtest_mark_number("USB_MSC_UAS_STATUS_TRB_LO",
                                (uint32_t)status_trb);
    realtest_mark_number("USB_MSC_UAS_STATUS_TRB_HIGH",
                                (uint32_t)(status_trb >> 32));
    realtest_mark_number("USB_MSC_UAS_STATUS_DOORBELL",
                                xhci_msc_endpoint_id(
                                    transport->uas_status_endpoint, true) |
                                ((uint32_t)stream_id << 16));

    if (length != 0U) {
        if (!xhci_uas_queue_transfer(
                state, transport, data_ring, data_enqueue, data_cycle,
                &transport->uas_data_buffer, buffer, length,
                direction_in ? transport->uas_data_in_endpoint :
                               transport->uas_data_out_endpoint,
                direction_in, stream_id, &data_trb)) {
            kernel_serial_write_serial_only("OS_UAS_DATA_QUEUE_FAIL\r\n");
            return K_EIO;
        }
        data_submitted = true;
        if (emit_diagnostic) {
            kernel_serial_printf_serial_only(
                "OS_UAS_DATA_QUEUED TRB=%u\r\n", (uint32_t)data_trb);
        }
    }

    if (!xhci_uas_queue_transfer(
            state, transport, &transport->uas_command_ring,
            &transport->uas_command_enqueue, &transport->uas_command_cycle,
            &transport->uas_command_buffer, command_iu,
            XHCI_UAS_COMMAND_LENGTH, transport->uas_command_endpoint, false,
            0U, &command_trb)) {
        kernel_serial_write_serial_only("OS_UAS_COMMAND_QUEUE_FAIL\r\n");
        return K_EIO;
    }
    if (emit_diagnostic) {
        kernel_serial_printf_serial_only(
            "OS_UAS_COMMAND_QUEUED TRB=%u\r\n", (uint32_t)command_trb);
    }

    xhci_uas_ring_doorbell(state, transport->slot,
                           transport->uas_status_endpoint, true,
                           stream_id);
    if (length != 0U) {
        xhci_uas_ring_doorbell(
            state, transport->slot,
            direction_in ? transport->uas_data_in_endpoint :
                           transport->uas_data_out_endpoint,
            direction_in, stream_id);
    }
    xhci_uas_ring_doorbell(state, transport->slot,
                           transport->uas_command_endpoint, false, 0U);
    if (emit_diagnostic) {
        kernel_serial_printf_serial_only(
            "OS_UAS_DOORBELLS SLOT=%u STREAM=%u\r\n",
            transport->slot, stream_id);
    }

    uint64_t deadline = xhci_controller_timeout_deadline(5000000000ULL);
    while (!command_done || !status_done || !data_done) {
        xhci_trb_t event;
        if (!xhci_next_ring_event(state, &event)) {
            if (xhci_controller_timeout_reached(deadline)) {
                realtest_mark("USB_MSC_UAS_TIMEOUT");
                kernel_serial_printf_serial_only(
                    "OS_UAS_TIMEOUT SLOT=%u STREAM=%u\r\n",
                    transport->slot, stream_id);
                result = K_ETIMEDOUT;
                goto out;
            }
            __asm__ volatile ("pause");
            continue;
        }

        uint32_t type = (event.control >> XHCI_TRB_TYPE_SHIFT) & 0x3FU;
        uint8_t event_slot = (uint8_t)(event.control >> XHCI_TRB_SLOT_SHIFT);
        uint32_t event_endpoint =
            (event.control >> XHCI_TRB_ENDPOINT_SHIFT) & 0x1FU;
        if (type != XHCI_TRANSFER_EVENT_TYPE || event_slot != transport->slot ||
            (event_endpoint != command_id && event_endpoint != status_id &&
             (length == 0U || event_endpoint != data_id))) {
            if (!xhci_handle_hid_io_event(state, &event) &&
                !xhci_defer_event(state, &event)) {
                kernel_serial_printf_serial_only(
                    "OS_UAS_EVENT_QUEUE_FAIL SLOT=%u TYPE=%u CTRL=%u\r\n",
                    transport->slot, type, event.control);
                result = K_EIO;
                goto out;
            }
            continue;
        }

        uint32_t completion = event.status >> XHCI_COMPLETION_SHIFT;
        uint32_t residual = event.status & 0x00FFFFFFU;
        if (completion != XHCI_COMPLETION_SUCCESS &&
            completion != XHCI_COMPLETION_SHORT_PACKET) {
            xhci_slot_device_t *slot_device =
                xhci_topology_slot(transport->slot);
            xhci_dma_page_t *output_page =
                slot_device != 0 && slot_device->used ?
                &slot_device->context.output_context : 0;
            xhci_dma_region_t *failure_ring = 0;
            xhci_dma_region_t *failure_streams = 0;
            uint32_t *failure_context = 0;
            uint32_t context_words = state->context_size / sizeof(uint32_t);

            if (event_endpoint == command_id) {
                failure_ring = &transport->uas_command_ring;
            } else if (event_endpoint == status_id) {
                failure_ring = status_ring;
                failure_streams = transport->uas_streams ?
                                  &transport->uas_status_streams : 0;
            } else if (event_endpoint ==
                       xhci_msc_endpoint_id(transport->uas_data_in_endpoint,
                                            true)) {
                failure_ring = transport->uas_streams ?
                               &transport->uas_data_in_stream_ring :
                               &transport->uas_data_in_ring;
                failure_streams = transport->uas_streams ?
                                  &transport->uas_data_in_streams : 0;
            } else if (event_endpoint ==
                       xhci_msc_endpoint_id(transport->uas_data_out_endpoint,
                                            false)) {
                failure_ring = transport->uas_streams ?
                               &transport->uas_data_out_stream_ring :
                               &transport->uas_data_out_ring;
                failure_streams = transport->uas_streams ?
                                  &transport->uas_data_out_streams : 0;
            }
            if (failure_ring != 0)
                dma_sync_for_cpu(&failure_ring->mapping);
            if (failure_streams != 0)
                dma_sync_for_cpu(&failure_streams->mapping);
            if (output_page != 0 && output_page->cpu != 0 &&
                event_endpoint <= 31U && context_words != 0U) {
                dma_sync_for_cpu(&output_page->mapping);
                failure_context =
                    (uint32_t *)output_page->cpu +
                    context_words * event_endpoint;
            }
            realtest_mark_number("USB_MSC_UAS_EVENT_CODE", completion);
            realtest_mark_number("USB_MSC_UAS_EVENT_ENDPOINT",
                                        event_endpoint);
            realtest_mark_number("USB_MSC_UAS_EVENT_STATUS",
                                        event.status);
            realtest_mark_number("USB_MSC_UAS_EVENT_CONTROL",
                                        event.control);
            realtest_mark_number("USB_MSC_UAS_EVENT_PTR_LO",
                                        (uint32_t)event.parameter);
            realtest_mark_number("USB_MSC_UAS_EVENT_PTR_HIGH",
                                        (uint32_t)(event.parameter >> 32));
            if (failure_context != 0) {
                realtest_mark_number("USB_MSC_UAS_FAIL_CTX0",
                                            failure_context[0]);
                realtest_mark_number("USB_MSC_UAS_FAIL_CTX1",
                                            failure_context[1]);
                realtest_mark_number("USB_MSC_UAS_FAIL_CTX2",
                                            failure_context[2]);
                realtest_mark_number("USB_MSC_UAS_FAIL_CTX3",
                                            failure_context[3]);
                realtest_mark_number("USB_MSC_UAS_FAIL_CTX4",
                                            failure_context[4]);
            }
            realtest_mark_number("USB_MSC_UAS_FAIL_STREAM_LO",
                failure_streams != 0 && failure_streams->cpu != 0 ?
                (uint32_t)((uint64_t *)failure_streams->cpu)
                    [stream_id * 2U] : 0U);
            realtest_mark_number("USB_MSC_UAS_FAIL_RING_CTRL",
                failure_ring != 0 && failure_ring->cpu != 0 ?
                ((xhci_trb_t *)failure_ring->cpu)[0].control : 0U);
            realtest_mark_number("USB_MSC_UAS_FAIL_RING_STATUS",
                failure_ring != 0 && failure_ring->cpu != 0 ?
                ((xhci_trb_t *)failure_ring->cpu)[0].status : 0U);
            realtest_mark_number("USB_MSC_UAS_FAIL_RING_PARAM_LO",
                failure_ring != 0 && failure_ring->cpu != 0 ?
                (uint32_t)((xhci_trb_t *)failure_ring->cpu)[0].parameter : 0U);
            realtest_mark_number("USB_MSC_UAS_FAIL_RING_PARAM_HIGH",
                failure_ring != 0 && failure_ring->cpu != 0 ?
                (uint32_t)(((xhci_trb_t *)failure_ring->cpu)[0].parameter >> 32) :
                0U);
            realtest_mark_number(
                "USB_MSC_UAS_FAIL_USBSTS",
                xhci_controller_read32(state,
                    state->operational_offset + 0x04U));
            kernel_serial_printf_serial_only(
                "OS_UAS_EVENT_FAIL SLOT=%u STREAM=%u CODE=%u ENDPOINT=%u PTR=%u\r\n",
                transport->slot, stream_id, completion, event_endpoint,
                (uint32_t)event.parameter);
            dma_sync_for_cpu(&transport->uas_status_buffer.mapping);
            realtest_mark_number("USB_MSC_UAS_FAIL_IU0", status_iu[0]);
            realtest_mark_number("USB_MSC_UAS_FAIL_IU1", status_iu[1]);
            realtest_mark_number("USB_MSC_UAS_FAIL_IU2", status_iu[2]);
            realtest_mark_number("USB_MSC_UAS_FAIL_IU3", status_iu[3]);
            result = K_EIO;
            goto out;
        }
        if (event_endpoint == command_id) {
            if (event.parameter == command_trb) command_done = true;
            continue;
        }
        if (event_endpoint == data_id) {
            if (!data_submitted || event.parameter != data_trb ||
                residual > length) continue;
            uint32_t completed = length - residual;
            if (direction_in && !dma_mapping_active(&transport->direct_mapping)) {
                dma_sync_for_cpu(&transport->uas_data_buffer.mapping);
                for (uint32_t offset = 0U; offset < completed; ++offset)
                    ((uint8_t *)buffer)[offset] =
                        ((const uint8_t *)transport->uas_data_buffer.cpu)[offset];
            }
            if (actual != 0) *actual = completed;
            __atomic_add_fetch(dma_mapping_active(&transport->direct_mapping) ?
                &g_xhci_msc_direct_bytes : &g_xhci_msc_bounce_bytes,
                completed, __ATOMIC_RELAXED);
            data_done = true;
            continue;
        }

        if (event.parameter != status_trb || residual > XHCI_UAS_STATUS_LENGTH)
            continue;
        uint32_t status_length = XHCI_UAS_STATUS_LENGTH - residual;
        dma_sync_for_cpu(&transport->uas_status_buffer.mapping);
        if (status_length < 4U) {
            result = K_EIO;
            goto out;
        }
        uint8_t iu_id = status_iu[0];
        if (xhci_uas_iu_tag(status_iu) != XHCI_UAS_TASK_TAG) {
            realtest_mark_number("USB_MSC_UAS_TAG",
                                        xhci_uas_iu_tag(status_iu));
            result = K_EIO;
            goto out;
        }
        if (iu_id == XHCI_UAS_READ_READY_IU ||
            iu_id == XHCI_UAS_WRITE_READY_IU) {
            if (length == 0U || !data_submitted ||
                (iu_id == XHCI_UAS_READ_READY_IU) != direction_in) {
                result = K_EIO;
                goto out;
            }
            /* 数据流在命令前提交。Ready IU 只会消费此状态 TD，因此要布置下一个状态 TD。 */
            for (uint32_t offset = 0U;
                 offset < XHCI_UAS_STATUS_LENGTH; ++offset)
                status_iu[offset] = 0U;
            if (!xhci_uas_queue_transfer(
                    state, transport, status_ring, status_enqueue,
                    status_cycle,
                    &transport->uas_status_buffer, 0,
                    XHCI_UAS_STATUS_LENGTH, transport->uas_status_endpoint,
                    true, stream_id, &status_trb)) {
                result = K_EIO;
                goto out;
            }
            xhci_uas_ring_doorbell(state, transport->slot,
                                   transport->uas_status_endpoint, true,
                                   stream_id);
            continue;
        }
        if (iu_id == XHCI_UAS_STATUS_IU) {
            if (status_length < 7U || status_iu[6] != 0U) {
                realtest_mark_number(
                    "USB_MSC_UAS_SCSI_STATUS",
                    status_length >= 7U ? status_iu[6] : 0xFFU);
                result = K_EIO;
                goto out;
            }
            status_done = true;
        } else if (iu_id == XHCI_UAS_RESPONSE_IU) {
            if (status_length < 8U || status_iu[7] != 0x08U) {
                realtest_mark_number(
                    "USB_MSC_UAS_RESPONSE",
                    status_length >= 8U ? status_iu[7] : 0xFFU);
                result = K_EIO;
                goto out;
            }
            status_done = true;
        } else {
            realtest_mark_number("USB_MSC_UAS_IU", iu_id);
            result = K_EIO;
            goto out;
        }
    }
    result = K_OK;
    if (emit_diagnostic) {
        kernel_serial_printf_serial_only(
            "OS_UAS_SCSI_OK SLOT=%u STREAM=%u LEN=%u\r\n",
            transport->slot, stream_id, actual != 0 ? *actual : 0U);
    }

out:
    xhci_event_handler_complete(state);
    return result;
}



bool xhci_usb_mass_storage_configured(void) {
    return g_xhci_usb_msc;
}

bool xhci_usb_msc_query(uint8_t slot, uint8_t *interface_number,
                        uint8_t *bulk_in, uint8_t *bulk_out) {
    if (slot == 0U || interface_number == 0 || bulk_in == 0 || bulk_out == 0)
        return false;
    xhci_msc_transport_t *transport = &g_xhci_msc_transports[slot];
    if (!transport->configured) return false;
    if (transport->uas) {
        if (transport->uas_data_in_endpoint == 0U ||
            transport->uas_data_out_endpoint == 0U) return false;
        *interface_number = transport->interface_number;
        *bulk_in = transport->uas_data_in_endpoint;
        *bulk_out = transport->uas_data_out_endpoint;
        return true;
    }
    if (transport->bulk_in_endpoint == 0U ||
        transport->bulk_out_endpoint == 0U) return false;
    *interface_number = transport->interface_number;
    *bulk_in = transport->bulk_in_endpoint;
    *bulk_out = transport->bulk_out_endpoint;
    return true;
}

bool xhci_usb_msc_is_uas(uint8_t slot) {
    return slot != 0U && g_xhci_msc_transports[slot].configured &&
           g_xhci_msc_transports[slot].uas;
}

uint32_t xhci_usb_msc_prepare_buffer(uint8_t slot, uint32_t requested) {
    if (slot == 0U || requested == 0U) return 0U;
    xhci_state_t *state = xhci_controller_state();
    if (state == 0 || !state->initialized) return 0U;
    xhci_event_lock(state);
    xhci_msc_transport_t *transport = &g_xhci_msc_transports[slot];
    xhci_dma_region_t *buffer = transport->uas ? &transport->uas_data_buffer :
                                               &transport->data_buffer;
    uint32_t capacity = 0U;
    if (transport->configured && !dma_mapping_active(&transport->direct_mapping)) {
        if (requested > XHCI_MSC_MAX_TRANSFER) requested = XHCI_MSC_MAX_TRANSFER;
        while (requested >= PAGE_SIZE) {
            if (xhci_msc_reserve(state, buffer, requested)) break;
            requested /= 2U;
        }
        capacity = buffer->page_count * PAGE_SIZE;
    }
    xhci_event_unlock(state);
    if (xhci_event_pending(state)) (void)xhci_schedule_deferred_work();
    return capacity;
}

bool xhci_usb_msc_recover_uas(uint8_t slot) {
    xhci_state_t *state = xhci_controller_state();
    xhci_msc_transport_t *transport;
    bool ok;

    if (slot == 0U || state == 0 || state->device_slot != slot)
        return false;
    transport = &g_xhci_msc_transports[slot];
    if (!transport->configured || !transport->uas) return false;

    realtest_mark_number("USB_MSC_UAS_RECOVERY_BEGIN", slot);
    xhci_event_lock(state);
    ok = xhci_stop_uas_transport(state, transport);
    if (!xhci_drop_uas_endpoint_contexts(state, transport)) ok = false;
    if (ok) {
        /* STOP 和 Configure Endpoint 完成后，控制器不再引用这些环。重建完整 UAS 传输，使每个环、流上下文
         * 和取出周期都从已知状态开始。 */
        xhci_msc_transport_release(slot);
        ok = xhci_configure_uas_endpoints(state);
    }
    xhci_event_unlock(state);
    if (xhci_event_pending(state)) (void)xhci_schedule_deferred_work();
    realtest_mark(ok ? "USB_MSC_UAS_RECOVERY_OK" :
                         "USB_MSC_UAS_RECOVERY_FAIL");
    return ok;
}

bool xhci_usb_msc_fallback_to_bot(uint8_t slot) {
    xhci_state_t *state = xhci_controller_state();
    bool ok;

    if (slot == 0U || state == 0 || state->device_slot != slot) return false;
    if (!xhci_usb_msc_is_uas(slot)) return true;
    realtest_mark_number("USB_MSC_UAS_FALLBACK_BEGIN", slot);
    ok = xhci_configure_msc_bot_endpoints(state);
    if (ok && !xhci_usb_msc_is_uas(slot)) {
        realtest_mark("USB_MSC_UAS_FALLBACK_OK");
        return true;
    }
    realtest_mark("USB_MSC_UAS_FALLBACK_FAIL");
    return false;
}

kstatus_t xhci_usb_msc_scsi_command(uint8_t slot, const uint8_t *command,
                                    uint8_t command_length, void *buffer,
                                    uint32_t length, bool direction_in,
                                    uint32_t *actual) {
    xhci_state_t *state;
    xhci_msc_transport_t *transport;
    kstatus_t status;

    if (slot == 0U || !xhci_usb_msc_is_uas(slot)) return K_EINVAL;
    state = xhci_controller_state();
    transport = &g_xhci_msc_transports[slot];
    xhci_event_lock(state);
    if (dma_mapping_active(&transport->direct_mapping)) {
        xhci_event_unlock(state);
        return K_EIO;
    }
    if (length != 0U && !xhci_msc_reserve(state, &transport->uas_data_buffer, length)) {
        xhci_event_unlock(state);
        return K_ENOMEM;
    }
    bool direct = length != 0U &&
                  xhci_msc_direct_map(state, transport, buffer, length, direction_in);
    status = xhci_uas_scsi_command_locked(
        state, transport, command, command_length, buffer, length,
        direction_in, actual);
    if (direct) {
        bool stopped = status == K_OK || xhci_stop_uas_transport(state, transport);
        if (!xhci_msc_direct_finish(state, transport,
                (uint8_t)xhci_msc_endpoint_id(direction_in ? transport->uas_data_in_endpoint :
                    transport->uas_data_out_endpoint, direction_in), stopped)) status = K_EIO;
    }
    xhci_event_unlock(state);
    if (xhci_event_pending(state)) (void)xhci_schedule_deferred_work();
    return status;
}

kstatus_t xhci_usb_bulk_session_begin(uint8_t slot) {
    xhci_state_t *state = xhci_controller_state();
    uint64_t deadline;

    if (slot == 0U || state == 0 || !state->initialized) return K_EINVAL;
    deadline = xhci_controller_timeout_deadline(1000000000ULL);
    for (;;) {
        if (xhci_event_try_lock(state)) break;
        if (xhci_controller_timeout_reached(deadline)) {
            realtest_mark_number("USB_MSC_EVENT_LOCK_TIMEOUT", slot);
            realtest_mark_number(
                "USB_MSC_EVENT_LOCK_OWNER",
                __atomic_load_n(&state->event_lock_owner, __ATOMIC_ACQUIRE));
            return K_ETIMEDOUT;
        }
        __asm__ volatile ("pause");
    }
    xhci_msc_transport_t *transport = &g_xhci_msc_transports[slot];
    if (!transport->configured || transport->slot != slot) {
        xhci_event_unlock(state);
        return K_EDEVREMOVED;
    }
    return K_OK;
}

void xhci_usb_bulk_session_end(uint8_t slot) {
    (void)slot;
    /* 同步 CBW/DATA/CSW 消费者推进 ERDP 时，不会逐个 TRB 清除 EHB。仍持有事件环时先结束当前批次，再恢复
     * 延迟 HID/Hub 处理；BOT 错误路径也适用此规则。 */
    xhci_event_handler_complete(xhci_controller_state());
    xhci_event_unlock(xhci_controller_state());
    if (xhci_event_pending(xhci_controller_state())) (void)xhci_schedule_deferred_work();
}

kstatus_t xhci_usb_bulk_transfer_locked(uint8_t slot, uint8_t endpoint,
                                        bool direction_in, void *buffer,
                                        uint32_t length, uint32_t *actual) {
    if (slot == 0U) return K_EINVAL;
    xhci_msc_transport_t *transport = &g_xhci_msc_transports[slot];
    if (!transport->configured || transport->slot != slot)
        return K_EDEVREMOVED;
    return xhci_msc_transfer_locked(xhci_controller_state(), transport, endpoint,
                                    direction_in, buffer, length, actual);
}
