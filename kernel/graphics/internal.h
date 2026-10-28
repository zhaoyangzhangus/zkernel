#pragma once

#include <kernel/input.h>
#include <kernel/object.h>
#include <kernel/process.h>
#include <kernel/rect.h>
#include <kernel/desktop_interaction.h>
#include <kernel/caption_text.h>
#include <kernel/shared_section.h>
#include <kernel/spinlock.h>
#include <kernel/wait.h>
#include <kernel/window_geometry.h>
#include <kernel/window_server.h>
#include <kernel/window_surface_exchange.h>

#include "png.h"

/* 启动状态用于唤醒目标发布，创建权单独保护线程对象，防止并发清零或失败回滚互相覆盖。 */
static inline bool window_worker_claim_start(atomic_bool *starting,
                                             const atomic_bool *started) {
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(starting, &expected, true,
            memory_order_acq_rel, memory_order_acquire)) return false;
    if (atomic_load_explicit(started, memory_order_acquire)) {
        atomic_store_explicit(starting, false, memory_order_release);
        return false;
    }
    return true;
}

bool window_minimize_locked(window_server_window_t *window);
void window_input_release_pointer_locked(window_server_window_t *window);
void window_enqueue_minimized_event_locked(window_server_window_t *window);

/* REFACTOR_P7A_WINDOW_INTERNAL_OWNER：Window 具体状态保持私有。 */

#ifndef WINDOW_DAMAGE_MAX_RECTS
#define WINDOW_DAMAGE_MAX_RECTS 16U
#endif
#ifndef WINDOW_DAMAGE_TILE_SIZE
#define WINDOW_DAMAGE_TILE_SIZE 64U
#endif
#ifndef WINDOW_DAMAGE_MAX_TILES_X
#define WINDOW_DAMAGE_MAX_TILES_X 128U
#endif
#ifndef WINDOW_DAMAGE_MAX_TILES_Y
#define WINDOW_DAMAGE_MAX_TILES_Y 128U
#endif
#ifndef WINDOW_DAMAGE_TILE_COUNT
#define WINDOW_DAMAGE_TILE_COUNT \
    (WINDOW_DAMAGE_MAX_TILES_X * WINDOW_DAMAGE_MAX_TILES_Y)
#endif
#ifndef WINDOW_DAMAGE_TILE_WORDS
#define WINDOW_DAMAGE_TILE_WORDS \
    ((WINDOW_DAMAGE_TILE_COUNT + 63U) / 64U)
#endif
#ifndef WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS
#define WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS 4096U
#endif
#ifndef WINDOW_RENDER_PLAN_MAX_SPANS
#define WINDOW_RENDER_PLAN_MAX_SPANS 4096U
#endif

/* 在一个输入泵周期内最多处理一次活动拖动。路由器负责此预算；
 * 底层窗口策略仅消费生成的批次。 */
#ifndef WINDOW_COMPOSITOR_DRAG_INPUT_EVENTS
#define WINDOW_COMPOSITOR_DRAG_INPUT_EVENTS \
    (WINDOW_EVENT_CAPACITY * 4U)
#endif

/* 帧所有者和扫描输出发布所有者共用的私有发布策略。
 * 这些阈值保持现有事务策略，同时避免将实现放入 compositor.c。 */
#define WINDOW_COMPOSITOR_PUBLICATION_YIELD_PIXELS (128U * 1024U)
#define WINDOW_COMPOSITOR_ATOMIC_PIXELS (128U * 1024U)
#define WINDOW_EVENT_CAPACITY     64U
#define WINDOW_SERVER_MAX_WINDOWS 64U
/* Ring0 标题栏占据整个窗口拖动区域。 */
#define WINDOW_DRAG_REGION_HEIGHT 31U
/* 固定圆角半径，合成器使用预计算的扫描线表。 */
#define WINDOW_CORNER_RADIUS      8U

/*
 * Window 的具体状态是图形模块私有对象。公开调用方只接收不透明指针，并通过窗口服务器
 * API/访问器操作。
 */
