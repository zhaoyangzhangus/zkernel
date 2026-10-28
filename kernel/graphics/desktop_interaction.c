#include <uapi/window_theme.h>
#include <kernel/string.h>
#include "internal.h"

static struct window_control_document *g_desktop_overlay_text;

/* 每个标签独立终止，布局和绘制共用字节偏移，避免中文偏移错位。 */
static const char desktop_menu_text[] =
    "打开\0整理  ›\0更换壁纸\0"
    "左侧\0右侧\0顶部\0底部\0设置\0重启\0关机";

static void desktop_menu_label(os_control_node_t *node, uint32_t label) {
    uint32_t offset=0;
    while (label--) { while (desktop_menu_text[offset]) ++offset; ++offset; }
    uint32_t length=0;
    while (desktop_menu_text[offset+length]) ++length;
    node->text_offset=offset;node->text_length=length;
}

static bool desktop_switch_candidate(const window_server_window_t *window) {
    return window && (window->flags & OS_WINDOW_VISIBLE) && !window->client_show_inactive;
}

static void desktop_switch_damage(void) {
    /* 仅覆盖中间栏的固定外包围盒；选择变化不会触发全屏重绘。 */
    uint32_t width = g_window_server.display_width < 800U ? g_window_server.display_width : 800U;
    uint32_t height = g_window_server.display_height < 90U ? g_window_server.display_height : 90U;
    window_mark_rect_locked((int32_t)((g_window_server.display_width-width)/2U),
        (int32_t)((g_window_server.display_height-height)/2U), width, height);
}

static void desktop_switch_close(bool commit) {
    desktop_interaction_t *state = &g_window_server.desktop;
    if (!state->switch_open) return;
    desktop_switch_damage();
    state->switch_open = false;
    window_server_window_t *window = commit && state->switch_selected < state->switch_count ?
        window_scene_find_locked(state->switch_windows[state->switch_selected].identifier) : 0;
    if (desktop_switch_candidate(window)) {
        bool minimized = window->minimized;
        window_animation_restore_locked(window);
        window->minimized = false;
        window_scene_focus_locked(window);
        window_mark_window_locked(window);
        if (minimized) window_enqueue_minimized_event_locked(window);
    }
    /* 切换栏消失或叠放顺序改变后，静止指针也须立即离开旧控件并进入新控件。 */
    window_input_refresh_pointer_locked();
}

void desktop_capture_state_locked(void) {
    desktop_interaction_t *state = &g_window_server.desktop;
    uint32_t running = 0, active = 0;
    for (uint32_t i=0; i<g_window_server.scene.count; ++i) {
        const window_server_window_t *window = g_window_server.scene.children[i];
        if (!desktop_switch_candidate(window) || !window->desktop_app ||
            window->desktop_app >= DESKTOP_APP_SLOTS) continue;
        running |= 1U << window->desktop_app;
        if (window->identifier == g_window_server.focused_identifier && !window->minimized)
            active |= 1U << window->desktop_app;
    }
    uint32_t changed = (running ^ state->running_mask) | (active ^ state->active_mask);
    state->running_mask = running;
    state->active_mask = active;
    for (uint32_t app=1; app<DESKTOP_APP_SLOTS; ++app)
        if (changed & (1U << app)) desktop_mark_app_locked(app);
    if (state->switch_open) {
        /* 窗口可能在切换期间退出；丢弃失效标识，绝不保留窗口指针。 */
        uint32_t selected = state->switch_windows[state->switch_selected].identifier;
        uint32_t count=0, selection=0;
        for (uint32_t i=0; i<state->switch_count; ++i) {
            if (!desktop_switch_candidate(window_scene_find_locked(state->switch_windows[i].identifier))) continue;
            if (state->switch_windows[i].identifier == selected) selection = count;
            state->switch_windows[count++] = state->switch_windows[i];
        }
        if (count != state->switch_count) desktop_switch_damage();
        state->switch_count = count;
        state->switch_selected = selection;
        if (!count) desktop_switch_close(false);
    }
}

