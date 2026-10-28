#include <kernel/console.h>
#include <kernel/kmem.h>
#include <kernel/realtest.h>
#include <usb/device.h>
#include <usb/hub.h>
#include "internal.h"
#include "audio_descriptor.h"

/* REFACTOR_P8_XHCI_ENUMERATION_OWNER：USB 描述符解析、端点上下文设置和设备枚举
 * 属于独立的冷路径策略。 */

#define XHCI_RING_TRB_COUNT 256U
#define XHCI_ADDRESS_DEVICE_TYPE 11U
#define XHCI_CONFIGURE_ENDPOINT_TYPE 12U
#define XHCI_EVALUATE_CONTEXT_TYPE 13U
#define XHCI_LINK_TRB_TYPE 6U
#define XHCI_TRB_CYCLE (1U << 0)
#define XHCI_TRB_LINK_TOGGLE_CYCLE (1U << 1)
#define XHCI_PORTSC_SPEED_MASK 0x0FU
#define XHCI_SETUP_GET_DESCRIPTOR 6U
#define XHCI_SETUP_SET_CONFIGURATION 9U
#define XHCI_SETUP_SET_INTERFACE 11U
#define XHCI_DESCRIPTOR_DEVICE 1U
#define XHCI_DESCRIPTOR_CONFIGURATION 2U
#define XHCI_DESCRIPTOR_INTERFACE 4U
#define XHCI_DESCRIPTOR_ENDPOINT 5U
#define XHCI_DESCRIPTOR_CS_INTERFACE 0x24U
#define XHCI_DESCRIPTOR_HUB 0x29U
#define XHCI_DESCRIPTOR_SS_HUB 0x2AU
#define XHCI_DESCRIPTOR_DEVICE_LENGTH 18U
#define USB_CLASS_AUDIO 0x01U
#define USB_AUDIO_SUBCLASS_STREAM 0x02U
#define USB_CLASS_HID 0x03U
#define USB_HID_SUBCLASS_BOOT 0x01U
#define USB_HID_PROTOCOL_KEYBOARD 0x01U
#define USB_HID_PROTOCOL_MOUSE 0x02U
#define USB_HID_SET_PROTOCOL 0x0BU
#define USB_HID_PROTOCOL_BOOT 0x00U
#define USB_CLASS_HUB 0x09U
#define USB_CLASS_MASS_STORAGE 0x08U
#define USB_MSC_SUBCLASS_SCSI 0x06U
#define USB_MSC_PROTOCOL_BULK_ONLY 0x50U
#define USB_MSC_PROTOCOL_UAS 0x62U
#define USB_SS_ENDPOINT_COMPANION 0x30U
#define USB_UAS_PIPE_USAGE 0x24U
#define USB_UAS_PIPE_COMMAND 1U
#define USB_UAS_PIPE_STATUS 2U
#define USB_UAS_PIPE_DATA_IN 3U
#define USB_UAS_PIPE_DATA_OUT 4U
#define USB_CLASS_WIRELESS 0xE0U
#define USB_BT_SUBCLASS 0x01U
#define USB_BT_PROTOCOL 0x01U
#define USB_TRANSFER_BULK 0x02U
#define USB_TRANSFER_INTERRUPT 0x03U
#define USB_TRANSFER_ISOCHRONOUS 0x01U
#define XHCI_SLOT_MTT (1U << 25)
#define XHCI_SLOT_TT_PORT_SHIFT 8U
#define XHCI_SLOT_STATE_SHIFT 27U
#define XHCI_SLOT_STATE_MASK 0x1FU
#define XHCI_SLOT_STATE_ADDRESSED 2U
#define USB_HUB_PROTOCOL_MULTI_TT 2U
#define USB_HUB_MIN_POWER_GOOD_MS 100U

static bool xhci_resolve_tt(
    uint8_t child_speed,
    uint8_t parent_slot,
    uint8_t parent_port,
    uint8_t *tt_slot,
    uint8_t *tt_port,
    bool *tt_multi)
{
    if(tt_slot == 0 ||
       tt_port == 0 ||
       tt_multi == 0)
    {
        return false;
    }

    *tt_slot =
        0U;

    *tt_port =
        0U;

    *tt_multi =
        false;

    /*
    * 只有低速/全速设备会使用外部 USB2 TT。
     */
    if(child_speed > 2U ||
       parent_slot == 0U)
    {
        return true;
    }

    if(parent_port == 0U)
    {
        return false;
    }

    xhci_slot_device_t *parent =
        xhci_topology_slot(parent_slot);

    if(!parent->used ||
       !parent->is_hub ||
       parent->context.device_slot !=
           parent_slot)
    {
        return false;
    }

    /*
    * 设备直接连接到高速 Hub 下方时：
    * 该 Hub 负责拆分事务。
     */
    if(parent->context.device_speed == 3U)
    {
        *tt_slot =
            parent_slot;

        *tt_port =
            parent_port;

        *tt_multi =
            parent->hub_protocol ==
                USB_HUB_PROTOCOL_MULTI_TT;

        return true;
    }

    /*
    * 全速/低速 Hub 自身可能位于高速 TT 后方。
    * 其所有全速/低速后代都属于同一个上游 TT 域。
     */
    if(parent->tt_slot != 0U)
    {
        *tt_slot =
            parent->tt_slot;

        *tt_port =
            parent->tt_port;

        *tt_multi =
            parent->tt_multi;
    }

    return true;
}


