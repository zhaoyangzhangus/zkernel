#include "internal.h"

window_server_window_t *window_scene_find_locked(uint32_t identifier) {
    if (identifier == 0U) return 0;
    for (uint32_t index = 0U; index < g_window_server.scene.count; ++index) {
        window_server_window_t *window = g_window_server.scene.children[index];
        if (window != 0 && window->identifier == identifier) return window;
    }
    return 0;
}

window_server_window_t *window_scene_keyboard_locked(void) {
    window_server_window_t *window =
        window_scene_find_locked(g_window_server.focused_identifier);
    if (window != 0 && !window->minimized &&
        (window->flags & OS_WINDOW_VISIBLE) != 0U) return window;
    for (uint32_t index = g_window_server.scene.count; index != 0U; --index) {
        window = g_window_server.scene.children[index - 1U];
        if (window != 0 && !window->minimized && !window->client_show_inactive &&
            (window->flags & OS_WINDOW_VISIBLE) != 0U) {
            window_scene_set_focus_identifier_locked(window->identifier);
            return window;
        }
    }
    return 0;
}

/* 桌面菜单、切换器、图标拖动和窗口装饰拖动都属于场景捕获，不能让后方控件获得悬停。 */
bool window_scene_pointer_captured_locked(void) {
    return (g_window_server.desktop.menu_open || g_window_server.desktop.switch_open ||
        g_window_server.desktop.pressed_app != DESKTOP_APP_NONE ||
        g_window_server.dragging_identifier != 0U);
}

window_server_window_t *window_scene_pointer_window_locked(void) {
    if (window_scene_pointer_captured_locked()) return 0;
    return window_scene_hit_test_locked(g_window_server.pointer_x, g_window_server.pointer_y);
}
