#include <stdint.h>
#include <stdbool.h>


#include <usb/hub.h>
#include "internal.h"


#define USB_MAX_HUBS 256


static usb_hub_t
usb_hubs[
    USB_MAX_HUBS];



usb_hub_t *
usb_hub_get(
    uint8_t slot)
{
    if(slot==0)
    {
        return 0;
    }


    if(!usb_hubs[slot].used)
    {
        return 0;
    }


    return
        &usb_hubs[slot];
}



bool
usb_hub_init(
    uint8_t slot,
    uint8_t ports)
{
    if(slot==0)
    {
        return false;
    }


    usb_hub_t *hub =
        &usb_hubs[slot];


    __builtin_memset(
        hub,
        0,
        sizeof(*hub));


    hub->used=true;

    hub->slot_id=slot;


    if(ports >
       USB_HUB_MAX_PORTS)
    {
        ports=USB_HUB_MAX_PORTS;
    }


    hub->port_count=ports;


    return true;
}



void
usb_hub_port_connect(
    uint8_t hub_slot,
    uint8_t port,
    uint8_t child_slot)
{
    usb_hub_t *hub =
        usb_hub_get(
            hub_slot);


    if(!hub)
    {
        return;
    }


    if(port==0 ||
       port>USB_HUB_MAX_PORTS)
    {
        return;
    }


    hub->child_slots[
        port-1]=child_slot;
}


void
usb_hub_port_disconnect(
    uint8_t hub_slot,
    uint8_t port,
    uint8_t child_slot)
{
    usb_hub_t *hub =
        usb_hub_get(
            hub_slot);

    if(hub == 0 ||
       port == 0U ||
       port > USB_HUB_MAX_PORTS)
    {
        return;
    }

    uint8_t *entry =
        &hub->child_slots[
            port - 1U];

    /*
    * child_slot == 0 表示无条件清除。
     *
    * 否则，仅当关系仍指向正在移除的设备时才清除。
     */
    if(child_slot == 0U ||
       *entry == child_slot)
    {
        *entry = 0U;
    }
}


void
usb_hub_remove(
    uint8_t slot)
{
    if(slot == 0U)
    {
        return;
    }

    __builtin_memset(
        &usb_hubs[slot],
        0,
        sizeof(usb_hubs[slot]));
}


/* REFACTOR_P8_XHCI_HUB_OWNER：端口控制和变化确认。 */

#define XHCI_SETUP_GET_STATUS 0U
#define XHCI_SETUP_SET_FEATURE 3U
#define XHCI_SETUP_CLEAR_FEATURE 1U
bool xhci_hub_get_port_status_device(
    xhci_state_t *state,
    xhci_device_context_t *device,
    uint8_t port,
    uint32_t *status_out)
{
    uint8_t request[8] = {
        0xA3U,
        XHCI_SETUP_GET_STATUS,
        0x00U,
        0x00U,
        port,
        0x00U,
        0x04U,
        0x00U,
    };


    if(state == 0 ||
       device == 0 ||
       status_out == 0 ||
       port == 0U)
    {
        return false;
    }


    if(!xhci_submit_control_transfer_device(
           state,
           device,
           request,
           4U,
           true))
    {
        return false;
    }


    uint8_t *status =
        (uint8_t *)
            device->
                descriptor_buffer.cpu;


    *status_out =
        (uint32_t)status[0] |
        ((uint32_t)status[1] << 8) |
        ((uint32_t)status[2] << 16) |
        ((uint32_t)status[3] << 24);


    return true;
}


/*
 * 兼容包装函数。
 *
 * V3.10.1B 刻意让现有 Hub 调用方继续使用旧的 state->device 工作上下文行为。
 */




/*
 * V3.10.1C Hub 显式设置特性
 *
 * Hub EP0 状态属于传入的设备上下文。
 */
bool xhci_hub_set_port_feature_device(
    xhci_state_t *state,
    xhci_device_context_t *device,
    uint8_t port,
    uint16_t feature)
{
    uint8_t request[8] = {
        0x23U,
        XHCI_SETUP_SET_FEATURE,
        (uint8_t)feature,
        (uint8_t)(feature >> 8),
        port,
        0x00U,
        0x00U,
        0x00U,
    };


    if(state == 0 ||
       device == 0 ||
       port == 0U)
    {
        return false;
    }


    return
        xhci_submit_control_transfer_device(
            state,
            device,
            request,
            0U,
            false);
}


