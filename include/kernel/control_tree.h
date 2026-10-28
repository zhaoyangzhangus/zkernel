#pragma once

#include <kernel/rect.h>
#include <uapi/control_tree.h>
#include <uapi/window.h>

/* 布局、命中和损坏投影共用原点，不能让标题栏控件挤占内容 Surface。 */
static inline Rect control_tree_window_viewport(const os_control_node_t *nodes,
    uint32_t count, uint32_t width, uint32_t height, uint32_t flags) {
    int32_t top = count && (nodes[0].flags & OS_CONTROL_WINDOW_FRAME) ?
        -(int32_t)os_window_caption_height(flags) : 0;
    return (Rect){0, top, (int32_t)width, (int32_t)height};
}

/* 计算样式独立保存，不能把继承结果写回应用提交的只读节点。 */
typedef struct control_tree_style {
    uint32_t background, foreground, border_color, border_width, radius, font_size;
    uint32_t background_alpha;
    uint32_t padding;
} control_tree_style_t;

typedef struct control_tree_box {
    Rect bounds;
    Rect clip;
    bool visible;
    bool enabled;
    bool inert;
    uint16_t depth;
    uint16_t subtree_end;
    control_tree_style_t style;
} control_tree_box_t;

/* 只读节点与独立布局结果分离；调用方在完整成功后发布布局，不能暴露失败的半份结果。 */
bool control_tree_validate(const os_control_node_t *nodes, uint32_t count, uint32_t text_size);
bool control_tree_layout(const os_control_node_t *nodes, uint32_t count,
    uint32_t text_size, Rect viewport, control_tree_box_t *boxes);
/* 仅接受已经完整校验且保持只读的内核快照；尺寸变化不重新校验节点标识。 */
bool control_tree_layout_validated(const os_control_node_t *nodes,uint32_t count,
    Rect viewport,control_tree_box_t *boxes);
/* 在同一次布局中收集多个查询结果，不分配整树缓存，也不读取合成线程的缓存。
 * 节点必须为已校验的只读快照；失败可能已回调部分节点，调用方须丢弃查询结果。 */
typedef void (*control_tree_visit_fn)(void *context, uint32_t index,
    const os_control_node_t *node, const control_tree_box_t *box);
bool control_tree_visit_validated(const os_control_node_t *nodes, uint32_t count,
    Rect viewport, control_tree_visit_fn visit, void *context);
uint32_t control_tree_hit_test(const os_control_node_t *nodes, uint32_t count,
    const control_tree_box_t *boxes, int32_t x, int32_t y);
/* 输入侧只遍历已验证的只读树，以深度有界栈计算命中，不分配整树布局或读取绘制缓存。 */
uint32_t control_tree_locate(const os_control_node_t *nodes, uint32_t count,
    Rect viewport, int32_t x, int32_t y, control_tree_box_t *box);

/* 点击目标与附加查询共用一次布局；失败不发布目标，回调结果也须丢弃。 */
bool control_tree_locate_query(const os_control_node_t *nodes,uint32_t count,
    Rect viewport,int32_t x,int32_t y,uint32_t *index,control_tree_box_t *box,
    control_tree_visit_fn visit,void *context);

/* 在当前布局中查询稳定标识和循环焦点；跳过隐藏、禁用及完全裁剪的交互控件。 */
uint32_t control_tree_find(const os_control_node_t *nodes, uint32_t count,
    Rect viewport, uint32_t identifier, control_tree_box_t *box);
/* 焦点导航与附加几何查询共用一次布局。失败不修改 next/box，
 * 回调可能已收到部分节点，调用方须丢弃这些结果。 */
bool control_tree_focus_next_query(const os_control_node_t *nodes, uint32_t count,
    Rect viewport, uint32_t current, bool reverse, uint32_t *next,
    control_tree_box_t *box, control_tree_visit_fn visit, void *context);

typedef void (*control_tree_damage_fn)(void *context, Rect damage);
/* 对已验证的树投影原始 Surface 的像素损坏；不缩放、不分配布局缓存。
 * 返回失败时可能已发出部分保守区域，调用方必须追加完整恢复区域。 */
bool control_tree_surface_damage(const os_control_node_t *nodes, uint32_t count,
    Rect viewport, uint32_t surface, uint32_t width, uint32_t height, Rect damage,
    control_tree_damage_fn emit, void *context);

typedef void (*control_tree_paint_fn)(void *context, const os_control_node_t *node,
    const control_tree_box_t *box, Rect damage);
void control_tree_paint(const os_control_node_t *nodes, uint32_t count,
    const control_tree_box_t *boxes, Rect damage, control_tree_paint_fn paint, void *context);
/* 直接遍历已验证且在当前视口可布局的只读树；不分配整树布局缓存。
 * 调用方必须先验证布局成功，失败时可能已执行部分绘制回调。 */
bool control_tree_paint_view(const os_control_node_t *nodes, uint32_t count,
    Rect viewport, Rect damage, control_tree_paint_fn paint, void *context);

/* 返回从根到命中节点的索引路径，供捕获、目标和冒泡阶段共享同一条路由。 */
uint32_t control_tree_event_path(const os_control_node_t *nodes, uint32_t count,
    uint32_t target, uint32_t *path, uint32_t capacity);
