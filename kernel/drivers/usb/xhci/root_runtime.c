#include "internal.h"

#include <kernel/console.h>
#include <kernel/realtest.h>
#include <kernel/xhci.h>

/* REFACTOR_P8_XHCI_ROOT_RUNTIME_OWNER：根端口复位、Slot 子树生命周期、
 * 运行时连接/移除及自测重新枚举流程。 */

#define XHCI_PORT_REGISTER_BASE   0x400U
#define XHCI_PORT_REGISTER_STRIDE 0x10U
#define XHCI_PORTSC_CCS           (1U << 0)
#define XHCI_PORTSC_PED           (1U << 1)
#define XHCI_PORTSC_PR            (1U << 4)
#define XHCI_PORTSC_PLS_MASK      (0x0FU << 5)
#define XHCI_PORTSC_PP            (1U << 9)
#define XHCI_PORTSC_SPEED_SHIFT   10U
#define XHCI_PORTSC_SPEED_MASK    0x0FU
#define XHCI_PORTSC_CHANGE_MASK   (0x7FU << 17)
#define XHCI_PORTSC_WRC           (1U << 19)
#define XHCI_PORTSC_PRC           (1U << 21)
#define XHCI_PORTSC_WPR           (1U << 31)
#define XHCI_PORTSC_NEUTRAL_MASK  ((1U << 0) | (1U << 3) | \
                                   (0x0FU << 10) | XHCI_PORTSC_PLS_MASK | \
                                   XHCI_PORTSC_PP | \
                                   (3U << 14) | (7U << 25) | (1U << 30))

/* 物理端口稳定期间可能重复报告相同链路状态变化。每个控制器/端口只记录一次不同的
 * PortSC 值，使 realtest 状态在后续启动阶段仍有参考价值。 */
static uint32_t g_xhci_last_ack_state[2][256];
static bool g_xhci_ack_state_valid[2][256];

static uint32_t xhci_root_port_offset(const xhci_state_t *state,
                                      uint8_t port) {
    return state->operational_offset + XHCI_PORT_REGISTER_BASE +
           (uint32_t)(port - 1U) * XHCI_PORT_REGISTER_STRIDE;
}

static uint32_t xhci_port_state_to_neutral(uint32_t portsc) {
    return portsc & XHCI_PORTSC_NEUTRAL_MASK;
}

static bool xhci_ack_root_port_changes(xhci_state_t *state, uint8_t port) {
    uint32_t offset;
    uint32_t portsc;
    uint32_t changes;

    if (state == 0 || port == 0U || port > state->max_ports) return false;
    offset = xhci_root_port_offset(state, port);
    portsc = xhci_controller_read32(state, offset);
    if (portsc == UINT32_MAX) return false;
    changes = portsc & XHCI_PORTSC_CHANGE_MASK;
    if (changes == 0U) return true;
    uint32_t controller_index =
        state == xhci_hid_controller_state() ? 1U : 0U;
    if (!g_xhci_ack_state_valid[controller_index][port] ||
        g_xhci_last_ack_state[controller_index][port] != portsc) {
        g_xhci_ack_state_valid[controller_index][port] = true;
        g_xhci_last_ack_state[controller_index][port] = portsc;
        realtest_mark_number("XHCI_PORT_ACK", port);
        realtest_mark_number("XHCI_PORT_ACK_STATE", portsc);
    }
    return xhci_controller_write32(
        state, offset, xhci_port_state_to_neutral(portsc) | changes);
}

