#include <kernel/console.h>
#include "internal.h"

/* REFACTOR_P7A_INPUT_OWNER：标题栏捕获清理和事件泵边界。 */

#ifndef OS_REALTEST
#define OS_REALTEST 0
#endif

void window_clear_title_capture_locked(void) {
    g_window_server.title_pressed_identifier = 0U;
    g_window_server.title_pressed_button = WINDOW_TITLE_BUTTON_NONE;
    g_window_server.title_pressed_pointer_x = 0U;
    g_window_server.title_pressed_pointer_y = 0U;
    g_window_server.title_pressed_client = false;
    g_window_server.title_pressed_event = (input_event_t){0};
}

void window_input_reset_desktop_state_locked(void) {
    g_window_server.desktop = (desktop_interaction_t){0};
    g_window_server.desktop_pending_launch = DESKTOP_APP_NONE;
    g_window_server.desktop_gui_mask = 0U;
    g_window_server.desktop_tab_consumed = false;
    g_window_server.desktop_focus_cycle_requested = false;
}

uint32_t window_input_take_desktop_launch_locked(void) {
    uint32_t app = g_window_server.desktop_pending_launch;
    g_window_server.desktop_pending_launch = DESKTOP_APP_NONE;
    return app;
}

bool window_input_take_focus_cycle_request_locked(void) {
    bool requested = g_window_server.desktop_focus_cycle_requested;
    g_window_server.desktop_focus_cycle_requested = false;
    return requested;
}

bool window_title_capture_moved_locked(void) {
    uint32_t dx;
    uint32_t dy;

    if (g_window_server.title_pressed_identifier == 0U) {
        return false;
    }

    dx = g_window_server.pointer_x >= g_window_server.title_pressed_pointer_x ?
         g_window_server.pointer_x - g_window_server.title_pressed_pointer_x :
         g_window_server.title_pressed_pointer_x - g_window_server.pointer_x;
    dy = g_window_server.pointer_y >= g_window_server.title_pressed_pointer_y ?
         g_window_server.pointer_y - g_window_server.title_pressed_pointer_y :
         g_window_server.title_pressed_pointer_y - g_window_server.pointer_y;

    return dx >= WINDOW_TITLE_DRAG_DISTANCE ||
           dy >= WINDOW_TITLE_DRAG_DISTANCE;
}

uint32_t window_input_cursor_shape_locked(void) {
    uint32_t edges = g_window_server.resize_edges;
    if (g_window_server.dragging_identifier == 0U) {
        edges = window_input_resize_edges_locked(
            window_scene_pointer_window_locked(),
            g_window_server.pointer_x, g_window_server.pointer_y);
    }
    bool horizontal = (edges & (WINDOW_RESIZE_LEFT | WINDOW_RESIZE_RIGHT)) != 0U;
    bool vertical = (edges & (WINDOW_RESIZE_TOP | WINDOW_RESIZE_BOTTOM)) != 0U;
    if (horizontal && vertical) {
        bool left = (edges & WINDOW_RESIZE_LEFT) != 0U;
        bool top = (edges & WINDOW_RESIZE_TOP) != 0U;
        return left == top ? WINDOW_CURSOR_SIZE_NWSE : WINDOW_CURSOR_SIZE_NESW;
    }
    if (horizontal) return WINDOW_CURSOR_SIZE_WE;
    if (vertical) return WINDOW_CURSOR_SIZE_NS;
    return WINDOW_CURSOR_ARROW;
}

static window_server_window_t *keyboard_window_locked(void) {
    return window_scene_keyboard_locked();
}

static uint32_t desktop_number_shortcut_app(uint32_t key_code) {
    static const uint32_t apps[] = {
        DESKTOP_APP_FILES,
        DESKTOP_APP_TERMINAL,
        DESKTOP_APP_NOTES,
        DESKTOP_APP_SETTINGS,
        DESKTOP_APP_TASKMGR,
        DESKTOP_APP_CHROME,
        DESKTOP_APP_IMAGEVIEW,
    };
    if (key_code < 0x1EU || key_code >= 0x1EU +
            sizeof(apps) / sizeof(apps[0])) {
        return DESKTOP_APP_NONE;
    }
    return apps[key_code - 0x1EU];
}