struct window_control_document;
struct window_scene_root;
struct window_server_window {
    object_header_t object;
    /* 父节点由场景注册表维护，控件文档与 Surface 位于此窗口之下。 */
    struct window_scene_root *scene_parent;
    process_t *owner;
    shared_section_t *section;
    uint32_t identifier;
    uint32_t desktop_app;
    uint64_t animation_start;
    bool animation_seen;
    bool animation_restore;
    int32_t animation_dx, animation_dy;
    bool surface_content_ready;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint32_t background;
    uint64_t buffer_size;
    uint64_t owner_address;

    uint64_t surface_span;
    bool surface_exchange_enabled;
    window_surface_exchange_t surface_exchange;
    uint32_t surface_writer_width;
    uint32_t surface_writer_height;
    uint32_t surface_front_width, surface_front_height;
    wait_queue_t surface_waitq;

    /* 完整分配的表面后备区以及首次呈现遥测数据。 */
    uint64_t create_tsc;
    uint64_t surface_fault_count_at_create;
    uint64_t surface_populated_bytes;
    /* 为兼容增量对象布局而保留；延迟填充路径已不再写入此槽位。 */
    uint64_t surface_populate_pending_end;
    bool surface_metrics_reported;

    /* 旧式 PRESENT 完成机制只会在自身提交后控制复用。它无法阻止下一次生产者写入期间出现
     * 无关快照；交换模式槽位租约会单独提供该所有权保证。 */
    uint64_t present_sequence;
    atomic_uint_fast64_t compositor_presented_sequence;
    /* 为尚未完成的 PRESENT 等待者保存持久终止状态。 */
    atomic_bool registry_removed;

    void *compositor_cache;
    struct window_control_document *controls;
    uint64_t control_revision;
    uint32_t control_hover, control_pressed, control_focus;
    uint32_t control_text_target,control_text_anchor,control_text_end,control_text_ctrl;
    /* 0 无捕获，1 已按下，2 已发生拖选；回到起点不能重新激活按钮。 */
    uint8_t control_text_drag;
    Rect control_text_bounds;
    uint32_t control_text_font;
    uint32_t control_key_target, control_key_code, control_shift;
    /* 输入线程独占的可见按钮范围，与合成器布局缓存分离。 */
    Rect control_hover_damage;
    uint32_t control_input_width, control_input_height;
    /* 范围控件的输入几何独立于绘制缓存；版本及视口变化后不得继续复用。 */
    Rect control_range_bounds;
    uint32_t control_range_target, control_range_width, control_range_height, control_range_flags;
    uint64_t control_range_revision;

    /* 保留场景/扫描输出已提交的几何信息。 */
    int32_t compositor_presented_x;
    int32_t compositor_presented_y;
    uint32_t compositor_presented_width;
    uint32_t compositor_presented_height;
    bool compositor_presented_valid;

    bool dirty;
    bool client_hidden;
    bool client_pointer_capture;
    bool client_button_capture;
    bool client_show_inactive;
    bool resize_pending;
    bool maximized;
    bool minimized;
    bool fullscreen;
    int32_t fullscreen_restore_x;
    int32_t fullscreen_restore_y;
    uint32_t fullscreen_restore_width;
    uint32_t fullscreen_restore_height;
    uint32_t fullscreen_restore_flags;

    int32_t restore_x;
    int32_t restore_y;
    uint32_t restore_width;
    uint32_t restore_height;

    char title[32];
    caption_text_t *caption;
    os_window_event_t events[WINDOW_EVENT_CAPACITY];
    uint32_t event_read;
    uint32_t event_write;
    uint32_t event_count;
    bool event_wake_pending;
    wait_queue_t event_waitq;
};

typedef struct window_motion_batch {
    bool active;
    uint32_t device_id;
    uint16_t flags;
    uint64_t timestamp;
    int64_t delta_x;
    int64_t delta_y;
} window_motion_batch_t;

#define WINDOW_CURSOR_WIDTH 24U
#include "cursor_shape.h"
#define WINDOW_CURSOR_HEIGHT 24U
#define WINDOW_CURSOR_HOTSPOT_X 3U
#define WINDOW_CURSOR_HOTSPOT_Y 1U
#define WINDOW_DESKTOP_LAUNCH_QUEUE_CAPACITY 8U

extern const uint32_t g_linux_cursor_argb[
    WINDOW_CURSOR_WIDTH * WINDOW_CURSOR_HEIGHT];

typedef Rect Rect;

