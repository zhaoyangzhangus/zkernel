#include "internal.h"

/* REFACTOR_P8_XHCI_EVENT_DISPATCH_OWNER：Event TRB 分类和协议移交与控制器生命周期/发布流程分离。 */

#define XHCI_PORT_STATUS_CHANGE_TYPE 34U
#define XHCI_PORT_REGISTER_BASE      0x400U
#define XHCI_PORT_REGISTER_STRIDE    0x10U
#define XHCI_RING_TRB_COUNT          256U

bool xhci_process_event_ring(xhci_state_t *state,
                             uint32_t budget,
                             const xhci_event_dispatch_ops_t *ops) {
    bool consumed = false;
    bool topology_event = false;
    bool hub_event = false;

    if (state == 0 || !state->initialized || ops == 0 ||
        ops->handle_root_port_event == 0 || ops->hub_runtime == 0) {
        return false;
    }
    for (uint32_t i = 0U; i < budget; ++i) {
        xhci_trb_t event;
        uint32_t type;

        if (!xhci_next_event(state, &event)) break;
        type = (event.control >> XHCI_TRB_TYPE_SHIFT) & 0x3FU;
        if (type == XHCI_PORT_STATUS_CHANGE_TYPE) {
            uint8_t port;

            port = (uint8_t)(event.parameter >> 24);
            if (port != 0U && port <= state->max_ports) {
                uint32_t port_offset =
                    state->operational_offset + XHCI_PORT_REGISTER_BASE +
                    (uint32_t)(port - 1U) * XHCI_PORT_REGISTER_STRIDE;
                uint32_t portsc = xhci_controller_read32(state, port_offset);

                (void)ops->handle_root_port_event(state, port, portsc);
                topology_event = true;
                consumed = true;
            }
        } else if (type == XHCI_TRANSFER_EVENT_TYPE &&
                   xhci_handle_bt_transfer_event(state, &event)) {
            consumed = true;
        } else if (type == XHCI_TRANSFER_EVENT_TYPE &&
                   xhci_hub_runtime_handle_transfer_event(state, &event)) {
            topology_event = true;
            hub_event = true;
            consumed = true;
        } else if (type == XHCI_TRANSFER_EVENT_TYPE &&
                   xhci_handle_hid_transfer_event(state, &event)) {
            consumed = true;
        } else if (type == XHCI_TRANSFER_EVENT_TYPE &&
                   xhci_handle_audio_transfer_event(state, &event)) {
            consumed = true;
        }
    }

    /* 仅在收到 xHCI 事件后协调 Hub 状态。 */
    if (topology_event && xhci_topology_hub_configured() &&
        xhci_hub_runtime_reconcile(state, ops->hub_runtime)) {
        consumed = true;
    }
    if (hub_event) {
        xhci_hub_runtime_rearm(state, ops->hub_runtime);
    }
    return consumed;
}

bool xhci_drain_startup_events(xhci_state_t *state,
                               const xhci_event_dispatch_ops_t *ops) {
    if (state == 0 || !state->initialized || ops == 0) return false;

    /* 启动拓扑处理有界；无变化的 Hub 状态 TD 会继续保持 NAK。 */
    for (uint32_t pass = 0U; pass < 32U; ++pass) {
        if (!xhci_event_pending(state)) return true;
        (void)xhci_process_event_ring(state, XHCI_RING_TRB_COUNT, ops);
    }
    return !xhci_event_pending(state);
}