static void desktop_switch_advance(const input_event_t *event) {
    desktop_interaction_t *state = &g_window_server.desktop;
    if (!state->switch_open) {
        if (state->pressed_app) {
            desktop_mark_app_locked(state->pressed_app);
            state->pressed_app=0;
            state->dragging=false;
            state->swallow_release=true;
        }
        state->switch_count = state->switch_selected = 0;
        for (uint32_t i=g_window_server.scene.count; i>0 && state->switch_count<DESKTOP_SWITCH_WINDOWS; --i) {
            const window_server_window_t *window = g_window_server.scene.children[i-1];
            if (!desktop_switch_candidate(window)) continue;
            uint32_t slot = state->switch_count++;
            state->switch_windows[slot].identifier = window->identifier;
            for (uint32_t n=0; n<32; ++n) state->switch_windows[slot].title[n] = window->title[n];
            state->switch_windows[slot].title[31] = 0;
            if (window->identifier == g_window_server.focused_identifier) state->switch_selected = slot;
        }
        if (!state->switch_count) return;
        state->switch_open = true;
        /* 修饰键按下已交给原窗口；切换栏接管前配对释放，避免应用留下 Alt/Shift 按住状态。 */
        window_server_window_t *focused = window_scene_find_locked(g_window_server.focused_identifier);
        if (focused) {
            const uint32_t codes[4] = {0xE2,0xE6,0xE1,0xE5};
            for (uint32_t i=0; i<4; ++i) {
                if (!((i<2 ? state->alt_mask : state->shift_mask) & (1U << (i%2)))) continue;
                input_event_t release = *event;
                release.code = codes[i]; release.value = INPUT_VALUE_RELEASE;
                window_enqueue_event_locked(focused, &release);
            }
        }
    } else desktop_capture_state_locked();
    if (!state->switch_count) return;
    state->switch_selected = (state->switch_selected +
        (state->shift_mask ? state->switch_count-1U : 1U)) % state->switch_count;
    desktop_switch_damage();
}

static uint32_t desktop_switch_tree(const desktop_interaction_t *state, uint32_t width,
    uint32_t height, os_control_node_t nodes[6], char text[160], uint32_t entries[5], Rect *viewport) {
    uint32_t w=width<800U?width:800U, h=height<90U?height:90U;
    *viewport=(Rect){(int32_t)((width-w)/2U),(int32_t)((height-h)/2U),
        (int32_t)((width+w)/2U),(int32_t)((height+h)/2U)};
    uint32_t visible=w/150U;
    if (!visible) visible=1;
    if (visible>5) visible=5;
    if (visible>state->switch_count) visible=state->switch_count;
    nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
        .layout=OS_CONTROL_ROW,.padding=8,.gap=4,.radius=OS_THEME_RADIUS,
        .background=OS_THEME_SURFACE,.foreground=OS_THEME_TEXT,.font_size=16,
        .border_width=1,.border_color=OS_THEME_BORDER,.flags=OS_CONTROL_CLIP_CHILDREN};
    uint32_t start=state->switch_selected>visible/2U?state->switch_selected-visible/2U:0;
    if (start+visible>state->switch_count) start=state->switch_count-visible;
    for (uint32_t i=0; i<visible; ++i) {
        uint32_t entry=start+i, length=0;
        entries[i]=entry;
        while (length<31 && state->switch_windows[entry].title[length]) {
            text[i*32+length]=state->switch_windows[entry].title[length]; ++length;
        }
        if (!length) {text[i*32]='?';length=1;}
        text[i*32+length]=0;
        nodes[i+1]=(os_control_node_t){.identifier=i+2,.parent=0,.kind=OS_CONTROL_BUTTON,
            .flags=OS_CONTROL_INHERIT_TEXT|OS_CONTROL_FLEX_GROW,.width=0,.height=OS_CONTROL_FILL,
            .radius=OS_THEME_RADIUS,.background=entry==state->switch_selected?OS_THEME_SELECTED:OS_THEME_TOOLBAR,
            .border_width=entry==state->switch_selected?2:0,.border_color=OS_THEME_ACCENT,
            .text_offset=i*32,.text_length=length};
    }
    return visible+1;
}

