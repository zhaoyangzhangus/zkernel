#include <kernel/console.h>
#include <kernel/input.h>
#include "internal.h"

#define USB_HID_PROTOCOL_KEYBOARD 0x01U
#define USB_HID_PROTOCOL_MOUSE 0x02U

/* REFACTOR_P8_XHCI_HID_OWNER：报告解码和输入发布。 */
static atomic_bool g_xhci_input_seen;
static atomic_bool g_xhci_mouse_input_seen;
static atomic_bool g_xhci_keyboard_input_seen;

void xhci_hid_init(void) {
    atomic_init(&g_xhci_input_seen, false);
    atomic_init(&g_xhci_mouse_input_seen, false);
    atomic_init(&g_xhci_keyboard_input_seen, false);
}
static bool xhci_hid_report_contains(const uint8_t *report, uint8_t key) {
    if (report == 0 || key == 0U) return false;
    for (uint32_t i = 2U; i < 8U; ++i) {
        if (report[i] == key) return true;
    }
    return false;
}

static bool xhci_hid_previous_contains(const xhci_hid_report_context_t *device,
                                       uint8_t key) {
    if (device == 0 || key == 0U) return false;
    for (uint32_t i = 0U; i < 6U; ++i) {
        if (device->previous_keys[i] == key) return true;
    }
    return false;
}

static uint32_t xhci_hid_input_device_id(const xhci_hid_report_context_t *context) {
    /* Slot 在 Disable Slot 执行前始终唯一；该操作也限定了由其 DMA 报告缓冲区生成的
     * 事件的生命周期。 */
    return context == 0 ? 0x5848U : 0x58480000U | context->device_slot;
}

static void xhci_hid_emit_event(const xhci_hid_report_context_t *context,
                                uint16_t type, uint32_t code, int32_t value) {
    input_event_t event = {
        .timestamp = 0U,
        .device_id = xhci_hid_input_device_id(context),
        .type = type,
        .flags = 0U,
        .code = code,
        .value = value,
    };
    if (!atomic_exchange_explicit(&g_xhci_input_seen, true,
                                  memory_order_acq_rel)) {
        kernel_serial_write_serial_only("OS_USB_INPUT_EVENT_OK\r\n");
    }
    (void)input_core_push(&event);
}

static void xhci_hid_emit_pointer(const xhci_hid_report_context_t *context,
                                  uint16_t buttons_changed, uint8_t buttons,
                                  int32_t dx, int32_t dy, int32_t wheel) {
    input_pointer_motion_t motion = {
        .timestamp = 0U,
        .device_id = xhci_hid_input_device_id(context),
        .flags = 0U,
        .buttons_changed = buttons_changed,
        .buttons = buttons,
        .dx = dx,
        .dy = dy,
        .wheel = wheel,
    };
    if (!atomic_exchange_explicit(&g_xhci_input_seen, true,
                                  memory_order_acq_rel)) {
        kernel_serial_write_serial_only("OS_USB_INPUT_EVENT_OK\r\n");
    }
    (void)input_core_push_pointer(&motion);
}

static void xhci_hid_emit_key(const xhci_hid_report_context_t *context,
                              uint8_t key, int32_t value) {
    xhci_hid_emit_event(context, INPUT_EVENT_KEY, key, value);
}

static void xhci_hid_consume_keyboard_report(xhci_hid_report_context_t *state,
                                              const uint8_t *report) {
    uint8_t modifier;
    bool changed;
    if (state == 0 || report == 0) return;
    modifier = report[0];
    changed = modifier != state->previous_modifier;
    if (!changed) {
        for (uint32_t i = 2U; i < 8U; ++i) {
            if (report[i] != state->previous_keys[i - 2U]) {
                changed = true;
                break;
            }
        }
    }
    if (changed && !atomic_exchange_explicit(&g_xhci_keyboard_input_seen, true,
                                             memory_order_acq_rel)) {
        kernel_serial_write_serial_only("OS_USB_KEYBOARD_EVENT_OK\r\n");
    }
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
        uint8_t mask = (uint8_t)(1U << bit);
        if ((modifier & mask) != (state->previous_modifier & mask)) {
            xhci_hid_emit_key(state, (uint8_t)(0xE0U + bit),
                              (modifier & mask) != 0U ? INPUT_VALUE_PRESS :
                                                        INPUT_VALUE_RELEASE);
        }
    }
    for (uint32_t i = 2U; i < 8U; ++i) {
        uint8_t previous = state->previous_keys[i - 2U];
        if (previous != 0U && !xhci_hid_report_contains(report, previous)) {
            xhci_hid_emit_key(state, previous, INPUT_VALUE_RELEASE);
        }
    }
    for (uint32_t i = 2U; i < 8U; ++i) {
        uint8_t current = report[i];
        if (current >= 4U && !xhci_hid_previous_contains(state, current)) {
            xhci_hid_emit_key(state, current, INPUT_VALUE_PRESS);
        }
    }
    /* 所有成员检查都必须使用同一份上一轮报告。检查下一按键时更新槽位，可能会在键盘
     * 仅对按住的按键重新排序或压紧排列时，错误地产生按下事件。 */
    for (uint32_t i = 2U; i < 8U; ++i) {
        uint8_t current = report[i];
        state->previous_keys[i - 2U] = current;
    }
    state->previous_modifier = modifier;
}

static void xhci_hid_consume_mouse_report(xhci_hid_report_context_t *state,
                                           const uint8_t *report,
                                           uint32_t report_length) {
    uint8_t current_buttons;
    uint16_t changed_buttons;
    if (state == 0 || report == 0 || report_length < 3U) return;
    if (!atomic_exchange_explicit(&g_xhci_mouse_input_seen, true,
                                  memory_order_acq_rel)) {
        kernel_serial_write_serial_only("OS_USB_MOUSE_EVENT_OK\r\n");
    }
    current_buttons = report[0] & 0x07U;
    changed_buttons = (uint16_t)(current_buttons ^ state->previous_buttons);
    /* Boot mouse 报告使用有符号 8 位相对坐标。输入 ABI 特意采用屏幕坐标（X 正向向右，
     * Y 正向向下）。与常见桌面 HID 栈相同，QEMU 的 USB boot mouse 后端会将物理向下
     * 移动报告为正 Y 增量，因此直接传递该值，不再进行第二次反转。 */
    int32_t delta_x = (int32_t)(int8_t)report[1];
    int32_t hid_delta_y = (int32_t)(int8_t)report[2];
    /* 可选的第 4 个字节表示 Boot mouse 滚轮。保留 HID 有符号方向；由使用方决定滚动方向。 */
    int32_t wheel = report_length >= 4U ? (int32_t)(int8_t)report[3] : 0;
    if (changed_buttons != 0U || delta_x != 0 || hid_delta_y != 0 ||
        wheel != 0) {
        xhci_hid_emit_pointer(state, changed_buttons, current_buttons,
                              delta_x, hid_delta_y, wheel);
    }
    state->previous_buttons = current_buttons;
}

void xhci_hid_consume(xhci_hid_report_context_t *device,
                                    uint32_t report_length) {
    const uint8_t *report;
    if (device == 0 || device->report == 0) return;
    report = device->report;
    if (device->protocol == USB_HID_PROTOCOL_KEYBOARD && report_length >= 8U) {
        xhci_hid_consume_keyboard_report(device, report);
    } else if (device->protocol == USB_HID_PROTOCOL_MOUSE &&
               report_length >= 3U) {
        xhci_hid_consume_mouse_report(device, report, report_length);
    }
}