/* 桌面是唯一场景根。子节点顺序同时定义绘制顺序、命中优先级和窗口切换顺序。
 * 桌面文件加载出的控件树也挂在此处，不再维护独立的全局控件列表。 */
typedef struct window_scene_root {
    window_server_window_t *children[WINDOW_SERVER_MAX_WINDOWS];
    uint32_t count;
    const os_control_node_t *controls;
    uint32_t control_count;
    /* 保存上次实际指针命中，而非从旧坐标重新猜测；窗口移动或遮挡后仍能正确离开。 */
    uint32_t hovered_window_identifier;
    uint32_t hovered_title_button;
} window_scene_root_t;

typedef struct window_server_state {
    spinlock_t lock;
    wait_queue_t event_waitq;
    wait_queue_t worker_waitq;
    atomic_uint_fast64_t worker_generation;

    window_server_window_t *event_ready_windows[WINDOW_SERVER_MAX_WINDOWS];
    uint32_t event_ready_count;
    bool event_ready_overflow;

    window_scene_root_t scene;
    uint32_t next_identifier;
    uint32_t focused_identifier;
    process_t *manager;
    uint32_t display_width;
    uint32_t display_height;
    uint32_t display_stride;
    uint32_t display_format;
    volatile uint32_t *framebuffer;
    volatile uint32_t *composite_framebuffer;
    uint32_t pointer_x;
    uint32_t pointer_y;
    uint32_t presented_pointer_x;
    uint32_t presented_pointer_y;
    uint32_t presented_cursor_shape;
    bool presented_pointer_valid;
    uint32_t desktop_hovered_app;
    desktop_interaction_t desktop;
    uint32_t desktop_pending_launch;
    uint32_t desktop_gui_mask;
    bool desktop_tab_consumed;
    bool desktop_focus_cycle_requested;
    uint32_t dragging_identifier;
    int32_t drag_offset_x;
    int32_t drag_offset_y;
    uint32_t resize_edges;
    bool drag_blit_valid;
    int32_t drag_old_x;
    int32_t drag_old_y;
    int32_t drag_new_x;
    int32_t drag_new_y;
    uint32_t drag_width;
    uint32_t drag_height;

    uint32_t title_pressed_identifier;
    uint32_t title_pressed_button;
    uint32_t title_pressed_pointer_x;
    uint32_t title_pressed_pointer_y;
    bool title_pressed_client;
    input_event_t title_pressed_event;

    thread_t worker;
    atomic_bool worker_started;
    wait_queue_t desktop_launch_waitq;
    thread_t desktop_launch_worker;
    atomic_bool desktop_launch_worker_started;
    /* 0=空闲，UINT64_MAX=启动中，否则为所属 Chrome PID。 */
    atomic_uint_fast64_t desktop_chrome_launch_pid;
    atomic_bool desktop_chrome_relaunch_pending;
    /* 由 WindowLock 保护；最后一个所属窗口已移除。 */
    uint64_t desktop_chrome_closing_pid;
    atomic_uint desktop_launch_queue_count;
    uint32_t desktop_launch_queue[
        WINDOW_DESKTOP_LAUNCH_QUEUE_CAPACITY];
    uint32_t desktop_launch_queue_head;
    uint32_t desktop_launch_queue_tail;
    thread_t desktop_asset_worker;
    atomic_bool desktop_asset_worker_started;
    bool kernel_ready;
    bool dirty;
    bool composing;
    bool damage_full;
    bool damage_tiles_active;
    uint32_t damage_count;
    Rect damage_rects[WINDOW_DAMAGE_MAX_RECTS];
    uint64_t damage_tiles[WINDOW_DAMAGE_TILE_WORDS];
    Rect damage_bounds;
} window_server_state_t;

typedef desktop_png_image_t desktop_asset_image_t;

