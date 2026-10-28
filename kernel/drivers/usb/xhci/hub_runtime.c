#include "internal.h"

#include <arch/x86_64/paging.h>
#include <kernel/console.h>
#include <usb/hub.h>

/* REFACTOR_P8_XHCI_HUB_RUNTIME_OWNER：Hub 运行时事件和重新挂起状态。 */

#define XHCI_HUB_RESET_TIMEOUT_NS 1000000000ULL
#define XHCI_HUB_RESET_POLL_NS       1000000ULL
#define XHCI_HUB_RESET_MAX_POLLS          1000U

static bool xhci_hub_reset_port(
    xhci_state_t *state,
    xhci_device_context_t *hub,
    uint8_t port,
    uint32_t *status_out)
{
    if(state == 0 || hub == 0 || status_out == 0 || port == 0U ||
       !xhci_hub_set_port_feature_device(
           state, hub, port, USB_HUB_FEATURE_PORT_RESET))
    {
        return false;
    }

    uint64_t deadline =
        xhci_controller_timeout_deadline(XHCI_HUB_RESET_TIMEOUT_NS);
    for(uint32_t poll = 0U; poll < XHCI_HUB_RESET_MAX_POLLS; ++poll)
    {
        uint32_t status = 0U;
        if(xhci_hub_get_port_status_device(state, hub, port, &status))
        {
            *status_out = status;
            if((status & 1U) != 0U && (status & 2U) != 0U)
            {
                return true;
            }
        }

        if(xhci_controller_timeout_reached(deadline))
        {
            break;
        }
        xhci_controller_delay_ns(XHCI_HUB_RESET_POLL_NS);
    }
    return false;
}

/*
 * Transfer Event 标识永久 Hub Slot。因此运行时 Hub 状态更新到 Slot.context；
 * 枚举期间 state->device 仍保留给尚未发布的子设备。
 */
bool xhci_hub_runtime_handle_transfer_event(
    xhci_state_t *state,
    const xhci_trb_t *event)
{
    if(state == 0 ||
       event == 0 ||
       ((event->control >> XHCI_TRB_TYPE_SHIFT) & 0x3FU) !=
           XHCI_TRANSFER_EVENT_TYPE)
    {
        return false;
    }

    uint8_t slot =
        (uint8_t)(event->control >> XHCI_TRB_SLOT_SHIFT);
    if(slot == 0U)
    {
        return false;
    }

    xhci_slot_device_t *slot_dev =
        xhci_topology_slot(slot);

    if(!slot_dev->used ||
       !slot_dev->is_hub ||
       slot_dev->context.device_slot != slot ||
       slot_dev->context.hub_endpoint == 0U)
    {
        return false;
    }

    xhci_device_context_t *device =
        &slot_dev->context;
    uint32_t endpoint_id =
        (event->control >> XHCI_TRB_ENDPOINT_SHIFT) & 0x1FU;

    if(endpoint_id != (uint32_t)device->hub_endpoint * 2U + 1U)
    {
        return false;
    }

    uint32_t completion =
        event->status >> XHCI_COMPLETION_SHIFT;
    device->hub_transfer_pending = false;

    if(completion == XHCI_COMPLETION_SUCCESS)
    {
        uint32_t bytes =
            ((uint32_t)device->hub_port_count + 8U) / 8U;
        uint32_t bitmap = 0U;

        if(bytes > device->hub_max_packet)
        {
            bytes = device->hub_max_packet;
        }
        if(bytes > PAGE_SIZE)
        {
            bytes = PAGE_SIZE;
        }

        dma_sync_for_cpu(&device->hub_report.mapping);
        for(uint32_t port = 1U;
            port <= device->hub_port_count;
            ++port)
        {
            uint32_t byte = port / 8U;
            uint32_t bit = port % 8U;

            if(byte < bytes &&
               (((const uint8_t *)device->hub_report.cpu)[byte] &
                (1U << bit)) != 0U)
            {
                bitmap |= 1U << (port - 1U);
            }
        }

        device->hub_change_bitmap |= bitmap;
        /* 先完成状态协调并确认 C_PORT_*，之后再重新挂起传输。 */
    }
    else if(completion != 0U)
    {
        /* 拓扑协调会在取消操作后执行最终移除。 */
        xhci_set_error(73U + completion);
    }

    return true;
}

