#pragma once

#include <kernel/control_tree.h>

#define DESKTOP_APP_SLOTS 8U
#define DESKTOP_MENU_NODES 13U
#define DESKTOP_SWITCH_WINDOWS 64U

/* 输入所有者修改状态，合成器只读取按帧复制的状态；不能修改已发布的 UI 节点。 */
typedef struct desktop_interaction {
    struct { int32_t x, y; bool placed; } positions[DESKTOP_APP_SLOTS];
    uint32_t pressed_app, top_app;
    uint32_t press_x, press_y;
    int32_t origin_x, origin_y;
    bool dragging, swallow_release;
    bool menu_open;
    int32_t menu_x, menu_y;
    uint32_t menu_app, menu_hover, menu_pressed, menu_submenu;
    /* 0 表示无请求，1 为重启，2 为关机；输入泵释放窗口锁后执行。 */
    uint32_t power_request;
    uint32_t running_mask, active_mask;
    uint32_t alt_mask, shift_mask;
    bool switch_open, tab_consumed;
    uint32_t switch_count, switch_selected;
    struct { uint32_t identifier; char title[32]; } switch_windows[DESKTOP_SWITCH_WINDOWS];
} desktop_interaction_t;

static inline void desktop_place_box(const desktop_interaction_t *state,
    uint32_t app, Rect viewport, control_tree_box_t *box) {
    if (!state || !app || app >= DESKTOP_APP_SLOTS || !state->positions[app].placed) return;
    int32_t width = box->bounds.x1 - box->bounds.x0;
    int32_t height = box->bounds.y1 - box->bounds.y0;
    box->bounds = (Rect){state->positions[app].x, state->positions[app].y,
        state->positions[app].x + width, state->positions[app].y + height};
    /* 拖离原容器后仍受桌面视口裁剪，而不是被原来的停靠栏裁掉。 */
    box->clip = viewport;
}

/* 菜单也是普通控件树，命中、布局、主题和绘制均复用应用控件实现。 */
Rect desktop_menu_tree(const desktop_interaction_t *state, uint32_t width,
    uint32_t height, os_control_node_t nodes[DESKTOP_MENU_NODES]);