static uint32_t desktop_switch_hit(void) {
    os_control_node_t nodes[6]; char text[160]; uint32_t entries[5]; Rect viewport;
    uint32_t count=desktop_switch_tree(&g_window_server.desktop,g_window_server.display_width,
        g_window_server.display_height,nodes,text,entries,&viewport);
    uint32_t hit=control_tree_locate(nodes,count,viewport,(int32_t)g_window_server.pointer_x,
        (int32_t)g_window_server.pointer_y,0);
    return hit && hit!=OS_CONTROL_NO_PARENT ? entries[hit-1] : UINT32_MAX;
}

Rect desktop_menu_tree(const desktop_interaction_t *state, uint32_t width,
    uint32_t height, os_control_node_t nodes[DESKTOP_MENU_NODES]) {
    memset(nodes,0,sizeof(*nodes)*DESKTOP_MENU_NODES);
    int32_t w=width<224U?(int32_t)width:224;
    bool settings_menu=state->menu_app==DESKTOP_APP_SETTINGS;
    uint32_t rows=settings_menu?6U:(state->menu_app?4U:3U);
    int32_t h=(int32_t)os_control_menu_height(16U,rows);
    int32_t x=state->menu_x,y=state->menu_y;
    /* 窄屏给两个面板各留一半宽度，避免子菜单盖住触发项并误吃点击。 */
    if (width>1U && width<448U) {
        w=(int32_t)(width/2U);
        x=state->menu_x<(int32_t)(width/2U)?0:(int32_t)width-w;
    }
    if (x>(int32_t)width-w) x=(int32_t)width-w;
    if (y>(int32_t)height-h) y=(int32_t)height-h;
    if (x<0) x=0;
    if (y<0) y=0;
    nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,
        .kind=OS_CONTROL_CONTAINER,.layout=OS_CONTROL_ABSOLUTE,
        .flags=OS_CONTROL_BACKGROUND_ALPHA,.font_size=16,.foreground=OS_THEME_TEXT};
    nodes[1]=(os_control_node_t){.identifier=2,.parent=0,.kind=OS_CONTROL_CONTAINER,
        .flags=OS_CONTROL_MENU_PANEL|OS_CONTROL_INHERIT_TEXT,.x=x,.y=y,.width=w};
    for (uint32_t i=0;i<3U;++i) {
        nodes[i+2]=(os_control_node_t){.identifier=10U+i,.parent=1,.kind=OS_CONTROL_LIST_ITEM,
            .flags=OS_CONTROL_INHERIT_TEXT|(i==0 && !state->menu_app?OS_CONTROL_HIDDEN:0U)};
        desktop_menu_label(&nodes[i+2],i);
    }
    int32_t sx=x+w-1,sy=y+4+(state->menu_app?32:0);
    if (sx+w>(int32_t)width) sx=x-w+1;
    if (sx<0) sx=0;
    /* 两个面板同属一个透明根节点，子菜单向屏幕内展开。 */
    nodes[8]=(os_control_node_t){.identifier=3,.parent=0,.kind=OS_CONTROL_CONTAINER,
        .flags=OS_CONTROL_MENU_PANEL|OS_CONTROL_INHERIT_TEXT|
            (state->menu_submenu?0U:OS_CONTROL_HIDDEN),.x=sx,.y=sy,.width=w};
    for (uint32_t i=0;i<4U;++i) {
        nodes[i+9]=(os_control_node_t){.identifier=20U+i,
            .parent=8,.kind=OS_CONTROL_LIST_ITEM,.flags=OS_CONTROL_INHERIT_TEXT|
                (!state->menu_submenu?OS_CONTROL_HIDDEN:0U)};
        desktop_menu_label(&nodes[i+9],3U+i);
    }
    for (uint32_t i=0;i<3U;++i) {
        nodes[i+5]=(os_control_node_t){.identifier=13U+i,.parent=1,
            .kind=OS_CONTROL_LIST_ITEM,.flags=OS_CONTROL_INHERIT_TEXT|
                (i && !settings_menu?OS_CONTROL_HIDDEN:0U)};
        desktop_menu_label(&nodes[i+5],7U+i);
    }
    return (Rect){0,0,(int32_t)width,(int32_t)height};
}

