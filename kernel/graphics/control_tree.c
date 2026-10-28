#include <kernel/control_tree.h>
#include <uapi/window_theme.h>
#include <uapi/control_validation.h>

static control_tree_style_t control_style(const os_control_node_t *node,
    const control_tree_style_t *parent) {
    const control_tree_style_t initial = {.background=OS_THEME_SURFACE,
        .foreground=OS_THEME_TEXT, .border_color=OS_THEME_BORDER};
    if (!parent) parent = &initial;
    control_tree_style_t style = {
        .padding = node->padding,
        .background = node->flags & OS_CONTROL_INHERIT_BACKGROUND ? parent->background : node->background,
        .foreground = node->flags & OS_CONTROL_INHERIT_FOREGROUND ? parent->foreground : node->foreground,
        .font_size = node->flags & OS_CONTROL_INHERIT_FONT ? parent->font_size : node->font_size,
        .border_color = node->flags & OS_CONTROL_INHERIT_BORDER_COLOR ? parent->border_color : node->border_color,
        .border_width = node->flags & OS_CONTROL_INHERIT_BORDER_WIDTH ? parent->border_width : node->border_width,
        .radius = node->flags & OS_CONTROL_INHERIT_RADIUS ? parent->radius : node->radius,
        .background_alpha = node->flags & OS_CONTROL_INHERIT_BACKGROUND ? parent->background_alpha :
            (node->flags & OS_CONTROL_BACKGROUND_ALPHA) != 0U,
    };
    if (node->flags & (OS_CONTROL_MENU_PANEL|OS_CONTROL_DIALOG_PANEL)) {
        style.background=OS_THEME_SURFACE;
        style.foreground=OS_THEME_TEXT;
        style.border_color=OS_THEME_BORDER;
        style.border_width=1U;
        style.radius=((style.font_size ? style.font_size : 16U)*OS_THEME_RADIUS+8U)/16U;
        style.background_alpha=false;
    }
    return style;
}

static Rect control_intersection(Rect a, Rect b) {
    Rect result = {a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0,
        a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1};
    if (rect_is_empty(result)) return (Rect){0};
    return result;
}

bool control_tree_validate(const os_control_node_t *nodes, uint32_t count, uint32_t text_size) {
    return os_control_tree_validate(nodes,count,text_size);
}

typedef struct control_parent_layout {
    uint32_t flags,layout,padding,gap;
    int32_t scroll_x,scroll_y;
} control_parent_layout_t;

typedef struct control_layout_state {
    uint32_t index;
    int64_t cursor;
    bool has_child;
    uint32_t grown;
    int64_t free_space, extra_gap, justify_offset;
    uint32_t gap_remainder;
    uint32_t dialog_text_index;
    bool automatic_dialog;
    control_tree_box_t box;
    control_parent_layout_t resolved;
} control_layout_state_t;