/*
 * Compatibility wrapper.
 *
 * Existing Hub paths intentionally keep their old state->device
 * working-context behavior in V3.10.1C.
 */





/*
 * V3.10.6B10B3 HUB PORT CHANGE ACK
 *
 * USB Hub interrupt-IN reports that one or more wPortChange bits are
 * pending.  GET_STATUS does not acknowledge those bits: each asserted
 * change bit must be cleared with the matching C_PORT_* feature.
 *
 * Keeping stale change bits asserted makes a Hub interrupt endpoint
 * complete repeatedly and, on QEMU, can make the pre-detach wakeup race
 * ahead of the actual CONNECTION status transition.
 */
bool xhci_hub_clear_port_feature_device(
    xhci_state_t *state,
    xhci_device_context_t *device,
    uint8_t port,
    uint16_t feature)
{
    uint8_t request[8] = {
        0x23U,
        XHCI_SETUP_CLEAR_FEATURE,
        (uint8_t)feature,
        (uint8_t)(feature >> 8),
        port,
        0x00U,
        0x00U,
        0x00U,
    };


    if(state == 0 ||
       device == 0 ||
       device->device_slot == 0U ||
       port == 0U)
    {
        return false;
    }


    return
        xhci_submit_control_transfer_device(
            state,
            device,
            request,
            0U,
            false);
}


bool xhci_hub_ack_port_changes_device(
    xhci_state_t *state,
    xhci_device_context_t *device,
    uint8_t port,
    uint32_t status)
{
    uint16_t change =
        (uint16_t)(
            status >> 16);

    bool ok =
        true;


    if((change &
        USB_HUB_PORT_CHANGE_CONNECTION) != 0U &&
       !xhci_hub_clear_port_feature_device(
           state,
           device,
           port,
           USB_HUB_FEATURE_C_PORT_CONNECTION))
    {
        ok =
            false;
    }


    if((change &
        USB_HUB_PORT_CHANGE_ENABLE) != 0U &&
       !xhci_hub_clear_port_feature_device(
           state,
           device,
           port,
           USB_HUB_FEATURE_C_PORT_ENABLE))
    {
        ok =
            false;
    }


    if((change &
        USB_HUB_PORT_CHANGE_SUSPEND) != 0U &&
       !xhci_hub_clear_port_feature_device(
           state,
           device,
           port,
           USB_HUB_FEATURE_C_PORT_SUSPEND))
    {
        ok =
            false;
    }


    if((change &
        USB_HUB_PORT_CHANGE_OVER_CURRENT) != 0U &&
       !xhci_hub_clear_port_feature_device(
           state,
           device,
           port,
           USB_HUB_FEATURE_C_PORT_OVER_CURRENT))
    {
        ok =
            false;
    }


    if((change &
        USB_HUB_PORT_CHANGE_RESET) != 0U &&
       !xhci_hub_clear_port_feature_device(
           state,
           device,
           port,
           USB_HUB_FEATURE_C_PORT_RESET))
    {
        ok =
            false;
    }


    return ok;
}


bool xhci_hub_ack_all_port_changes_device(
    xhci_state_t *state,
    xhci_device_context_t *device)
{
    if(state == 0 ||
       device == 0 ||
       device->device_slot == 0U ||
       device->hub_port_count == 0U)
    {
        return false;
    }


    bool ok =
        true;


    for(uint32_t port = 1U;
        port <= device->hub_port_count;
        ++port)
    {
        uint32_t status =
            0U;


        if(!xhci_hub_get_port_status_device(
               state,
               device,
               (uint8_t)port,
               &status) ||
           !xhci_hub_ack_port_changes_device(
               state,
               device,
               (uint8_t)port,
               status))
        {
            ok =
                false;
        }
    }


    return ok;
}


uint8_t xhci_hub_port_speed(uint32_t status) {
    if ((status & (1U << 10)) != 0U) return 3U;
    if ((status & (1U << 9)) != 0U) return 2U;
    return 1U;
}