static uint32_t desktop_menu_hit(void) {
    os_control_node_t nodes[DESKTOP_MENU_NODES];
    Rect viewport=desktop_menu_tree(&g_window_server.desktop,g_window_server.display_width,
        g_window_server.display_height,nodes);
    uint32_t index=control_tree_locate(nodes,DESKTOP_MENU_NODES,viewport,
        (int32_t)g_window_server.pointer_x,(int32_t)g_window_server.pointer_y,0);
    return index!=OS_CONTROL_NO_PARENT && nodes[index].kind==OS_CONTROL_LIST_ITEM ?
        nodes[index].identifier:0;
}

static void desktop_menu_damage(void) {
    os_control_node_t nodes[DESKTOP_MENU_NODES];control_tree_box_t boxes[DESKTOP_MENU_NODES];
    Rect viewport=desktop_menu_tree(&g_window_server.desktop,g_window_server.display_width,
        g_window_server.display_height,nodes);
    if (!control_tree_layout(nodes,DESKTOP_MENU_NODES,sizeof(desktop_menu_text)-1U,viewport,boxes)) return;
    const uint32_t panels[]={1U,8U};
    for (uint32_t i=0;i<2U;++i) {
        uint32_t n=panels[i];if (nodes[n].flags&OS_CONTROL_HIDDEN) continue;
        Rect r=boxes[n].bounds;
        if (!rect_is_empty(r)) window_mark_rect_locked(r.x0,r.y0,(uint32_t)(r.x1-r.x0),(uint32_t)(r.y1-r.y0));
    }
}

static void desktop_menu_close(void) {
    if (g_window_server.desktop.menu_open) desktop_menu_damage();
    g_window_server.desktop.menu_open = false;
    g_window_server.desktop.menu_submenu=0;
    g_window_server.desktop.menu_hover = g_window_server.desktop.menu_pressed = 0;
}

/* 左右两侧沿纵轴居中，上下两侧沿横轴居中；放不下时向桌面内部换列或换行。 */
static void desktop_arrange(uint32_t direction) {
    desktop_interaction_t *state=&g_window_server.desktop;
    Rect viewport={0,0,(int32_t)g_window_server.display_width,(int32_t)g_window_server.display_height};
    if (rect_is_empty(viewport) || direction>3U) return;
    bool arrange_columns=direction<2U;
    uint32_t width=0,height=0,count=0;
    struct {uint32_t app;control_tree_box_t box;} icons[DESKTOP_APP_SLOTS];
    /* 整理期间窗口锁保持不变，一次取得的基础几何可用于旧位置和新位置。 */
    for (uint32_t app=1;app<DESKTOP_APP_SLOTS;++app) {
        control_tree_box_t box;
        if (!desktop_base_icon_box_locked(app,&box)) continue;
        uint32_t w=(uint32_t)(box.bounds.x1-box.bounds.x0),h=(uint32_t)(box.bounds.y1-box.bounds.y0);
        if (w>width) width=w;
        if (h>height) height=h;
        icons[count].app=app;icons[count++].box=box;
    }
    if (!width || !height) return;
    uint32_t screen_w=(uint32_t)viewport.x1,screen_h=(uint32_t)viewport.y1;
    uint32_t margin=18U,gap=12U;
    if (screen_w<=width+2U*margin || screen_h<=height+2U*margin) margin=0U;
    uint32_t columns=screen_w>2U*margin ? (screen_w-2U*margin+gap)/(width+gap) : 1U;
    uint32_t rows=screen_h>2U*margin ? (screen_h-2U*margin+gap)/(height+gap) : 1U;
    if (!columns) columns=1U;
    if (!rows) rows=1U;
    uint32_t used_columns=arrange_columns?(count+rows-1U)/rows:(count<columns?count:columns);
    uint32_t used_rows=arrange_columns?(count<rows?count:rows):(count+columns-1U)/columns;
    uint32_t extent_w=used_columns?used_columns*width+(used_columns-1U)*gap:0U;
    uint32_t extent_h=used_rows?used_rows*height+(used_rows-1U)*gap:0U;
    for (uint32_t slot=0;slot<count;++slot) {
        uint32_t app=icons[slot].app;
        control_tree_box_t box=icons[slot].box;
        desktop_place_box(state,app,viewport,&box);
        desktop_mark_icon_box_locked(&box);
        uint32_t col=!arrange_columns ? slot%columns : (slot/rows)%columns;
        uint32_t row=!arrange_columns ? (slot/columns)%rows : slot%rows;
        uint32_t w=(uint32_t)(box.bounds.x1-box.bounds.x0),h=(uint32_t)(box.bounds.y1-box.bounds.y0);
        int32_t x=(int32_t)(margin+col*(width+gap)),y=(int32_t)(margin+row*(height+gap));
        if (direction==1U) x=(int32_t)screen_w-(int32_t)margin-(int32_t)w-(int32_t)(col*(width+gap));
        if (direction==3U) y=(int32_t)screen_h-(int32_t)margin-(int32_t)h-(int32_t)(row*(height+gap));
        if (arrange_columns && screen_h>extent_h)
            y=(int32_t)((screen_h-extent_h)/2U+row*(height+gap));
        if (!arrange_columns && screen_w>extent_w)
            x=(int32_t)((screen_w-extent_w)/2U+col*(width+gap));
        if (x<0) x=0;
        if (y<0) y=0;
        state->positions[app].x=x;state->positions[app].y=y;state->positions[app].placed=true;
        desktop_place_box(state,app,viewport,&box);
        desktop_mark_icon_box_locked(&box);
    }
    state->top_app=0;
    window_input_refresh_pointer_locked();
}