extern atomic_bool g_desktop_assets_available;
extern desktop_asset_image_t g_desktop_wallpaper_asset;
extern desktop_asset_image_t g_desktop_app_assets[8];
typedef struct desktop_app_binding {
    char path[256], icon[256], title[32];
    uint32_t node_index;
} desktop_app_binding_t;
/* 资源线程完整校验桌面 UI 后发布，只读绑定随桌面资源一起可见。 */
extern desktop_app_binding_t g_desktop_programs[8];
/* 捕获后由合成器持有的逐帧不可变数据。快照创建后，窗口生产者可以继续修改实时场景。 */
typedef struct compositor_window_view {
    window_server_window_t *reference;
    struct window_control_document *controls;
    uint32_t control_hover, control_pressed, control_focus, control_key_target;
    uint32_t control_text_target,control_text_anchor,control_text_end;
    shared_section_t *section;

    uint32_t identifier;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint32_t background;
    uint64_t buffer_size;
    uint64_t owner_address;
    uint64_t present_sequence;
    char title[32];
    caption_text_t *caption;
    uint64_t surface_offset;
    uint32_t surface_slot;
    uint32_t surface_width, surface_height;
    bool surface_lease;
    uint32_t animation_progress;
    uint32_t animation_kind;
    int32_t animation_dx, animation_dy;
    bool resize_pending;
    bool maximized;
} compositor_window_view_t;

void window_surface_snapshot_capture_locked(compositor_window_view_t *view);
void window_surface_snapshot_release(compositor_window_view_t *view);
void window_control_document_get(struct window_control_document *document);
void window_control_document_put(struct window_control_document *document);
void compositor_control_tree(const compositor_window_view_t *view);
void compositor_capture_window_locked(window_server_window_t *window, compositor_window_view_t *view);
void window_animation_depart_locked(window_server_window_t *window, bool minimize);
void window_animation_restore_locked(window_server_window_t *window);
bool desktop_icon_box_locked(uint32_t app, control_tree_box_t *box);
void window_animation_presented_locked(const compositor_window_view_t *view);
void window_animation_capture_locked(compositor_window_view_t *view);
void window_animation_snapshot_locked(void);
void window_animation_tick(void);
bool window_animation_ready(void);
uint64_t window_animation_wait_ns(void);
void compositor_animation_window(const compositor_window_view_t *view);
void compositor_animation_layers(void);
void window_control_surface_damage_locked(window_server_window_t *window,
    int32_t x, int32_t y, uint32_t width, uint32_t height);
/* 锁内只固定不可变文档，剪贴板分配与发布必须在解锁后完成。 */
typedef struct window_control_copy_request {
    struct window_control_document *document;
    uint32_t first,size;
} window_control_copy_request_t;
window_control_copy_request_t window_control_copy_selection_locked(const os_input_event_t *event);
void window_control_copy_complete(window_control_copy_request_t *request);
void window_control_clear_text_selection_locked(window_server_window_t *window);
bool window_control_route_locked(window_server_window_t *window, const os_input_event_t *event,
    int32_t x, int32_t y, bool pointer_inside, bool hover_prepared);
void window_control_pointer_hover_locked(window_server_window_t *window,
    const os_input_event_t *input, bool pointer_inside);
bool window_control_caption_hit_locked(window_server_window_t *window, int32_t screen_x, int32_t screen_y);
void window_control_keyboard_blur_locked(window_server_window_t *window);
bool window_control_cancel_pointer_press_locked(window_server_window_t *window);
void window_enqueue_control_event_locked(window_server_window_t *window,
    const os_input_event_t *input, uint32_t target, uint16_t action, int32_t x, int32_t y);
bool window_control_event_metadata_locked(const window_server_window_t *window,
    uint32_t target, os_control_event_t *event);

typedef struct compositor_snapshot {
    compositor_window_view_t windows[WINDOW_SERVER_MAX_WINDOWS];
    uint32_t window_count;
    uint32_t focused_identifier;
    uint32_t theme;
    uint32_t title_pressed_identifier;
    uint32_t title_pressed_button;
    uint32_t hovered_window_identifier;
    uint32_t hovered_title_button;
    uint32_t pointer_x;
    uint32_t pointer_y;
    uint32_t cursor_shape;
    uint32_t desktop_hovered_app;
    bool desktop_cache_ready;
    desktop_interaction_t desktop;
    uint32_t dragging_identifier;
    uint64_t animation_layers;
    uint64_t animation_time;
    bool animations;
    bool drag_blit_valid;
    int32_t drag_old_x;
    int32_t drag_old_y;
    int32_t drag_new_x;
    int32_t drag_new_y;
    uint32_t drag_width;
    uint32_t drag_height;
    uint32_t damage_count;
    Rect damage_rects[WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS];
    bool damage_full_captured;
    bool damage_tiles_captured;
    uint64_t damage_tiles[WINDOW_DAMAGE_TILE_WORDS];
    Rect damage_bounds;
} compositor_snapshot_t;