static bool xhci_prepare_device_resources(xhci_state_t *state, uint8_t slot,
                                          uint8_t port, uint8_t speed,
                                          uint8_t root_port, uint8_t parent_slot,
                                          uint8_t parent_port,
                                          uint8_t tt_slot,
                                          uint8_t tt_port,
                                          bool tt_multi,
                                          uint32_t route_string) {
    if (state == 0 || slot == 0U || port == 0U || speed == 0U ||
        root_port == 0U) return false;
    if (!xhci_alloc_page(state, &state->input_context, DMA_TO_DEVICE) ||
        !xhci_alloc_page(state, &state->output_context, DMA_BIDIRECTIONAL) ||
        !xhci_alloc_page(state, &state->ep0_ring, DMA_BIDIRECTIONAL) ||
        !xhci_alloc_page(state, &state->descriptor_buffer, DMA_BIDIRECTIONAL)) {
        xhci_set_error(53U);
        xhci_free_device_resources(state);
        return false;
    }
    volatile uint64_t *dcbaa = (volatile uint64_t *)state->dcbaa.cpu;
    dcbaa[slot] = xhci_dma_address(&state->output_context.mapping);

    uint8_t *input = (uint8_t *)state->input_context.cpu;
    uint32_t *control = (uint32_t *)input;
    uint32_t *slot_context = (uint32_t *)(input + state->context_size);
    uint32_t *ep0_context = (uint32_t *)(input + state->context_size * 2U);
    control[0] = 0U;
    control[1] = 0x3U;
    slot_context[0] =
        ((uint32_t)(
            speed &
            XHCI_PORTSC_SPEED_MASK) <<
         20) |
        (route_string &
         0x000FFFFFU) |
        (1U << 27) |
        (tt_multi
             ? XHCI_SLOT_MTT
             : 0U);

    slot_context[1] =
        (uint32_t)root_port <<
        16;

    /*
    * DW2 不是软件层的父设备标识。
     *
    * 它表示为此低速/全速设备提供服务的外部高速 Transaction Translator。
     */
    slot_context[2] =
        (uint32_t)tt_slot |
        ((uint32_t)tt_port <<
         XHCI_SLOT_TT_PORT_SHIFT);
    slot_context[3] = 0U;
    uint32_t max_packet = speed >= 3U ? 64U : 8U;
    if (speed >= 4U) max_packet = 512U;
    ep0_context[0] = 0U;
    ep0_context[1] = (3U << 1) | (4U << 3) | (max_packet << 16);
    ep0_context[2] = (uint32_t)xhci_dma_address(&state->ep0_ring.mapping) | 1U;
    ep0_context[3] = (uint32_t)(xhci_dma_address(&state->ep0_ring.mapping) >> 32);
    /* Endpoint Context DW4：平均 TRB 长度。控制 TD 始终包含一个 8 字节 Setup TRB；
     * QEMU 容忍零值，但某些实体 xHCI 实现会因此不调度 EP0。 */
    ep0_context[4] = 8U;

    xhci_trb_t *ring = (xhci_trb_t *)state->ep0_ring.cpu;
    ring[XHCI_RING_TRB_COUNT - 1U].parameter =
        xhci_dma_address(&state->ep0_ring.mapping);
    ring[XHCI_RING_TRB_COUNT - 1U].control =
        (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
        XHCI_TRB_CYCLE | XHCI_TRB_LINK_TOGGLE_CYCLE;
    dma_sync_for_device(&state->dcbaa.mapping);
    dma_sync_for_device(&state->input_context.mapping);
    dma_sync_for_device(&state->output_context.mapping);
    dma_sync_for_device(&state->ep0_ring.mapping);
    dma_sync_for_device(&state->descriptor_buffer.mapping);
    state->device_slot = slot;
    state->device_port = port;
    state->device_speed = speed;
    state->root_port = root_port;
    state->parent_slot = parent_slot;
    state->parent_port = parent_port;
    state->route_string = route_string;
    state->ep0_enqueue = 0U;
    return true;
}

static uint32_t xhci_ep0_descriptor_max_packet(
    const xhci_device_context_t *device, const uint8_t *descriptor) {
    uint32_t raw;

    if (device == 0 || descriptor == 0) return 0U;
    if (device->device_speed >= 4U) return 512U;
    raw = descriptor[7];
    if (raw == 8U || raw == 16U || raw == 32U || raw == 64U) return raw;

    /* USB 2 设备必须报告上述取值之一。部分固件路径会通过 USB2 companion 端口暴露
     * SuperSpeed 编码值（9）；此时保留对应速度的 EP0 默认值，不要向 xHCI 上下文
     * 写入不可能的包大小。 */
    return device->device_speed >= 3U ? 64U : 8U;
}

static uint32_t xhci_ep0_initial_max_packet(
    const xhci_device_context_t *device) {
    if (device == 0) return 0U;
    if (device->device_speed >= 4U) return 512U;
    return device->device_speed >= 3U ? 64U : 8U;
}

static bool xhci_prepare_ep0_update(xhci_state_t *state,
                                    const xhci_device_context_t *device,
                                    const uint8_t *descriptor,
                                    bool evaluate_only) {
    uint32_t *input;
    uint32_t *control;
    uint32_t *ep0;
    uint32_t max_packet;
    uint64_t dequeue;

    if (state == 0 || device == 0 || descriptor == 0 ||
        state->context_size == 0U || state->input_context.cpu == 0 ||
        state->output_context.cpu == 0 || state->ep0_ring.cpu == 0) {
        return false;
    }
    input = (uint32_t *)state->input_context.cpu;
    control = input;
    ep0 = input + (state->context_size * 2U) / sizeof(uint32_t);
    max_packet = xhci_ep0_descriptor_max_packet(device, descriptor);
    if (max_packet == 0U || max_packet > 512U) return false;

    if (evaluate_only) {
        const uint32_t *output_ep0;
        uint32_t context_words = state->context_size / sizeof(uint32_t);

        dma_sync_for_cpu(&state->output_context.mapping);
        output_ep0 = (const uint32_t *)state->output_context.cpu +
                     context_words;
        for (uint32_t index = 0U; index < 5U; ++index) {
            ep0[index] = output_ep0[index];
        }
    }

    /* 输入 Endpoint Context 中的 Endpoint State 只用于输出。 */
    ep0[0] &= ~0x7U;
    ep0[1] = (ep0[1] & 0x0000FFFFU) | (max_packet << 16);
    dequeue = xhci_dma_address(&device->ep0_ring.mapping) +
              (uint64_t)device->ep0_enqueue * sizeof(xhci_trb_t);
    ep0[2] = (uint32_t)dequeue | XHCI_TRB_CYCLE;
    ep0[3] = (uint32_t)(dequeue >> 32);
    control[0] = 0U;
    control[1] = evaluate_only ? (1U << 1) : 0x3U;
    dma_sync_for_device(&state->input_context.mapping);
    return true;
}

static bool xhci_evaluate_ep0_max_packet(
    xhci_state_t *state, const xhci_device_context_t *device,
    const uint8_t *descriptor) {
    if (!xhci_prepare_ep0_update(state, device, descriptor, true)) {
        return false;
    }
    realtest_mark_number("XHCI_EP0_EVALUATE_BEGIN",
                                device->device_slot);
    if (!xhci_submit_command_ex(
            state, XHCI_EVALUATE_CONTEXT_TYPE, device->device_slot, 0U,
            xhci_dma_address(&device->input_context.mapping), 0)) {
        realtest_mark_number("XHCI_EP0_EVALUATE_ERROR",
                                    xhci_last_error());
        return false;
    }
    realtest_mark_number("XHCI_EP0_EVALUATE_OK",
                                device->device_slot);
    return true;
}

static bool xhci_prepare_address_update(xhci_state_t *state,
                                        const xhci_device_context_t *device,
                                        const uint8_t *descriptor) {
    return xhci_prepare_ep0_update(state, device, descriptor, false);
}

static bool xhci_slot_is_addressed(xhci_state_t *state, uint8_t slot) {
    uint32_t slot_state;

    if (state == 0 || state->output_context.cpu == 0) return false;
    dma_sync_for_cpu(&state->output_context.mapping);
    slot_state = ((const uint32_t *)state->output_context.cpu)[3];
    slot_state = (slot_state >> XHCI_SLOT_STATE_SHIFT) &
                 XHCI_SLOT_STATE_MASK;

    kernel_serial_printf_serial_only(
        "OS_DIAG_XHCI_SLOT_ADDRESSED SLOT=%u STATE=%u\r\n",
        slot, slot_state);
    realtest_mark_number("XHCI_SLOT_STATE", slot_state);
    return slot_state == XHCI_SLOT_STATE_ADDRESSED;
}

static bool xhci_complete_device_address(
    xhci_state_t *state, const xhci_device_context_t *device,
    const uint8_t *descriptor) {
    uint32_t initial_max_packet;
    uint32_t descriptor_max_packet;

    if (state == 0 || device == 0 || descriptor == 0 ||
        device->device_slot == 0U) {
        return false;
    }
    initial_max_packet = xhci_ep0_initial_max_packet(device);
    descriptor_max_packet =
        xhci_ep0_descriptor_max_packet(device, descriptor);
    if (initial_max_packet == 0U || descriptor_max_packet == 0U) {
        return false;
    }

    realtest_mark_number("XHCI_ADDRESS_SLOT", device->device_slot);
    realtest_mark_number("XHCI_EP0_MPS", descriptor_max_packet);
    if (descriptor_max_packet != initial_max_packet &&
        !xhci_evaluate_ep0_max_packet(state, device, descriptor)) {
        return false;
    }
    if (!xhci_prepare_address_update(state, device, descriptor) ||
        !xhci_submit_address_device(
            state, device->device_slot,
            xhci_dma_address(&device->input_context.mapping), false)) {
        realtest_mark_number("XHCI_ADDRESS_ERROR", xhci_last_error());
        return false;
    }
    return xhci_slot_is_addressed(state, device->device_slot);
}

/*
 * V3.10.1A 显式 EP0 设备上下文
 *
 * 控制器所有的状态：
 *   MMIO / 门铃 / 事件环
 *
 * 设备所有的状态：
 *   Slot ID / EP0 环 / EP0 入队位置 / 描述符缓冲区
 */
/*
 * V3.10.1F 已移除控制传输包装函数
 *
 * 每个控制传输调用方现在都显式指定设备上下文。
 * 此阶段仅改变 API 形式；旧调用方显式传入 &state->device，所有权语义保持不变。
 */
/*
 * 旧版工作上下文包装函数。
 *
 * V3.10.1A 刻意不修改任何现有调用方。
 * 后续变更会逐个子系统迁移到显式设备上下文辅助函数。
 */





static bool xhci_read_hub_descriptor(xhci_state_t *state) {
    uint8_t request[8] = {
        0xA0U, XHCI_SETUP_GET_DESCRIPTOR, 0x00U, XHCI_DESCRIPTOR_HUB,
        0x00U, 0x00U, 0x09U, 0x00U,
    };
    uint8_t descriptor_type = XHCI_DESCRIPTOR_HUB;
    uint8_t descriptor_length = 9U;
    uint8_t *descriptor;
    if (state == 0 || !state->hub_interface_present) return false;
    if (state->device_speed >= 4U) {
        descriptor_type = XHCI_DESCRIPTOR_SS_HUB;
        descriptor_length = 12U;
        request[3] = descriptor_type;
        request[6] = descriptor_length;
    }
    if (!xhci_submit_control_transfer_device(state, &state->device, request,
                                              descriptor_length, true)) {
        return false;
    }
    descriptor = (uint8_t *)state->descriptor_buffer.cpu;
    if (descriptor[0] < 3U || descriptor[1] != descriptor_type ||
        descriptor[2] == 0U || descriptor[2] > 15U) {
        xhci_set_error(68U);
        return false;
    }
    state->hub_port_count =
        descriptor[2];

    uint16_t hub_characteristics =
        (uint16_t)descriptor[3] |
        ((uint16_t)descriptor[4] << 8);

    state->hub_tt_think_time =
        (uint8_t)(
            (hub_characteristics >> 5) &
            0x03U);
    uint32_t power_good_ms = (uint32_t)descriptor[5] * 2U;

    /* Linux 会在查找子设备前为每个外部 Hub 端口上电，即使 Hub 报告不支持电源切换也如此。
     * 某些设备会模拟该请求，并且只有发送请求后才会报告连接状态。
     */
    uint32_t powered_ports = 0U;
    for (uint32_t port = 1U; port <= state->hub_port_count; ++port) {
        if (xhci_hub_set_port_feature_device(
                state, &state->device, (uint8_t)port,
                USB_HUB_FEATURE_PORT_POWER)) {
            ++powered_ports;
        }
    }

    if (power_good_ms < USB_HUB_MIN_POWER_GOOD_MS) {
        power_good_ms = USB_HUB_MIN_POWER_GOOD_MS;
    }
    kernel_serial_printf_serial_only(
        "OS_DIAG_HUB_POWER SLOT=%u PORTS=%u POWERED=%u DELAY_MS=%u\r\n",
        state->device.device_slot, state->hub_port_count, powered_ports,
        power_good_ms);
    xhci_controller_delay_ns((uint64_t)power_good_ms * 1000000ULL);

    state->hub_configured =
        true;
    return true;
}

static bool xhci_read_configuration_descriptor(xhci_state_t *state) {
    uint8_t request[8] = {
        0x80U, XHCI_SETUP_GET_DESCRIPTOR, 0x00U,
        XHCI_DESCRIPTOR_CONFIGURATION, 0x00U, 0x00U, 0x09U, 0x00U,
    };
    uint8_t *descriptor;
    uint32_t total_length;

    if (state == 0 ||
        !xhci_submit_control_transfer_device(
            state, &state->device, request, 9U, true)) {
        return false;
    }
    descriptor = (uint8_t *)state->descriptor_buffer.cpu;
    if (descriptor[0] < 9U ||
        descriptor[1] != XHCI_DESCRIPTOR_CONFIGURATION) {
        xhci_set_error(57U);
        return false;
    }
    total_length = (uint32_t)descriptor[2] |
                   ((uint32_t)descriptor[3] << 8);
    if (total_length < 9U || total_length > PAGE_SIZE) {
        xhci_set_error(57U);
        return false;
    }
    realtest_mark_number("XHCI_CONFIG_LENGTH", total_length);
    if (total_length == 9U) return true;

    request[6] = (uint8_t)total_length;
    request[7] = (uint8_t)(total_length >> 8);
    return xhci_submit_control_transfer_device(
        state, &state->device, request, total_length, true);
}

static bool xhci_parse_configuration(xhci_state_t *state) {
    uint8_t *descriptor;
    uint32_t total_length;
    bool auxiliary;
    bool hid_interface = false;
    uint8_t hid_candidate = 0U;
    uint8_t hid_count = 0U;

    bool hub_interface = false;
    bool bt_interface = false;
    bool msc_interface = false;
    bool uas_interface = false;
    uint8_t uas_last_endpoint = 0U;
    uint16_t uas_last_max_packet = 0U;
    uint8_t uas_last_max_burst = 0U;
    uint8_t uas_last_max_streams = 0U;
    if (state == 0) return false;
    auxiliary = state == xhci_hid_controller_state();
    descriptor = (uint8_t *)state->descriptor_buffer.cpu;
    if (descriptor[0] < 9U || descriptor[1] != XHCI_DESCRIPTOR_CONFIGURATION) {
        return false;
    }
    total_length = (uint32_t)descriptor[2] | ((uint32_t)descriptor[3] << 8);
    if (total_length > PAGE_SIZE) total_length = PAGE_SIZE;
    state->configuration_value = descriptor[5];
    state->hid_interface = 0U;
    state->hid_protocol = 0U;
    state->hid_endpoint = 0U;
    state->hid_max_packet = 0U;
    state->hid_interval = 0U;
    state->hid_secondary.interface_number = 0U;
    state->hid_secondary.protocol = 0U;
    state->hid_secondary.endpoint = 0U;
    state->hid_secondary.max_packet = 0U;
    state->hid_secondary.interval = 0U;
    state->audio_endpoint = 0U;
    state->audio_max_packet = 0U;
    state->audio_interval = 0U;
    state->audio_channels = 0U;
    state->audio_bit_resolution = 0U;
    state->audio_sample_rate = 0U;
    state->hub_interface = 0U;
    state->hub_interface_present = false;
    state->hub_port_count = 0U;
    state->hub_protocol = 0U;
    state->hub_tt_think_time = 0U;
    state->hub_configured = false;
    state->hub_endpoint = 0U;
    state->hub_max_packet = 0U;
    state->hub_interval = 0U;
    state->hub_enqueue = 0U;
    state->hub_cycle = 1U;
    state->hub_transfer_pending = false;
    state->hub_change_bitmap = 0U;
    state->msc_interface = 0U;
    state->msc_bulk_in_endpoint = 0U;
    state->msc_bulk_out_endpoint = 0U;
    state->msc_bulk_in_max_burst = 0U;
    state->msc_bulk_out_max_burst = 0U;
    state->msc_bulk_in_max_packet = 0U;
    state->msc_bulk_out_max_packet = 0U;
    state->msc_configured = false;
    state->msc_uas_present = false;
    state->msc_uas_interface = 0U;
    state->msc_uas_alternate = 0U;
    state->msc_uas_command_endpoint = 0U;
    state->msc_uas_status_endpoint = 0U;
    state->msc_uas_data_in_endpoint = 0U;
    state->msc_uas_data_out_endpoint = 0U;
    state->msc_uas_command_address = 0U;
    state->msc_uas_status_address = 0U;
    state->msc_uas_data_in_address = 0U;
    state->msc_uas_data_out_address = 0U;
    state->msc_uas_command_max_burst = 0U;
    state->msc_uas_status_max_burst = 0U;
    state->msc_uas_data_in_max_burst = 0U;
    state->msc_uas_data_out_max_burst = 0U;
    state->msc_uas_status_max_streams = 0U;
    state->msc_uas_data_in_max_streams = 0U;
    state->msc_uas_data_out_max_streams = 0U;
    state->msc_uas_command_max_packet = 0U;
    state->msc_uas_status_max_packet = 0U;
    state->msc_uas_data_in_max_packet = 0U;
    state->msc_uas_data_out_max_packet = 0U;
    state->bt_event_endpoint = 0U;
    state->bt_acl_in_endpoint = 0U;
    state->bt_acl_out_endpoint = 0U;
    state->bt_event_max_packet = 0U;
    state->bt_acl_in_max_packet = 0U;
    state->bt_acl_out_max_packet = 0U;
    state->bt_event_interval = 0U;
    for (uint32_t offset = 0U; offset + 2U <= total_length;) {
        uint8_t length = descriptor[offset];
        uint8_t type = descriptor[offset + 1U];
        if (length < 2U || offset + length > total_length) return false;
        if (type == XHCI_DESCRIPTOR_INTERFACE && length >= 9U) {
            uint8_t interface_class = descriptor[offset + 5U];
            uint8_t interface_subclass = descriptor[offset + 6U];
            uint8_t alternate = descriptor[offset + 3U];
            uint8_t interface_protocol = descriptor[offset + 7U];
            /* Boot protocol 为驱动提供固定报告格式，避免在中断快速路径中解析报告描述符。 */
            hid_interface = interface_class == USB_CLASS_HID &&
                            interface_subclass == USB_HID_SUBCLASS_BOOT &&
                            (interface_protocol == USB_HID_PROTOCOL_KEYBOARD ||
                             interface_protocol == USB_HID_PROTOCOL_MOUSE) &&
                            alternate == 0U;
            hid_candidate = 0U;
            if (hid_interface && hid_count < 2U) {
                hid_candidate = ++hid_count;
                if (hid_candidate == 1U) {
                    state->hid_interface = descriptor[offset + 2U];
                    state->hid_protocol = interface_protocol;
                } else {
                    state->hid_secondary.interface_number =
                        descriptor[offset + 2U];
                    state->hid_secondary.protocol = interface_protocol;
                }
            }
            hub_interface =
                interface_class == USB_CLASS_HUB &&
                alternate == 0U;
            bt_interface = interface_class == USB_CLASS_WIRELESS &&
                           interface_subclass == USB_BT_SUBCLASS &&
                           descriptor[offset + 7U] == USB_BT_PROTOCOL;
            msc_interface =
                interface_class == USB_CLASS_MASS_STORAGE &&
                interface_subclass == USB_MSC_SUBCLASS_SCSI &&
                interface_protocol == USB_MSC_PROTOCOL_BULK_ONLY &&
                alternate == 0U;
            uas_interface =
                interface_class == USB_CLASS_MASS_STORAGE &&
                interface_subclass == USB_MSC_SUBCLASS_SCSI &&
                interface_protocol == USB_MSC_PROTOCOL_UAS;
            uas_last_endpoint = 0U;
            uas_last_max_packet = 0U;
            uas_last_max_burst = 0U;
            uas_last_max_streams = 0U;
            if (msc_interface) {
                state->msc_interface = descriptor[offset + 2U];
            }
            if (uas_interface) {
                state->msc_uas_present = true;
                state->msc_uas_interface = descriptor[offset + 2U];
                state->msc_uas_alternate = alternate;
            }
            if (hub_interface) {
                state->hub_interface = descriptor[offset + 2U];
                state->hub_interface_present = true;
                state->hub_protocol = interface_protocol;
            }
        } else if (type == USB_SS_ENDPOINT_COMPANION && length >= 3U &&
                   uas_interface && uas_last_endpoint != 0U) {
            uas_last_max_burst = descriptor[offset + 2U];
            if (length >= 4U)
                uas_last_max_streams = descriptor[offset + 3U] & 0x1FU;
        } else if (type == USB_UAS_PIPE_USAGE && length >= 4U &&
                   uas_interface && uas_last_endpoint != 0U) {
            uint8_t pipe_id = descriptor[offset + 2U];
            switch (pipe_id) {
            case USB_UAS_PIPE_COMMAND:
                state->msc_uas_command_address = uas_last_endpoint;
                state->msc_uas_command_endpoint = uas_last_endpoint & 0x0FU;
                state->msc_uas_command_max_packet = uas_last_max_packet;
                state->msc_uas_command_max_burst = uas_last_max_burst;
                break;
            case USB_UAS_PIPE_STATUS:
                state->msc_uas_status_address = uas_last_endpoint;
                state->msc_uas_status_endpoint = uas_last_endpoint & 0x0FU;
                state->msc_uas_status_max_packet = uas_last_max_packet;
                state->msc_uas_status_max_burst = uas_last_max_burst;
                state->msc_uas_status_max_streams = uas_last_max_streams;
                break;
            case USB_UAS_PIPE_DATA_IN:
                state->msc_uas_data_in_address = uas_last_endpoint;
                state->msc_uas_data_in_endpoint = uas_last_endpoint & 0x0FU;
                state->msc_uas_data_in_max_packet = uas_last_max_packet;
                state->msc_uas_data_in_max_burst = uas_last_max_burst;
                state->msc_uas_data_in_max_streams = uas_last_max_streams;
                break;
            case USB_UAS_PIPE_DATA_OUT:
                state->msc_uas_data_out_address = uas_last_endpoint;
                state->msc_uas_data_out_endpoint = uas_last_endpoint & 0x0FU;
                state->msc_uas_data_out_max_packet = uas_last_max_packet;
                state->msc_uas_data_out_max_burst = uas_last_max_burst;
                state->msc_uas_data_out_max_streams = uas_last_max_streams;
                break;
            default:
                break;
            }
            uas_last_endpoint = 0U;
        } else if (type == XHCI_DESCRIPTOR_ENDPOINT && length >= 7U && hid_interface &&
                   (descriptor[offset + 2U] & 0x80U) != 0U &&
                   (descriptor[offset + 3U] & 0x03U) == 0x03U) {
            if (hid_candidate == 1U && state->hid_endpoint == 0U) {
                uint8_t endpoint = descriptor[offset + 2U] & 0x0FU;
                uint16_t max_packet = (uint16_t)descriptor[offset + 4U] |
                                      ((uint16_t)descriptor[offset + 5U] << 8);
                if (endpoint != 0U && max_packet != 0U && max_packet <= PAGE_SIZE) {
                    state->hid_endpoint = endpoint;
                    state->hid_max_packet = max_packet;
                    state->hid_interval = descriptor[offset + 6U];
                }
            } else if (hid_candidate == 2U &&
                       state->hid_secondary.endpoint == 0U) {
                uint8_t endpoint = descriptor[offset + 2U] & 0x0FU;
                uint16_t max_packet = (uint16_t)descriptor[offset + 4U] |
                                      ((uint16_t)descriptor[offset + 5U] << 8);
                if (endpoint != 0U && max_packet != 0U && max_packet <= PAGE_SIZE) {
                    state->hid_secondary.endpoint = endpoint;
                    state->hid_secondary.max_packet = max_packet;
                    state->hid_secondary.interval = descriptor[offset + 6U];
                }
            }
        } else if (type == XHCI_DESCRIPTOR_ENDPOINT && length >= 7U && hub_interface &&
                   (descriptor[offset + 2U] & 0x80U) != 0U &&
                   (descriptor[offset + 3U] & 0x03U) == USB_TRANSFER_INTERRUPT) {
            if (state->hub_endpoint == 0U) {
                state->hub_endpoint = descriptor[offset + 2U] & 0x0FU;
                state->hub_max_packet = (uint16_t)descriptor[offset + 4U] |
                                        ((uint16_t)descriptor[offset + 5U] << 8);
                state->hub_interval = descriptor[offset + 6U];
            }
        } else if (type == XHCI_DESCRIPTOR_ENDPOINT && length >= 7U &&
                   uas_interface &&
                   (descriptor[offset + 3U] & 0x03U) == USB_TRANSFER_BULK) {
            uas_last_endpoint = descriptor[offset + 2U];
            uas_last_max_packet = (uint16_t)descriptor[offset + 4U] |
                                  ((uint16_t)descriptor[offset + 5U] << 8);
            uas_last_max_burst = 0U;
        } else if (type == XHCI_DESCRIPTOR_ENDPOINT && length >= 7U &&
                   msc_interface &&
                   (descriptor[offset + 3U] & 0x03U) == USB_TRANSFER_BULK) {
            uint8_t endpoint = descriptor[offset + 2U] & 0x0FU;
            bool direction_in = (descriptor[offset + 2U] & 0x80U) != 0U;
            uint16_t max_packet =
                (uint16_t)descriptor[offset + 4U] |
                ((uint16_t)descriptor[offset + 5U] << 8);
            if (endpoint != 0U && max_packet != 0U) {
                if (direction_in && state->msc_bulk_in_endpoint == 0U) {
                    state->msc_bulk_in_endpoint = endpoint;
                    state->msc_bulk_in_max_packet = max_packet;
                    if (offset + length + 2U <= total_length &&
                        descriptor[offset + length + 1U] == 0x30U &&
                        descriptor[offset + length] >= 3U) {
                        state->msc_bulk_in_max_burst =
                            descriptor[offset + length + 2U];
                    }
                } else if (!direction_in &&
                           state->msc_bulk_out_endpoint == 0U) {
                    state->msc_bulk_out_endpoint = endpoint;
                    state->msc_bulk_out_max_packet = max_packet;
                    if (offset + length + 2U <= total_length &&
                        descriptor[offset + length + 1U] == 0x30U &&
                        descriptor[offset + length] >= 3U) {
                        state->msc_bulk_out_max_burst =
                            descriptor[offset + length + 2U];
                    }
                }
            }
        } else if (type == XHCI_DESCRIPTOR_ENDPOINT && length >= 7U && bt_interface) {
            uint8_t endpoint = descriptor[offset + 2U] & 0x0FU;
            uint8_t direction_in = descriptor[offset + 2U] & 0x80U;
            uint8_t transfer_type = descriptor[offset + 3U] & 0x03U;
            uint16_t max_packet = (uint16_t)descriptor[offset + 4U] |
                                  ((uint16_t)descriptor[offset + 5U] << 8);
            /* Alternate setting 0 可能公开零带宽等时端点。忽略这些端点，但始终继续处理下一个描述符；
             * 过去在此处继续循环会导致无限循环。 */
            if (endpoint != 0U && max_packet != 0U) {
                if (direction_in != 0U &&
                    transfer_type == USB_TRANSFER_INTERRUPT &&
                    state->bt_event_endpoint == 0U) {
                    state->bt_event_endpoint = endpoint;
                    state->bt_event_max_packet = max_packet;
                    state->bt_event_interval = descriptor[offset + 6U];
                } else if (direction_in != 0U &&
                           transfer_type == USB_TRANSFER_BULK &&
                           state->bt_acl_in_endpoint == 0U) {
                    state->bt_acl_in_endpoint = endpoint;
                    state->bt_acl_in_max_packet = max_packet;
                } else if (direction_in == 0U &&
                           transfer_type == USB_TRANSFER_BULK &&
                           state->bt_acl_out_endpoint == 0U) {
                    state->bt_acl_out_endpoint = endpoint;
                    state->bt_acl_out_max_packet = max_packet;
                }
            }
        }
        offset += length;
    }
    xhci_audio_descriptor_t audio;
    if (xhci_select_audio_descriptor(descriptor, total_length, &audio)) {
        state->audio_interface = audio.interface_number;
        state->audio_alt_setting = audio.alternate;
        state->audio_protocol = audio.protocol;
        state->audio_control_interface = audio.control_interface;
        state->audio_clock = audio.clock;
        state->audio_mute_unit=audio.mute_unit;
        state->audio_volume_unit=audio.volume_unit;
        state->audio_volume_channels=audio.volume_channels;
        state->audio_endpoint = audio.endpoint;
        state->audio_endpoint_in = false;
        state->audio_interval = audio.interval;
        state->audio_max_packet = audio.max_packet;
        state->audio_channels = audio.channels;
        state->audio_bit_resolution = audio.bits;
        state->audio_sample_rate = audio.rate;
    }
    if (state->msc_uas_present) {
        kernel_serial_printf_serial_only(
            "OS_UAS_DESC SLOT=%u ALT=%u CMD=%u STATUS=%u IN=%u OUT=%u "
            "MPS=%u/%u/%u/%u STREAMS=%u/%u/%u\r\n",
            state->device_slot, state->msc_uas_alternate,
            state->msc_uas_command_endpoint, state->msc_uas_status_endpoint,
            state->msc_uas_data_in_endpoint, state->msc_uas_data_out_endpoint,
            state->msc_uas_command_max_packet,
            state->msc_uas_status_max_packet,
            state->msc_uas_data_in_max_packet,
            state->msc_uas_data_out_max_packet,
            state->msc_uas_status_max_streams,
            state->msc_uas_data_in_max_streams,
            state->msc_uas_data_out_max_streams);
        realtest_mark_number("USB_MSC_UAS_DESC_INTERFACE",
                                    state->msc_uas_interface);
        realtest_mark_number("USB_MSC_UAS_DESC_ALTERNATE",
                                    state->msc_uas_alternate);
        realtest_mark_number("USB_MSC_UAS_DESC_COMMAND",
                                    state->msc_uas_command_endpoint);
        realtest_mark_number("USB_MSC_UAS_DESC_STATUS",
                                    state->msc_uas_status_endpoint);
        realtest_mark_number("USB_MSC_UAS_DESC_DATA_IN",
                                    state->msc_uas_data_in_endpoint);
        realtest_mark_number("USB_MSC_UAS_DESC_DATA_OUT",
                                    state->msc_uas_data_out_endpoint);
        realtest_mark_number("USB_MSC_UAS_DESC_COMMAND_MPS",
                                    state->msc_uas_command_max_packet);
        realtest_mark_number("USB_MSC_UAS_DESC_STATUS_MPS",
                                    state->msc_uas_status_max_packet);
        realtest_mark_number("USB_MSC_UAS_DESC_DATA_IN_MPS",
                                    state->msc_uas_data_in_max_packet);
        realtest_mark_number("USB_MSC_UAS_DESC_DATA_OUT_MPS",
                                    state->msc_uas_data_out_max_packet);
        realtest_mark_number("USB_MSC_UAS_DESC_STATUS_STREAMS",
                                    state->msc_uas_status_max_streams);
        realtest_mark_number("USB_MSC_UAS_DESC_DATA_IN_STREAMS",
                                    state->msc_uas_data_in_max_streams);
        realtest_mark_number("USB_MSC_UAS_DESC_DATA_OUT_STREAMS",
                                    state->msc_uas_data_out_max_streams);
    }
    if (auxiliary) {
        realtest_mark_number("XHCI_AUX_CONFIG_TOTAL_LENGTH",
                                    total_length);
        realtest_mark_number("XHCI_AUX_CONFIG_VALUE",
                                    state->configuration_value);
        realtest_mark_number("XHCI_AUX_HID_INTERFACE",
                                    state->hid_interface);
        realtest_mark_number("XHCI_AUX_HID_PROTOCOL",
                                    state->hid_protocol);
        realtest_mark_number("XHCI_AUX_HID_ENDPOINT",
                                    state->hid_endpoint);
        realtest_mark_number("XHCI_AUX_HID_MAX_PACKET",
                                    state->hid_max_packet);
        realtest_mark_number("XHCI_AUX_HID_INTERVAL",
                                    state->hid_interval);
        realtest_mark_number("XHCI_AUX_HID2_INTERFACE",
                                    state->hid_secondary.interface_number);
        realtest_mark_number("XHCI_AUX_HID2_PROTOCOL",
                                    state->hid_secondary.protocol);
        realtest_mark_number("XHCI_AUX_HID2_ENDPOINT",
                                    state->hid_secondary.endpoint);
        realtest_mark_number("XHCI_AUX_HID2_MAX_PACKET",
                                    state->hid_secondary.max_packet);
    }
    return ((state->hid_endpoint != 0U && state->hid_max_packet != 0U) ||
            (state->hid_secondary.endpoint != 0U &&
             state->hid_secondary.max_packet != 0U)) ||
           (state->audio_endpoint != 0U && state->audio_max_packet != 0U) ||
           state->hub_interface_present ||
           (state->msc_bulk_in_endpoint != 0U &&
            state->msc_bulk_out_endpoint != 0U &&
            state->msc_bulk_in_max_packet != 0U &&
            state->msc_bulk_out_max_packet != 0U) ||
           (state->msc_uas_present &&
            state->msc_uas_command_endpoint != 0U &&
            state->msc_uas_status_endpoint != 0U &&
            state->msc_uas_data_in_endpoint != 0U &&
            state->msc_uas_data_out_endpoint != 0U &&
            state->msc_uas_command_max_packet != 0U &&
            state->msc_uas_status_max_packet != 0U &&
            state->msc_uas_data_in_max_packet != 0U &&
            state->msc_uas_data_out_max_packet != 0U) ||
           (state->bt_event_endpoint != 0U &&
            (state->bt_acl_in_endpoint != 0U || state->bt_acl_out_endpoint != 0U));
}

/*
 * V3.10.1B 显式读取 Hub 状态
 *
 * Hub EP0 状态属于传入的设备上下文。
 */
/*
 * V3.10.1E 已移除 Hub 控制包装函数
 *
 * 所有 Hub 控制调用方现在都显式指定设备上下文。
 * 此阶段的运行时调用方仍传入 &state->device。
 */
/*
 * V3.9.6 初始 Hub Slot 遍历
 *
 * 初始下游枚举现在完全基于 Slot Table。
 *
 * 新发现的 Hub Slot 会在后续轮次处理，因此无需 inventory 即可支持 xHCI 支持的任意 Hub 嵌套。
 */
/*
 * V3.10.1D 启动阶段直接使用 Hub 上下文
 *
 * 启动阶段的 Hub 控制传输直接操作规范 Slot.context。
 *
 * 子设备枚举期间，state->device 保留给该子设备使用。
 */
/*
 * V3.10.6B10B4 Hub 运行时挂起屏障
 *
 * 只有在显式启动拓扑遍历达到稳定基线后，才允许存在 Hub interrupt-IN TD。
 * 这样可避免枚举期间的端口复位被当作运行时热插拔事件。
 */
/*
 * V3.10.6B10B8 启动事件排空
 *
 * Configure Endpoint 阶段会挂起 Hub 端点，使 QEMU 建立常规 NAK/重试所有权。
 * 初始下游探测随后会产生预期的 C_CONNECTION/C_ENABLE/C_RESET 通知。
 *
 * 向运行时开放 MSI-X 前先消费这些启动完成事件。常规事件处理程序执行与运行时相同的协调/ACK/重新挂起逻辑，
 * 因此 Hub 只有一套状态机。
 */


static uint8_t xhci_interrupt_interval(
    uint8_t speed,
    uint8_t b_interval)
{
    if(b_interval == 0U)
    {
        return 0U;
    }

    /*
    * 本驱动使用的 xHCI 速度 ID：
     *
    *   1 = 全速
    *   2 = 低速
    *   3 = 高速
    *   4 = 超高速
     */
    if(speed >= 3U)
    {
        uint8_t value =
            b_interval;

        if(value > 16U)
        {
            value =
                16U;
        }

        return
            (uint8_t)(
                value - 1U);
    }

    /*
    * 全速/低速设备的 bInterval 以 1 ms 帧为单位。
     *
    * 转换：
     *
    *     毫秒 -> 微帧
     *
    * 然后向下取整到最接近的 2 次幂，遵循成熟主机控制器驱动采用的 xHCI 调度规则。
     */
    uint32_t microframes =
        (uint32_t)b_interval *
        8U;

    uint8_t exponent =
        0U;

    while(exponent < 15U &&
          (1U <<
           (uint32_t)(
               exponent + 1U)) <=
              microframes)
    {
        ++exponent;
    }

    /*
    * FS/LS 周期端点的有效实用范围：
     *
    *   2^3 微帧 = 1 ms
    *   2^10 微帧 = 128 ms
     */
    if(exponent < 3U)
    {
        exponent =
            3U;
    }

    if(exponent > 10U)
    {
        exponent =
            10U;
    }

    return exponent;
}


void xhci_init_endpoint_context(xhci_state_t *state, uint32_t *endpoint,
                                uint8_t interval, uint8_t type,
                                uint16_t max_packet,
                                const dma_mapping_t *ring) {
    uint64_t address = xhci_dma_address(ring);
    uint8_t encoded_interval =
        interval;

    /*
    * 端点类型：
     *
    *   3 = 中断输出
    *   7 = 中断输入
     */
    if(type == 3U ||
       type == 7U)
    {
        encoded_interval =
            xhci_interrupt_interval(
                state->device_speed,
                interval);
    }

    xhci_endpoint_context_encode(endpoint, encoded_interval, type,
                                  max_packet, address);
    /* Linux 将 bulk 端点的 Average TRB Length 保持为零：初始化端点时尚不知道传输缓冲区大小。
     * 周期端点则使用包大小作为保守负载值。 */
    if (type == 3U || type == 7U)
        xhci_endpoint_context_set_max_esit_payload(endpoint, max_packet);
    if (type == 3U || type == 7U)
        xhci_endpoint_context_set_average_trb_length(endpoint, max_packet);
    else if (type == 4U && state != 0 && state->hci_version >= 0x0100U)
        xhci_endpoint_context_set_average_trb_length(endpoint, 8U);
}

/*
 * V3.10.5A 显式 BT 队列
 *
 * Bluetooth 端点/环生产者状态属于显式设备上下文。控制器 MMIO 状态仍保存在 xhci_state_t 中。
 */
static bool xhci_configure_hid_endpoint(xhci_state_t *state) {
    uint32_t endpoint_id;
    uint32_t secondary_endpoint_id = 0U;
    uint32_t last_endpoint_id;
    bool has_primary;
    bool has_secondary;
    if (state == 0 ||
        (state->hid_endpoint == 0U && state->hid_secondary.endpoint == 0U)) {
        return false;
    }
    if (state->hid_endpoint != 0U &&
        ((state->hid_protocol != USB_HID_PROTOCOL_KEYBOARD &&
          state->hid_protocol != USB_HID_PROTOCOL_MOUSE) ||
         state->hid_max_packet == 0U || state->hid_max_packet > PAGE_SIZE)) {
        return false;
    }
    if (state->hid_secondary.endpoint != 0U &&
        ((state->hid_secondary.protocol != USB_HID_PROTOCOL_KEYBOARD &&
          state->hid_secondary.protocol != USB_HID_PROTOCOL_MOUSE) ||
         state->hid_secondary.max_packet == 0U ||
         state->hid_secondary.max_packet > PAGE_SIZE)) {
        return false;
    }
    has_primary = state->hid_endpoint != 0U;
    has_secondary = state->hid_secondary.endpoint != 0U;
    endpoint_id = has_primary ?
        (uint32_t)state->hid_endpoint * 2U + 1U : 0U;
    if (has_primary && endpoint_id > 31U) return false;
    if (has_secondary) {
        secondary_endpoint_id =
            (uint32_t)state->hid_secondary.endpoint * 2U + 1U;
        if (secondary_endpoint_id > 31U ||
            secondary_endpoint_id == endpoint_id) return false;
    }
    /* Context Entries 表示最高启用 DCI，而不是首个 HID DCI。复合接收器通常将鼠标放在 DCI 3、
     * 键盘放在 DCI 7。若只声明 DCI 3，即使设置了 Add Context 标志，控制器也会忽略有效的键盘端点上下文。 */
    last_endpoint_id = endpoint_id > secondary_endpoint_id ?
        endpoint_id : secondary_endpoint_id;
    if (state == xhci_hid_controller_state()) {
        realtest_mark_number("XHCI_AUX_HID_CONTEXT_ENTRIES",
                                    last_endpoint_id);
    }
    if (state->hid_endpoint != 0U &&
        (!xhci_alloc_page(state, &state->hid_ring, DMA_BIDIRECTIONAL) ||
         !xhci_alloc_page(state, &state->hid_report, DMA_FROM_DEVICE))) {
        xhci_free_page(&state->hid_ring);
        xhci_free_page(&state->hid_report);
        return false;
    }
    if (has_secondary &&
        (!xhci_alloc_page(state, &state->hid_secondary.ring,
                          DMA_BIDIRECTIONAL) ||
         !xhci_alloc_page(state, &state->hid_secondary.report,
                          DMA_FROM_DEVICE))) {
        xhci_free_page(&state->hid_secondary.ring);
        xhci_free_page(&state->hid_secondary.report);
        xhci_free_page(&state->hid_ring);
        xhci_free_page(&state->hid_report);
        return false;
    }
    state->hid_enqueue = 0U;
    state->hid_cycle = 1U;
    state->hid_transfer_pending = false;
    uint8_t *input = (uint8_t *)state->input_context.cpu;
    uint32_t *control = (uint32_t *)input;
    uint32_t *input_slot = (uint32_t *)(input + state->context_size);
    uint32_t *output_slot = (uint32_t *)state->output_context.cpu;
    for (uint32_t i = 0; i < 4U; ++i) input_slot[i] = output_slot[i];
    input_slot[0] &= ~(0x1FU << 27);
    input_slot[0] |= last_endpoint_id << 27;
    control[0] = 0U;
    /* Add Context Flags 必须同时包含 Slot Context 和目标端点。 */
    control[1] = 1U;
    if (has_primary) control[1] |= 1U << endpoint_id;
    if (has_secondary) control[1] |= 1U << secondary_endpoint_id;
    if (has_primary) {
        uint32_t *endpoint = (uint32_t *)(input +
            state->context_size * (endpoint_id + 1U));
        xhci_init_endpoint_context(state, endpoint, state->hid_interval, 7U,
                                   state->hid_max_packet,
                                   &state->hid_ring.mapping);
        xhci_trb_t *ring = (xhci_trb_t *)state->hid_ring.cpu;
        ring[XHCI_RING_TRB_COUNT - 1U].parameter =
            xhci_dma_address(&state->hid_ring.mapping);
        ring[XHCI_RING_TRB_COUNT - 1U].control =
            (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
            XHCI_TRB_CYCLE | XHCI_TRB_LINK_TOGGLE_CYCLE;
    }
    if (has_secondary) {
        uint8_t *input_secondary = (uint8_t *)state->input_context.cpu;
        uint32_t *endpoint_secondary = (uint32_t *)(input_secondary +
            state->context_size * (secondary_endpoint_id + 1U));
        state->hid_secondary.enqueue = 0U;
        state->hid_secondary.cycle = 1U;
        state->hid_secondary.transfer_pending = false;
        xhci_init_endpoint_context(
            state, endpoint_secondary, state->hid_secondary.interval, 7U,
            state->hid_secondary.max_packet,
            &state->hid_secondary.ring.mapping);
        xhci_trb_t *secondary_ring =
            (xhci_trb_t *)state->hid_secondary.ring.cpu;
        secondary_ring[XHCI_RING_TRB_COUNT - 1U].parameter =
            xhci_dma_address(&state->hid_secondary.ring.mapping);
        secondary_ring[XHCI_RING_TRB_COUNT - 1U].control =
            (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
            XHCI_TRB_CYCLE | XHCI_TRB_LINK_TOGGLE_CYCLE;
    }
    dma_sync_for_device(&state->input_context.mapping);
    if (has_primary) {
        dma_sync_for_device(&state->hid_ring.mapping);
        dma_sync_for_device(&state->hid_report.mapping);
    }
    if (has_secondary) {
        dma_sync_for_device(&state->hid_secondary.ring.mapping);
        dma_sync_for_device(&state->hid_secondary.report.mapping);
    }
    if (!xhci_submit_command(state, XHCI_CONFIGURE_ENDPOINT_TYPE,
                             state->device_slot,
                             xhci_dma_address(&state->input_context.mapping), 0)) {
        if (has_primary) {
            xhci_free_page(&state->hid_ring);
            xhci_free_page(&state->hid_report);
        }
        if (has_secondary) {
            xhci_free_page(&state->hid_secondary.ring);
            xhci_free_page(&state->hid_secondary.report);
        }
        return false;
    }
    if (has_primary &&
        !xhci_queue_hid_report(state, &state->device)) return false;
    return !has_secondary ||
           xhci_queue_hid_report_secondary(state, &state->device);
}

static void xhci_enumeration_rollback(bool publish_topology,
                                       uint8_t slot,
                                       uint8_t parent_slot,
                                       uint8_t parent_port) {
    if (publish_topology) {
        xhci_unpublish_slot_device(slot, parent_slot, parent_port);
    }
}

static void xhci_mark_aux_enumeration_stage(bool auxiliary_hid,
                                             uint32_t stage) {
    if (auxiliary_hid) {
        realtest_mark_number("XHCI_AUX_ENUM_STAGE", stage);
    }
}

/* 保持常规枚举器现有的失败路径不变，同时使辅助 HID 路径独立于主 Slot Table。 */
#define xhci_unpublish_slot_device(slot, parent_slot, parent_port) \
    xhci_enumeration_rollback(publish_topology, slot, parent_slot, parent_port)

static bool xhci_enumerate_device_mode(xhci_state_t *state, uint8_t slot,
                                       uint8_t port, uint8_t speed,
                                       uint8_t root_port,
                                       uint8_t parent_slot,
                                       uint8_t parent_port,
                                       uint32_t route_string,
                                       bool publish_topology) {
    bool auxiliary_hid = !publish_topology;

    xhci_mark_aux_enumeration_stage(auxiliary_hid, 1U);
    if (auxiliary_hid) xhci_clear_error();

    /*
    * V3.7.2 枚举失败回滚
     *
    * 硬件枚举完全成功前，Slot Table 和 USB Core 对象可能已经创建。
     *
    * 所有失败出口都必须移除这些临时软件对象。硬件 Disable Slot 和 DMA 拆除仍由现有调用方负责。
     */


    /*
    * V3.5.2 绑定 USB CORE
     *
    * 将 xHCI Slot ID 绑定到 USB Core 设备。
     */


    /*
    * V3.5.3 同步 USB 描述符
     *
    * 将 xHCI 枚举结果写入 USB Core 对象。
     */

    usb_device_t *usb_dev = publish_topology ? usb_alloc_device(slot) : 0;


    if(usb_dev != 0)
    {
        usb_dev->speed =
            speed;

        usb_dev->parent_slot =
            parent_slot;

        usb_dev->parent_port =
            parent_port;


        /*
         * 设备标识。
         *
         * 这些字段稍后由描述符解析器填写。
         */
        usb_dev->device_class =
            0;

        usb_dev->subclass =
            0;

        usb_dev->protocol =
            0;
    }

    /*
    * V3.6.4 绑定 USB HUB 类
     *
    * Hub 设备拥有独立的类对象。
     */

    if(publish_topology && usb_dev != 0 &&
       usb_dev->device_class == 0x09)
    {
        usb_hub_init(
            slot,
            0);
    }






    /*
    * V3.2 Slot Table 镜像
     *
    * 保持旧的 inventory 路径不变。
    * 此处仅创建原生 Slot ID 视图。
     */
    /*
    * parent_slot/parent_port 仍表示软件拓扑。
     *
    * tt_slot/tt_port 描述 xHCI Slot Context DW2 所需的硬件分割事务域。
     */
    uint8_t tt_slot = 0U;
    uint8_t tt_port = 0U;
    bool tt_multi = false;
    if(!xhci_resolve_tt(
           speed,
           parent_slot,
           parent_port,
           &tt_slot,
           &tt_port,
           &tt_multi) ||
       (publish_topology && !xhci_topology_begin_slot(
           slot,
           speed,
           parent_slot,
           parent_port,
           root_port,
           route_string,
           tt_slot,
           tt_port,
           tt_multi)))
    {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);

        return false;
    }


    static const uint8_t get_device_descriptor_header[8] = {
        0x80U, XHCI_SETUP_GET_DESCRIPTOR, 0x00U, XHCI_DESCRIPTOR_DEVICE,
        0x00U, 0x00U, 0x08U, 0x00U
    };
    static const uint8_t get_device_descriptor[8] = {
        0x80U, XHCI_SETUP_GET_DESCRIPTOR, 0x00U, XHCI_DESCRIPTOR_DEVICE,
        0x00U, 0x00U, XHCI_DESCRIPTOR_DEVICE_LENGTH, 0x00U

    /* V3.2 Slot Table 镜像完成 */
};
    uint8_t set_configuration[8] = {0};
    uint8_t set_interface[8] = {0};
    /* 高速 EP0 已是 64 字节。AMD 辅助控制器一次收到完整描述符时更可靠；
     * 低速/全速设备仍需先探测 8 字节 MPS。 */
    bool probe_ep0_size = publish_topology || speed < 3U;
    if (!xhci_prepare_device_resources(state, slot, port, speed, root_port,
                                        parent_slot, parent_port,
                                        tt_slot,
                                        tt_port,
                                        tt_multi,
                                        route_string)) {
        xhci_mark_aux_enumeration_stage(auxiliary_hid, 2U);
        if (xhci_last_error() == 0U) xhci_set_error(53U);
        do {
            xhci_unpublish_slot_device(slot, parent_slot, parent_port);
            return false;
        } while (0);
    }
    xhci_mark_aux_enumeration_stage(auxiliary_hid, 2U);
    if (!xhci_submit_address_device(
            state, slot, xhci_dma_address(&state->input_context.mapping), true)) {
        xhci_mark_aux_enumeration_stage(auxiliary_hid, 3U);
        if (xhci_last_error() == 0U) xhci_set_error(55U);
        do {
            xhci_unpublish_slot_device(slot, parent_slot, parent_port);
            return false;
        } while (0);
    }
    xhci_mark_aux_enumeration_stage(auxiliary_hid, 3U);
    if (probe_ep0_size) {
        if (!xhci_submit_control_transfer_device(
                state, &state->device, get_device_descriptor_header,
                8U, true)) {
            xhci_mark_aux_enumeration_stage(auxiliary_hid, 4U);
            do {
                xhci_unpublish_slot_device(slot, parent_slot, parent_port);
                return false;
            } while (0);
        }
        xhci_mark_aux_enumeration_stage(auxiliary_hid, 4U);
        uint8_t *header = (uint8_t *)state->descriptor_buffer.cpu;
        if (auxiliary_hid) {
            realtest_mark_number("XHCI_AUX_EP0_MPS", header[7]);
            realtest_mark_number("XHCI_AUX_EP0_MPS_EFFECTIVE",
                xhci_ep0_descriptor_max_packet(&state->device, header));
        }
        /* 首个 Address Device 命令使用 BSR，使 EP0 能在 Slot 仍处于 Default 状态时读取描述符头。
         * 即使 bMaxPacketSize0 已与初始值匹配，也必须再次使用非 BSR 命令；该命令会分配 USB 地址，
         * 并将 Slot 转换到 Addressed 状态。 */
        if (!xhci_complete_device_address(state, &state->device, header)) {
            xhci_mark_aux_enumeration_stage(auxiliary_hid, 5U);
            do {
                xhci_unpublish_slot_device(slot, parent_slot, parent_port);
                return false;
            } while (0);
        }
        xhci_mark_aux_enumeration_stage(auxiliary_hid, 6U);
    }
    xhci_mark_aux_enumeration_stage(auxiliary_hid, 7U);
    if (!xhci_submit_control_transfer_device(state, &state->device,
                                              get_device_descriptor,
                                              XHCI_DESCRIPTOR_DEVICE_LENGTH,
                                              true)) {
        realtest_mark_number("XHCI_AUX_FULL_ERROR", xhci_last_error());
        do {
            xhci_unpublish_slot_device(slot, parent_slot, parent_port);
            return false;
        } while (0);
    }
    xhci_mark_aux_enumeration_stage(auxiliary_hid, 8U);
    uint8_t *descriptor = (uint8_t *)state->descriptor_buffer.cpu;
    if (descriptor[0] < XHCI_DESCRIPTOR_DEVICE_LENGTH ||
        descriptor[1] != XHCI_DESCRIPTOR_DEVICE) {
        xhci_set_error(56U);
        do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
    }
    if (publish_topology) {
        uint16_t vendor = (uint16_t)descriptor[8] |
                          ((uint16_t)descriptor[9] << 8);
        uint16_t product = (uint16_t)descriptor[10] |
                           ((uint16_t)descriptor[11] << 8);
        realtest_mark_number("XHCI_DEVICE_SLOT", slot);
        realtest_mark_number("XHCI_DEVICE_ROOT", root_port);
        realtest_mark_number("XHCI_DEVICE_VID", vendor);
        realtest_mark_number("XHCI_DEVICE_PID", product);
        realtest_mark_number("XHCI_DEVICE_CLASS", descriptor[4]);
        kernel_serial_printf_serial_only(
            "OS_DIAG_USB_DEVICE SLOT=%u ROOT=%u PARENT=%u PORT=%u "
            "VID=%u PID=%u\r\n",
            slot, root_port, parent_slot, parent_port, vendor, product);
    }
    if (auxiliary_hid && !probe_ep0_size &&
        !xhci_complete_device_address(state, &state->device, descriptor)) {
        xhci_mark_aux_enumeration_stage(auxiliary_hid, 9U);
        if (xhci_last_error() == 0U) xhci_set_error(58U);
        do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
    }
    xhci_mark_aux_enumeration_stage(auxiliary_hid, 9U);
    if (!xhci_read_configuration_descriptor(state) ||
        !xhci_parse_configuration(state)) {
        /* 保留 EP0 完成/超时错误码。替换为通用解析错误码会使真实硬件故障无法区分，也会掩盖控制器是否
         * 实际完成了请求。 */
        if (xhci_last_error() == 0U) xhci_set_error(57U);
        do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
    }
    set_configuration[0] = 0x00U;
    set_configuration[1] = XHCI_SETUP_SET_CONFIGURATION;
    set_configuration[2] = state->configuration_value;
    if (auxiliary_hid) {
        realtest_mark_number("XHCI_AUX_SET_CONFIGURATION",
                                    state->configuration_value);
    }
    if (!xhci_submit_control_transfer_device(state, &state->device,  set_configuration, 0U, false)) {
        if (auxiliary_hid) {
            realtest_mark_number("XHCI_AUX_SET_CONFIGURATION_ERROR",
                                        xhci_last_error());
        }
        xhci_set_error(58U);
        do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
    }
    /* Hub 绗竴闃舵鍙畬鎴愮被璇嗗埆鍜?Hub 鎻忚堪绗﹁鍙栵紱涓嬫父绔彛鐨?     * route string銆乻lot/context 闅旂浠ュ強绔彛浜嬩欢澶勭悊鍦ㄥ悗缁樁娈垫帴鍏ャ€?*/
    if (state->hub_interface_present) {
        if (!xhci_read_hub_descriptor(state)) do {
        kernel_serial_write_serial_only("OS_DIAG_HUB_DESCRIPTOR_FAIL\r\n");
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
        kernel_serial_write_serial_only("OS_DIAG_HUB_DESCRIPTOR_OK\r\n");

        /*
         * 启用 interrupt-IN 前先建立干净的 Hub 变化基线。现有下游设备会由
         * xhci_hub_runtime_probe_downstream() 显式发现，因此此处清除旧的 C_CONNECTION 状态不会漏掉设备。
         */
        if(!xhci_hub_ack_all_port_changes_device(
               state,
               &state->device))
        {
            kernel_serial_write_serial_only("OS_DIAG_HUB_ACK_FAIL\r\n");
            if(xhci_last_error() == 0U)
            {
                xhci_set_error(68U);
            }

            do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
        }
        kernel_serial_write_serial_only("OS_DIAG_HUB_ACK_OK\r\n");

        if (state->hub_endpoint != 0U && !xhci_configure_hub_endpoint(state)) {
            kernel_serial_write_serial_only(
                "OS_DIAG_HUB_CONFIGURE_FAIL\r\n");
            if (xhci_last_error() == 0U) xhci_set_error(68U);
            do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
        }
        kernel_serial_write_serial_only("OS_DIAG_HUB_CONFIGURE_OK\r\n");
    } else if (state->bt_event_endpoint != 0U) {
        if (!xhci_configure_bt_endpoints(state)) {
            if (xhci_last_error() == 0U) xhci_set_error(65U);
            do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
        }
    } else if (state->audio_endpoint != 0U) {
        /* UAC2 在时钟实体上设置频率；UAC1 使用端点采样频率控制。 */
        uint8_t frequency[8] = {0x22U, 1U, 0U, 1U,
            state->audio_endpoint, 0U, 3U, 0U};
        if (state->audio_protocol == 0x20U) {
            frequency[0] = 0x21U;
            frequency[4] = state->audio_control_interface;
            frequency[5] = state->audio_clock;
            frequency[6] = 4U;
        }
        uint8_t *rate = state->device.descriptor_buffer.cpu;
        rate[0] = 0x80U; rate[1] = 0xBBU; rate[2] = 0U; rate[3] = 0U;
        if (!xhci_submit_control_transfer_device(state, &state->device,
                frequency, frequency[6], false)) {
            xhci_set_error(59U);
            xhci_unpublish_slot_device(slot, parent_slot, parent_port);
            return false;
        }
        /* 耳机复位后可能保持硬件静音；只初始化播放链上明确可写的控制。 */
        uint8_t feature[8]={0x21U,1U,0U,1U,state->audio_control_interface,0U,1U,0U};
        if (state->audio_mute_unit) {
            feature[5]=state->audio_mute_unit;rate[0]=0U;
            if (!xhci_submit_control_transfer_device(state,&state->device,feature,1U,false)) {
                xhci_set_error(59U);xhci_unpublish_slot_device(slot,parent_slot,parent_port);return false;
            }
        }
        if (state->audio_volume_unit) {
            feature[3]=2U;feature[5]=state->audio_volume_unit;feature[6]=2U;
            for (uint8_t channel=0U;channel<8U;++channel) {
                if (!(state->audio_volume_channels&(1U<<channel))) continue;
                feature[2]=channel;rate[0]=rate[1]=0U;
                if (!xhci_submit_control_transfer_device(state,&state->device,feature,2U,false)) {
                    xhci_set_error(59U);xhci_unpublish_slot_device(slot,parent_slot,parent_port);return false;
                }
            }
        }
        set_interface[0] = 0x01U;
        set_interface[1] = XHCI_SETUP_SET_INTERFACE;
        set_interface[2] = state->audio_alt_setting;
        set_interface[4] = state->audio_interface;
        if (!xhci_submit_control_transfer_device(state, &state->device,  set_interface, 0U, false)) {
            xhci_set_error(59U);
            do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
        }
    }
    if (state->hid_endpoint != 0U ||
        state->hid_secondary.endpoint != 0U) {
        /* Boot 子类接口当前可能处于报告协议。为其固定格式的中断 TRB 启用前，先选择 Boot 协议。 */
        if (state->hid_endpoint != 0U) {
            uint8_t set_protocol[8] = {
                0x21U, USB_HID_SET_PROTOCOL, USB_HID_PROTOCOL_BOOT, 0x00U,
                state->hid_interface, 0x00U, 0x00U, 0x00U,
            };
            if (!xhci_submit_control_transfer_device(
                    state, &state->device, set_protocol, 0U, false)) {
                xhci_set_error(59U);
                do {
                    xhci_unpublish_slot_device(slot, parent_slot,
                                                parent_port);
                    return false;
                } while (0);
            }
        }
        if (state->hid_secondary.endpoint != 0U) {
            uint8_t secondary_set_protocol[8] = {
                0x21U, USB_HID_SET_PROTOCOL, USB_HID_PROTOCOL_BOOT, 0x00U,
                state->hid_secondary.interface_number, 0x00U, 0x00U, 0x00U,
            };
            if (!xhci_submit_control_transfer_device(
                    state, &state->device, secondary_set_protocol, 0U,
                    false)) {
                xhci_set_error(59U);
                do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
            }
        }
        if (!xhci_configure_hid_endpoint(state)) {
            if (xhci_last_error() == 0U) xhci_set_error(59U);
            do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
        }
    } else if ((state->msc_uas_present &&
                state->msc_uas_command_endpoint != 0U &&
                state->msc_uas_status_endpoint != 0U &&
                state->msc_uas_data_in_endpoint != 0U &&
                state->msc_uas_data_out_endpoint != 0U) ||
               (state->msc_bulk_in_endpoint != 0U &&
                state->msc_bulk_out_endpoint != 0U)) {
        if (!xhci_configure_msc_endpoints(state)) {
            if (xhci_last_error() == 0U) xhci_set_error(165U);
            do {
                xhci_unpublish_slot_device(slot, parent_slot, parent_port);
                return false;
            } while (0);
        }
    } else if (state->audio_endpoint != 0U) {
        if (!xhci_configure_audio_endpoint(state)) {
            if (xhci_last_error() == 0U) xhci_set_error(63U);
            do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
        }
    } else if (state->bt_event_endpoint != 0U) {
        /* Bluetooth 璁惧宸茬粡鍦ㄤ笂闈㈢殑澶氱鐐归厤缃矾寰勪腑瀹屾垚鍒濆鍖栥€?*/
    } else if (state->hub_configured) {
        /* Hub 绗竴闃舵娌℃湁渚涘唴鏍告暟鎹潰浣跨敤鐨勬櫘閫氭暟鎹鐐广€?*/
    } else {
        if (xhci_last_error() == 0U) xhci_set_error(59U);
        do {
        xhci_unpublish_slot_device(
            slot,
            parent_slot,
            parent_port);
        return false;
    } while (0);
    }


return true;
}

#undef xhci_unpublish_slot_device

bool xhci_enumerate_device(xhci_state_t *state, uint8_t slot,
                           uint8_t port, uint8_t speed,
                           uint8_t root_port, uint8_t parent_slot,
                           uint8_t parent_port, uint32_t route_string) {
    return xhci_enumerate_device_mode(state, slot, port, speed, root_port,
                                      parent_slot, parent_port, route_string,
                                      true);
}

bool xhci_enumerate_hid_device(xhci_state_t *state, uint8_t slot,
                               uint8_t port, uint8_t speed,
                               uint8_t root_port) {
    return xhci_enumerate_device_mode(state, slot, port, speed, root_port,
                                      0U, 0U, 0U, false);
}