/* 输入即时布局与绘制缓存经过同一解析，不修改应用快照或改变事件索引。 */
static const os_control_node_t *control_popup_node(const os_control_node_t *nodes,uint32_t count,
    uint32_t index,control_layout_state_t *parent,os_control_node_t *node) {
    const os_control_node_t *source=nodes+index;
    bool menu_child=parent && (parent->resolved.flags&OS_CONTROL_MENU_PANEL);
    bool dialog_child=parent && parent->automatic_dialog;
    bool action_child=parent && (parent->resolved.flags&OS_CONTROL_DIALOG_ACTIONS);
    /* 普通控件直接使用不可变节点，不能为弹层功能逐项复制整棵树。 */
    if (!(source->flags&(OS_CONTROL_MENU_PANEL|OS_CONTROL_DIALOG_PANEL)) &&
        !menu_child && !dialog_child && !action_child) return source;
    *node=*source;
    uint32_t font=(node->flags&OS_CONTROL_INHERIT_FONT) && parent ? parent->box.style.font_size : node->font_size;
    font=os_control_popup_font(font);
    uint32_t inset=(font+2U)/4U;
    if (node->flags&OS_CONTROL_MENU_PANEL) {
        node->layout=OS_CONTROL_COLUMN;
        node->padding=inset;node->gap=0;
        node->scroll_x=node->scroll_y=0;
        node->flags|=OS_CONTROL_CLIP_CHILDREN;
        node->flags&=~(OS_CONTROL_JUSTIFY_CENTER|OS_CONTROL_JUSTIFY_END|
            OS_CONTROL_JUSTIFY_BETWEEN|OS_CONTROL_ALIGN_CENTER|OS_CONTROL_ALIGN_END|OS_CONTROL_ALIGN_STRETCH);
        uint32_t rows=0;
        for (uint32_t j=index+1U;j<count;++j) {
            if (nodes[j].parent<index) break;
            if (nodes[j].parent==index && !(nodes[j].flags&OS_CONTROL_HIDDEN)) ++rows;
        }
        node->height=(int32_t)os_control_menu_height(font,rows);
    } else if ((node->flags&OS_CONTROL_DIALOG_PANEL) && !node->width && !node->height) {
        uint32_t labels=0;bool actions=false;
        for (uint32_t j=index+1U;j<count;++j) {
            if (nodes[j].parent<index) break;
            if (nodes[j].parent!=index || (nodes[j].flags&OS_CONTROL_HIDDEN)) continue;
            if (nodes[j].kind==OS_CONTROL_TEXT) ++labels;
            if (nodes[j].flags&OS_CONTROL_DIALOG_ACTIONS) actions=true;
        }
        uint32_t gap=font/2U;
        uint32_t text_height=labels ? font*2U+(labels-1U)*(font+8U+gap) : 0U;
        uint32_t preferred=text_height+font*2U+(actions ? gap+font*2U : 0U);
        if (preferred<font*9U) preferred=font*9U;
        node->width=(int32_t)os_control_dialog_width(font);
        node->height=(int32_t)preferred;
        node->layout=OS_CONTROL_ABSOLUTE;node->padding=node->gap=0U;
        node->scroll_x=node->scroll_y=0;
        node->flags|=OS_CONTROL_CLIP_CHILDREN;
        node->flags&=~(OS_CONTROL_JUSTIFY_CENTER|OS_CONTROL_JUSTIFY_END|
            OS_CONTROL_JUSTIFY_BETWEEN|OS_CONTROL_ALIGN_CENTER|OS_CONTROL_ALIGN_END|OS_CONTROL_ALIGN_STRETCH);
    } else if (dialog_child) {
        font=os_control_popup_font(parent->box.style.font_size);
        uint32_t w=(uint32_t)(parent->box.bounds.x1-parent->box.bounds.x0);
        uint32_t h=(uint32_t)(parent->box.bounds.y1-parent->box.bounds.y0);
        uint32_t pad=font;
        if (pad>w/2U) pad=w/2U;
        if (pad>h/2U) pad=h/2U;
        uint32_t button_height=font*2U;
        if (button_height>h-2U*pad) button_height=h-2U*pad;
        uint32_t actions_y=h-pad-button_height;
        node->flags|=OS_CONTROL_INHERIT_FONT;
        node->padding=0U;
        if (node->flags&OS_CONTROL_DIALOG_ACTIONS) {
            node->x=(int32_t)pad;node->y=(int32_t)actions_y;
            node->width=(int32_t)(w-2U*pad);node->height=(int32_t)button_height;
            node->layout=OS_CONTROL_ROW;node->gap=font/2U;
            node->background=0;node->flags|=OS_CONTROL_BACKGROUND_ALPHA|OS_CONTROL_CLIP_CHILDREN;
        } else if (node->flags&OS_CONTROL_DIALOG_CLOSE) {
            node->width=node->height=(int32_t)(font*2U);
            node->x=(int32_t)w-(int32_t)(font*5U/2U);node->y=(int32_t)(font/2U);
            node->background=0;node->flags|=OS_CONTROL_BACKGROUND_ALPHA;
            node->radius=(font+2U)/4U;
            /* 矮面板优先保留操作按钮，关闭叉号不能覆盖确认或取消的命中区域。 */
            if (w<font*4U || h<font*4U || font/2U+font*2U>actions_y)
                node->flags|=OS_CONTROL_HIDDEN;
        } else if (node->kind==OS_CONTROL_TEXT) {
            uint32_t ordinal=parent->dialog_text_index;
            if (!(node->flags&OS_CONTROL_HIDDEN)) ++parent->dialog_text_index;
            uint32_t y=pad+(ordinal ? font*2U+font/2U+(ordinal-1U)*(font+8U+font/2U) : 0U);
            uint32_t line=ordinal ? font+8U : font*2U;
            if (y>=actions_y) line=0U;
            else if (line>actions_y-y) line=actions_y-y;
            node->x=(int32_t)pad;node->y=(int32_t)y;
            node->width=(int32_t)(w-2U*pad);node->height=(int32_t)line;
            if (!ordinal) node->width=node->width>(int32_t)(font*2U) ? node->width-(int32_t)(font*2U) : 0;
            node->background=0;node->flags|=OS_CONTROL_BACKGROUND_ALPHA;
        }
    } else if (action_child && node->kind==OS_CONTROL_BUTTON) {
        font=os_control_popup_font(parent->box.style.font_size);
        node->x=node->y=0;node->width=0;node->height=OS_CONTROL_FILL;
        node->flags|=OS_CONTROL_FLEX_GROW|OS_CONTROL_INHERIT_FONT;
        node->radius=(font+1U)/3U;node->border_width=1U;node->border_color=OS_THEME_BORDER;
    } else if (menu_child) {
        font=parent->box.style.font_size ? parent->box.style.font_size : 16U;
        node->x=node->y=0;node->width=OS_CONTROL_FILL;node->height=(int32_t)(font*2U);
        node->padding=(font*3U+2U)/4U;
        node->background=OS_THEME_SURFACE;node->border_width=0U;
        node->radius=(font+2U)/4U;
        node->flags&=~(OS_CONTROL_FLEX_GROW|OS_CONTROL_INHERIT_BACKGROUND|
            OS_CONTROL_INHERIT_BORDER_WIDTH|OS_CONTROL_INHERIT_RADIUS|OS_CONTROL_BACKGROUND_ALPHA);
        node->flags|=OS_CONTROL_INHERIT_FONT;
    }
    return node;
}

