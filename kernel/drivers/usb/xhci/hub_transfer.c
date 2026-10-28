#include "internal.h"

#include <kernel/console.h>

/* REFACTOR_P8_XHCI_HUB_TRANSFER_OWNER：Hub 中断状态环配置和队列提交
 * 由同一个直接传输所有者负责。 */

#define XHCI_RING_TRB_COUNT 256U
#define XHCI_LINK_TRB_TYPE 6U
#define XHCI_CONFIGURE_ENDPOINT_TYPE 12U
#define XHCI_EVALUATE_CONTEXT_TYPE 13U
#define XHCI_SET_TR_DEQUEUE_POINTER_TYPE 16U
#define XHCI_STOP_ENDPOINT_TYPE 15U
#define XHCI_TRB_CYCLE (1U << 0)
#define XHCI_TRB_INTERRUPT_ON_COMPLETION (1U << 5)
#define XHCI_TRB_LINK_TOGGLE_CYCLE (1U << 1)
#define XHCI_SLOT_MTT (1U << 25)
#define XHCI_SLOT_HUB (1U << 26)
#define XHCI_SLOT_MAX_PORTS_SHIFT 24U
#define XHCI_SLOT_TT_THINK_SHIFT 16U
#define USB_HUB_PROTOCOL_MULTI_TT 2U