typedef struct compositor_render_span {
    Rect rect;
    uint32_t first_window;
    uint64_t window_mask;
} compositor_render_span_t;

typedef struct compositor_tile_meta {
    uint64_t touch_mask;
    uint64_t opaque_mask;
} compositor_tile_meta_t;

typedef struct compositor_surface_page_cache compositor_surface_page_cache_t;
typedef struct compositor_surface_source_context {
    vm_object_t *object;
    compositor_surface_page_cache_t *cache;
    uint64_t row_bytes;
    bool readable;
} compositor_surface_source_context_t;

extern compositor_snapshot_t g_compositor_snapshot;
void compositor_snapshot_test_clear_damage_tiles(void);
bool compositor_snapshot_test_set_damage_tile(uint32_t tile_index);
extern compositor_render_span_t g_compositor_render_plan[];
extern uint32_t g_compositor_render_plan_count;
extern bool g_compositor_render_plan_valid;
void compositor_render_plan_reset(void);
extern compositor_tile_meta_t g_compositor_tile_meta[];
bool compositor_tile_metadata_build(const compositor_snapshot_t *snapshot);
bool compositor_tile_metadata_valid(void);
uint32_t compositor_tile_occlusion_floor(uint32_t tile_x, uint32_t tile_y,
                                         uint32_t window_count);
const compositor_tile_meta_t *compositor_tile_metadata_at(uint32_t tile_x,
                                                           uint32_t tile_y);
bool compositor_tile_self_test(void);
uint32_t compositor_snapshot_tiles_to_rects(compositor_snapshot_t *snapshot,
                                            uint32_t capacity);
bool compositor_snapshot_damage_bounds(const compositor_snapshot_t *snapshot,
                                       Rect *bounds);
void compositor_collapse_drag_damage(compositor_snapshot_t *snapshot);
bool compositor_view_intersects_snapshot_damage(
    const compositor_window_view_t *view,
    const compositor_snapshot_t *snapshot);
uint32_t compositor_snapshot_occlusion_floor(
    const compositor_snapshot_t *snapshot);
bool compositor_view_opaque_bounds(const compositor_window_view_t *view,
    int64_t *left, int64_t *top, int64_t *right, int64_t *bottom);
bool compositor_view_fully_covers_snapshot_bounds(
    const compositor_window_view_t *window,
    const Rect *bounds);
Rect compositor_registry_damage_bounds_locked(void);
bool compositor_registry_window_intersects_damage_bounds_locked(
    const window_server_window_t *window, const Rect *bounds);
bool compositor_build_render_plan(
    const compositor_snapshot_t *snapshot);
void compositor_blit_drag_overlap(void);
void desktop_draw_wallpaper_locked(void);
void desktop_shell_init(void);
bool desktop_shell_assets_available(void);
bool desktop_shell_build_cache(void);
void desktop_shell_publish_cache_locked(void);
bool desktop_shell_cache_ready(void);
bool desktop_shell_start_asset_worker(uint32_t compositor_cpu);
bool desktop_alpha_self_test(void);

void compositor_fill_locked(int32_t x, int32_t y,
                            uint32_t width, uint32_t height,
                            uint32_t color);
void compositor_fill_rounded_locked(int32_t x, int32_t y,
                                    uint32_t width, uint32_t height,
                                    uint32_t radius, uint32_t color);
uint32_t compositor_topmost_damage_cover(void);
void compositor_fill_span_wb(volatile uint32_t *destination,
                             uint32_t pixels, uint32_t color);
void compositor_cursor_locked(void);
bool compositor_window_intersects_damage_locked(
    const compositor_window_view_t *window);
void compositor_draw_window_locked(
    const compositor_window_view_t *window);
void compositor_standard_caption_locked(const compositor_window_view_t *window);
void compositor_surface_locked(
    const compositor_window_view_t *window);
uint32_t compositor_corner_inset(uint32_t row,
                                 uint32_t width,
                                 uint32_t height);