#if OS_REALTEST
/* 仅保留自动化输入回归所需的缩放按键验证。 */
static void input_route_diag_key(const input_event_t *event,
                                 const window_server_window_t *target,
                                 bool delivered) {
    if (event == 0 || target == 0 || !delivered ||
        event->type != INPUT_EVENT_KEY || event->value != INPUT_VALUE_PRESS ||
        event->code != 0x2EU) return;
    kernel_serial_printf_serial_only(
        "OS_DIAG_INPUT_ROUTE code=%u value=%u focus=%u target=%u delivered=%u\r\n",
        event->code, (uint32_t)event->value,
        g_window_server.focused_identifier, target->identifier, 1U);
}
#else
static void input_route_diag_key(const input_event_t *event,
                                 const window_server_window_t *target,
                                 bool delivered) {
    (void)event;
    (void)target;
    (void)delivered;
}
#endif

static void focus_locked(window_server_window_t *window) {
    window_scene_focus_locked(window);
}

/* 悬停失效区域必须与绘制和命中共用按钮几何，不能固定为三倍 32 像素。 */
static void window_mark_caption_button_locked(const window_server_window_t *window, uint32_t button) {
    int32_t x,y;
    if (!window || !button || !window_standard_caption(window->flags) ||
        !window_caption_button_rect(window->flags,window->x,window->y,window->width,button,&x,&y)) return;
    window_mark_rect_locked(x,y,os_window_caption_button_width(window->flags),
        os_window_caption_height(window->flags));
}

uint32_t window_title_button_at_locked(
    const window_server_window_t *window,
    uint32_t pointer_x,
    uint32_t pointer_y) {

    static const uint32_t buttons[] = {
        WINDOW_TITLE_BUTTON_CLOSE,
        WINDOW_TITLE_BUTTON_MAXIMIZE,
        WINDOW_TITLE_BUTTON_MINIMIZE,
    };

    if (window == 0 || window_control_has_caption(window->controls) || (window_client_decorations(window->flags) &&
                       !window_standard_caption(window->flags))) {
        return WINDOW_TITLE_BUTTON_NONE;
    }
    /* 外侧缩放区域优先于标题栏控件，包括关闭按钮旁的右上角。
     * 光标提示和点击行为必须一致。 */
    if (window_input_resize_edges_locked(window, pointer_x, pointer_y) != 0U)
        return WINDOW_TITLE_BUTTON_NONE;

    for (uint32_t index = 0U;
         index < sizeof(buttons) / sizeof(buttons[0]);
         ++index) {

        int32_t x;
        int32_t y;

        if (!window_caption_button_rect(window->flags, window->x,
                                      window->y,
                                      window->width,
                                      buttons[index],
                                      &x,
                                      &y)) {
            continue;
        }

        if ((int64_t)pointer_x >= x &&
            (int64_t)pointer_y >= y &&
            (int64_t)pointer_x < (int64_t)x +
                (window_standard_caption(window->flags) ? os_window_caption_button_width(window->flags) : WINDOW_TITLE_BUTTON_SIZE) &&
            (int64_t)pointer_y < (int64_t)y +
                (window_standard_caption(window->flags) ? os_window_caption_height(window->flags) : WINDOW_TITLE_BUTTON_SIZE)) {
            return buttons[index];
        }
    }

    return WINDOW_TITLE_BUTTON_NONE;
}

/*
 * 在可缩放窗口的普通和最大化几何状态之间切换。
 *
 * width/height 始终表示客户端尺寸。生成缩放事件前，需从可用工作区中减去装饰尺寸。
 */

void window_input_release_pointer_locked(window_server_window_t *window) {
    if (window == 0) return;
    /* 捕获与控件按压属于同一次交互，隐藏后必须一起结束，避免恢复时残留。 */
    window->client_pointer_capture = false;
    window->client_button_capture = false;
    window->control_text_drag=false;window->control_text_ctrl=0U;
    window->control_hover = 0U;
    window->control_pressed = 0U;
    window->control_hover_damage = (Rect){0};
}