void xhci_hub_runtime_rearm(
    xhci_state_t *state,
    const xhci_hub_runtime_ops_t *ops)
{
    if(state == 0 ||
       !state->initialized ||
       ops == 0 ||
       ops->queue_status == 0)
    {
        return;
    }

    uint32_t limit = state->max_slots;
    if(limit >= XHCI_MAX_SLOT_TABLE)
    {
        limit = XHCI_MAX_SLOT_TABLE - 1U;
    }

    for(uint32_t slot = 1U;
        slot <= limit;
        ++slot)
    {
        xhci_slot_device_t *slot_dev =
            xhci_topology_slot(slot);

        if(!slot_dev->used ||
           !slot_dev->is_hub ||
           slot_dev->context.device_slot != (uint8_t)slot ||
           slot_dev->context.hub_endpoint == 0U ||
           slot_dev->context.hub_transfer_pending)
        {
            continue;
        }

        (void)ops->queue_status(state, &slot_dev->context);
    }
}

bool xhci_hub_runtime_start(
    xhci_state_t *state,
    const xhci_hub_runtime_ops_t *ops)
{
    if(state == 0 ||
       !state->initialized ||
       ops == 0 ||
       ops->restart_status == 0)
    {
        return false;
    }

    uint32_t limit = state->max_slots;
    if(limit >= XHCI_MAX_SLOT_TABLE)
    {
        limit = XHCI_MAX_SLOT_TABLE - 1U;
    }

    /* Normalize every published Hub before exposing runtime hotplug. */
    for(uint32_t slot = 1U;
        slot <= limit;
        ++slot)
    {
        xhci_slot_device_t *slot_dev =
            xhci_topology_slot(slot);

        if(!slot_dev->used ||
           !slot_dev->is_hub ||
           slot_dev->context.device_slot != (uint8_t)slot ||
           slot_dev->context.hub_endpoint == 0U)
        {
            continue;
        }

        xhci_device_context_t *hub =
            &slot_dev->context;

        kernel_serial_write_serial_only("OS_DIAG_HUB_ARM_BEGIN\r\n");
        if(!xhci_hub_ack_all_port_changes_device(state, hub))
        {
            kernel_serial_write_serial_only("OS_DIAG_HUB_ARM_ACK_FAIL\r\n");
            if(xhci_last_error() == 0U)
            {
                xhci_set_error(75U);
            }
            return false;
        }

        if(!ops->restart_status(state, hub))
        {
            if(xhci_last_error() == 0U)
            {
                xhci_set_error(76U);
            }
            return false;
        }

        kernel_serial_write_serial_only("OS_DIAG_HUB_ARM_QUEUED\r\n");
    }

    return true;
}

static bool xhci_hub_runtime_ops_ready(
    const xhci_hub_runtime_ops_t *ops,
    bool need_remove)
{
    return ops != 0 &&
           ops->submit_command != 0 &&
           ops->enumerate_device != 0 &&
           ops->free_device_resources != 0 &&
           ops->publish_working_device != 0 &&
           (!need_remove || ops->remove_device_subtree != 0) &&
           ops->find_child != 0 &&
           ops->child_route != 0 &&
           ops->zero_device_context != 0 &&
           ops->clear_device_flags != 0;
}

/*
 * Startup enumeration is a bounded Slot Table walk.  Newly published Hub
 * Slots are deliberately processed on the next pass, which supports nested
 * Hub topology without an inventory-side device model.
 */
