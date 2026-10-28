#pragma once

#include <stdbool.h>
#include <stdint.h>

#define USB_HUB_MAX_PORTS 32U

/* xHCI Hub 所有者使用的 USB 2.0 Hub 类特性选择器。 */
#define USB_HUB_FEATURE_PORT_RESET 4U
#define USB_HUB_FEATURE_PORT_POWER 8U
#define USB_HUB_FEATURE_C_PORT_CONNECTION 16U
#define USB_HUB_FEATURE_C_PORT_ENABLE 17U
#define USB_HUB_FEATURE_C_PORT_SUSPEND 18U
#define USB_HUB_FEATURE_C_PORT_OVER_CURRENT 19U
#define USB_HUB_FEATURE_C_PORT_RESET 20U

#define USB_HUB_PORT_CHANGE_CONNECTION (1U << 0)
#define USB_HUB_PORT_CHANGE_ENABLE (1U << 1)
#define USB_HUB_PORT_CHANGE_SUSPEND (1U << 2)
#define USB_HUB_PORT_CHANGE_OVER_CURRENT (1U << 3)
#define USB_HUB_PORT_CHANGE_RESET (1U << 4)

typedef struct usb_hub {
    bool used;
    uint8_t slot_id;
    uint8_t port_count;
    uint8_t protocol;
    uint8_t child_slots[USB_HUB_MAX_PORTS];
} usb_hub_t;

usb_hub_t *usb_hub_get(uint8_t slot);
bool usb_hub_init(uint8_t slot, uint8_t ports);
void usb_hub_port_connect(uint8_t hub_slot, uint8_t port,
                          uint8_t child_slot);
void usb_hub_port_disconnect(uint8_t hub_slot, uint8_t port,
                             uint8_t child_slot);
void usb_hub_remove(uint8_t slot);
