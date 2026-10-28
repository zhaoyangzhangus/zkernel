#include "internal.h"

/* REFACTOR_P8_XHCI_EVENT_RUNTIME_OWNER：Event Ring 消费、延迟事件交接和
 * 中断器完成处理由同一个底层所有者负责。 */

#define XHCI_RING_TRB_COUNT 256U
#define XHCI_TRB_CYCLE (1U << 0)
#define XHCI_RUNTIME_ERDP 0x18U
#define XHCI_RUNTIME_INTR0 0x20U
#define XHCI_ERDP_EHB (1ULL << 3)
#define XHCI_IMAN_IP (1U << 0)
#define XHCI_IMAN_IE (1U << 1)

bool xhci_event_pending(const xhci_state_t *state) {
    const volatile xhci_trb_t *event_ring;
    uint32_t control;
    if (state == 0 || !state->initialized) return false;
    if (state->deferred_event_count != 0U) return true;
    event_ring = (const volatile xhci_trb_t *)state->event_ring.cpu;
    if (event_ring == 0) return false;
    dma_sync_for_cpu((dma_mapping_t *)&state->event_ring.mapping);
    control = event_ring[state->event_index].control;
    return (control & XHCI_TRB_CYCLE) == state->event_cycle;
}

void xhci_event_handler_complete(xhci_state_t *state) {
    uint64_t dequeue;
    if (state == 0 || !state->initialized) return;
    dequeue = xhci_dma_address(&state->event_ring.mapping) +
              (uint64_t)state->event_index * sizeof(xhci_trb_t);
    /* EHB 为 RW1C：软件消费一批事件后必须写 1 清除控制器的事件处理忙状态。
     * 在 QEMU 上只写 ERDP 而不写 EHB 会使该位保持置位，并屏蔽后续 HID 中断。 */
    (void)xhci_controller_write64(state, state->runtime_offset + XHCI_RUNTIME_INTR0 +
                       XHCI_RUNTIME_ERDP, dequeue | XHCI_ERDP_EHB);
    (void)xhci_controller_write32(state, state->runtime_offset + XHCI_RUNTIME_INTR0,
                       XHCI_IMAN_IP | XHCI_IMAN_IE);
}



bool xhci_next_ring_event(xhci_state_t *state, xhci_trb_t *event) {
    const volatile xhci_trb_t *event_ring;
    uint32_t control;
    if (state == 0 || event == 0) return false;
    /*
    * xHCI 事件环归控制器所有。软件只能通过推进 ERDP 消费事件，绝不能清除或改写
    * Event TRB。清除 cycle 位会在消费者周期状态回绕时让空的旧条目看起来有效，
    * 此后 HID 完成事件将永久丢失。
     */
    event_ring = (const volatile xhci_trb_t *)state->event_ring.cpu;
    dma_sync_for_cpu(&state->event_ring.mapping);
    control = event_ring[state->event_index].control;
    if ((control & XHCI_TRB_CYCLE) != state->event_cycle) {
        /* 合并唤醒后事件环为空是正常情况。 */
        return false;
    }
    event->parameter = event_ring[state->event_index].parameter;
    event->status = event_ring[state->event_index].status;
    event->control = control;
    state->event_index = (state->event_index + 1U) % XHCI_RING_TRB_COUNT;
    if (state->event_index == 0) state->event_cycle ^= 1U;
    uint64_t event_dequeue = xhci_dma_address(&state->event_ring.mapping) +
                             (uint64_t)state->event_index * sizeof(xhci_trb_t);

    /*
    * 推进 ERDP 以记账环空间，但不要为每个 Event TRB 单独清除 EHB。
     *
    * EHB 属于整个事件处理批次。在此清除可能导致控制器在当前延迟工作线程仍在排空
    * 同一事件环时再次生成 MSI-X。
     *
    * xhci_event_handler_complete() 会在批次结束后恰好清除一次 EHB。
     */
    (void)xhci_controller_write64(state, state->runtime_offset + XHCI_RUNTIME_INTR0 +
                       XHCI_RUNTIME_ERDP, event_dequeue);
    return true;
}

bool xhci_defer_event(xhci_state_t *state, const xhci_trb_t *event) {
    if (state == 0 || event == 0 ||
        !xhci_event_queue_push(state->deferred_events,
                               &state->deferred_event_tail,
                               &state->deferred_event_count, event)) {
        if (state != 0) xhci_set_error(28U);
        return false;
    }
    return true;
}

/* 统一事件消费者优先处理同步等待阶段暂存的事件，再读取硬件事件环。 */
bool xhci_next_event(xhci_state_t *state, xhci_trb_t *event) {
    if (state == 0 || event == 0) return false;
    if (xhci_event_queue_pop(state->deferred_events,
                             &state->deferred_event_head,
                             &state->deferred_event_count, event)) return true;
    return xhci_next_ring_event(state, event);
}