void compositor_surface_source_prepare(
    const compositor_window_view_t *window,
    compositor_surface_source_context_t *context);
compositor_surface_page_cache_t *compositor_surface_cache_get(
    const compositor_window_view_t *view);
void compositor_copy_wb_pixels(
    uint32_t *destination,
    const uint32_t *source,
    uint32_t pixels);
bool compositor_copy_self_test(void);
uint8_t *compositor_surface_page_resolve(
    const compositor_surface_source_context_t *context,
    uint64_t page_index);
void compositor_surface_fill_destination(
    uint32_t *wb_destination,
    volatile uint32_t *device_destination,
    uint32_t pixels,
    uint32_t color);
void compositor_fill_wb_pixels(
    uint32_t *destination,
    uint32_t pixels,
    uint32_t color);
void compositor_fill_surface_pixels(
    volatile uint32_t *destination,
    uint32_t pixels,
    uint32_t color);
void compositor_surface_copy_destination(
    uint32_t *wb_destination,
    volatile uint32_t *device_destination,
    const uint32_t *source,
    uint32_t pixels);
void compositor_copy_surface_span(
    const compositor_window_view_t *window,
    const compositor_surface_source_context_t *surface,
    uint64_t source_row_offset,
    uint32_t first_column,
    uint32_t last_column,
    uint32_t *wb_destination,
    volatile uint32_t *device_destination,
    uint64_t *cached_page_index,
    uint8_t **cached_base);
bool compositor_damage_inside_surface_interior(
    const compositor_window_view_t *window);
void compositor_titlebar_locked(
    const compositor_window_view_t *window,
    uint32_t frame_color);
uint32_t compositor_occlusion_floor_for_damage(void);
void compositor_region_locked(
    const compositor_render_span_t *planned_span);
void compositor_commit_snapshot(void);
bool compositor_native_retry_ready(void);
uint64_t compositor_native_retry_wait_ns(void);
bool compositor_snapshot_overwrites_presented_cursor(void);

/* 事件路由器与命中测试单元共用的输入/调整大小策略。 */
#define WINDOW_RESIZE_GRAB 8U
#define WINDOW_MIN_WIDTH 160U
#define WINDOW_MIN_HEIGHT 96U
#define WINDOW_TITLE_DRAG_DISTANCE 6U

/* Ring0 装饰区和拖动策略共用的常量。 */
#define WINDOW_COMPOSITOR_RETAINED_DRAG 0U
#define DESKTOP_TOPBAR_HEIGHT 0U

enum {
    DESKTOP_APP_NONE = 0U,
    DESKTOP_APP_FILES = 1U,
    DESKTOP_APP_TERMINAL = 2U,
    DESKTOP_APP_NOTES = 3U,
    DESKTOP_APP_SETTINGS = 4U,
    DESKTOP_APP_TASKMGR = 5U,
    DESKTOP_APP_CHROME = 6U,
    DESKTOP_APP_IMAGEVIEW = 7U,
    DESKTOP_APP_WALLPAPER = 8U,
};

enum {
    WINDOW_RESIZE_LEFT = 1U << 0,
    WINDOW_RESIZE_RIGHT = 1U << 1,
    WINDOW_RESIZE_TOP = 1U << 2,
    WINDOW_RESIZE_BOTTOM = 1U << 3,
};

extern window_server_state_t g_window_server;
extern atomic_bool g_window_dirty_hint;
extern atomic_uint_fast64_t g_window_dirty_generation;
extern atomic_uint_fast64_t g_window_dirty_notified_generation;

window_server_window_t *window_scene_find_locked(uint32_t identifier);
window_server_window_t *window_scene_hit_test_locked(uint32_t x, uint32_t y);
window_server_window_t *window_scene_keyboard_locked(void);
bool window_scene_pointer_captured_locked(void);
window_server_window_t *window_scene_pointer_window_locked(void);
void window_scene_set_focus_identifier_locked(uint32_t identifier);
void window_scene_focus_locked(window_server_window_t *window);
uint32_t window_input_resize_edges_locked(const window_server_window_t *window,
                                          uint32_t x, uint32_t y);