enum {CONTROL_SUBTREE_FLEX=0x8000U,CONTROL_SUBTREE_END=0x7FFFU};

static bool control_layout_index(const os_control_node_t *nodes,uint32_t count,uint16_t *subtrees) {
    _Static_assert(OS_CONTROL_TREE_MAX_NODES<CONTROL_SUBTREE_FLEX,"subtree index width");
    uint16_t ancestors[OS_CONTROL_TREE_MAX_DEPTH];
    uint32_t depth=0;
    for (uint32_t i=0;i<count;++i) subtrees[i]=0;
    for (uint32_t i=0;i<count;++i) {
        if (i) {
            while (depth && ancestors[depth-1U]!=nodes[i].parent) {
                uint32_t ended=ancestors[--depth];
                subtrees[ended]=(uint16_t)(i|(subtrees[ended]&CONTROL_SUBTREE_FLEX));
            }
            if (!depth) return false;
            if ((nodes[i].flags&OS_CONTROL_FLEX_GROW) && !(nodes[i].flags&OS_CONTROL_HIDDEN))
                subtrees[nodes[i].parent]|=CONTROL_SUBTREE_FLEX;
        }
        if (depth==OS_CONTROL_TREE_MAX_DEPTH) return false;
        ancestors[depth++]=(uint16_t)i;
    }
    while (depth) {
        uint32_t ended=ancestors[--depth];
        subtrees[ended]=(uint16_t)(count|(subtrees[ended]&CONTROL_SUBTREE_FLEX));
    }
    return true;
}