bool desktop_route_key_locked(const input_event_t *event) {
    desktop_interaction_t *state = &g_window_server.desktop;
    if (event->type != INPUT_EVENT_KEY) return false;
    if (event->code==0xE2U || event->code==0xE6U) {
        uint32_t bit=event->code==0xE2U?1U:2U;
        bool captured=state->switch_open;
        if (event->value==INPUT_VALUE_RELEASE) state->alt_mask &= ~bit;
        else state->alt_mask |= bit;
        if (captured && !state->alt_mask) desktop_switch_close(true);
        return captured;
    }
    if (event->code==0xE1U || event->code==0xE5U) {
        uint32_t bit=event->code==0xE1U?1U:2U;
        if (event->value==INPUT_VALUE_RELEASE) state->shift_mask &= ~bit;
        else state->shift_mask |= bit;
        return state->switch_open;
    }
    if (event->code==0x2BU && (state->alt_mask || state->tab_consumed)) {
        if (event->value==INPUT_VALUE_RELEASE) state->tab_consumed=false;
        else {
            desktop_menu_close();
            state->tab_consumed=true;
            desktop_switch_advance(event);
        }
        return true;
    }
    if (state->switch_open) {
        if (event->code==0x29U && event->value==INPUT_VALUE_PRESS) desktop_switch_close(false);
        if (event->code==0x28U && event->value==INPUT_VALUE_PRESS) desktop_switch_close(true);
        return true;
    }
    if (event->code == 0x29U && state->pressed_app) {
        desktop_mark_app_locked(state->pressed_app);
        state->pressed_app = 0;
        state->dragging = false;
        state->swallow_release = true;
        return true;
    }
    if (!state->menu_open) return false;
    if (event->code == 0x29U && event->value == INPUT_VALUE_PRESS) desktop_menu_close();
    /* 弹出菜单期间，按键不能泄漏给原来聚焦的应用。 */
    return true;
}

