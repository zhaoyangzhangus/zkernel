#include <arch/x86_64/paging.h>
#include <kernel/xhci.h>
#include "internal.h"

/* REFACTOR_P8_XHCI_BLUETOOTH_OWNER：USB Bluetooth 传输和事件。 */

#define XHCI_RING_TRB_COUNT 256U
#define XHCI_CONFIGURE_ENDPOINT_TYPE 12U
#define XHCI_LINK_TRB_TYPE 6U
#define XHCI_TRB_TYPE_SHIFT 10U
#define XHCI_TRB_SLOT_SHIFT 24U
#define XHCI_TRB_ENDPOINT_SHIFT 16U
#define XHCI_TRB_INTERRUPT_ON_COMPLETION (1U << 5)
#define XHCI_TRB_LINK_TOGGLE_CYCLE (1U << 1)
#define XHCI_TRANSFER_EVENT_TYPE 32U
#define XHCI_COMPLETION_SHIFT 24U
#define XHCI_COMPLETION_SUCCESS 1U

typedef struct xhci_bt_transport {
    xhci_state_t *state;
    uint8_t slot;
} xhci_bt_transport_t;


static xhci_bt_transport_t
g_xhci_bt_transports[
    XHCI_MAX_SLOT_TABLE];
void xhci_bt_transport_release(uint8_t slot) {
    if (slot == 0U) return;
    __builtin_memset(&g_xhci_bt_transports[slot],
                     0,
                     sizeof(g_xhci_bt_transports[slot]));
}