static bool control_layout_walk(const os_control_node_t *nodes, uint32_t count,
    Rect viewport, control_tree_box_t *boxes, int32_t px, int32_t py,
    uint32_t *hit, control_tree_box_t *hit_box, control_tree_visit_fn visit, void *context) {
    _Static_assert(OS_CONTROL_TREE_MAX_NODES <= UINT16_MAX &&
                   OS_CONTROL_TREE_MAX_DEPTH <= UINT16_MAX, "layout index width");
    control_layout_state_t stack[OS_CONTROL_TREE_MAX_DEPTH];
    uint16_t subtrees[OS_CONTROL_TREE_MAX_NODES];
    uint32_t depth = 0U;
    if (!nodes || !count || count > OS_CONTROL_TREE_MAX_NODES || rect_is_empty(viewport))
        return false;
    /* 普通行列布局无需统计剩余空间；输入侧不能为未使用的 Flex 功能反复扫描子树。 */
    bool distribution = false;
    for (uint32_t i = 0U; i < count; ++i) {
        if (nodes[i].flags & (OS_CONTROL_FLEX_GROW|OS_CONTROL_JUSTIFY_CENTER|
            OS_CONTROL_JUSTIFY_END|OS_CONTROL_JUSTIFY_BETWEEN)) { distribution=true; break; }
    }
    if (distribution && !control_layout_index(nodes,count,subtrees)) return false;
    for (uint32_t i = 0U; i < count; ++i) {
        if (i) while (depth && stack[depth-1U].index!=nodes[i].parent) {
            if (boxes) boxes[stack[depth-1U].index].subtree_end=(uint16_t)i;
            --depth;
        }
        os_control_node_t resolved;
        const os_control_node_t *node = control_popup_node(nodes,count,i,
            depth ? stack+depth-1U : 0,&resolved);
        control_tree_box_t box = {0};
        if (i == 0U) {
            /* 根尺寸由窗口服务器决定，应用不能突破窗口视口。 */
            box.bounds = box.clip = viewport;
            box.visible = !(node->flags & OS_CONTROL_HIDDEN);
            box.enabled = !(node->flags & OS_CONTROL_DISABLED);
            box.inert = node->kind==OS_CONTROL_CONTAINER && (node->flags&OS_CONTROL_INERT)==OS_CONTROL_INERT;
            box.style = control_style(node, 0);
        } else {
            if (!depth) return false;
            control_layout_state_t *state = stack + depth - 1U;
            const control_parent_layout_t *parent = &state->resolved;
            const control_tree_box_t *parent_box = &state->box;
            int64_t left = parent_box->bounds.x0, top = parent_box->bounds.y0;
            int64_t width = (int64_t)parent_box->bounds.x1 - left;
            int64_t height = (int64_t)parent_box->bounds.y1 - top;
            int64_t pad_x = parent->padding, pad_y = parent->padding;
            if (pad_x > width / 2) pad_x = width / 2;
            if (pad_y > height / 2) pad_y = height / 2;
            width -= pad_x * 2; height -= pad_y * 2;
            bool participates = !(node->flags & OS_CONTROL_HIDDEN);
            int64_t advance = state->cursor;
            if (state->has_child && participates) {
                advance += parent->gap + state->extra_gap;
                if (state->gap_remainder) { ++advance; --state->gap_remainder; }
            }
            if (!state->has_child && participates) advance += state->justify_offset;
            int64_t x = node->x, y = node->y;
            if (parent->layout == OS_CONTROL_ROW) x += advance;
            if (parent->layout == OS_CONTROL_COLUMN) y += advance;
            int64_t w = node->width == OS_CONTROL_FILL ? width - x : node->width;
            int64_t h = node->height == OS_CONTROL_FILL ? height - y : node->height;
            if (node->flags & OS_CONTROL_FLEX_GROW) {
                if (parent->layout == OS_CONTROL_ROW && node->width == OS_CONTROL_FILL) w = 0;
                if (parent->layout == OS_CONTROL_COLUMN && node->height == OS_CONTROL_FILL) h = 0;
            }
            if (w < 0) w = 0;
            if (h < 0) h = 0;
            if (node->flags&(OS_CONTROL_MENU_PANEL|OS_CONTROL_DIALOG_PANEL)) {
                if (w>width) w=width;
                if (h>height) h=height;
                if (node->flags&OS_CONTROL_DIALOG_PANEL) {x=(width-w)/2;y=(height-h)/2;}
                else {
                    if (x>width-w) x=width-w;
                    if (y>height-h) y=height-h;
                    if (x<0) x=0;
                    if (y<0) y=0;
                }
            }
            if (participates && (node->flags & OS_CONTROL_FLEX_GROW) && state->grown) {
                /* 余数交给后续子项，所有子项分配总和严格等于可用空间。 */
                int64_t extra = state->free_space / state->grown;
                state->free_space -= extra; --state->grown;
                if (parent->layout == OS_CONTROL_ROW) w += extra;
                else h += extra;
            }
            if (parent->layout != OS_CONTROL_ABSOLUTE) {
                int64_t cross = parent->layout == OS_CONTROL_ROW ? height - h : width - w;
                int64_t offset = 0;
                if (parent->flags & OS_CONTROL_ALIGN_STRETCH) {
                    if (parent->layout == OS_CONTROL_ROW) h = height;
                    else w = width;
                } else if (cross > 0) {
                    if (parent->flags & OS_CONTROL_ALIGN_CENTER) offset = cross / 2;
                    else if (parent->flags & OS_CONTROL_ALIGN_END) offset = cross;
                }
                if (parent->layout == OS_CONTROL_ROW) y += offset;
                else x += offset;
            }
            if (participates && parent->layout != OS_CONTROL_ABSOLUTE) {
                state->cursor = parent->layout == OS_CONTROL_ROW ? x + w : y + h;
                state->has_child = true;
            }
            left += pad_x + x - parent->scroll_x;
            top += pad_y + y - parent->scroll_y;
            if (left < INT32_MIN || top < INT32_MIN || left > INT32_MAX || top > INT32_MAX ||
                left + w > INT32_MAX || top + h > INT32_MAX) return false;
            box.bounds = (Rect){(int32_t)left, (int32_t)top, (int32_t)(left + w), (int32_t)(top + h)};
            box.clip = parent_box->clip;
            if (parent->flags & OS_CONTROL_CLIP_CHILDREN)
                box.clip = control_intersection(box.clip, parent_box->bounds);
            box.visible = parent_box->visible && participates;
            box.enabled = parent_box->enabled && !(node->flags & OS_CONTROL_DISABLED);
            box.inert = (parent_box->inert && !(node->flags&OS_CONTROL_DISABLED)) ||
                (node->kind==OS_CONTROL_CONTAINER && (node->flags&OS_CONTROL_INERT)==OS_CONTROL_INERT);
            box.style = control_style(node, &parent_box->style);
        }
        box.depth = depth;
        if (boxes) boxes[i] = box;
        if (visit) visit(context, i, nodes+i, &box);
        if (hit && box.visible && box.enabled && !(node->flags & OS_CONTROL_POINTER_PASSTHROUGH) &&
            px >= box.bounds.x0 && px < box.bounds.x1 && py >= box.bounds.y0 && py < box.bounds.y1 &&
            px >= box.clip.x0 && px < box.clip.x1 && py >= box.clip.y0 && py < box.clip.y1) {
            *hit = i;
            if (hit_box) *hit_box = box;
        }
        if (depth == OS_CONTROL_TREE_MAX_DEPTH) return false;
        control_layout_state_t state = {.index = i, .box = box,
            .automatic_dialog=(nodes[i].flags&OS_CONTROL_DIALOG_PANEL) && !nodes[i].width && !nodes[i].height,
            .resolved={node->flags,node->layout,node->padding,node->gap,node->scroll_x,node->scroll_y}};
        if (node->layout != OS_CONTROL_ABSOLUTE && distribution && ((subtrees[i]&CONTROL_SUBTREE_FLEX) ||
            (node->flags & (OS_CONTROL_JUSTIFY_CENTER | OS_CONTROL_JUSTIFY_END | OS_CONTROL_JUSTIFY_BETWEEN)))) {
            int64_t extent = node->layout == OS_CONTROL_ROW ?
                (int64_t)box.bounds.x1 - box.bounds.x0 : (int64_t)box.bounds.y1 - box.bounds.y0;
            int64_t padding = node->padding;
            if (padding > extent / 2) padding = extent / 2;
            extent -= padding * 2;
            int64_t used = 0;
            uint32_t children = 0U;
            /* 边界索引直接跳过后代；每个直接子项最多统计一次，隐藏项不占空间。 */
            for (uint32_t j=i+1U;j<count && nodes[j].parent==i;j=subtrees[j]&CONTROL_SUBTREE_END) {
                const os_control_node_t *child = nodes + j;
                if (child->flags & OS_CONTROL_HIDDEN) continue;
                int64_t offset = node->layout == OS_CONTROL_ROW ? child->x : child->y;
                int64_t size = node->layout == OS_CONTROL_ROW ? child->width : child->height;
                if (children++) used += node->gap;
                if (size == OS_CONTROL_FILL) size = child->flags & OS_CONTROL_FLEX_GROW ? 0 : extent - used - offset;
                if (size < 0) size = 0;
                used += offset + size;
                if (child->flags & OS_CONTROL_FLEX_GROW) ++state.grown;
            }
            state.free_space = extent > used ? extent - used : 0;
            if (!state.grown) {
                if (node->flags & OS_CONTROL_JUSTIFY_CENTER) state.justify_offset = state.free_space / 2;
                else if (node->flags & OS_CONTROL_JUSTIFY_END) state.justify_offset = state.free_space;
                else if ((node->flags & OS_CONTROL_JUSTIFY_BETWEEN) && children > 1U) {
                    state.extra_gap = state.free_space / (children - 1U);
                    state.gap_remainder = (uint32_t)(state.free_space % (children - 1U));
                }
            }
        }
        stack[depth++] = state;
    }
    /* 复用布局遍历的退栈顺序记录子树范围，不额外扫描，也不扩大布局缓存。 */
    while (depth) {
        if (boxes) boxes[stack[depth - 1U].index].subtree_end = (uint16_t)count;
        --depth;
    }
    return true;
}