/* 隐藏窗口，但不销毁其对象、surface 或客户端几何信息。 */
bool window_minimize_locked(
    window_server_window_t *window) {

    window_server_window_t *new_focused = 0;

    if (window == 0 ||
        window->minimized ||
        (window->flags & OS_WINDOW_VISIBLE) == 0U) {
        return false;
    }

    /*
    * 擦除当前可见区域。
     */
    window_mark_window_locked(
        window);

    window_animation_depart_locked(window, true);
    window->animation_start = 0;
    window->animation_seen = false;
    window->minimized = true;
    window->dirty = true;

    /*
    * 取消此窗口拥有的所有装饰捕获。
     */
    if (g_window_server.dragging_identifier ==
        window->identifier) {
        window_input_clear_drag_locked();
    }

    if (g_window_server.title_pressed_identifier ==
        window->identifier) {
        window_clear_title_capture_locked();
    }

    /*
    * 将焦点转移到剩余最上层的普通窗口。
     */
    if (g_window_server.focused_identifier ==
        window->identifier) {

        window_scene_set_focus_identifier_locked(0U);

        for (uint32_t index = g_window_server.scene.count;
             index != 0U;
             --index) {

            window_server_window_t *candidate =
                g_window_server.scene.children[index - 1U];

            if (candidate == 0 ||
                candidate == window ||
                candidate->minimized ||
                candidate->client_show_inactive ||
                (candidate->flags &
                 OS_WINDOW_VISIBLE) == 0U) {
                continue;
            }

            new_focused = candidate;
            window_scene_set_focus_identifier_locked(candidate->identifier);

            break;
        }
    }

    if (new_focused != 0) {
        window_mark_window_locked(
            new_focused);
    }

    /* 先通知旧命中离开，再清理捕获和按压状态。 */
    window_input_refresh_pointer_locked();
    window_input_release_pointer_locked(window);
    window_enqueue_minimized_event_locked(window);
    return true;
}


static bool window_toggle_maximize_locked(
    window_server_window_t *window) {

    int32_t new_x;
    int32_t new_y;

    uint32_t new_width;
    uint32_t new_height;

    uint32_t available_height;

    if (window == 0 || window->fullscreen ||
        (window->flags &
         OS_WINDOW_RESIZABLE) == 0U ||
        g_window_server.display_width <=
            WINDOW_FRAME_EXTRA ||
        g_window_server.display_height <=
            DESKTOP_TOPBAR_HEIGHT +
            WINDOW_TITLEBAR_HEIGHT +
            WINDOW_FRAME_EXTRA) {

        return false;
    }

    /*
    * 修改旧几何信息前先标记其损坏区域。
     */
    window_mark_window_locked(
        window);

    if (!window->maximized) {
        /*
         * 保留精确的常规客户区几何信息。
         */
        window->restore_x =
            window->x;

        window->restore_y =
            window->y;

        window->restore_width =
            window->width;

        window->restore_height =
            window->height;

        new_x = 0;
        new_y =
            (int32_t)
                DESKTOP_TOPBAR_HEIGHT;

        new_width =
            g_window_server.display_width -
            window_frame_extra(window->flags);

        available_height =
            g_window_server.display_height -
            DESKTOP_TOPBAR_HEIGHT;

        new_height =
            available_height -
            window_titlebar_height(window->flags) -
            window_frame_extra(window->flags);

        window->maximized =
            true;

    } else {
        /*
         * 恢复进入最大化状态时捕获的精确几何信息。
         */
        new_x =
            window->restore_x;

        new_y =
            window->restore_y;

        new_width =
            window->restore_width;

        new_height =
            window->restore_height;

        if (new_width == 0U ||
            new_height == 0U) {

            return false;
        }

        window->maximized =
            false;
    }

    window->x =
        new_x;

    window->y =
        new_y;

    window->width =
        new_width;

    window->height =
        new_height;

    window->dirty =
        true;

    /*
    * 在 Ring3 通过完整表面更新确认新的跨度/几何信息前，合成器会继续使用调整大小安全路径。
     */
    window->resize_pending =
        true;

    /*
    * 存在装饰区捕获时进行最大化，不得留下任何过期的拖动/调整大小状态。
     */
    if (g_window_server.dragging_identifier ==
        window->identifier) {
        window_input_clear_drag_locked();
    }

    window_mark_window_locked(
        window);

    window_enqueue_resize_event_locked(
        window);

    window_input_refresh_pointer_locked();
    return true;
}