bool desktop_route_motion_locked(void) {
    desktop_interaction_t *state = &g_window_server.desktop;
    if (state->switch_open) {
        return true;
    }
    if (state->menu_open) {
        uint32_t hover = desktop_menu_hit();
        if (hover != state->menu_hover) {
            desktop_menu_damage();
            if (hover==11U) state->menu_submenu=1U;
            else if (hover==10U || (hover>=12U && hover<=15U)) state->menu_submenu=0;
            state->menu_hover = hover;
            desktop_menu_damage();
        }
        return true;
    }
    uint32_t app = state->pressed_app;
    if (!app) return false;
    int64_t dx = (int64_t)g_window_server.pointer_x-state->press_x;
    int64_t dy = (int64_t)g_window_server.pointer_y-state->press_y;
    if (!state->dragging && dx > -4 && dx < 4 && dy > -4 && dy < 4) return true;
    control_tree_box_t box;
    if (!desktop_base_icon_box_locked(app, &box)) return true;
    state->dragging = true;
    desktop_mark_app_locked(app);
    int64_t x = state->origin_x+dx, y = state->origin_y+dy;
    int64_t max_x = (int64_t)g_window_server.display_width-(box.bounds.x1-box.bounds.x0);
    int64_t max_y = (int64_t)g_window_server.display_height-(box.bounds.y1-box.bounds.y0);
    if (x > max_x) x = max_x;
    if (y > max_y) y = max_y;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    state->positions[app].x = (int32_t)x;
    state->positions[app].y = (int32_t)y;
    state->positions[app].placed = true;
    state->top_app = app;
    desktop_mark_app_locked(app);
    return true;
}

bool desktop_route_button_locked(const input_event_t *event, bool over_window) {
    if (event->type != INPUT_EVENT_BUTTON) return false;
    desktop_interaction_t *state = &g_window_server.desktop;
    if (state->switch_open) {
        if (event->code==INPUT_BUTTON_LEFT && event->value==INPUT_VALUE_PRESS) {
            uint32_t hit=desktop_switch_hit();
            if (hit!=UINT32_MAX) state->switch_selected=hit;
            desktop_switch_close(hit!=UINT32_MAX);
            state->swallow_release=true;
        }
        return true;
    }
    if (event->code == INPUT_BUTTON_LEFT && event->value == INPUT_VALUE_RELEASE && state->swallow_release) {
        state->swallow_release = false;
        return true;
    }
    if (state->menu_open) {
        if (event->code == INPUT_BUTTON_LEFT) {
            uint32_t item = desktop_menu_hit();
            if (event->value == INPUT_VALUE_PRESS) {
                state->menu_pressed = item;
                desktop_menu_damage();
                if (!item) {
                    desktop_menu_close();
                    state->swallow_release = true;
                }
            } else if (event->value == INPUT_VALUE_RELEASE) {
                uint32_t pressed=state->menu_pressed,app=state->menu_app;
                if (item==11U) {
                    desktop_menu_damage();state->menu_submenu=1U;
                    state->menu_pressed=0;desktop_menu_damage();return true;
                }
                desktop_menu_close();
                if (item && item==pressed) {
                    if (item==10U && !g_window_server.desktop_pending_launch)
                        g_window_server.desktop_pending_launch=app;
                    if (item>=20U && item<=23U) desktop_arrange(item-20U);
                    if (item==12U && !g_window_server.desktop_pending_launch)
                        g_window_server.desktop_pending_launch=DESKTOP_APP_WALLPAPER;
                    if (item==13U && !g_window_server.desktop_pending_launch)
                        g_window_server.desktop_pending_launch=DESKTOP_APP_SETTINGS;
                    if (item==14U || item==15U) state->power_request=item-13U;
                }
            }
            return true;
        }
        if (event->code != INPUT_BUTTON_RIGHT || event->value != INPUT_VALUE_PRESS || over_window)
            return true;
        desktop_menu_close();
    }
    if (event->code == INPUT_BUTTON_LEFT && state->pressed_app) {
        if (event->value == INPUT_VALUE_RELEASE) {
            uint32_t app = state->pressed_app;
            if (!state->dragging && !over_window &&
                desktop_app_at_locked(g_window_server.pointer_x, g_window_server.pointer_y) == app &&
                !g_window_server.desktop_pending_launch)
                g_window_server.desktop_pending_launch = app;
            state->pressed_app = 0;
            state->dragging = false;
            desktop_mark_app_locked(app);
        }
        return true;
    }
    /* 拖动捕获期间，其他按钮不得改变菜单或把事件送入应用。 */
    if (state->pressed_app) return true;
    if (over_window) return false;
    uint32_t app = desktop_app_at_locked(g_window_server.pointer_x, g_window_server.pointer_y);
    if (event->code == INPUT_BUTTON_RIGHT) {
        if (event->value == INPUT_VALUE_PRESS) {
            state->menu_open = true;
            state->menu_submenu=0;
            state->menu_x = (int32_t)g_window_server.pointer_x;
            state->menu_y = (int32_t)g_window_server.pointer_y;
            state->menu_app = app;
            state->menu_hover = state->menu_pressed = 0;
            desktop_menu_damage();
        }
        return true;
    }
    if (event->code == INPUT_BUTTON_LEFT && event->value == INPUT_VALUE_PRESS && app) {
        control_tree_box_t box;
        if (!desktop_base_icon_box_locked(app, &box)) return true;
        desktop_place_box(state, app, (Rect){0}, &box);
        state->pressed_app = app;
        state->press_x = g_window_server.pointer_x;
        state->press_y = g_window_server.pointer_y;
        state->origin_x = box.bounds.x0;
        state->origin_y = box.bounds.y0;
        state->dragging = false;
        state->top_app = app;
        desktop_mark_app_locked(app);
        return true;
    }
    return false;
}