bool xhci_hub_runtime_probe_downstream(
    xhci_state_t *state,
    const xhci_hub_runtime_ops_t *ops)
{
    if(state == 0 ||
       !xhci_hub_runtime_ops_ready(ops, false))
    {
        return false;
    }

    uint64_t processed[4] = { 0U, 0U, 0U, 0U };
    uint32_t limit = state->max_slots;
    if(limit >= XHCI_MAX_SLOT_TABLE)
    {
        limit = XHCI_MAX_SLOT_TABLE - 1U;
    }

    for(;;)
    {
        bool progress = false;

        for(uint32_t slot_index = 1U;
            slot_index <= limit;
            ++slot_index)
        {
            uint32_t word = slot_index >> 6;
            uint64_t mask = 1ULL << (slot_index & 63U);
            if((processed[word] & mask) != 0U)
            {
                continue;
            }

            xhci_slot_device_t *slot_dev =
                xhci_topology_slot(slot_index);
            if(!slot_dev->used ||
               !slot_dev->is_hub ||
               slot_dev->context.device_slot != (uint8_t)slot_index ||
               slot_dev->context.hub_port_count == 0U)
            {
                continue;
            }

            processed[word] |= mask;
            progress = true;
            xhci_device_context_t *hub = &slot_dev->context;

            for(uint32_t port = 1U;
                port <= hub->hub_port_count;
                ++port)
            {
                uint32_t status = 0U;
                uint8_t child_slot = 0U;
                bool enabled = false;

                if(!xhci_hub_get_port_status_device(
                       state, hub, (uint8_t)port, &status))
                {
                    continue;
                }

                (void)xhci_hub_ack_port_changes_device(
                    state, hub, (uint8_t)port, status);
                if((status & 1U) == 0U)
                {
                    continue;
                }

                if(ops->find_child(
                       hub->device_slot, (uint8_t)port, &child_slot))
                {
                    continue;
                }

                (void)xhci_hub_set_port_feature_device(
                    state, hub, (uint8_t)port, USB_HUB_FEATURE_PORT_POWER);
                bool reset_ok = xhci_hub_reset_port(
                    state, hub, (uint8_t)port, &status);
                enabled = reset_ok;

                uint32_t route = 0U;
                if(!enabled ||
                   !ops->child_route(
                       hub, (uint8_t)port, &route))
                {
                    continue;
                }

                (void)xhci_hub_ack_port_changes_device(
                    state, hub, (uint8_t)port, status);
                uint8_t speed = xhci_hub_port_speed(status);

                /* state->device is unpublished child working storage. */
                ops->zero_device_context(&state->device);
                ops->clear_device_flags();
                if(!ops->submit_command(
                       state, XHCI_COMMAND_RING_TYPE, 0U, 0U,
                       &child_slot) ||
                   child_slot == 0U ||
                   !ops->enumerate_device(
                       state, child_slot, (uint8_t)port, speed,
                       hub->root_port, hub->device_slot,
                       (uint8_t)port, route))
                {
                    if(child_slot != 0U)
                    {
                        (void)ops->submit_command(
                            state, XHCI_DISABLE_SLOT_TYPE,
                            child_slot, 0U, 0);
                    }
                    (void)ops->free_device_resources(state);
                    ops->clear_device_flags();
                    continue;
                }

                uint8_t kind =
                    xhci_context_kind(&state->device);
                if(kind != 0U &&
                   ops->publish_working_device(state))
                {
                }
                else
                {
                    (void)ops->submit_command(
                        state, XHCI_DISABLE_SLOT_TYPE,
                        child_slot, 0U, 0);
                    (void)ops->free_device_resources(state);
                    ops->clear_device_flags();
                }
            }

            ops->zero_device_context(&state->device);
            ops->clear_device_flags();
        }

        if(!progress)
        {
            break;
        }
    }

    /* Runtime Hub TDs are armed only at the startup/runtime barrier. */
    return true;
}