const char *window_shell_program_path(uint32_t app);
const char *window_shell_app_title(uint32_t app);
bool window_shell_title_matches(const char *title, const char *expected);
uint32_t desktop_app_at_locked(uint32_t x, uint32_t y);
bool desktop_base_icon_box_locked(uint32_t app, control_tree_box_t *box);
bool desktop_route_button_locked(const input_event_t *event, bool over_window);
bool desktop_route_motion_locked(void);
bool desktop_route_key_locked(const input_event_t *event);
void desktop_draw_menu(void);
void desktop_prepare_text(void);
void desktop_capture_state_locked(void);
uint32_t desktop_window_app_locked(const window_server_window_t *window);
void compositor_control_nodes(const os_control_node_t *nodes, uint32_t count,
    const control_tree_box_t *boxes, const char *text, uint32_t hover, uint32_t pressed,
    const struct window_control_document *prepared);
struct window_control_document *window_control_prepare_overlay(struct window_control_document *previous,
    const os_control_node_t *nodes,uint32_t count,const char *text,uint32_t text_size,
    const control_tree_box_t *boxes);
void desktop_mark_icon_box_locked(const control_tree_box_t *box);
void desktop_mark_app_locked(uint32_t app);
void desktop_set_hovered_app_locked(uint32_t app);

uint32_t window_damage_tile_columns_locked(void);
uint32_t window_damage_tile_rows_locked(void);
bool window_damage_tile_is_set_locked(uint32_t tile_x, uint32_t tile_y);
void window_damage_reset_locked(void);
void window_damage_clear_pending_locked(void);
void window_damage_rotate_locked(void);
void window_mark_dirty_locked(void);
void window_mark_rect_locked(int32_t x, int32_t y,
                             uint32_t width, uint32_t height);
void window_mark_window_locked(const window_server_window_t *window);
void window_mark_surface_locked(const window_server_window_t *window,
                                int32_t x, int32_t y,
                                uint32_t width, uint32_t height);
void window_mark_moved_rect_locked(int32_t old_x, int32_t old_y,
                                   int32_t new_x, int32_t new_y,
                                   uint32_t width, uint32_t height,
                                   bool render_new_position);
void window_mark_drag_corner_repair_locked(int32_t x, int32_t y,
                                           uint32_t outer_width,
                                           uint32_t outer_height,
                                           uint32_t flags);
void window_mark_moved_cursor_locked(uint32_t old_x, uint32_t old_y,
                                     uint32_t new_x, uint32_t new_y);
void window_coalesce_damage_locked(void);

/* 事件路由边界。这些调用每个输入事务或帧边界执行一次，不会用于合成器逐像素循环。 */
void window_lock(void);
void window_unlock(void);
void window_display_reset_locked(void);
void window_buffer_reset_locked(void);
void window_buffer_set_target_locked(volatile uint32_t *target);
void window_clear_title_capture_locked(void);
bool window_title_capture_moved_locked(void);
void window_input_reset_desktop_state_locked(void);
uint32_t window_input_take_desktop_launch_locked(void);
bool window_input_take_focus_cycle_request_locked(void);
void window_input_set_pointer_locked(uint32_t x, uint32_t y);
uint32_t window_title_button_at_locked(
    const window_server_window_t *window,
    uint32_t pointer_x,
    uint32_t pointer_y);
bool window_flush_event_wakes(void);
void window_event_reset_ready_locked(void);
void window_enqueue_event_locked(window_server_window_t *window,
                                 const input_event_t *event);
void window_enqueue_client_input_locked(window_server_window_t *window,
                                       const os_input_event_t *event);
void window_enqueue_close_request_locked(window_server_window_t *window);
void window_enqueue_resize_event_locked(window_server_window_t *window);
void window_enqueue_moved_event_locked(window_server_window_t *window);
void route_input_locked(const input_event_t *event);
void window_input_refresh_pointer_locked(void);
void window_route_motion_axis_locked(const window_motion_batch_t *batch,
                                     uint32_t code, int64_t delta);
uint32_t window_resize_edges_locked(const window_server_window_t *window,
                                    uint32_t x, uint32_t y);
bool window_resize_locked(window_server_window_t *window,
                          int32_t delta_x, int32_t delta_y);
void window_input_reset_drag_locked(void);
void window_input_clear_drag_locked(void);
void window_input_begin_drag_locked(uint32_t identifier,
                                    int32_t offset_x,
                                    int32_t offset_y,
                                    uint32_t resize_edges);