void window_control_caption_action_locked(window_server_window_t *window,uint32_t action) {
    if (!window) return;
    if (action==OS_CONTROL_WINDOW_CLOSE) window_enqueue_close_request_locked(window);
    else if (action==OS_CONTROL_WINDOW_MAXIMIZE) (void)window_toggle_maximize_locked(window);
    else if (action==OS_CONTROL_WINDOW_MINIMIZE) (void)window_minimize_locked(window);
}
kstatus_t window_server_set_maximized_owned(process_t *owner,
    window_server_window_t *window, uint32_t maximized) {
    if (!owner || !window || maximized > 1U || !window_server_kernel_ready()) return K_EINVAL;
    window_lock();
    if (window->owner != owner || !window->identifier ||
        window_scene_find_locked(window->identifier) != window) {
        window_unlock();
        return K_EPERM;
    }
    if (window->fullscreen) { window_unlock(); return K_EINVAL; }
    if (window->maximized == (maximized != 0U)) {
        window_unlock();
        return K_OK;
    }
    bool changed = window_toggle_maximize_locked(window);
    window_unlock();
    if (changed) window_server_notify_worker();
    return changed ? K_OK : K_EINVAL;
}

/* 普通路由和捕获路由使用相同的有界屏幕坐标。滚轮/未知轴保持不变，但不得丢弃客户端事件。 */
static bool window_route_relative_pointer_locked(const input_event_t *event) {
    uint32_t old_x = g_window_server.pointer_x;
    uint32_t old_y = g_window_server.pointer_y;
    uint32_t extent;
    int64_t next;
    if (event->code == INPUT_REL_X) {
        extent = g_window_server.display_width;
        next = (int64_t)old_x + event->value;
    } else if (event->code == INPUT_REL_Y) {
        extent = g_window_server.display_height;
        next = (int64_t)old_y + event->value;
    } else {
        return false;
    }
    if (extent == 0U || next < 0) next = 0;
    else if (next >= extent) next = (int64_t)extent - 1;
    uint32_t x = event->code == INPUT_REL_X ? (uint32_t)next : old_x;
    uint32_t y = event->code == INPUT_REL_Y ? (uint32_t)next : old_y;
    if (x == old_x && y == old_y) return false;
    window_input_set_pointer_locked(x, y);
    return true;
}

/* 所有物理指针事件先更新同一个场景命中，再按捕获或普通路径分派实际输入。
 * 离开旧节点必须先于进入新节点；捕获不能阻止桌面和其他控件更新悬停。 */
static window_server_window_t *window_route_pointer_scene_locked(const input_event_t *event) {
    if (event->type != INPUT_EVENT_RELATIVE && event->type != INPUT_EVENT_BUTTON) return 0;
    uint32_t old_x = g_window_server.pointer_x, old_y = g_window_server.pointer_y;
    if (event->type == INPUT_EVENT_RELATIVE) (void)window_route_relative_pointer_locked(event);
    window_server_window_t *old_hover = window_scene_find_locked(
        g_window_server.scene.hovered_window_identifier);
    /* 物理命中只计算一次；桌面捕获只屏蔽悬停，不改变按钮路由的遮挡判断。 */
    window_server_window_t *pointer_hit = window_scene_hit_test_locked(
        g_window_server.pointer_x, g_window_server.pointer_y);
    window_server_window_t *new_hover =
        window_scene_pointer_captured_locked() ? 0 : pointer_hit;
    os_input_event_t input = {.timestamp=event->timestamp, .device_id=event->device_id,
        .type=event->type, .flags=event->flags, .code=event->code, .value=event->value};
    if (old_hover != new_hover) window_control_pointer_hover_locked(old_hover, &input, false);
    window_control_pointer_hover_locked(new_hover, &input, true);
    uint32_t old_button = g_window_server.scene.hovered_title_button;
    uint32_t new_button = window_title_button_at_locked(new_hover,
        g_window_server.pointer_x, g_window_server.pointer_y);
    if (old_hover != new_hover || old_button != new_button) {
        window_mark_caption_button_locked(old_hover, old_button);
        window_mark_caption_button_locked(new_hover, new_button);
    }
    g_window_server.scene.hovered_window_identifier = new_hover ? new_hover->identifier : 0U;
    g_window_server.scene.hovered_title_button = new_button;
    uint32_t hovered = DESKTOP_APP_NONE;
    if (!new_hover && g_window_server.dragging_identifier == 0U &&
        !g_window_server.desktop.menu_open && !g_window_server.desktop.switch_open &&
        !g_window_server.desktop.dragging)
        hovered = desktop_app_at_locked(g_window_server.pointer_x, g_window_server.pointer_y);
    if (hovered != g_window_server.desktop_hovered_app) {
        desktop_mark_app_locked(g_window_server.desktop_hovered_app);
        desktop_mark_app_locked(hovered);
        desktop_set_hovered_app_locked(hovered);
    }
    if (old_x != g_window_server.pointer_x || old_y != g_window_server.pointer_y)
        window_mark_moved_cursor_locked(old_x, old_y,
            g_window_server.pointer_x, g_window_server.pointer_y);
    return pointer_hit;
}