bool control_tree_layout(const os_control_node_t *nodes, uint32_t count,
    uint32_t text_size, Rect viewport, control_tree_box_t *boxes) {
    return boxes && control_tree_validate(nodes, count, text_size) &&
        control_tree_layout_validated(nodes,count,viewport,boxes);
}

bool control_tree_layout_validated(const os_control_node_t *nodes,uint32_t count,
    Rect viewport,control_tree_box_t *boxes) {
    return boxes && control_layout_walk(nodes,count,viewport,boxes,0,0,0,0,0,0);
}

bool control_tree_visit_validated(const os_control_node_t *nodes, uint32_t count,
    Rect viewport, control_tree_visit_fn visit, void *context) {
    return visit && control_layout_walk(nodes,count,viewport,0,0,0,0,0,visit,context);
}

bool control_tree_locate_query(const os_control_node_t *nodes,uint32_t count,
    Rect viewport,int32_t x,int32_t y,uint32_t *index,control_tree_box_t *box,
    control_tree_visit_fn visit,void *context) {
    uint32_t hit=OS_CONTROL_NO_PARENT;
    control_tree_box_t result={0};
    if (!index || !control_layout_walk(nodes,count,viewport,0,x,y,&hit,&result,visit,context))
        return false;
    *index=hit;
    if (box && hit!=OS_CONTROL_NO_PARENT) *box=result;
    return true;
}