void window_input_set_drag_blit_valid_locked(bool valid);
void window_input_record_drag_frame_locked(
    bool blit_valid,
    int32_t old_x, int32_t old_y,
    int32_t new_x, int32_t new_y,
    uint32_t width, uint32_t height);
bool window_drag_reuse_safe_locked(const window_server_window_t *window,
                                   int32_t old_x, int32_t old_y,
                                   int32_t new_x, int32_t new_y,
                                   uint32_t width, uint32_t height);
void window_drag_move_locked(window_server_window_t *window,
                             int32_t new_x, int32_t new_y);
bool window_route_drag_motion_batch_locked(
    const window_motion_batch_t *batch);
void window_flush_motion_batch_locked(window_motion_batch_t *batch);
void window_route_pointer_transaction_locked(const input_event_t *event);
uint64_t window_surface_page_span(uint32_t width, uint32_t height);
void desktop_cycle_window_focus(void);
bool desktop_restore_minimized_app(uint32_t app);
kstatus_t desktop_launch_program(uint32_t app);
bool desktop_chrome_launch_guard_self_test(void);
void desktop_chrome_window_opened_locked(process_t *owner);
void desktop_chrome_window_closed_locked(process_t *owner);
bool desktop_chrome_process_released_locked(uint64_t pid);
bool desktop_shell_start_launch_worker(uint32_t compositor_cpu);
bool desktop_launch_enqueue(uint32_t app);
void window_server_pump_input_mode(bool compose_now);
void window_server_pump_publication_motion(void);
void compositor_frame_run(void);
uint64_t compositor_publication_damage_pixels(void);
void compositor_publication_rect(const Rect *rect, bool allow_yield);
void compositor_publication_rects(const Rect *rects, uint32_t count, bool allow_yield);
void compositor_publication_drag_old_exposure(void);
bool compositor_snapshot_begin_locked(void);
void compositor_snapshot_plan(void);
void compositor_render_snapshot(void);
void compositor_snapshot_finish(void);
bool window_present_cursor_overlay(bool force,
                                   int64_t *out_left,
                                   int64_t *out_top,
                                   int64_t *out_right,
                                   int64_t *out_bottom);
void window_present_cursor_prepare_rect(const Rect *rect);
bool window_present_cursor_publish_span(
    volatile uint32_t *destination,
    const uint32_t *source,
    uint32_t x,
    uint32_t y,
    uint32_t pixels);
bool window_present_cursor_into_buffer(volatile uint32_t *destination,
                                       uint32_t pointer_x,
                                       uint32_t pointer_y,
                                       uint32_t shape);
uint32_t window_input_cursor_shape_locked(void);
void window_present_cursor_reset_locked(void);
void compositor_present_cursor_direct(bool force);
void compositor_reset_state_locked(void);
void compositor_copy_wc_scanline(
    volatile uint32_t *destination,
    const uint32_t *source,
    uint32_t pixels);

/* 窗口生命周期边界。注册表和对象生命周期由 window.c 管理；在相关策略迁入对应图形单元前，
 * 这些低频事件队列/移除辅助函数仍由此处共享。 */
extern const object_ops_t g_window_object_ops;
void window_registry_reset_locked(void);
bool window_registry_append_locked(window_server_window_t *window);
bool window_registry_remove_locked(window_server_window_t *window);
void window_registry_move_to_front_locked(window_server_window_t *window);
void remove_window_locked(window_server_window_t *window);
void window_event_schedule_wake_locked(window_server_window_t *window);
void window_server_notify_worker(void);
bool window_display_prepare(void);
void window_buffer_prepare(void);

/* 应用声明按钮几何，服务端执行窗口生命周期操作。 */
bool window_control_has_caption(const struct window_control_document *document);
void window_control_caption_action_locked(window_server_window_t *window, uint32_t action);
void compositor_control_caption_icon(int64_t x,int64_t y,uint32_t width,uint32_t height,
    uint32_t action,bool maximized,bool active);

uint32_t *desktop_wallpaper_build_cache(const desktop_asset_image_t *asset);
kstatus_t desktop_wallpaper_stage(uint32_t *cache);
void desktop_wallpaper_apply_pending(void);

bool desktop_wallpaper_pending(void);