static bool xhci_prepare_connected_port(xhci_state_t *state,
                                        uint8_t port,
                                        uint8_t *speed_out) {
    uint32_t offset;
    uint32_t portsc;
    uint32_t reset_value;
    uint32_t reset_bit;
    uint32_t reset_change;
    bool enabled = false;
    bool auxiliary_hid = state == xhci_hid_controller_state();

    if (state == 0 || speed_out == 0 || port == 0U ||
        port > state->max_ports) {
        return false;
    }
    offset = xhci_root_port_offset(state, port);
    portsc = xhci_controller_read32(state, offset);
    if (portsc == UINT32_MAX || (portsc & XHCI_PORTSC_CCS) == 0U) {
        return false;
    }
    realtest_mark_number("XHCI_PORT_BEGIN", port);
    realtest_mark_number("XHCI_PORTSC_BEFORE", portsc);

    uint32_t initial_speed =
        (portsc >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK;
    bool use_warm_reset = initial_speed >= 4U && state->pci != 0 &&
                          state->pci->vendor_id == 0x1022U &&
                          state->pci->device_id == 0x43FCU;
    reset_bit = use_warm_reset ? XHCI_PORTSC_WPR : XHCI_PORTSC_PR;
    /* 同时实现 USB2 和 USB3 协议视图的硬件在 warm reset 时也会置位普通 reset-change 位。
     * 枚举前清除两者，避免待处理端口事件掩盖首次传输。 */
    reset_change = use_warm_reset ?
                   (XHCI_PORTSC_WRC | XHCI_PORTSC_PRC) : XHCI_PORTSC_PRC;
    realtest_mark(use_warm_reset ?
                         "XHCI_PORT_RESET_WARM" :
                         "XHCI_PORT_RESET_COLD");

    /* WRC 是写 1 清除的完成位。开始新的 warm reset 前先清除过期的变化标记。 */
    if (use_warm_reset &&
        (portsc & (XHCI_PORTSC_WRC | XHCI_PORTSC_PRC)) != 0U) {
        (void)xhci_controller_write32(
            state, offset,
            xhci_port_state_to_neutral(portsc) |
                (XHCI_PORTSC_WRC | XHCI_PORTSC_PRC));
        portsc = xhci_controller_read32(state, offset);
    }

    /* 重置每个新连接到根端口的设备；PED 可能反映固件状态，而非 USB 设备的 Default 状态。 */
    reset_value = xhci_port_state_to_neutral(portsc) |
                  reset_bit;
    if (!xhci_controller_write32(state, offset, reset_value)) {
        realtest_mark("XHCI_PORT_RESET_WRITE_FAIL");
        xhci_set_error(50U);
        return false;
    }
    if (use_warm_reset) {
        uint64_t reset_deadline =
            xhci_controller_timeout_deadline(1000000000ULL);
        uint32_t reset_spins = 0U;
        xhci_controller_delay_ns(1000000ULL);
        for (;;) {
            portsc = xhci_controller_read32(state, offset);
            if ((portsc & XHCI_PORTSC_PED) != 0U &&
                (portsc & XHCI_PORTSC_WRC) != 0U) {
                enabled = true;
                break;
            }
            if ((reset_deadline != UINT64_MAX &&
                 xhci_controller_timeout_reached(reset_deadline)) ||
                (reset_deadline == UINT64_MAX &&
                 ++reset_spins >= 1000000U)) break;
            __asm__ volatile ("pause");
        }
    } else if (auxiliary_hid) {
        /* USB2 复位完成时间不能用很少的 CPU 自旋次数限定。固件接管过的设备退出旧控制器
         * 状态时，真实 AMD 根端口可能需要数毫秒。 */
        uint64_t reset_deadline =
            xhci_controller_timeout_deadline(1000000000ULL);
        uint32_t reset_spins = 0U;
        for (;;) {
            portsc = xhci_controller_read32(state, offset);
            if ((portsc & XHCI_PORTSC_PED) != 0U &&
                (portsc & XHCI_PORTSC_PR) == 0U) {
                enabled = true;
                break;
            }
            if ((reset_deadline != UINT64_MAX &&
                 xhci_controller_timeout_reached(reset_deadline)) ||
                (reset_deadline == UINT64_MAX &&
                 ++reset_spins >= 5000000U)) break;
            __asm__ volatile ("pause");
        }
    } else {
        /* 端口复位完成由硬件事件决定，不应按 CPU 自旋次数估算。
         * USB2 Hub 通常需要数毫秒才能离开 Polling；旧的短循环会在 xHC 报告 PED 前超时。 */
        uint64_t reset_deadline =
            xhci_controller_timeout_deadline(1000000000ULL);
        uint32_t reset_spins = 0U;
        for (;;) {
            portsc = xhci_controller_read32(state, offset);
            if ((portsc & XHCI_PORTSC_PED) != 0U &&
                (portsc & XHCI_PORTSC_PR) == 0U) {
                enabled = true;
                break;
            }
            if ((reset_deadline != UINT64_MAX &&
                 xhci_controller_timeout_reached(reset_deadline)) ||
                (reset_deadline == UINT64_MAX &&
                 ++reset_spins >= 5000000U)) break;
            __asm__ volatile ("pause");
        }
    }
    if (!enabled) {
        realtest_mark_number("XHCI_PORTSC_RESET_LAST", portsc);
        realtest_mark("XHCI_PORT_RESET_TIMEOUT");
        xhci_set_error(51U);
        return false;
    }
    (void)xhci_controller_write32(
        state, offset,
        xhci_port_state_to_neutral(portsc) |
            reset_change);
    realtest_mark_number("XHCI_PORTSC_AFTER", portsc);
    xhci_controller_delay_ns(50000000ULL);

    uint32_t speed = (portsc >> XHCI_PORTSC_SPEED_SHIFT) &
                     XHCI_PORTSC_SPEED_MASK;
    if (speed == 0U) {
        realtest_mark("XHCI_PORT_SPEED_INVALID");
        xhci_set_error(52U);
        return false;
    }
    *speed_out = (uint8_t)speed;
    return true;
}

static void xhci_attach_device(xhci_state_t *state, uint8_t port) {
    uint8_t speed = 0U;
    uint8_t slot = 0U;

    if (!xhci_prepare_connected_port(state, port, &speed)) return;
    if (!xhci_submit_command(state, XHCI_COMMAND_RING_TYPE, 0U, 0U, &slot) ||
        slot == 0U ||
        !xhci_enumerate_device(state, slot, port, speed, port, 0U, 0U, 0U)) {
        if (slot != 0U) {
            (void)xhci_submit_command(state, XHCI_DISABLE_SLOT_TYPE, slot,
                                       0U, 0);
        }
        (void)xhci_free_device_resources(state);
        xhci_clear_device_flags();
        if (xhci_last_error() == 0U) xhci_set_error(62U);
    }
}

static bool xhci_slot_has_root_port(const xhci_state_t *state, uint8_t port) {
    uint32_t limit;

    if (state == 0 || port == 0U) return false;
    limit = state->max_slots;
    if (limit >= XHCI_MAX_SLOT_TABLE) limit = XHCI_MAX_SLOT_TABLE - 1U;
    for (uint32_t slot = 1U; slot <= limit; ++slot) {
        const xhci_slot_device_t *slot_dev = xhci_topology_slot(slot);
        if (slot_dev->used && slot_dev->context.device_slot == (uint8_t)slot &&
            slot_dev->context.root_port == port) {
            return true;
        }
    }
    return false;
}

bool xhci_remove_device_subtree(xhci_state_t *state, uint8_t slot) {
    xhci_slot_device_t *slot_dev;
    uint8_t children[16];
    bool removed = false;

    if (state == 0 || slot == 0U) return false;
    slot_dev = xhci_topology_slot(slot);
    if (!slot_dev->used || slot_dev->context.device_slot != slot) {
        return false;
    }
    for (uint32_t port = 0U; port < 16U; ++port) {
        children[port] = slot_dev->child_slots[port];
    }
    for (uint32_t port = 0U; port < 16U; ++port) {
        uint8_t child = children[port];
        if (child != 0U && child != slot &&
            xhci_remove_device_subtree(state, child)) {
            removed = true;
        }
    }
    if (xhci_release_slot_device(state, slot)) removed = true;
    return removed;
}

static bool xhci_remove_root_port_devices(xhci_state_t *state, uint8_t port) {
    uint32_t limit;
    bool changed = false;

    if (state == 0 || port == 0U) return false;
    limit = state->max_slots;
    if (limit >= XHCI_MAX_SLOT_TABLE) limit = XHCI_MAX_SLOT_TABLE - 1U;
    for (uint32_t slot = 1U; slot <= limit; ++slot) {
        xhci_slot_device_t *slot_dev = xhci_topology_slot(slot);
        if (!slot_dev->used || slot_dev->context.device_slot != (uint8_t)slot ||
            slot_dev->context.root_port != port ||
            slot_dev->context.parent_slot != 0U) {
            continue;
        }
        if (xhci_remove_device_subtree(state, (uint8_t)slot)) changed = true;
    }
    xhci_recompute_topology(state);
    return changed;
}

static bool xhci_attach_root_runtime_device(xhci_state_t *state,
                                            uint8_t port) {
    uint8_t kind;

    if (state == 0 || port == 0U || port > state->max_ports ||
        state->device.device_slot != 0U) {
        return false;
    }
    xhci_attach_device(state, port);
    if (state->device.device_slot == 0U) {
        xhci_recompute_topology(state);
        return false;
    }
    kind = xhci_context_kind(&state->device);
    if (kind == 0U || !xhci_publish_working_device(state)) {
        if (state->device.device_slot != 0U) {
            (void)xhci_release_working_device(state);
        }
        xhci_recompute_topology(state);
        return false;
    }
    return true;
}

bool xhci_handle_root_port_event(xhci_state_t *state, uint8_t port,
                                 uint32_t portsc) {
    bool changed = false;

    if (state == 0 || port == 0U || port > state->max_ports ||
        portsc == UINT32_MAX) {
        return false;
    }

    /* 启动事件排空前已完整枚举根端口。这些复位变化通知描述的是已有清单，而不是新插入的设备。在此处
     * 重新枚举可能会将可选设备故障升级为启动失败。xHCI 就绪后，运行时事件走常规连接路径。 */
    if (!xhci_runtime_ready()) {
        return xhci_ack_root_port_changes(state, port);
    }
    if ((portsc & XHCI_PORTSC_CCS) == 0U) {
        changed = xhci_remove_root_port_devices(state, port);
    } else if (!xhci_slot_has_root_port(state, port)) {
        changed = xhci_attach_root_runtime_device(state, port);
    }
    bool acknowledged = xhci_ack_root_port_changes(state, port);
    if (changed) {
        kernel_serial_write_serial_only("OS_USB_RUNTIME_ROOT_CHANGE\r\n");
    }
    return acknowledged && (changed || (portsc & XHCI_PORTSC_CCS) != 0U);
}

bool xhci_reenumerate_self_test(xhci_state_t *state,
                                xhci_device_context_t *device) {
    uint8_t old_slot;
    uint8_t port;
    uint32_t offset;
    xhci_slot_device_t *old_slot_dev;

    if (state == 0 || device == 0 || device->device_slot == 0U ||
        device->device_port == 0U) {
        return false;
    }
    if (device->parent_slot != 0U) return true;
    old_slot = device->device_slot;
    port = device->root_port != 0U ? device->root_port : device->device_port;
    if (port == 0U || port > state->max_ports) {
        xhci_set_error(66U);
        return false;
    }
    offset = xhci_root_port_offset(state, port);
    old_slot_dev = xhci_topology_slot(old_slot);
    if (!old_slot_dev->used || &old_slot_dev->context != device ||
        old_slot_dev->context.device_slot != old_slot ||
        state->device.device_slot != 0U) {
        xhci_set_error(66U);
        return false;
    }
    if (!xhci_remove_device_subtree(state, old_slot)) {
        xhci_set_error(66U);
        return false;
    }
    uint32_t portsc = xhci_controller_read32(state, offset);
    if (portsc == UINT32_MAX || (portsc & XHCI_PORTSC_CCS) == 0U) {
        xhci_set_error(66U);
        return false;
    }

    xhci_attach_device(state, port);
    if (state->device.device_slot == 0U) {
        if (xhci_last_error() == 0U) xhci_set_error(67U);
        return false;
    }
    if (xhci_context_kind(&state->device) == 0U ||
        !xhci_publish_working_device(state)) {
        if (state->device.device_slot != 0U) {
            (void)xhci_release_working_device(state);
        }
        if (xhci_last_error() == 0U) xhci_set_error(67U);
        return false;
    }
    if (!xhci_hub_runtime_probe_downstream(state, &g_xhci_hub_runtime_ops)) {
        /* 根端口存储设备已可用。断开的或不受支持的可选下游端口不得导致其被丢弃。 */
        if (xhci_usb_mass_storage_configured()) {
            realtest_mark("XHCI_HUB_OPTIONAL_FAIL_WITH_MSC");
        } else {
            if (xhci_last_error() == 0U) xhci_set_error(67U);
            return false;
        }
    }
    xhci_recompute_topology(state);
    if (state->device.device_slot != 0U ||
        !xhci_slot_has_root_port(state, port)) {
        xhci_set_error(67U);
        return false;
    }
    return true;
}

bool xhci_probe_connected_ports(xhci_state_t *state, uint8_t *selected_slot) {
    uint32_t limit;
    uint32_t first_error = 0U;

    if (state == 0 || selected_slot == 0) return false;
    *selected_slot = 0U;
    for (uint32_t port = 1U; port <= state->max_ports; ++port) {
        uint32_t offset = xhci_root_port_offset(state, (uint8_t)port);
        uint32_t portsc = xhci_controller_read32(state, offset);
        uint8_t speed = 0U;
        uint8_t slot = 0U;
        uint32_t enumeration_error = 0U;

        if (portsc == UINT32_MAX || (portsc & XHCI_PORTSC_CCS) == 0U ||
            !xhci_prepare_connected_port(state, (uint8_t)port, &speed) ||
            !xhci_submit_command(state, XHCI_COMMAND_RING_TYPE, 0U, 0U,
                                  &slot) || slot == 0U) {
            continue;
        }
        if (xhci_enumerate_device(state, slot, (uint8_t)port, speed,
                                  (uint8_t)port, 0U, 0U, 0U) &&
            xhci_context_kind(&state->device) != 0U &&
            xhci_publish_working_device(state)) {
            /* 枚举每个已连接根端口。USB2 和 SuperSpeed 配套 Hub 通常占用不同根端口；若遇到第一个 Hub 后
             * 停止扫描，可能会漏掉 USB2 配套 Hub 下的所有低速/全速设备。单端口失败仍视为可选故障，后续
             * 扫描继续进行。 */
            continue;
        }

        enumeration_error = xhci_last_error();
        if (slot != 0U) {
            (void)xhci_submit_command(state, XHCI_DISABLE_SLOT_TYPE, slot,
                                       0U, 0);
        }
        (void)xhci_free_device_resources(state);
        xhci_zero_device_context(&state->device);
        xhci_clear_device_flags();
        xhci_set_error(enumeration_error != 0U ? enumeration_error : 70U);
        if (first_error == 0U) {
            first_error = xhci_last_error();
        }
        kernel_serial_printf_serial_only(
            "OS_XHCI_ENUM_FAIL PORT=%u SLOT=%u PORTSC=%u SPEED=%u "
            "ERROR=%u\r\n",
            port, slot, xhci_controller_read32(state, offset), speed,
            xhci_last_error());
        /* 单个可选键盘、Hub 或音频设备故障不得导致此控制器上已发布的存储设备被丢弃。继续扫描，并由拓扑
         * 选择器选取可用 Slot。 */
        continue;
    }

    kernel_serial_write_serial_only(
        "OS_DIAG_XHCI_ROOT_SCAN_COMPLETE\r\n");

    if (!xhci_hub_runtime_probe_downstream(state, &g_xhci_hub_runtime_ops)) {
        if (!xhci_usb_mass_storage_configured()) return false;
        realtest_mark("XHCI_HUB_OPTIONAL_FAIL_WITH_MSC");
    }
    xhci_recompute_topology(state);
    limit = state->max_slots;
    if (limit >= XHCI_MAX_SLOT_TABLE) limit = XHCI_MAX_SLOT_TABLE - 1U;
    for (uint32_t pass = 0U; pass < 2U; ++pass) {
        for (uint32_t slot = 1U; slot <= limit; ++slot) {
            xhci_slot_device_t *slot_dev = xhci_topology_slot(slot);
            uint8_t kind;
            if (!slot_dev->used ||
                slot_dev->context.device_slot != (uint8_t)slot) {
                continue;
            }
            kind = xhci_context_kind(&slot_dev->context);
            if (kind == 0U || (pass == 0U && kind == 4U)) continue;
            *selected_slot = (uint8_t)slot;
            return true;
        }
    }
    if (first_error != 0U) xhci_set_error(first_error);
    return false;
}

/* 探测不属于主 USB 拓扑的控制器设备。此路径有意仅支持直连根端口的 HID 设备：其 Slot 和 DMA 上下文
 * 保持为辅助控制器私有。 */
bool xhci_probe_hid_controller(xhci_state_t *state) {
    if (state == 0 || !state->initialized || state->device.device_slot != 0U) {
        return false;
    }

    for (uint32_t port = 1U; port <= state->max_ports; ++port) {
        uint32_t offset = xhci_root_port_offset(state, (uint8_t)port);
        uint32_t portsc = xhci_controller_read32(state, offset);
        uint8_t speed = 0U;
        uint8_t slot = 0U;

        if (portsc == UINT32_MAX || (portsc & XHCI_PORTSC_CCS) == 0U) {
            continue;
        }
        realtest_mark_number("XHCI_AUX_PORT", port);
        realtest_mark_number("XHCI_AUX_PORTSC", portsc);
        if (!xhci_prepare_connected_port(state, (uint8_t)port, &speed)) {
            realtest_mark_number("XHCI_AUX_PREP_ERROR",
                                        xhci_last_error());
            continue;
        }
        realtest_mark_number("XHCI_AUX_SPEED", speed);
        if (!xhci_submit_command(state, XHCI_COMMAND_RING_TYPE, 0U, 0U,
                                 &slot) || slot == 0U) {
            realtest_mark_number("XHCI_AUX_SLOT_ERROR",
                                        xhci_last_error());
            continue;
        }
        realtest_mark_number("XHCI_AUX_SLOT", slot);
        if (xhci_enumerate_hid_device(state, slot, (uint8_t)port, speed,
                                       (uint8_t)port)) {
            realtest_mark_number("XHCI_AUX_HID_PORT", port);
            realtest_mark_number("XHCI_AUX_HID_SLOT", slot);
            realtest_mark_number("XHCI_AUX_HID_PROTOCOL",
                                        state->device.hid_protocol);
            kernel_serial_printf_serial_only(
                "OS_XHCI_AUX_HID_OK PORT=%u SLOT=%u PROTOCOL=%u\r\n",
                port, slot, state->device.hid_protocol);
            return true;
        }
        realtest_mark_number("XHCI_AUX_ENUM_ERROR", xhci_last_error());

        (void)xhci_free_hid_device_resources(state);
        xhci_zero_device_context(&state->device);
    }
    return false;
}