uint32_t control_tree_locate(const os_control_node_t *nodes, uint32_t count,
    Rect viewport, int32_t x, int32_t y, control_tree_box_t *box) {
    uint32_t hit=OS_CONTROL_NO_PARENT;
    (void)control_tree_locate_query(nodes,count,viewport,x,y,&hit,box,0,0);
    return hit;
}

typedef struct control_focus_query {
    uint32_t current, first, selected;
    bool reverse, seen, complete, copy_box;
    control_tree_box_t first_box, selected_box;
    control_tree_visit_fn visit;
    void *context;
} control_focus_query_t;

typedef struct control_surface_damage_query {
    uint32_t surface;
    Rect source;
    control_tree_damage_fn emit;
    void *context;
} control_surface_damage_query_t;

static void control_surface_damage_visit(void *raw, uint32_t index, const os_control_node_t *node,
    const control_tree_box_t *box) {
    (void)index;
    control_surface_damage_query_t *query = raw;
    if (node->kind != OS_CONTROL_SURFACE || node->surface != query->surface || !box->visible) return;
    Rect clip = control_intersection(box->bounds, box->clip);
    /* 先以宽整数平移，再裁剪并收窄，远处滚动原点不能回绕到屏幕内。 */
    int64_t left = (int64_t)box->bounds.x0 + query->source.x0;
    int64_t top = (int64_t)box->bounds.y0 + query->source.y0;
    int64_t right = (int64_t)box->bounds.x0 + query->source.x1;
    int64_t bottom = (int64_t)box->bounds.y0 + query->source.y1;
    if (left < clip.x0) left = clip.x0;
    if (top < clip.y0) top = clip.y0;
    if (right > clip.x1) right = clip.x1;
    if (bottom > clip.y1) bottom = clip.y1;
    if (left < right && top < bottom)
        query->emit(query->context, (Rect){(int32_t)left, (int32_t)top, (int32_t)right, (int32_t)bottom});
}