/* 几何变化后复用场景命中，不移动指针、不转发原始按键或按钮动作，也不改变焦点。 */
void window_input_refresh_pointer_locked(void) {
    uint32_t low,high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    input_event_t event={.timestamp=((uint64_t)high<<32U)|low,
        .type=INPUT_EVENT_RELATIVE,.code=INPUT_REL_X};
    (void)window_route_pointer_scene_locked(&event);
}

/* 捕获只决定输入接收者；坐标、悬停和光标发布由上方公共入口处理。菜单仍能接收窗外点击。 */
static bool route_client_capture_locked(const input_event_t *event) {
    if (event->type != INPUT_EVENT_RELATIVE &&
        event->type != INPUT_EVENT_BUTTON) return false;
    window_server_window_t *captured = 0;
    for (uint32_t i = 0; i < g_window_server.scene.count; ++i) {
        window_server_window_t *candidate = g_window_server.scene.children[i];
        if (candidate != 0 && (candidate->client_pointer_capture ||
                              candidate->client_button_capture) &&
            !candidate->client_hidden && !candidate->minimized &&
            ((candidate->flags & OS_WINDOW_VISIBLE) != 0U ||
             (candidate->client_pointer_capture &&
              candidate->present_sequence == 0U))) {
            captured = candidate;
            break;
        }
    }
    if (captured == 0) return false;
    if (event->type == INPUT_EVENT_BUTTON && event->code == INPUT_BUTTON_LEFT &&
        event->value == INPUT_VALUE_PRESS &&
        (captured->flags & OS_WINDOW_EXPLICIT_MOVE_RESIZE) != 0U)
        captured->client_button_capture = true;
    if (event->type == INPUT_EVENT_BUTTON && event->code == INPUT_BUTTON_LEFT &&
        event->value == INPUT_VALUE_RELEASE)
        captured->client_button_capture = false;
    window_enqueue_event_locked(captured, event);
    return true;
}