bool xhci_queue_hub_status_device(xhci_state_t *state,
    xhci_device_context_t *device) {
    xhci_trb_t *ring;
    uint32_t index;
    uint32_t endpoint_id;
    if (state == 0 || !state->initialized || device->hub_ring.cpu == 0 ||
        device->hub_report.cpu == 0 || device->hub_endpoint == 0U ||
        device->hub_max_packet == 0U || device->hub_transfer_pending) return false;
    if (device->hub_max_packet > PAGE_SIZE) return false;
    index = device->hub_enqueue;
    if (index >= XHCI_RING_TRB_COUNT - 1U) return false;
    endpoint_id = (uint32_t)device->hub_endpoint * 2U + 1U;
    if (endpoint_id > 31U) return false;
    for (uint32_t i = 0U; i < device->hub_max_packet; ++i) {
        ((uint8_t *)device->hub_report.cpu)[i] = 0U;
    }
    ring = (xhci_trb_t *)device->hub_ring.cpu;
    if (!xhci_transfer_encode_normal(
            &ring[index], xhci_dma_address(&device->hub_report.mapping),
            device->hub_max_packet, XHCI_TRB_INTERRUPT_ON_COMPLETION,
            device->hub_cycle)) return false;
    ++index;
    if (index == XHCI_RING_TRB_COUNT - 1U) {
        ring[XHCI_RING_TRB_COUNT - 1U].control =
            (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
            device->hub_cycle | XHCI_TRB_LINK_TOGGLE_CYCLE;
        device->hub_enqueue = 0U;
        device->hub_cycle ^= 1U;
    } else {
        device->hub_enqueue = index;
    }
    device->hub_transfer_pending = true;
    dma_sync_for_device(&device->hub_ring.mapping);
    dma_sync_for_device(&device->hub_report.mapping);
    dma_wmb();
    *(volatile uint32_t *)(state->mmio + state->doorbell_offset +
                           (uint32_t)device->device_slot * sizeof(uint32_t)) = endpoint_id;
    __asm__ volatile ("mfence" : : : "memory");
    return true;
}

/*
 * 供尚未发布的枚举/设置路径使用的兼容包装函数。
 * 已发布的 Hub 运行时事件绝不使用此包装函数。
 */
bool xhci_configure_hub_endpoint(xhci_state_t *state) {
    uint32_t endpoint_id;
    uint32_t *input;
    uint32_t *control;
    uint32_t *input_slot;
    uint32_t *output_slot;
    uint32_t *endpoint;
    if (state == 0 || state->hub_endpoint == 0U ||
        state->hub_max_packet == 0U || state->hub_max_packet > PAGE_SIZE) return false;
    endpoint_id = (uint32_t)state->hub_endpoint * 2U + 1U;
    if (endpoint_id > 31U ||
        !xhci_alloc_page(state, &state->hub_ring, DMA_BIDIRECTIONAL) ||
        !xhci_alloc_page(state, &state->hub_report, DMA_FROM_DEVICE)) {
        xhci_free_page(&state->hub_ring);
        xhci_free_page(&state->hub_report);
        return false;
    }
    xhci_ring_init_link(state->hub_ring.cpu,
                        xhci_dma_address(&state->hub_ring.mapping),
                        XHCI_RING_TRB_COUNT);
    state->hub_enqueue = 0U;
    state->hub_cycle = 1U;
    state->hub_transfer_pending = false;
    state->hub_change_bitmap = 0U;
    input = (uint32_t *)state->input_context.cpu;
    control = input;
    input_slot = (uint32_t *)((uint8_t *)input + state->context_size);
    output_slot = (uint32_t *)state->output_context.cpu;
    endpoint = (uint32_t *)((uint8_t *)input +
                            state->context_size * (endpoint_id + 1U));
    for (uint32_t i = 0U;
         i < 4U;
         ++i)
    {
        input_slot[i] =
            output_slot[i];
    }

    /*
    * 告知 xHC 此 Slot 表示一个 Hub。
     */
    input_slot[0] |=
        XHCI_SLOT_HUB;

    /*
    * MTT 仅适用于高速多 TT Hub。
     */
    input_slot[0] &=
        ~XHCI_SLOT_MTT;

    if(state->device_speed == 3U &&
       state->hub_protocol ==
           USB_HUB_PROTOCOL_MULTI_TT)
    {
        input_slot[0] |=
            XHCI_SLOT_MTT;
    }

    /*
    * 下游端口数量。
     */
    input_slot[1] &=
        ~(0xFFU <<
          XHCI_SLOT_MAX_PORTS_SHIFT);

    input_slot[1] |=
        (uint32_t)state->hub_port_count <<
        XHCI_SLOT_MAX_PORTS_SHIFT;

    /*
    * TT Think Time 适用于高速 USB2 Hub。
    * 全速 Hub 的 Slot Context 必须将其设为零。
     */
    input_slot[2] &=
        ~(0x03U <<
          XHCI_SLOT_TT_THINK_SHIFT);

    if(state->device_speed == 3U)
    {
        input_slot[2] |=
            ((uint32_t)
                 state->hub_tt_think_time &
             0x03U) <<
            XHCI_SLOT_TT_THINK_SHIFT;
    }

    /* Hub metadata is a Slot Context update.  Keep it separate from adding
     * the interrupt endpoint, as Linux does, and clear DW3 because Device
     * Address and Slot State are output-only fields.  QEMU rejects a combined
     * update carrying the Addressed state with Context State Error. */
    input_slot[3] = 0U;
    control[0] = 0U;
    control[1] = 1U;
    kernel_serial_printf_serial_only(
        "OS_DIAG_HUB_CONTEXT SLOT=%u HCI=%u OUT0=%u OUT1=%u "
        "OUT2=%u OUT3=%u IN0=%u IN1=%u IN2=%u\r\n",
        state->device_slot, state->hci_version,
        output_slot[0], output_slot[1], output_slot[2], output_slot[3],
        input_slot[0], input_slot[1], input_slot[2]);
    dma_sync_for_device(&state->input_context.mapping);
    if (!xhci_submit_command(
            state,
            state->hci_version > 0x0095U ? XHCI_CONFIGURE_ENDPOINT_TYPE :
                                          XHCI_EVALUATE_CONTEXT_TYPE,
            state->device_slot,
            xhci_dma_address(&state->input_context.mapping), 0)) {
        kernel_serial_printf_serial_only(
            "OS_DIAG_HUB_CONTEXT_FAIL ERROR=%u\r\n",
            xhci_last_error());
        xhci_free_page(&state->hub_ring);
        xhci_free_page(&state->hub_report);
        return false;
    }
    kernel_serial_write_serial_only("OS_DIAG_HUB_CONTEXT_OK\r\n");

    dma_sync_for_cpu(&state->output_context.mapping);
    for (uint32_t i = 0U; i < 4U; ++i) {
        input_slot[i] = output_slot[i];
    }
    input_slot[0] &= ~(0x1FU << 27);
    input_slot[0] |= endpoint_id << 27;
    input_slot[3] = 0U;
    control[0] = 0U;
    control[1] = 1U | (1U << endpoint_id);
    xhci_init_endpoint_context(state, endpoint,
        state->hub_interval == 0U ? 1U : state->hub_interval, 7U,
        state->hub_max_packet, &state->hub_ring.mapping);
    dma_sync_for_device(&state->input_context.mapping);
    dma_sync_for_device(&state->hub_ring.mapping);
    dma_sync_for_device(&state->hub_report.mapping);
    if (!xhci_submit_command(state, XHCI_CONFIGURE_ENDPOINT_TYPE,
                             state->device_slot,
                             xhci_dma_address(&state->input_context.mapping), 0)) {
        kernel_serial_printf_serial_only(
            "OS_DIAG_HUB_ENDPOINT_FAIL ERROR=%u\r\n",
            xhci_last_error());
        xhci_free_page(&state->hub_ring);
        xhci_free_page(&state->hub_report);
        return false;
    }
    kernel_serial_write_serial_only("OS_DIAG_HUB_ENDPOINT_OK\r\n");

    /*
     * V3.10.6B10B8 HUB STATUS TD LIFETIME
     *
     * Keep one Hub interrupt-IN TD armed from the moment the endpoint becomes
     * RUNNING.  This is the path QEMU reliably keeps NAKed/retryable and later
     * wakes through usb_wakeup().
     *
     * Startup PORT_RESET / ENABLE / CONNECTION changes are intentionally
     * allowed to complete this TD.  They are drained and acknowledged at the
     * explicit startup->runtime barrier before MSI-X runtime begins.
     */
    bool queued = xhci_queue_hub_status_device(state, &state->device);
    kernel_serial_write_serial_only(
        queued ? "OS_DIAG_HUB_STATUS_QUEUE_OK\r\n" :
                 "OS_DIAG_HUB_STATUS_QUEUE_FAIL\r\n");
    return queued;
}

static bool xhci_is_endpoint_transfer_event(const xhci_trb_t *event,
                                            uint8_t slot,
                                            uint8_t endpoint_id) {
    if (event == 0 ||
        ((event->control >> XHCI_TRB_TYPE_SHIFT) & 0x3FU) !=
            XHCI_TRANSFER_EVENT_TYPE) {
        return false;
    }
    return (uint8_t)(event->control >> XHCI_TRB_SLOT_SHIFT) == slot &&
           ((event->control >> XHCI_TRB_ENDPOINT_SHIFT) & 0x1FU) ==
               endpoint_id;
}

bool xhci_drop_endpoint_transfer_events(xhci_state_t *state,
                                        uint8_t slot,
                                        uint8_t endpoint_id) {
    xhci_trb_t keep[XHCI_DEFERRED_EVENT_COUNT];
    uint32_t keep_count = 0U;
    uint32_t deferred_count;

    if (state == 0 || slot == 0U || endpoint_id == 0U) return false;
    deferred_count = state->deferred_event_count;
    for (uint32_t i = 0U; i < deferred_count; ++i) {
        xhci_trb_t event = state->deferred_events[state->deferred_event_head];
        state->deferred_event_head =
            (state->deferred_event_head + 1U) % XHCI_DEFERRED_EVENT_COUNT;
        --state->deferred_event_count;
        if (xhci_is_endpoint_transfer_event(&event, slot, endpoint_id)) {
            continue;
        }
        if (keep_count >= XHCI_DEFERRED_EVENT_COUNT) return false;
        keep[keep_count++] = event;
    }

    state->deferred_event_head = 0U;
    state->deferred_event_tail = 0U;
    state->deferred_event_count = 0U;
    for (uint32_t i = 0U; i < keep_count; ++i) {
        if (!xhci_defer_event(state, &keep[i])) return false;
    }

    for (uint32_t i = 0U; i < XHCI_RING_TRB_COUNT; ++i) {
        xhci_trb_t event;
        if (!xhci_next_ring_event(state, &event)) break;
        if (xhci_is_endpoint_transfer_event(&event, slot, endpoint_id)) {
            continue;
        }
        if (!xhci_defer_event(state, &event)) return false;
    }
    return true;
}

bool xhci_restart_hub_status_endpoint(xhci_state_t *state,
                                      xhci_device_context_t *hub) {
    uint8_t slot;
    uint32_t endpoint_id;
    xhci_trb_t *ring;

    if (state == 0 || hub == 0 || hub->device_slot == 0U ||
        hub->hub_endpoint == 0U || hub->hub_ring.cpu == 0 ||
        hub->hub_report.cpu == 0) {
        return false;
    }
    slot = hub->device_slot;
    endpoint_id = (uint32_t)hub->hub_endpoint * 2U + 1U;
    if (endpoint_id == 0U || endpoint_id > 31U) return false;

    kernel_serial_write_serial_only("OS_DIAG_HUB_RESYNC_BEGIN\r\n");
    if (!xhci_submit_command_ex(state, XHCI_STOP_ENDPOINT_TYPE, slot,
                                (uint8_t)endpoint_id, 0U, 0)) {
        kernel_serial_write_serial_only("OS_DIAG_HUB_RESYNC_STOP_FAIL\r\n");
        return false;
    }
    hub->hub_transfer_pending = false;
    if (!xhci_drop_endpoint_transfer_events(state, slot,
                                             (uint8_t)endpoint_id)) {
        kernel_serial_write_serial_only("OS_DIAG_HUB_RESYNC_DRAIN_FAIL\r\n");
        return false;
    }

    ring = (xhci_trb_t *)hub->hub_ring.cpu;
    for (uint32_t i = 0U; i < XHCI_RING_TRB_COUNT - 1U; ++i) {
        ring[i].parameter = 0U;
        ring[i].status = 0U;
        ring[i].control = 0U;
    }
    xhci_ring_init_link(hub->hub_ring.cpu,
                        xhci_dma_address(&hub->hub_ring.mapping),
                        XHCI_RING_TRB_COUNT);
    hub->hub_enqueue = 0U;
    hub->hub_cycle = 1U;
    hub->hub_transfer_pending = false;
    hub->hub_change_bitmap = 0U;
    dma_sync_for_device(&hub->hub_ring.mapping);
    dma_wmb();

    if (!xhci_submit_command_ex(
            state, XHCI_SET_TR_DEQUEUE_POINTER_TYPE, slot,
            (uint8_t)endpoint_id,
            xhci_dma_address(&hub->hub_ring.mapping) | 1ULL, 0)) {
        kernel_serial_write_serial_only(
            "OS_DIAG_HUB_RESYNC_DEQUEUE_FAIL\r\n");
        return false;
    }
    if (!xhci_queue_hub_status_device(state, hub)) {
        kernel_serial_write_serial_only("OS_DIAG_HUB_RESYNC_QUEUE_FAIL\r\n");
        return false;
    }
    kernel_serial_write_serial_only("OS_DIAG_HUB_RESYNC_OK\r\n");
    return true;
}