bool control_tree_surface_damage(const os_control_node_t *nodes, uint32_t count,
    Rect viewport, uint32_t surface, uint32_t width, uint32_t height, Rect damage,
    control_tree_damage_fn emit, void *context) {
    if (!emit || !surface) return false;
    Rect source;
    if (!rect_clip_to_extent(damage.x0, damage.y0, damage.x1, damage.y1, width, height, &source))
        return true;
    control_surface_damage_query_t query = {.surface=surface, .source=source, .emit=emit, .context=context};
    return control_layout_walk(nodes, count, viewport, 0, 0, 0, 0, 0, control_surface_damage_visit, &query);
}

static void control_focus_visit(void *raw, uint32_t index, const os_control_node_t *node,
    const control_tree_box_t *box) {
    control_focus_query_t *query = raw;
    if (query->visit) query->visit(query->context, index, node, box);
    if (node->identifier == query->current) {
        query->seen = true;
        if (query->reverse && query->selected != OS_CONTROL_NO_PARENT) query->complete = true;
    }
    if (!os_control_is_interactive(node->kind) || (node->flags & OS_CONTROL_SKIP_TAB) ||
        !box->visible || !box->enabled ||
        rect_is_empty(control_intersection(box->bounds, box->clip))) return;
    /* 正向只保存首项和当前项之后的首个候选；反向在遇到当前项后保留前驱。
     * 当前项之前没有候选时继续维护末项，用于反向循环。布局与附加回调仍完整执行。 */
    if (query->reverse) {
        if (query->complete) return;
        query->selected = index;
        if (query->copy_box) { query->selected_box = *box; }
    } else {
        if (query->first == OS_CONTROL_NO_PARENT) {
            query->first = index;
            if (query->copy_box) { query->first_box = *box; }
        }
        if (query->seen && node->identifier != query->current && query->selected == OS_CONTROL_NO_PARENT) {
            query->selected = index;
            if (query->copy_box) { query->selected_box = *box; }
        }
    }
}

typedef struct control_find_query {
    uint32_t identifier, index;
    bool copy_box;
    control_tree_box_t result_box;
} control_find_query_t;

static void control_find_visit(void *raw, uint32_t index, const os_control_node_t *node,
    const control_tree_box_t *box) {
    control_find_query_t *query = raw;
    if (node->identifier != query->identifier) return;
    query->index = index;
    if (query->copy_box) { query->result_box = *box; }
}

uint32_t control_tree_find(const os_control_node_t *nodes, uint32_t count,
    Rect viewport, uint32_t identifier, control_tree_box_t *box) {
    /* 标识查找不维护焦点导航候选；仅命中时保留所需几何结果。
     * 仍须完成布局，避免在后续节点失败时发布半份结果。 */
    control_find_query_t query = {.identifier=identifier,
        .index=OS_CONTROL_NO_PARENT, .copy_box=box != 0};
    if (!control_layout_walk(nodes, count, viewport, 0, 0, 0, 0, 0,
        control_find_visit, &query)) return OS_CONTROL_NO_PARENT;
    if (box && query.index != OS_CONTROL_NO_PARENT) *box = query.result_box;
    return query.index;
}

bool control_tree_focus_next_query(const os_control_node_t *nodes, uint32_t count,
    Rect viewport, uint32_t current, bool reverse, uint32_t *next,
    control_tree_box_t *box, control_tree_visit_fn visit, void *context) {
    if (!next) return false;
    control_focus_query_t query = {.current=current, .reverse=reverse, .copy_box=box != 0,
        .first=OS_CONTROL_NO_PARENT, .selected=OS_CONTROL_NO_PARENT,
        .visit=visit, .context=context};
    if (!control_layout_walk(nodes, count, viewport, 0, 0, 0, 0, 0,
        control_focus_visit, &query)) return false;
    bool first = !reverse && query.selected == OS_CONTROL_NO_PARENT;
    uint32_t index = first ? query.first : query.selected;
    if (box && index != OS_CONTROL_NO_PARENT) *box = first ? query.first_box : query.selected_box;
    *next = index;
    return true;
}