/* 快照已捕获，字形准备不持有窗口锁；悬停、按压和位置变化复用同一份文字缓存。 */
void desktop_prepare_text(void) {
    const desktop_interaction_t *state=&g_compositor_snapshot.desktop;
    if (!state->switch_open && !state->menu_open) return;
    os_control_node_t nodes[DESKTOP_MENU_NODES];control_tree_box_t boxes[DESKTOP_MENU_NODES];
    char text[160]={0};uint32_t count,text_size;Rect viewport;
    const char *labels;
    if (state->switch_open) {
        uint32_t entries[5];
        count=desktop_switch_tree(state,g_window_server.display_width,
            g_window_server.display_height,nodes,text,entries,&viewport);
        labels=text;text_size=sizeof(text);
    } else {
        count=DESKTOP_MENU_NODES;labels=desktop_menu_text;text_size=sizeof(desktop_menu_text)-1U;
        viewport=desktop_menu_tree(state,g_window_server.display_width,g_window_server.display_height,nodes);
    }
    struct window_control_document *next=0;
    if (control_tree_layout(nodes,count,text_size,viewport,boxes))
        next=window_control_prepare_overlay(g_desktop_overlay_text,nodes,count,labels,text_size,boxes);
    window_control_document_put(g_desktop_overlay_text);g_desktop_overlay_text=next;
}

void desktop_draw_menu(void) {
    const desktop_interaction_t *state = &g_compositor_snapshot.desktop;
    if (!g_desktop_overlay_text) return;
    if (state->switch_open) {
        os_control_node_t nodes[6]; control_tree_box_t boxes[6];
        char text[160]; uint32_t entries[5]; Rect viewport;
        uint32_t count=desktop_switch_tree(state,g_window_server.display_width,
            g_window_server.display_height,nodes,text,entries,&viewport);
        if (control_tree_layout(nodes,count,sizeof(text),viewport,boxes))
            compositor_control_nodes(nodes,count,boxes,text,0,0,g_desktop_overlay_text);
        return;
    }
    if (!state->menu_open) return;
    os_control_node_t nodes[DESKTOP_MENU_NODES];
    control_tree_box_t boxes[DESKTOP_MENU_NODES];
    Rect viewport = desktop_menu_tree(state, g_window_server.display_width,
        g_window_server.display_height, nodes);
    if (control_tree_layout(nodes, DESKTOP_MENU_NODES, sizeof(desktop_menu_text)-1U, viewport, boxes))
        compositor_control_nodes(nodes, DESKTOP_MENU_NODES, boxes, desktop_menu_text,
            state->menu_hover, state->menu_pressed,g_desktop_overlay_text);
}