static bool xhci_hub_runtime_reconcile_one(
    xhci_state_t *state,
    xhci_device_context_t *hub,
    const xhci_hub_runtime_ops_t *ops)
{
    bool changed = false;
    if(state == 0 ||
       hub == 0 ||
       !xhci_hub_runtime_ops_ready(ops, true) ||
       hub->device_slot == 0U ||
       hub->hub_port_count == 0U)
    {
        return false;
    }

    for(uint32_t port = 1U;
        port <= hub->hub_port_count;
        ++port)
    {
        uint32_t status = 0U;
        uint8_t child_slot = 0U;
        bool connected;
        bool child_present;
        bool reported_change;

        if(!xhci_hub_get_port_status_device(
               state, hub, (uint8_t)port, &status))
        {
            continue;
        }

        connected = (status & 1U) != 0U;
        child_present = ops->find_child(
            hub->device_slot, (uint8_t)port, &child_slot);
        reported_change =
            port <= 32U &&
            (hub->hub_change_bitmap & (1U << (port - 1U))) != 0U;
        if(child_present && reported_change)
        {
            kernel_serial_write_serial_only(
                connected
                    ? "OS_DIAG_HUB_CHILD_CONNECTED\r\n"
                    : "OS_DIAG_HUB_CHILD_DISCONNECTED\r\n");
        }

        (void)xhci_hub_ack_port_changes_device(
            state, hub, (uint8_t)port, status);

        if(!connected)
        {
            if(child_present || reported_change)
            {
                kernel_serial_write_serial_only(
                    "OS_DIAG_HUB_DISCONNECT\r\n");
                kernel_serial_write_serial_only(
                    child_present && child_slot != 0U
                        ? "OS_DIAG_HUB_CHILD_FOUND\r\n"
                        : "OS_DIAG_HUB_CHILD_MISSING\r\n");
            }

            if(child_present &&
               child_slot != 0U &&
               ops->remove_device_subtree(state, child_slot))
            {
                changed = true;
            }
            continue;
        }

        if(child_present)
        {
            continue;
        }

        (void)xhci_hub_set_port_feature_device(
            state, hub, (uint8_t)port,
            USB_HUB_FEATURE_PORT_POWER);
        bool enabled = xhci_hub_reset_port(
            state, hub, (uint8_t)port, &status);

        uint32_t route = 0U;
        if(!enabled ||
           !ops->child_route(hub, (uint8_t)port, &route))
        {
            continue;
        }

        (void)xhci_hub_ack_port_changes_device(
            state, hub, (uint8_t)port, status);
        uint8_t speed = xhci_hub_port_speed(status);
        uint8_t slot = 0U;

        ops->zero_device_context(&state->device);
        ops->clear_device_flags();
        if(!ops->submit_command(
               state, XHCI_COMMAND_RING_TYPE, 0U, 0U, &slot) ||
           slot == 0U ||
           !ops->enumerate_device(
               state, slot, (uint8_t)port, speed,
               hub->root_port, hub->device_slot,
               (uint8_t)port, route))
        {
            if(slot != 0U)
            {
                (void)ops->submit_command(
                    state, XHCI_DISABLE_SLOT_TYPE, slot, 0U, 0);
            }
            (void)ops->free_device_resources(state);
            ops->clear_device_flags();
            continue;
        }

        uint8_t kind = xhci_context_kind(&state->device);
        if(kind != 0U && ops->publish_working_device(state))
        {
            changed = true;
        }
        else
        {
            (void)ops->submit_command(
                state, XHCI_DISABLE_SLOT_TYPE, slot, 0U, 0);
            (void)ops->free_device_resources(state);
            ops->clear_device_flags();
        }
    }

    hub->hub_change_bitmap = 0U;
    ops->zero_device_context(&state->device);
    ops->clear_device_flags();
    return changed;
}

bool xhci_hub_runtime_reconcile(
    xhci_state_t *state,
    const xhci_hub_runtime_ops_t *ops)
{
    if(state == 0 ||
       !state->initialized ||
       !xhci_topology_hub_configured() ||
       !xhci_hub_runtime_ops_ready(ops, true))
    {
        return false;
    }

    if(state->device.device_slot != 0U)
    {
        return false;
    }

    ops->clear_device_flags();
    uint64_t processed[4] = { 0U, 0U, 0U, 0U };
    uint32_t limit = state->max_slots;
    if(limit >= XHCI_MAX_SLOT_TABLE)
    {
        limit = XHCI_MAX_SLOT_TABLE - 1U;
    }

    bool changed = false;
    for(;;)
    {
        bool progress = false;
        for(uint32_t slot = 1U;
            slot <= limit;
            ++slot)
        {
            uint32_t word = slot >> 6;
            uint64_t mask = 1ULL << (slot & 63U);
            if((processed[word] & mask) != 0U)
            {
                continue;
            }

            xhci_slot_device_t *slot_dev =
                xhci_topology_slot(slot);
            if(!slot_dev->used || !slot_dev->is_hub)
            {
                continue;
            }

            processed[word] |= mask;
            progress = true;
            xhci_device_context_t *hub = &slot_dev->context;
            if(hub->device_slot != (uint8_t)slot ||
               hub->hub_port_count == 0U)
            {
                continue;
            }

            if(xhci_hub_runtime_reconcile_one(state, hub, ops))
            {
                changed = true;
            }
        }

        if(!progress)
        {
            break;
        }
    }

    xhci_recompute_topology(state);
    if(changed)
    {
        kernel_serial_write_serial_only("OS_USB_RUNTIME_HUB_CHANGE\r\n");
    }
    return changed;
}
