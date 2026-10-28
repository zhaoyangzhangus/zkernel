#include <kernel/console.h>
#include <kernel/realtest.h>
#include "internal.h"

/* REFACTOR_P8_XHCI_CONTROL_TRANSFER_OWNER：EP0 控制传输 TRB 提交、完成匹配和
 * 超时诊断集中在同一所有者中。 */

#define XHCI_RING_TRB_COUNT 256U
#define XHCI_TRB_CYCLE (1U << 0)
#define XHCI_USBSTS 0x04U
#define XHCI_RUNTIME_IMAN 0x00U
#define XHCI_RUNTIME_INTR0 0x20U
#define XHCI_TRB_INTERRUPT_ON_SHORT_PACKET (1U << 2)
#define XHCI_TRB_TD_SIZE_SHIFT 17U
#define XHCI_TRB_TD_SIZE_MASK (0x1FU << XHCI_TRB_TD_SIZE_SHIFT)
#define XHCI_COMPLETION_SHORT_PACKET 13U

static bool xhci_take_deferred_control_event(
    xhci_state_t *state, uint8_t slot, xhci_trb_t *event) {
    uint32_t pending;
    if (state == 0 || event == 0) return false;
    pending = state->deferred_event_count;
    for (uint32_t index = 0U; index < pending; ++index) {
        uint32_t type;
        if (!xhci_event_queue_pop(state->deferred_events,
                                  &state->deferred_event_head,
                                  &state->deferred_event_count, event)) {
            return false;
        }
        type = (event->control >> XHCI_TRB_TYPE_SHIFT) & 0x3FU;
        if (type == XHCI_TRANSFER_EVENT_TYPE &&
            (uint8_t)(event->control >> XHCI_TRB_SLOT_SHIFT) == slot &&
            ((event->control >> XHCI_TRB_ENDPOINT_SHIFT) & 0x1FU) == 1U) {
            return true;
        }
        (void)xhci_event_queue_push(state->deferred_events,
                                    &state->deferred_event_tail,
                                    &state->deferred_event_count, event);
    }
    return false;
}