static bool control_contains(Rect rect, int32_t x, int32_t y) {
    return x >= rect.x0 && x < rect.x1 && y >= rect.y0 && y < rect.y1;
}

uint32_t control_tree_hit_test(const os_control_node_t *nodes, uint32_t count,
    const control_tree_box_t *boxes, int32_t x, int32_t y) {
    if (!nodes || !boxes || count > OS_CONTROL_TREE_MAX_NODES) return OS_CONTROL_NO_PARENT;
    /* 与绘制采用同一顺序，后绘制的兄弟子树优先命中。禁用和隐藏状态由布局继承。 */
    for (uint32_t i = count; i != 0U; --i) {
        uint32_t index = i - 1U;
        if (!boxes[index].visible || !boxes[index].enabled ||
            (nodes[index].flags & OS_CONTROL_POINTER_PASSTHROUGH)) continue;
        if (control_contains(boxes[index].bounds, x, y) && control_contains(boxes[index].clip, x, y))
            return index;
    }
    return OS_CONTROL_NO_PARENT;
}

void control_tree_paint(const os_control_node_t *nodes, uint32_t count,
    const control_tree_box_t *boxes, Rect damage, control_tree_paint_fn paint, void *context) {
    if (!nodes || !boxes || !paint || count > OS_CONTROL_TREE_MAX_NODES || rect_is_empty(damage)) return;
    for (uint32_t i = 0U; i < count; ++i) {
        Rect clip = control_intersection(damage, boxes[i].clip);
        Rect clipped = control_intersection(clip, boxes[i].bounds);
        /* 隐藏和继承裁剪影响整条分支；父边界只在明确裁剪子控件时才能用于跳过。 */
        if (!boxes[i].visible || rect_is_empty(clip) ||
            ((nodes[i].flags & OS_CONTROL_CLIP_CHILDREN) && rect_is_empty(clipped))) {
            uint32_t end = boxes[i].subtree_end;
            if (end > i && end <= count) i = end - 1U;
            continue;
        }
        if (!rect_is_empty(clipped)) paint(context, nodes + i, boxes + i, clipped);
    }
}

typedef struct control_paint_query {
    Rect damage;
    control_tree_paint_fn paint;
    void *context;
} control_paint_query_t;

static void control_paint_visit(void *raw, uint32_t index, const os_control_node_t *node,
    const control_tree_box_t *box) {
    (void)index;
    control_paint_query_t *query = raw;
    if (!box->visible) return;
    Rect clipped = control_intersection(control_intersection(query->damage, box->clip), box->bounds);
    if (!rect_is_empty(clipped)) query->paint(query->context, node, box, clipped);
}

bool control_tree_paint_view(const os_control_node_t *nodes, uint32_t count,
    Rect viewport, Rect damage, control_tree_paint_fn paint, void *context) {
    if (!paint) return false;
    control_paint_query_t query = {damage, paint, context};
    return control_layout_walk(nodes, count, viewport, 0, 0, 0, 0, 0, control_paint_visit, &query);
}

uint32_t control_tree_event_path(const os_control_node_t *nodes, uint32_t count,
    uint32_t target, uint32_t *path, uint32_t capacity) {
    uint32_t reverse[OS_CONTROL_TREE_MAX_DEPTH], length = 0U;
    if (!nodes || !path || count > OS_CONTROL_TREE_MAX_NODES || target >= count) return 0U;
    for (;;) {
        if (length == OS_CONTROL_TREE_MAX_DEPTH) return 0U;
        reverse[length++] = target;
        uint32_t parent = nodes[target].parent;
        if (parent == OS_CONTROL_NO_PARENT) {
            if (target != 0U) return 0U;
            break;
        }
        if (parent >= target) return 0U;
        target = parent;
    }
    if (capacity < length) return 0U;
    for (uint32_t i = 0U; i < length; ++i) path[i] = reverse[length - i - 1U];
    return length;
}