void route_input_locked(const input_event_t *event) {
    window_server_window_t *target = 0;
    bool deliver_input = true;
    uint32_t title_button =
        WINDOW_TITLE_BUTTON_NONE;

    if (event == 0) return;
    uint32_t old_pointer_x = g_window_server.pointer_x;
    uint32_t old_pointer_y = g_window_server.pointer_y;
    window_server_window_t *pointer_hit = window_route_pointer_scene_locked(event);
    if (route_client_capture_locked(event)) {
        window_coalesce_damage_locked();
        return;
    }
    if (desktop_route_key_locked(event)) return;
    if (desktop_route_button_locked(event, pointer_hit != 0)) {
        window_coalesce_damage_locked();
        return;
    }


    if (event->type == INPUT_EVENT_KEY) {
        window_server_window_t *keyboard_target = keyboard_window_locked();
        bool client_decorated =
            keyboard_target != 0 &&
            window_client_decorations(keyboard_target->flags);
        uint32_t shortcut_app = desktop_number_shortcut_app(event->code);

        /* 客户端装饰窗口也会接收修饰键和切换键事件。Ring0 桌面快捷键仅适用于旧式窗口。 */
        if (!client_decorated &&
            (event->code == 0xE3U || event->code == 0xE7U)) {
            uint32_t bit = event->code == 0xE3U ? 1U : 2U;
            if (event->value == INPUT_VALUE_RELEASE) {
                g_window_server.desktop_gui_mask &= ~bit;
            } else {
                g_window_server.desktop_gui_mask |= bit;
            }
            deliver_input = false;
        } else if (!client_decorated &&
                   g_window_server.desktop_gui_mask != 0U &&
                   shortcut_app != DESKTOP_APP_NONE) {
            deliver_input = false;
            if (event->value == INPUT_VALUE_PRESS) {
                if (g_window_server.desktop_pending_launch ==
                    DESKTOP_APP_NONE) {
                    g_window_server.desktop_pending_launch = shortcut_app;
                }
                desktop_mark_app_locked(shortcut_app);
            }
        } else if (!client_decorated && event->code == 0x2BU) {
            if (g_window_server.desktop_gui_mask != 0U) {
                deliver_input = false;
                g_window_server.desktop_tab_consumed = true;
                if (event->value == INPUT_VALUE_PRESS) {
                    g_window_server.desktop_focus_cycle_requested = true;
                }
            } else if (g_window_server.desktop_tab_consumed) {
                deliver_input = false;
            }

            if (event->value == INPUT_VALUE_RELEASE) {
                g_window_server.desktop_tab_consumed = false;
            }
        }
    }

    if (event->type == INPUT_EVENT_RELATIVE) {
        if (desktop_route_motion_locked()) {
            window_coalesce_damage_locked();
            return;
        }

        /*
         * 指针移动超过拖动阈值前，标题栏控件都视为待确认的点击候选项。此规则同时适用于旧式
         * 合成器控件和客户端绘制的标题栏按钮；静止按住鼠标时，点击不会因经过时间而取消。
         */
        if (g_window_server.dragging_identifier == 0U &&
            g_window_server.title_pressed_identifier != 0U) {
            window_server_window_t *pressed =
                window_scene_find_locked(
                    g_window_server.title_pressed_identifier);
            if (window_title_capture_moved_locked()) {
                if (g_window_server.title_pressed_client && pressed != 0 &&
                    (pressed->flags & OS_WINDOW_VISIBLE) != 0U &&
                    !pressed->maximized && !pressed->fullscreen) {
                    window_input_begin_drag_locked(
                        pressed->identifier,
                        (int32_t)g_window_server.title_pressed_pointer_x -
                            pressed->x,
                        (int32_t)g_window_server.title_pressed_pointer_y -
                            pressed->y,
                        0U);
                    window_clear_title_capture_locked();
                } else {
                    window_clear_title_capture_locked();
                }
            }
        }

        if (g_window_server.dragging_identifier != 0U) {
            window_server_window_t *dragged =
                window_scene_find_locked(g_window_server.dragging_identifier);
            if (dragged == 0 || (dragged->flags & OS_WINDOW_VISIBLE) == 0U) {
                window_input_clear_drag_locked();
            } else if (g_window_server.resize_edges != 0U) {
                int32_t delta_x =
                    (int32_t)((int64_t)g_window_server.pointer_x - old_pointer_x);
                int32_t delta_y =
                    (int32_t)((int64_t)g_window_server.pointer_y - old_pointer_y);
                (void)window_resize_locked(dragged, delta_x, delta_y);
                target = dragged;
                /* 装饰区捕获：鼠标增量由 Ring0 处理，不交给客户端。 */
                deliver_input = false;
            } else {
                int32_t new_x =
                    (int32_t)g_window_server.pointer_x -
                    g_window_server.drag_offset_x;
                int32_t new_y =
                    (int32_t)g_window_server.pointer_y -
                    g_window_server.drag_offset_y;
                window_drag_move_locked(dragged, new_x, new_y);
                target = dragged;
                deliver_input = false;
            }
        }

        if (target == 0) {
            target = pointer_hit;
        }
    } else if (event->type == INPUT_EVENT_BUTTON) {
        target = pointer_hit;

        if (target != 0) {
            title_button =
                window_title_button_at_locked(
                    target,
                    g_window_server.pointer_x,
                    g_window_server.pointer_y);
        }

        /*
         * 按钮语义将在下一步接入。
         *
         * 建立正确的 Ring0 所有权：
         *   - 标题栏控件点击绝不发送给 Ring3
         *   - 确定是点击还是拖动后再激活控件
         *   - 按下控件仍会聚焦其窗口
         */
        if (event->code ==
                INPUT_BUTTON_LEFT &&
            title_button !=
                WINDOW_TITLE_BUTTON_NONE) {

            deliver_input = false;

            if (event->value ==
                    INPUT_VALUE_PRESS &&
                target != 0) {

                /*
                 * 从此处开始捕获装饰区事件。
                 *
                 * 仅按下控件只会聚焦窗口，不会执行破坏性操作。
                 */
                focus_locked(target);

                g_window_server.title_pressed_identifier =
                    target->identifier;

                g_window_server.title_pressed_button =
                    title_button;
                g_window_server.title_pressed_pointer_x =
                    g_window_server.pointer_x;
                g_window_server.title_pressed_pointer_y =
                    g_window_server.pointer_y;
                g_window_server.title_pressed_client = false;
                g_window_server.title_pressed_event = *event;
                window_mark_window_locked(target);

            } else if (
                event->value ==
                    INPUT_VALUE_RELEASE) {

                /*
                 * 仅在满足以下条件时激活：
                 *
                 *   按下时的窗口 == 释放时的窗口
                 *   按下时的按钮 == 释放时的按钮
                 *
                 * 因此，在释放前拖离控件会取消该操作。
                 */
                if (target != 0 &&
                    g_window_server.title_pressed_identifier ==
                        target->identifier &&
                    g_window_server.title_pressed_button ==
                        title_button &&
                    !window_title_capture_moved_locked()) {

                    if (title_button ==
                        WINDOW_TITLE_BUTTON_CLOSE) {

                        window_enqueue_close_request_locked(
                            target);

                    } else if (
                        title_button ==
                        WINDOW_TITLE_BUTTON_MAXIMIZE) {

                        (void)window_toggle_maximize_locked(
                            target);

                    } else if (
                        title_button ==
                        WINDOW_TITLE_BUTTON_MINIMIZE) {

                        (void)window_minimize_locked(
                            target);
                    }
                }

                window_clear_title_capture_locked();
                if (target) window_mark_window_locked(target);
            }
        }


        if (event->code ==
                INPUT_BUTTON_LEFT &&
            event->value ==
                INPUT_VALUE_PRESS &&
            title_button ==
                WINDOW_TITLE_BUTTON_NONE) {
            window_clear_title_capture_locked();
        }

        if (event->code == INPUT_BUTTON_LEFT &&
            event->value == INPUT_VALUE_PRESS &&
            target != 0 &&
            title_button == WINDOW_TITLE_BUTTON_NONE) {
            uint32_t resize_edges;
            focus_locked(target);
            resize_edges = window_resize_edges_locked(
                target, g_window_server.pointer_x, g_window_server.pointer_y);

            if (resize_edges != 0U) {
                window_input_begin_drag_locked(
                    target->identifier, 0, 0, resize_edges);
                deliver_input = false;
            } else if (window_control_caption_hit_locked(target,
                           (int32_t)g_window_server.pointer_x, (int32_t)g_window_server.pointer_y)) {
                target->client_button_capture = true;
            } else if (((target->flags & OS_WINDOW_EXPLICIT_MOVE_RESIZE) != 0U &&
                        window_client_decorations(target->flags)) ||
                       (window_standard_caption(target->flags) &&
                       (int64_t)g_window_server.pointer_y - target->y >= os_window_caption_height(target->flags))) {
                /* 隐式内容按钮捕获会在释放时结束，即使释放位置在窗口之外也一样。显式 Chrome
                 * 菜单捕获独立处理。 */
                target->client_button_capture = true;
            } else if (
                window_client_decorations(target->flags) &&
                (int64_t)g_window_server.pointer_y - target->y >= 0 &&
                (int64_t)g_window_server.pointer_y - target->y <
                    (window_standard_caption(target->flags) ? os_window_caption_height(target->flags) : WINDOW_CLIENT_DRAG_REGION_HEIGHT)) {
                /* 内核无法仅凭标题栏 x 坐标推断客户端命中目标：Chromium 会将标签页和关闭按钮
                 * 放在旧式推测拖动条带内。暂存每次标题栏按下事件；若为静止点击则回放，并由上方
                 * 现有移动阈值判断何时转为窗口拖动。 */
                g_window_server.title_pressed_identifier =
                    target->identifier;
                g_window_server.title_pressed_button =
                    WINDOW_TITLE_BUTTON_NONE;
                g_window_server.title_pressed_pointer_x =
                    g_window_server.pointer_x;
                g_window_server.title_pressed_pointer_y =
                    g_window_server.pointer_y;
                g_window_server.title_pressed_client = true;
                g_window_server.title_pressed_event = *event;
                deliver_input = false;
            } else if (
                !target->maximized && !target->fullscreen &&
                (window_client_drag_region(
                     target->flags, target->x, target->y, target->width,
                     g_window_server.pointer_x,
                     g_window_server.pointer_y) ||
                 (!window_client_decorations(target->flags) &&
                  (int64_t)g_window_server.pointer_y - target->y <
                  WINDOW_DRAG_REGION_HEIGHT))) {
                window_input_begin_drag_locked(
                    target->identifier,
                    (int32_t)g_window_server.pointer_x - target->x,
                    (int32_t)g_window_server.pointer_y - target->y,
                    0U);
                deliver_input = false;
            }
        } else if (event->code == INPUT_BUTTON_LEFT &&
                   event->value == INPUT_VALUE_RELEASE) {
            if (g_window_server.title_pressed_client) {
                window_server_window_t *pressed =
                    window_scene_find_locked(
                        g_window_server.title_pressed_identifier);
                input_event_t press_event =
                    g_window_server.title_pressed_event;

                if (pressed != 0 && target != 0 &&
                    pressed->identifier == target->identifier &&
                    !window_title_capture_moved_locked()) {
                    press_event.value = INPUT_VALUE_PRESS;
                    window_enqueue_event_locked(pressed, &press_event);
                    window_enqueue_event_locked(pressed, event);
                }
                window_clear_title_capture_locked();
                deliver_input = false;
            } else if (g_window_server.dragging_identifier != 0U) {
            target = window_scene_find_locked(g_window_server.dragging_identifier);
                deliver_input = false;
            }
            window_clear_title_capture_locked();
            window_input_clear_drag_locked();
        }
    } else if (event->type == INPUT_EVENT_KEY) {
        /*
         * 旧式窗口仍使用合成器的切换键快捷键。客户端装饰表面拥有完整的输入约定，因此该快捷键
         * 必须原样传递给 Ring3，而不能被窗口服务器截获。
         */
        window_server_window_t *keyboard_target = keyboard_window_locked();
        if (event->value != INPUT_VALUE_RELEASE && event->code == 0x2BU &&
            (keyboard_target == 0 ||
             (!keyboard_target->controls && !window_client_decorations(keyboard_target->flags)))) {
            uint32_t position = 0U;
            if (g_window_server.scene.count != 0U) {
                for (uint32_t index = 0U; index < g_window_server.scene.count; ++index) {
                    window_server_window_t *candidate =
                        g_window_server.scene.children[index];
                    if (candidate != 0 && candidate->identifier ==
                        g_window_server.focused_identifier) {
                        position = index;
                        break;
                    }
                }
                for (uint32_t offset = 1U; offset <= g_window_server.scene.count; ++offset) {
                    uint32_t index = (position + offset) % g_window_server.scene.count;
                    window_server_window_t *candidate =
                        g_window_server.scene.children[index];
                    if (candidate != 0 &&
                        (candidate->flags & OS_WINDOW_VISIBLE) != 0U) {
                        focus_locked(candidate);
                        window_input_refresh_pointer_locked();
                        break;
                    }
                }
            }
        }
        target = keyboard_window_locked();
    }

    /* 装饰区先消费按下时，客户区不会收到事件，仍须清理它留下的文字选区。 */
    if(!deliver_input && target && event->type==INPUT_EVENT_BUTTON &&
        event->code==INPUT_BUTTON_LEFT && event->value==INPUT_VALUE_PRESS)
        window_control_clear_text_selection_locked(target);
    if (event->type == INPUT_EVENT_RELATIVE ||
        event->type == INPUT_EVENT_BUTTON) {
        window_coalesce_damage_locked();
    }
    input_route_diag_key(event, target, deliver_input && target != 0);
    if (deliver_input && target != 0) {
        window_enqueue_event_locked(target, event);
    }
}