bool xhci_submit_control_transfer_device(
    xhci_state_t *state,
    xhci_device_context_t *device,
    const uint8_t setup[8],
    uint32_t length,
    bool direction_in) {
    bool auxiliary;
    if (state == 0 || device == 0 || setup == 0 || length > PAGE_SIZE ||
        device->device_slot == 0U) return false;
    auxiliary = state == xhci_hid_controller_state();
    if (auxiliary) {
        uint32_t setup_low = (uint32_t)setup[0] |
                              ((uint32_t)setup[1] << 8) |
                              ((uint32_t)setup[2] << 16) |
                              ((uint32_t)setup[3] << 24);
        uint32_t setup_high = (uint32_t)setup[4] |
                               ((uint32_t)setup[5] << 8) |
                               ((uint32_t)setup[6] << 16) |
                               ((uint32_t)setup[7] << 24);
        realtest_mark_number("XHCI_AUX_CONTROL_INDEX",
                                    device->ep0_enqueue);
        realtest_mark_number("XHCI_AUX_CONTROL_LENGTH", length);
        realtest_mark_number("XHCI_AUX_CONTROL_SETUP_LO", setup_low);
        realtest_mark_number("XHCI_AUX_CONTROL_SETUP_HI", setup_high);
    }
    xhci_trb_t *ring = (xhci_trb_t *)device->ep0_ring.cpu;
    uint32_t index = device->ep0_enqueue;
    uint32_t transfer_trbs = length == 0U ? 2U : 3U;
    if (index + transfer_trbs >= XHCI_RING_TRB_COUNT) return false;
    uint32_t status_index;
    if (!xhci_transfer_encode_setup(&ring[index], setup, length,
                                    direction_in, XHCI_TRB_CYCLE)) {
        return false;
    }
    /* xHCI 0.96 将 Setup TRB 的 Transfer Type 位保留。较新的控制器按 xHCI 1.0 要求，
     * 使用这些位描述后续数据阶段。 */
    if (state->hci_version < 0x0100U) {
        ring[index].control &= ~(3U << 16);
    }
    if (length != 0U) {
        if (!xhci_transfer_encode_data(
                &ring[index + 1U],
                xhci_dma_address(&device->descriptor_buffer.mapping),
                length, direction_in, XHCI_TRB_CYCLE)) return false;
        /* 此实现依次发出一个 Data TRB 和一个 Status TRB。对 xHCI 1.0 及更新版本，
         * TD 的最后一个 TRB 的 TD Size 为 0。 */
        uint32_t td_size = 0U;
        if (state->hci_version < 0x0100U) td_size = length >> 10;
        if (td_size > 0x1FU) td_size = 0x1FU;
        ring[index + 1U].status &= ~XHCI_TRB_TD_SIZE_MASK;
        ring[index + 1U].status |= td_size << XHCI_TRB_TD_SIZE_SHIFT;
        /* IN 控制数据阶段必须报告短包，使设备以短包结束时 xHC 能推进到 Status TRB。
         * 普通 USB2 设备和 AMD xHCI 都需要此设置。 */
        if (direction_in) {
            ring[index + 1U].control |=
                XHCI_TRB_INTERRUPT_ON_SHORT_PACKET;
        }
        status_index = index + 2U;
    } else {
        status_index = index + 1U;
    }
    if (!xhci_transfer_encode_status(&ring[status_index], direction_in,
                                     XHCI_TRB_CYCLE)) return false;
    uint64_t ring_address = xhci_dma_address(&device->ep0_ring.mapping);
    uint64_t data_trb_address = ring_address +
                                (uint64_t)(index + 1U) * sizeof(xhci_trb_t);
    device->ep0_enqueue = index + transfer_trbs;
    dma_sync_for_device(&device->ep0_ring.mapping);
    dma_sync_for_device(&device->descriptor_buffer.mapping);
    dma_wmb();
    volatile uint32_t *doorbell =
        (volatile uint32_t *)(state->mmio + state->doorbell_offset +
                              (uint32_t)device->device_slot * sizeof(uint32_t));
    *doorbell = 1U;
    __asm__ volatile ("mfence" : : : "memory");
    /* 已提交的 MMIO 写入不保证在 CPU 开始轮询内存前到达 xHC。与 Linux 相同，
     * 读回同一个 doorbell。 */
    (void)*doorbell;
    uint64_t deadline = xhci_controller_timeout_deadline(5000000000ULL);
    for (;;) {
        xhci_trb_t event;
        if (!xhci_take_deferred_control_event(state, device->device_slot,
                                              &event) &&
            !xhci_next_ring_event(state, &event)) {
            if (xhci_controller_timeout_reached(deadline)) break;
            __asm__ volatile ("pause");
            continue;
        }
        uint32_t type = (event.control >> XHCI_TRB_TYPE_SHIFT) & 0x3FU;
        if (type != XHCI_TRANSFER_EVENT_TYPE) {
            (void)xhci_defer_event(state, &event);
            continue;
        }
        if ((uint8_t)(event.control >> XHCI_TRB_SLOT_SHIFT) != device->device_slot ||
            ((event.control >> XHCI_TRB_ENDPOINT_SHIFT) & 0x1FU) != 1U) {
            /* 当前控制传输只消费本设备的 EP0 事件；其他事件交给统一轮询器处理。 */
            (void)xhci_defer_event(state, &event);
            continue;
        }
        uint32_t completion = event.status >> XHCI_COMPLETION_SHIFT;
        /* ISP 会在控制 Status 阶段前报告数据阶段短包事件。这只是中间完成，不代表请求
         * 已完成；继续等待 Status 阶段 TRB。 */
        if (length != 0U && event.parameter == data_trb_address &&
            completion == XHCI_COMPLETION_SHORT_PACKET) {
            dma_sync_for_cpu(&device->descriptor_buffer.mapping);
            continue;
        }
        if (completion != XHCI_COMPLETION_SUCCESS &&
            completion != XHCI_COMPLETION_SHORT_PACKET) {
            xhci_set_error(54U + completion);
            if (auxiliary) {
                realtest_mark_number("XHCI_AUX_CONTROL_EVENT_PARAM_LO",
                                            (uint32_t)event.parameter);
                realtest_mark_number("XHCI_AUX_CONTROL_EVENT_STATUS",
                                            event.status);
                realtest_mark_number("XHCI_AUX_CONTROL_EVENT_CONTROL",
                                            event.control);
                realtest_mark_number("XHCI_AUX_CONTROL_EVENT_INDEX",
                                            state->event_index);
            }
            realtest_mark_xhci_control(
                "COMPLETION", 54U + completion, device->device_slot,
                device->device_port, length, state->event_index,
                state->event_cycle);
            kernel_serial_write("OS_XHCI_EP0_COMPLETION SLOT=");
            kernel_serial_write_u32(device->device_slot);
            kernel_serial_write(" PORT=");
            kernel_serial_write_u32(device->device_port);
            kernel_serial_write(" LEN=");
            kernel_serial_write_u32(length);
            kernel_serial_write(" CODE=");
            kernel_serial_write_u32(completion);
            kernel_serial_write(" EVENT_STATUS=");
            kernel_serial_write_u32(event.status);
            kernel_serial_write("\r\n");
            xhci_event_handler_complete(state);
            return false;
        }
        dma_sync_for_cpu(&device->descriptor_buffer.mapping);
        xhci_event_handler_complete(state);
        return true;
    }
    uint32_t usbsts = xhci_controller_read32(
        state, state->operational_offset + XHCI_USBSTS);
    uint32_t iman = xhci_controller_read32(
        state, state->runtime_offset + XHCI_RUNTIME_INTR0 +
                   XHCI_RUNTIME_IMAN);
    uint32_t *output_context = (uint32_t *)device->output_context.cpu;
    const xhci_trb_t *pending_event = 0;
    uint32_t *input_context = (uint32_t *)device->input_context.cpu;
    if (state->event_ring.cpu != 0) {
        pending_event = (const xhci_trb_t *)state->event_ring.cpu +
                        state->event_index;
    }
    realtest_mark_number("XHCI_EP0_USBSTS", usbsts);
    realtest_mark_number("XHCI_EP0_IMAN", iman);
    realtest_mark_number("XHCI_EP0_RING_CTRL", ring[index].control);
    realtest_mark_number("XHCI_EP0_RING_ADDR_LO",
        (uint32_t)xhci_dma_address(&device->ep0_ring.mapping));
    realtest_mark_number("XHCI_EP0_DATA_ADDR_LO",
        (uint32_t)xhci_dma_address(&device->descriptor_buffer.mapping));
    if (input_context != 0) {
        uint32_t input_ep0 = state->context_size * 2U / sizeof(uint32_t);
        realtest_mark_number("XHCI_EP0_IN_DW1",
                                    input_context[input_ep0 + 1U]);
        realtest_mark_number("XHCI_EP0_IN_DW2",
                                    input_context[input_ep0 + 2U]);
        realtest_mark_number("XHCI_EP0_IN_DW3",
                                    input_context[input_ep0 + 3U]);
    }
    realtest_mark_number("XHCI_EP0_DEFERRED",
                                state->deferred_event_count);
    for (uint32_t deferred = 0U;
         deferred < state->deferred_event_count && deferred < 8U;
         ++deferred) {
        uint32_t event_index =
            (state->deferred_event_head + deferred) % XHCI_DEFERRED_EVENT_COUNT;
        realtest_mark_number("XHCI_EP0_DEFERRED_CTRL",
                                    state->deferred_events[event_index].control);
    }
    if (pending_event != 0) {
        realtest_mark_number("XHCI_EP0_EVENT_CTRL",
                                    pending_event->control);
        realtest_mark_number("XHCI_EP0_EVENT_STATUS",
                                    pending_event->status);
    }
    realtest_mark_number("XHCI_EP0_PORTSC",
        xhci_controller_read32(state, state->operational_offset + 0x400U +
                                (uint32_t)(device->device_port - 1U) * 0x10U));
    if (output_context != 0) {
        realtest_mark_number("XHCI_EP0_OUT_DW0", output_context[8U]);
        realtest_mark_number("XHCI_EP0_OUT_DW1", output_context[9U]);
        realtest_mark_number("XHCI_EP0_OUT_DW2", output_context[10U]);
        realtest_mark_number("XHCI_EP0_OUT_DW3", output_context[11U]);
    }
    xhci_set_error(54U);
    realtest_mark_xhci_control(
        "TIMEOUT", 54U, device->device_slot, device->device_port, length,
        state->event_index, state->event_cycle);
    kernel_serial_write("OS_XHCI_EP0_TIMEOUT SLOT=");
    kernel_serial_write_u32(device->device_slot);
    kernel_serial_write(" PORT=");
    kernel_serial_write_u32(device->device_port);
    kernel_serial_write(" LEN=");
    kernel_serial_write_u32(length);
    kernel_serial_write(" EVENT_INDEX=");
    kernel_serial_write_u32(state->event_index);
    kernel_serial_write(" EVENT_CYCLE=");
    kernel_serial_write_u32(state->event_cycle);
    kernel_serial_write(" USBSTS=");
    kernel_serial_write_u32(usbsts);
    kernel_serial_write(" IMAN=");
    kernel_serial_write_u32(iman);
    kernel_serial_write("\r\n");
    xhci_event_handler_complete(state);
    return false;
}