static bool xhci_queue_bt_event_device(
    xhci_state_t *state,
    xhci_device_context_t *device) {
    if(device == 0 ||
       device->device_slot == 0U)
    {
        return false;
    }


    xhci_trb_t *ring;
    uint32_t endpoint_id;
    uint32_t index;
    if (state == 0 || device->bt_controller == 0 || device->bt_event_endpoint == 0U ||
        device->bt_event_ring.cpu == 0 || device->bt_event_buffer.cpu == 0 ||
        device->bt_event_transfer_pending) return false;
    endpoint_id = (uint32_t)device->bt_event_endpoint * 2U + 1U;
    index = device->bt_event_enqueue;
    if (endpoint_id > 31U || index >= XHCI_RING_TRB_COUNT - 1U ||
        device->bt_event_max_packet == 0U) return false;
    ring = (xhci_trb_t *)device->bt_event_ring.cpu;
    if (!xhci_transfer_encode_normal(
            &ring[index], xhci_dma_address(&device->bt_event_buffer.mapping),
            device->bt_event_max_packet,
            XHCI_TRB_INTERRUPT_ON_COMPLETION,
            device->bt_event_cycle)) return false;
    ++index;
    if (index == XHCI_RING_TRB_COUNT - 1U) {
        ring[XHCI_RING_TRB_COUNT - 1U].control =
            (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
            device->bt_event_cycle | XHCI_TRB_LINK_TOGGLE_CYCLE;
        device->bt_event_enqueue = 0U;
        device->bt_event_cycle ^= 1U;
    } else {
        device->bt_event_enqueue = index;
    }
    device->bt_event_transfer_pending = true;
    dma_sync_for_device(&device->bt_event_ring.mapping);
    dma_sync_for_device(&device->bt_event_buffer.mapping);
    dma_wmb();
    *(volatile uint32_t *)(state->mmio + state->doorbell_offset +
                           (uint32_t)device->device_slot * sizeof(uint32_t)) = endpoint_id;
    __asm__ volatile ("mfence" : : : "memory");
    return true;
}

/*
 * 兼容包装函数。
 *
 * V3.10.5A 阶段尚未迁移调用方。
 */
/*
 * V3.10.6B9F 已移除 BT 工作上下文包装函数
 *
 * Bluetooth 枚举现在显式使用 state->device。
 * Bluetooth 运行时路径仍通过 Slot ID 查找永久 Slot.context。
 */


static bool xhci_queue_bt_acl_in_device(
    xhci_state_t *state,
    xhci_device_context_t *device) {
    if(device == 0 ||
       device->device_slot == 0U)
    {
        return false;
    }


    xhci_trb_t *ring;
    uint32_t endpoint_id;
    uint32_t index;
    if (state == 0 || device->bt_controller == 0 || device->bt_acl_in_endpoint == 0U ||
        device->bt_acl_in_ring.cpu == 0 || device->bt_acl_in_buffer.cpu == 0 ||
        device->bt_acl_in_transfer_pending) return false;
    endpoint_id = (uint32_t)device->bt_acl_in_endpoint * 2U + 1U;
    index = device->bt_acl_in_enqueue;
    if (endpoint_id > 31U || index >= XHCI_RING_TRB_COUNT - 1U) return false;
    ring = (xhci_trb_t *)device->bt_acl_in_ring.cpu;
    if (!xhci_transfer_encode_normal(
            &ring[index], xhci_dma_address(&device->bt_acl_in_buffer.mapping),
            PAGE_SIZE, XHCI_TRB_INTERRUPT_ON_COMPLETION,
            device->bt_acl_in_cycle)) return false;
    ++index;
    if (index == XHCI_RING_TRB_COUNT - 1U) {
        ring[XHCI_RING_TRB_COUNT - 1U].control =
            (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
            device->bt_acl_in_cycle | XHCI_TRB_LINK_TOGGLE_CYCLE;
        device->bt_acl_in_enqueue = 0U;
        device->bt_acl_in_cycle ^= 1U;
    } else {
        device->bt_acl_in_enqueue = index;
    }
    device->bt_acl_in_transfer_pending = true;
    dma_sync_for_device(&device->bt_acl_in_ring.mapping);
    dma_sync_for_device(&device->bt_acl_in_buffer.mapping);
    dma_wmb();
    *(volatile uint32_t *)(state->mmio + state->doorbell_offset +
                           (uint32_t)device->device_slot * sizeof(uint32_t)) = endpoint_id;
    __asm__ volatile ("mfence" : : : "memory");
    return true;
}



static bool xhci_queue_bt_acl_out_device(
    xhci_state_t *state,
    xhci_device_context_t *device,
    size_t length) {
    if(device == 0 ||
       device->device_slot == 0U)
    {
        return false;
    }


    xhci_trb_t *ring;
    uint32_t endpoint_id;
    uint32_t index;
    if (state == 0 || device->bt_acl_out_endpoint == 0U ||
        device->bt_acl_out_ring.cpu == 0 || device->bt_acl_out_buffer.cpu == 0 ||
        device->bt_acl_out_buffer.mapping.device == 0 || length == 0U ||
        length > PAGE_SIZE || device->bt_acl_out_buffer.mapping.segment_count == 0U) {
        return false;
    }
    endpoint_id = (uint32_t)device->bt_acl_out_endpoint * 2U;
    index = device->bt_acl_out_enqueue;
    if (endpoint_id > 31U || index >= XHCI_RING_TRB_COUNT - 1U) return false;
    ring = (xhci_trb_t *)device->bt_acl_out_ring.cpu;
    if (!xhci_transfer_encode_normal(
            &ring[index], xhci_dma_address(&device->bt_acl_out_buffer.mapping),
            (uint32_t)length, XHCI_TRB_INTERRUPT_ON_COMPLETION,
            device->bt_acl_out_cycle)) return false;
    ++index;
    if (index == XHCI_RING_TRB_COUNT - 1U) {
        ring[XHCI_RING_TRB_COUNT - 1U].control =
            (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
            device->bt_acl_out_cycle | XHCI_TRB_LINK_TOGGLE_CYCLE;
        device->bt_acl_out_enqueue = 0U;
        device->bt_acl_out_cycle ^= 1U;
    } else {
        device->bt_acl_out_enqueue = index;
    }
    device->bt_acl_out_transfer_pending = true;
    dma_sync_for_device(&device->bt_acl_out_ring.mapping);
    dma_sync_for_device(&device->bt_acl_out_buffer.mapping);
    dma_wmb();
    *(volatile uint32_t *)(state->mmio + state->doorbell_offset +
                           (uint32_t)device->device_slot * sizeof(uint32_t)) = endpoint_id;
    __asm__ volatile ("mfence" : : : "memory");
    return true;
}


/*
 * V3.10.6B7B 已移除 BT ACL-OUT 包装函数
 */


/*
 * 使用当前安装在 state->device 中的上下文发送。
 *
 * 下方公开的 Bluetooth 回调先解析 Slot ID，并在必要时临时安装所属上下文。
 */
/*
 * V3.10.6B7B BT 显式发送核心
 *
 * 所有 Bluetooth TX 状态均属于传入的设备上下文。
 */
static kstatus_t xhci_bt_send_device(
    xhci_state_t *state,
    xhci_device_context_t *device,
    const uint8_t *packet,
    size_t length)
{
    uint8_t setup[8] = {
        0x20U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U
    };

    if(state == 0 ||
       device == 0 ||
       device->device_slot == 0U ||
       device->bt_controller == 0 ||
       packet == 0 ||
       length < 4U ||
       length - 1U > PAGE_SIZE)
    {
        return K_EINVAL;
    }

    /*
    * HCI ACL 数据包 -> USB Bulk OUT。
     */
    if(packet[0] == 0x02U)
    {
        if(device->bt_acl_out_endpoint == 0U ||
           device->bt_acl_out_transfer_pending ||
           device->bt_acl_out_buffer.cpu == 0 ||
           length - 1U > PAGE_SIZE)
        {
            return K_EBUSY;
        }

        for(size_t i = 1U;
            i < length;
            ++i)
        {
            ((uint8_t *)
                device->
                    bt_acl_out_buffer.cpu)
                [i - 1U] =
                    packet[i];
        }

        return
            xhci_queue_bt_acl_out_device(
                state,
                device,
                length - 1U)
            ? K_OK
            : K_EIO;
    }

    /*
    * HCI Command 数据包 -> USB 类控制传输。
     */
    if(packet[0] != 0x01U ||
       device->descriptor_buffer.cpu == 0)
    {
        return K_EINVAL;
    }

    for(size_t i = 1U;
        i < length;
        ++i)
    {
        ((uint8_t *)
            device->
                descriptor_buffer.cpu)
            [i - 1U] =
                packet[i];
    }

    setup[6] =
        (uint8_t)(
            (length - 1U) &
            0xFFU);

    setup[7] =
        (uint8_t)(
            (length - 1U) >>
            8);

    dma_sync_for_device(
        &device->
            descriptor_buffer.mapping);

    return
        xhci_submit_control_transfer_device(
            state,
            device,
            setup,
            (uint32_t)(
                length - 1U),
            false)
        ? K_OK
        : K_EIO;
}


/*
 * V3.7.7 BT Slot 传输
 *
 * Bluetooth Core -> xHCI 的传输现在通过 bt_controller 创建时永久绑定的 Slot ID 路由。
 */
/*
 * V3.10.6B7B 规范 BT 发送路径
 *
 * Bluetooth 传输永久绑定到一个硬件 Slot：
 *
 *     transport->slot
 *           |
 *           v
 *      Slot.context
 *           |
 *           v
 *   xhci_bt_send_device()
 *
 * 不查找 inventory。
 * 不激活 state->device。
 */
static kstatus_t xhci_bt_send(
    void *context,
    const uint8_t *packet,
    size_t length)
{
    xhci_bt_transport_t *transport =
        (xhci_bt_transport_t *)context;

    if(transport == 0 ||
       transport->state == 0 ||
       transport->slot == 0U)
    {
        return K_EINVAL;
    }

    xhci_state_t *state =
        transport->state;

    uint8_t slot =
        transport->slot;

    xhci_slot_device_t *slot_dev =
        xhci_topology_slot(slot);

    if(!slot_dev->used ||
       slot_dev->context.device_slot !=
           slot ||
       slot_dev->context.bt_controller ==
           0)
    {
        return K_EIO;
    }

    return
        xhci_bt_send_device(
            state,
            &slot_dev->context,
            packet,
            length);
}



bool xhci_configure_bt_endpoints(xhci_state_t *state) {
    uint32_t event_id;
    uint32_t acl_in_id;
    uint32_t acl_out_id;
    uint32_t *input;
    uint32_t *control;
    uint32_t *input_slot;
    uint32_t *output_slot;
    if (state == 0 || state->bt_event_endpoint == 0U || state->bt_event_max_packet == 0U ||
        (state->bt_acl_in_endpoint == 0U && state->bt_acl_out_endpoint == 0U)) return false;
    event_id = (uint32_t)state->bt_event_endpoint * 2U + 1U;
    acl_in_id = state->bt_acl_in_endpoint == 0U ? 0U :
                (uint32_t)state->bt_acl_in_endpoint * 2U + 1U;
    acl_out_id = state->bt_acl_out_endpoint == 0U ? 0U :
                 (uint32_t)state->bt_acl_out_endpoint * 2U;
    if (event_id > 31U || (acl_in_id != 0U && acl_in_id > 31U) ||
        (acl_out_id != 0U && acl_out_id > 31U) || event_id == acl_in_id ||
        event_id == acl_out_id || (acl_in_id != 0U && acl_in_id == acl_out_id)) return false;
    if (!xhci_alloc_page(state, &state->bt_event_ring, DMA_BIDIRECTIONAL) ||
        !xhci_alloc_page(state, &state->bt_event_buffer, DMA_FROM_DEVICE) ||
        (acl_in_id != 0U &&
         (!xhci_alloc_page(state, &state->bt_acl_in_ring, DMA_BIDIRECTIONAL) ||
          !xhci_alloc_page(state, &state->bt_acl_in_buffer, DMA_FROM_DEVICE))) ||
    (acl_out_id != 0U &&
     (!xhci_alloc_page(state, &state->bt_acl_out_ring, DMA_BIDIRECTIONAL) ||
      !xhci_alloc_page(state, &state->bt_acl_out_buffer, DMA_TO_DEVICE)))) return false;
    xhci_ring_init_link(state->bt_event_ring.cpu,
                        xhci_dma_address(&state->bt_event_ring.mapping),
                        XHCI_RING_TRB_COUNT);
    xhci_ring_init_link(state->bt_acl_in_ring.cpu,
                        xhci_dma_address(&state->bt_acl_in_ring.mapping),
                        XHCI_RING_TRB_COUNT);
    xhci_ring_init_link(state->bt_acl_out_ring.cpu,
                        xhci_dma_address(&state->bt_acl_out_ring.mapping),
                        XHCI_RING_TRB_COUNT);
    input = (uint32_t *)state->input_context.cpu;
    control = input;
    input_slot = (uint32_t *)((uint8_t *)input + state->context_size);
    output_slot = (uint32_t *)state->output_context.cpu;
    for (uint32_t i = 0U; i < 4U; ++i) input_slot[i] = output_slot[i];
    uint32_t context_entries = event_id;
    if (acl_in_id > context_entries) context_entries = acl_in_id;
    if (acl_out_id > context_entries) context_entries = acl_out_id;
    input_slot[0] &= ~(0x1FU << 27);
    input_slot[0] |= context_entries << 27;
    control[0] = 0U;
    control[1] = 1U | (1U << event_id) |
                 (acl_in_id == 0U ? 0U : 1U << acl_in_id) |
                 (acl_out_id == 0U ? 0U : 1U << acl_out_id);
    xhci_init_endpoint_context(state,
        (uint32_t *)((uint8_t *)input + state->context_size * (event_id + 1U)),
        state->bt_event_interval == 0U ? 1U : state->bt_event_interval, 7U,
        state->bt_event_max_packet, &state->bt_event_ring.mapping);
    if (acl_in_id != 0U) {
        xhci_init_endpoint_context(state,
            (uint32_t *)((uint8_t *)input + state->context_size * (acl_in_id + 1U)),
            0U, 6U, state->bt_acl_in_max_packet, &state->bt_acl_in_ring.mapping);
    }
    if (acl_out_id != 0U) {
        xhci_init_endpoint_context(state,
            (uint32_t *)((uint8_t *)input + state->context_size * (acl_out_id + 1U)),
            0U, 2U, state->bt_acl_out_max_packet, &state->bt_acl_out_ring.mapping);
    }
    /*
    * 在 Bluetooth Core 收到回调指针前，将此 Bluetooth 控制器永久绑定到其硬件 Slot。
     */
    if(state->device_slot == 0U)
    {
        return false;
    }


    xhci_bt_transport_t *transport =
        &g_xhci_bt_transports[
            state->device_slot];


    transport->state =
        state;

    transport->slot =
        state->device_slot;


    dma_sync_for_device(
        &state->input_context.mapping);
    if (!xhci_submit_command(state, XHCI_CONFIGURE_ENDPOINT_TYPE,
                             state->device_slot,
                             xhci_dma_address(&state->input_context.mapping), 0) ||
        bt_controller_create(
            xhci_bt_send,
            transport,
            &state->bt_controller) != K_OK ||
        bt_controller_start(state->bt_controller) != K_OK ||
        !xhci_queue_bt_event_device(
            state,
            &state->device) ||
        (acl_in_id != 0U && !xhci_queue_bt_acl_in_device(
            state,
            &state->device))) return false;
    return true;
}


/*
 * V3.10.6B7C 规范 BT 传输事件
 *
 * Transfer Event 的 Slot ID 是永久 Bluetooth 标识。
 *
 *     传输事件
 *          |
 *        Slot 标识
 *          |
 *          v
 *     Slot.context
 *          |
 *     +----+----+
 *     |         |
 * 完成回调    重新布置
 *
 * 已发布的 Bluetooth 运行时状态不会进入 state->device。
 * BT 事件路由不使用 inventory[]。
 */
bool xhci_handle_bt_transfer_event(
    xhci_state_t *state,
    const xhci_trb_t *event)
{
    if(state == 0 ||
       event == 0 ||
       ((event->control >>
         XHCI_TRB_TYPE_SHIFT) &
        0x3FU) !=
           XHCI_TRANSFER_EVENT_TYPE)
    {
        return false;
    }


    uint8_t slot =
        (uint8_t)(
            event->control >>
            XHCI_TRB_SLOT_SHIFT);


    if(slot == 0U)
    {
        return false;
    }


    xhci_slot_device_t *slot_dev =
        xhci_topology_slot(slot);


    /*
    * 只有已完整发布的规范 Bluetooth 上下文才拥有运行时 Transfer Event。
     */
    if(!slot_dev->used ||
       slot_dev->context.device_slot !=
           slot ||
       slot_dev->context.bt_controller ==
           0)
    {
        return false;
    }


    xhci_device_context_t *device =
        &slot_dev->context;


    uint32_t endpoint_id =
        (event->control >>
         XHCI_TRB_ENDPOINT_SHIFT) &
        0x1FU;


    uint32_t completion =
        event->status >>
        XHCI_COMPLETION_SHIFT;


    uint32_t residual =
        event->status &
        0x00FFFFFFU;


    /*
    * HCI Event interrupt-IN。
     */
    if(device->bt_event_endpoint !=
           0U &&
       endpoint_id ==
           (uint32_t)
               device->
                   bt_event_endpoint *
               2U + 1U)
    {
        device->bt_event_transfer_pending =
            false;


        if(completion ==
               XHCI_COMPLETION_SUCCESS &&
           residual <=
               device->
                   bt_event_max_packet)
        {
            size_t length =
                device->
                    bt_event_max_packet -
                residual;


            dma_sync_for_cpu(
                &device->
                    bt_event_buffer.mapping);


            if(length != 0U)
            {
                (void)bt_hci_receive_event(
                    device->bt_controller,
                    device->
                        bt_event_buffer.cpu,
                    length);
            }


            (void)xhci_queue_bt_event_device(
                state,
                device);
        }
        else
        {
            xhci_set_error(70U + completion);
        }


        return true;
    }


    /*
    * ACL bulk-IN。
     */
    if(device->bt_acl_in_endpoint !=
           0U &&
       endpoint_id ==
           (uint32_t)
               device->
                   bt_acl_in_endpoint *
               2U + 1U)
    {
        device->bt_acl_in_transfer_pending =
            false;


        if(completion ==
               XHCI_COMPLETION_SUCCESS &&
           residual <=
               PAGE_SIZE)
        {
            size_t length =
                PAGE_SIZE -
                residual;


            dma_sync_for_cpu(
                &device->
                    bt_acl_in_buffer.mapping);


            if(length != 0U)
            {
                (void)bt_acl_receive(
                    device->bt_controller,
                    device->
                        bt_acl_in_buffer.cpu,
                    length);
            }


            (void)xhci_queue_bt_acl_in_device(
                state,
                device);
        }
        else
        {
            xhci_set_error(71U + completion);
        }


        return true;
    }


    /*
    * ACL bulk-OUT 完成。
     */
    if(device->bt_acl_out_endpoint !=
           0U &&
       endpoint_id ==
           (uint32_t)
               device->
                   bt_acl_out_endpoint *
               2U)
    {
        device->bt_acl_out_transfer_pending =
            false;


        if(completion !=
           XHCI_COMPLETION_SUCCESS)
        {
            xhci_set_error(72U + completion);
        }


        return true;
    }


    return false;
}
