#include <kernel/xhci.h>
#include "internal.h"

/* REFACTOR_P8_XHCI_STATUS_OWNER：控制器公开存在状态、USB 拓扑状态投影和诊断错误查询。 */
/* REFACTOR_P8_XHCI_STATUS_STATE_OWNER：诊断状态仅在此处修改；其他 xHCI 单元使用下方私有 setter。 */

static bool g_xhci_present;
static uint32_t g_xhci_error;

void xhci_set_hardware_present(bool present) {
    g_xhci_present = present;
}

void xhci_clear_error(void) {
    g_xhci_error = 0U;
}

void xhci_set_error(uint32_t error) {
    g_xhci_error = error;
}

bool xhci_hardware_present(void) {
    return g_xhci_present;
}

bool xhci_usb_device_enumerated(void) {
    return xhci_topology_device_enumerated();
}

uint32_t xhci_usb_device_count(void) {
    return xhci_topology_device_count();
}

bool xhci_usb_hid_configured(void) {
    return xhci_topology_hid_configured();
}

bool xhci_usb_keyboard_configured(void) {
    return xhci_topology_keyboard_configured();
}

bool xhci_usb_mouse_configured(void) {
    return xhci_topology_mouse_configured();
}

bool xhci_usb_audio_configured(void) {
    return xhci_topology_audio_configured();
}

bool xhci_usb_hub_configured(void) {
    return xhci_topology_hub_configured();
}

uint8_t xhci_usb_hub_port_count(void) {
    return xhci_topology_hub_port_count();
}

bool xhci_usb_hub_downstream_configured(void) {
    return xhci_topology_hub_downstream_configured();
}

bool xhci_usb_bluetooth_configured(void) {
    return xhci_topology_bluetooth_configured();
}

uint32_t xhci_last_error(void) {
    return g_xhci_error;
}
