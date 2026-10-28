#include "internal.h"

/* 场景/Z-order 是焦点窗口 ID 的唯一修改所有者；调用方可复用已查到的旧节点。 */
static void window_scene_change_focus_locked(uint32_t identifier,
    window_server_window_t *old_focused) {
    if (g_window_server.focused_identifier == identifier) return;
    window_control_keyboard_blur_locked(old_focused);
    g_window_server.focused_identifier = identifier;
}

void window_scene_set_focus_identifier_locked(uint32_t identifier) {
    if (g_window_server.focused_identifier == identifier) return;
    window_scene_change_focus_locked(identifier,
        window_scene_find_locked(g_window_server.focused_identifier));
}

void window_scene_focus_locked(window_server_window_t *window) {
    window_server_window_t *old_focused;
    if (window == 0 || window->scene_parent != &g_window_server.scene ||
        window->minimized || (window->flags & OS_WINDOW_VISIBLE) == 0U) return;
    /* 顶层窗口已聚焦时无需查找、排序或制造重绘。其他情况只由注册表定位一次。 */
    if (g_window_server.scene.count != 0U &&
        g_window_server.scene.children[g_window_server.scene.count - 1U] == window &&
        g_window_server.focused_identifier == window->identifier) return;
    old_focused = window_scene_find_locked(g_window_server.focused_identifier);
    window_registry_move_to_front_locked(window);
    window_scene_change_focus_locked(window->identifier, old_focused);
    if (old_focused != window) window_mark_window_locked(old_focused);
    window_mark_window_locked(window);
}
