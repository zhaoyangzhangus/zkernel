#include "internal.h"

/* REFACTOR_P8_XHCI_AUDIO_RUNTIME_OWNER：音频 DMA/TRB 排队和端点 doorbell 提交
 * 仍由控制器侧直接所有者负责。 */

#define XHCI_RING_TRB_COUNT 256U
#define XHCI_LINK_TRB_TYPE 6U
#define XHCI_CONFIGURE_ENDPOINT_TYPE 12U
#define XHCI_TRB_CYCLE (1U << 0)
#define XHCI_TRB_INTERRUPT_ON_COMPLETION (1U << 5)
#define XHCI_TRB_SIA (1U << 31)
#define XHCI_TRB_LINK_TOGGLE_CYCLE (1U << 1)

bool xhci_configure_audio_endpoint(xhci_state_t *state) {
    uint32_t endpoint_id;
    uint32_t endpoint_type;
    /* 描述符和上下文先校验，拒绝时不分配 DMA，也不修改待提交的上下文。 */
    if (state == 0 || state->device_slot == 0U ||
        state->audio_endpoint == 0U || state->audio_endpoint > 15U ||
        state->audio_max_packet == 0U || state->audio_max_packet > PAGE_SIZE ||
        state->audio_interval == 0U ||
        (state->context_size != 32U && state->context_size != 64U) ||
        state->input_context.cpu == 0 || state->output_context.cpu == 0) return false;
    uint32_t interval = state->audio_interval - 1U;
    if (state->device_speed < 3U) interval += 3U;
    if (interval > 15U) return false;
    /* 保留旧配置及未释放映射的所有权，不能用一次新分配覆盖它们。 */
    if (state->audio_ring.page || state->audio_ring.cpu ||
        state->audio_ring.mapping.device || state->audio_buffer.page ||
        state->audio_buffer.cpu || state->audio_buffer.mapping.device) return false;
    endpoint_id = (uint32_t)state->audio_endpoint * 2U +
                  (state->audio_endpoint_in ? 1U : 0U);
    endpoint_type = state->audio_endpoint_in ? 5U : 1U;
    if (endpoint_id > 31U ||
        !xhci_alloc_page(state, &state->audio_ring, DMA_BIDIRECTIONAL) ||
        !xhci_alloc_page(state, &state->audio_buffer,
                         state->audio_endpoint_in ? DMA_FROM_DEVICE : DMA_TO_DEVICE)) {
        xhci_free_page(&state->audio_ring);
        xhci_free_page(&state->audio_buffer);
        return false;
    }
    state->audio_enqueue = 0U;
    state->audio_cycle = 1U;
    state->audio_transfer_pending = false;
    for (uint32_t i = 0; i < PAGE_SIZE; ++i) {
        ((uint8_t *)state->audio_buffer.cpu)[i] = 0U;
    }

    uint8_t *input = (uint8_t *)state->input_context.cpu;
    uint32_t *control = (uint32_t *)input;
    uint32_t *input_slot = (uint32_t *)(input + state->context_size);
    uint32_t *output_slot = (uint32_t *)state->output_context.cpu;
    uint32_t *endpoint = (uint32_t *)(input + state->context_size * (endpoint_id + 1U));
    for (uint32_t i = 0; i < 4U; ++i) input_slot[i] = output_slot[i];
    input_slot[0] &= ~(0x1FU << 27);
    input_slot[0] |= endpoint_id << 27;
    control[0] = 0U;
    control[1] = 1U | (1U << endpoint_id);
    endpoint[0] = interval << 16U;
    endpoint[1] = (endpoint_type << 3) |
                  ((uint32_t)state->audio_max_packet << 16);
    endpoint[2] = (uint32_t)xhci_dma_address(&state->audio_ring.mapping) | 1U;
    endpoint[3] = (uint32_t)(xhci_dma_address(&state->audio_ring.mapping) >> 32);
    endpoint[4] = state->audio_max_packet | ((uint32_t)state->audio_max_packet << 16U);
    xhci_trb_t *ring = (xhci_trb_t *)state->audio_ring.cpu;
    ring[XHCI_RING_TRB_COUNT - 1U].parameter =
        xhci_dma_address(&state->audio_ring.mapping);
    ring[XHCI_RING_TRB_COUNT - 1U].control =
        (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
        XHCI_TRB_CYCLE | XHCI_TRB_LINK_TOGGLE_CYCLE;
    dma_sync_for_device(&state->input_context.mapping);
    dma_sync_for_device(&state->audio_ring.mapping);
    dma_sync_for_device(&state->audio_buffer.mapping);
    if (!xhci_submit_command(state, XHCI_CONFIGURE_ENDPOINT_TYPE,
                             state->device_slot,
                             xhci_dma_address(&state->input_context.mapping), 0)) {
        xhci_free_page(&state->audio_ring);
        xhci_free_page(&state->audio_buffer);
        return false;
    }
    /* 端点配置完成后，等待统一音频服务提交第一个周期。 */
    return true;
}


bool xhci_queue_audio_transfer_device(
    xhci_state_t *state,
    xhci_device_context_t *device) {
    if (device == 0 || device->device_slot == 0U) return false;
    xhci_trb_t *ring;
    uint32_t index;
    uint32_t endpoint_id;
    uint32_t transfer_bytes;
    if (state == 0 || !state->initialized || state->mmio == 0 ||
        device->audio_enqueue >= XHCI_RING_TRB_COUNT - 1U ||
        device->audio_cycle > 1U || device->audio_ring.cpu == 0 ||
        device->audio_buffer.cpu == 0 || device->audio_transfer_pending ||
        device->audio_endpoint == 0U || device->audio_max_packet == 0U) return false;
    /* 只有应用持有的音频流可以提交 DMA，不再发送旧自测试静默包。 */
    if (device->audio_stream == 0 || !device->audio_stream_queued ||
        device->audio_frames == 0U) return false;
    uint64_t bytes = audio_stream_bytes_for_frames(device->audio_stream,
                                                   device->audio_frames);
    if (bytes == 0U || bytes > PAGE_SIZE) return false;
    transfer_bytes = (uint32_t)bytes;
    /* 每个服务间隔一个等时 TD，整个周期最后一包才通知完成。 */
    uint32_t interval = device->audio_interval - 1U;
    if (interval > 10U) return false;
    uint32_t frames = (device->device_speed >= 3U ? 6U : 48U) << interval;
    uint32_t packet_bytes = frames * device->audio_channels *
                            (device->audio_bit_resolution / 8U);
    if (!packet_bytes || packet_bytes > device->audio_max_packet) return false;
    uint32_t packets = (transfer_bytes + packet_bytes - 1U) / packet_bytes;
    if (!packets || packets >= XHCI_RING_TRB_COUNT - 1U) return false;
    endpoint_id = (uint32_t)device->audio_endpoint * 2U +
                  (device->audio_endpoint_in ? 1U : 0U);
    uint64_t buffer_address = xhci_dma_address(&device->audio_buffer.mapping);
    if (endpoint_id > 31U || buffer_address == 0U ||
        buffer_address > UINT64_MAX - (bytes - 1U)) return false;
    /* 所有可拒绝的参数先检查完，再复制音频和构造描述符。 */
    if (!device->audio_endpoint_in &&
        audio_stream_period_read(device->audio_stream, device->audio_period,
                                 device->audio_buffer.cpu, bytes) != K_OK) {
        return false;
    }
    ring = (xhci_trb_t *)device->audio_ring.cpu;
    uint32_t first = device->audio_enqueue;
    uint32_t first_cycle = device->audio_cycle;
    uint32_t cycle = first_cycle;
    index = first;
    uint32_t offset = 0U;
    for (uint32_t packet = 0U; packet < packets; ++packet) {
        uint32_t bytes = transfer_bytes - offset;
        if (bytes > packet_bytes) bytes = packet_bytes;
        uint32_t flags = XHCI_TRB_SIA;
        if (packet + 1U == packets) flags |= XHCI_TRB_INTERRUPT_ON_COMPLETION;
        /* 首包暂不交给硬件，避免控制器读取尚未准备完的后续包。 */
        xhci_trb_t encoded;
        if (!xhci_transfer_encode_isoch(&encoded,
                buffer_address + offset,
                bytes, flags, packet == 0U ? first_cycle ^ 1U :
                                             cycle)) return false;
        ring[index].parameter = encoded.parameter;
        ring[index].status = encoded.status;
        ring[index].control = encoded.control;
        offset += bytes;
        if (++index == XHCI_RING_TRB_COUNT - 1U) {
            ring[index].control = (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
                cycle | XHCI_TRB_LINK_TOGGLE_CYCLE;
            index = 0U;
            cycle ^= 1U;
        }
    }
    device->audio_enqueue = index;
    device->audio_cycle = (uint8_t)cycle;
    device->audio_transfer_first = (uint16_t)first;
    device->audio_transfer_packets = (uint16_t)packets;
    device->audio_transfer_generation = device->audio_stream_generation;
    device->audio_transfer_pending = true;
    dma_sync_for_device(&device->audio_buffer.mapping);
    dma_sync_for_device(&device->audio_ring.mapping);
    dma_wmb();
    /* 数据和整批描述符可见后，最后发布首包，再通知端点。 */
    ring[first].control = (ring[first].control & ~XHCI_TRB_CYCLE) | first_cycle;
    dma_sync_for_device(&device->audio_ring.mapping);
    dma_wmb();
    *(volatile uint32_t *)(state->mmio + state->doorbell_offset +
                           (uint32_t)device->device_slot * sizeof(uint32_t)) = endpoint_id;
    __asm__ volatile ("mfence" : : : "memory");
    return true;
}

/* Transfer Event Slot ID is the permanent Audio device identity.  Published
 * audio state is never routed through the enumeration scratch context. */
bool xhci_handle_audio_transfer_event(
    xhci_state_t *state,
    const xhci_trb_t *event) {
    uint8_t slot;
    xhci_slot_device_t *slot_dev;
    xhci_device_context_t *device;
    uint32_t endpoint_id;
    uint32_t expected_endpoint_id;
    uint32_t completion;

    if (state == 0 || event == 0 ||
        ((event->control >> XHCI_TRB_TYPE_SHIFT) & 0x3FU) !=
            XHCI_TRANSFER_EVENT_TYPE) {
        return false;
    }
    slot = (uint8_t)(event->control >> XHCI_TRB_SLOT_SHIFT);
    if (slot == 0U) return false;

    slot_dev = xhci_topology_slot(slot);
    if (!slot_dev->used || slot_dev->context.device_slot != slot ||
        slot_dev->context.audio_endpoint == 0U) {
        return false;
    }
    device = &slot_dev->context;
    endpoint_id = (event->control >> XHCI_TRB_ENDPOINT_SHIFT) & 0x1FU;
    expected_endpoint_id = (uint32_t)device->audio_endpoint * 2U +
                           (device->audio_endpoint_in ? 1U : 0U);
    if (endpoint_id != expected_endpoint_id) return false;

    completion = event->status >> XHCI_COMPLETION_SHIFT;
    /* 等时环暂时为空属于带宽通知，不是某个已提交 TD 的失败。 */
    if (completion == 14U || completion == 15U) return true;
    /* 只有当前周期的最后一个 TD 可以释放缓冲区；旧事件及中间包不能提前 ACK。 */
    if (!device->audio_transfer_pending) return true;
    /* 成功与错误都必须属于当前提交的 TD，拒绝旧周期及非 TRB 地址。 */
    uint64_t base = xhci_dma_address(&device->audio_ring.mapping);
    if (completion == 0U || (event->control & (1U << 2)) ||
        event->parameter < base) return true;
    uint64_t offset = event->parameter - base;
    if (offset % sizeof(xhci_trb_t) != 0U ||
        offset / sizeof(xhci_trb_t) >= XHCI_RING_TRB_COUNT - 1U) return true;
    uint32_t index = (uint32_t)(offset / sizeof(xhci_trb_t));
    uint32_t distance = (index + XHCI_RING_TRB_COUNT - 1U -
                         device->audio_transfer_first) % (XHCI_RING_TRB_COUNT - 1U);
    if (distance >= device->audio_transfer_packets) return true;
    if (completion == XHCI_COMPLETION_SUCCESS &&
        distance + 1U != device->audio_transfer_packets) return true;
    bool final_packet = distance + 1U == device->audio_transfer_packets;
    if (completion != XHCI_COMPLETION_SUCCESS && !final_packet) {
        /* 中间包失败不代表后续 DMA 已退出，错误保留到应用处理。 */
        if (device->audio_transfer_generation == device->audio_stream_generation) {
            device->audio_transfer_failed = true;
            xhci_set_error(64U + completion);
        }
        return true;
    }
    device->audio_transfer_pending = false;
    if (device->audio_transfer_generation != device->audio_stream_generation) {
        /* 旧 DMA 退出后才复用硬件缓冲；它不能 ACK 或覆盖新流的周期。 */
        if (completion == XHCI_COMPLETION_SUCCESS) ++device->audio_completed;
        if (device->audio_stream && device->audio_stream_running &&
            device->audio_stream_queued &&
            !xhci_queue_audio_transfer_device(state, device)) {
            device->audio_transfer_failed = true;
            device->audio_stream_queued = false;
        }
        return true;
    }
    if (completion == XHCI_COMPLETION_SUCCESS) {
        dma_sync_for_cpu(&device->audio_buffer.mapping);
        ++device->audio_completed;
        if (device->audio_stream != 0) {
            if (device->audio_endpoint_in && device->audio_stream_queued) {
                uint64_t bytes = audio_stream_bytes_for_frames(
                    device->audio_stream, device->audio_frames);
                if (bytes != 0U && bytes <= PAGE_SIZE) {
                    (void)audio_stream_period_write(
                        device->audio_stream, device->audio_period,
                        device->audio_buffer.cpu, bytes);
                }
            }
            device->audio_stream_queued = false;
        }
    } else if (completion != 0U) {
        device->audio_transfer_failed = true;
        device->audio_stream_queued = false;
        xhci_set_error(64U + completion);
    }
    return true;
}
