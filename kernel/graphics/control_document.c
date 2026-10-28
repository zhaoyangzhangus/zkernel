#include <arch/x86_64/uaccess.h>
#include <ascii_font.h>
#include <kernel/control_tree.h>
#include <kernel/control_selection.h>
#include <kernel/clipboard.h>
#include <kernel/control_raster.h>
#include <kernel/control_icons.h>
#include <kernel/control_text.h>
#include <uapi/utf8.h>
#include <uapi/control_validation.h>
#include <kernel/display.h>
#include <kernel/kmem.h>
#include <kernel/string.h>
#include <uapi/window_theme.h>
#include "internal.h"

_Static_assert(OS_CONTROL_TREE_MAX_TEXT<=UINT16_MAX+1U,"文字索引必须容纳文档全部字符起点");
_Static_assert(sizeof(control_text_cell_t)==8U,"字符索引不能增加每项内存占用");

typedef struct control_font_cache {
    atomic_uint references;
    uint32_t width, height;
    uint8_t pixels[];
} control_font_cache_t;

typedef struct window_control_document {
    atomic_uint references;
    uint32_t count, text_size;
    uint64_t revision;
    os_control_node_t *nodes;
    uint16_t *targets;
    char *text;
    /* 提交准备时的视口在发布后不可变，不能用合成器的可变缓存判断提交是否幂等。 */
    uint32_t prepared_width, prepared_height, prepared_flags;
    /* 仅合成器线程使用的布局缓存；应用数据在发布后只读。输入路由不能读取此缓存。 */
    control_tree_box_t *boxes;
    uint32_t layout_width, layout_height, layout_flags;
    bool layout_valid,has_selectable_text,has_caption;
    /* 提交线程按目标字号生成字形；合成线程只读缓存，不分配内存或改变全局字号。 */
    control_font_cache_t *fonts[97];
    control_text_run_t **runs;
    size_t run_bytes;
} window_control_document_t;

bool window_control_has_caption(const window_control_document_t *document) {
    return document && document->has_caption;
}

enum { CONTROL_TEXT_IDLE,CONTROL_TEXT_PRESSED,CONTROL_TEXT_DRAGGED };

static bool control_text_selectable(const os_control_node_t *node);

/* 事件生产复用提交校验生成的有序索引，不分配内存或重新排序。 */
static Rect control_document_text_bounds(const window_control_document_t *document,
    uint32_t index,const control_tree_box_t *box,uint32_t font) {
    const os_control_node_t *node=document->nodes+index;
    const control_text_run_t *run=document->runs?document->runs[index]:0;
    uint32_t width=control_selection_prepared_width(run,document->text+node->text_offset,node->text_length,font);
    return control_text_bounds_width(node,box,width,font);
}

static uint32_t control_target_index(const window_control_document_t *document, uint32_t target) {
    if (!document || !target) return OS_CONTROL_NO_PARENT;
    uint32_t first=0U, last=document->count;
    while (first<last) {
        uint32_t middle=first+(last-first)/2U;
        uint32_t index=document->targets[middle];
        const os_control_node_t *node=document->nodes+index;
        if (node->identifier<target) first=middle+1U;
        else if (node->identifier>target) last=middle;
        else return index;
    }
    return OS_CONTROL_NO_PARENT;
}

bool window_control_event_metadata_locked(const window_server_window_t *window,
    uint32_t target, os_control_event_t *event) {
    const window_control_document_t *document=window->controls;
    uint32_t index=control_target_index(document,target);
    if (index==OS_CONTROL_NO_PARENT) return false;
    const os_control_node_t *node=document->nodes+index;
    event->node_index=index;event->node_kind=node->kind;
    event->tree_revision=document->revision;event->user_data=node->user_data;
    return true;
}

void window_control_document_get(window_control_document_t *document) {
    if (document) atomic_fetch_add_explicit(&document->references, 1U, memory_order_relaxed);
}

void window_control_document_put(window_control_document_t *document) {
    if (!document || atomic_fetch_sub_explicit(&document->references, 1U,
        memory_order_acq_rel) != 1U) return;
    for (uint32_t size = 0U; size < 97U; ++size) {
        control_font_cache_t *font = document->fonts[size];
        if (font && atomic_fetch_sub_explicit(&font->references, 1U, memory_order_acq_rel) == 1U)
            kfree(font);
    }
    if (document->runs) for (uint32_t i=0;i<document->count;++i) {
        control_text_run_t *run=document->runs[i];
        if (run && atomic_fetch_sub_explicit(&run->references,1U,memory_order_acq_rel)==1U) kfree(run);
    }
    kfree(document->runs);
    kfree(document->nodes); kfree(document->boxes); kfree(document->text); kfree(document);
}

#define CONTROL_TEXT_BUDGET (8U*1024U*1024U)

static uint32_t control_text_unicode_offset(const char *text,uint32_t length) {
    for (uint32_t i=0;i<length;++i) if ((uint8_t)text[i]>=128U) return i;
    return length;
}

/* 只在提交阶段构建：哈希去重字形，预先保存字符位置；失败不影响旧文档。 */
static kstatus_t control_text_prepare(window_control_document_t *document,
    window_control_document_t *previous,uint32_t index) {
    const os_control_node_t *node=document->nodes+index;
    const char *text=document->text+node->text_offset;
    uint32_t unicode_offset=control_text_unicode_offset(text,node->text_length);
    if (unicode_offset==node->text_length) return K_OK;
    uint32_t height=document->boxes[index].style.font_size;
    if (!height) height=ascii_font_height();
    uint32_t half=os_control_text_cell_width(' ',height);
    if (!height || !half) return K_EIO;
    if (!document->runs) {
        document->runs=kzalloc((size_t)document->count*sizeof(*document->runs),0);
        if (!document->runs) return K_ENOMEM;
    }
    /* 节点插入或重排不改变文字身份，按稳定标识复用旧字形，不能依赖数组位置。 */
    uint32_t old_index=control_target_index(previous,node->identifier);
    control_text_run_t *run=previous && previous->runs && old_index!=OS_CONTROL_NO_PARENT ?
        previous->runs[old_index] : 0;
    if (run && run->height==height && previous->nodes[old_index].text_length==node->text_length &&
        !memcmp(text,previous->text+previous->nodes[old_index].text_offset,node->text_length)) {
        if (run->bytes>CONTROL_TEXT_BUDGET-document->run_bytes) return K_ENOMEM;
        atomic_fetch_add_explicit(&run->references,1U,memory_order_relaxed);
        document->runs[index]=run;document->run_bytes+=run->bytes;
        return K_OK;
    }
    /* 首次扫描已确认前缀每字节占一个字符，不再从头解码这段 ASCII。 */
    uint32_t count=unicode_offset;
    for (size_t offset=unicode_offset;offset<node->text_length;++count) {
        size_t used; (void)os_utf8_next(text+offset,node->text_length-offset,&used);
        offset+=used ? used : 1;
    }
    uint32_t capacity=1;while (capacity<count*2U) capacity*=2U;
    size_t cells_bytes=(size_t)count*sizeof(control_text_cell_t);
    size_t glyph_bytes=(size_t)count*sizeof(control_text_glyph_t);
    uint8_t *temporary=kmalloc(cells_bytes+glyph_bytes+(size_t)capacity*sizeof(uint32_t),0);
    if (!temporary) return K_ENOMEM;
    control_text_cell_t *cells=(void *)temporary;
    control_text_glyph_t *glyphs=(void *)(temporary+cells_bytes);
    uint32_t *table=(void *)(temporary+cells_bytes+glyph_bytes);
    /* 字符位置和新增字形均在读取前完整写入，只有查重表需要初始空槽。 */
    memset(table,0,(size_t)capacity*sizeof(*table));
    uint32_t unique=0,width=0,pixels=0,cell=0;
    kstatus_t status=K_OK;
    for (size_t offset=0;offset<node->text_length;) {
        size_t used;uint32_t code=os_utf8_next(text+offset,node->text_length-offset,&used);
        if (!used) {used=1;code=0xfffdU;}
        offset+=used;
        uint32_t slot=(code*2654435761U)&(capacity-1);
        while (table[slot] && glyphs[table[slot]-1].code!=code) slot=(slot+1)&(capacity-1);
        if (!table[slot]) {
            uint32_t glyph_width=os_control_text_cell_width(code,height);
            glyphs[unique]=(control_text_glyph_t){code,glyph_width,pixels};
            pixels+=glyph_width*height;table[slot]=++unique;
        }
        uint32_t glyph=table[slot]-1;
        width+=glyphs[glyph].width;cells[cell++]=(control_text_cell_t){.end=width,.glyph=(uint16_t)glyph,
            .byte_offset=(uint16_t)(offset-used)};
    }
    size_t bytes=sizeof(*run)+cells_bytes+(size_t)unique*sizeof(*glyphs)+pixels;
    if (bytes>CONTROL_TEXT_BUDGET-document->run_bytes) {status=K_ENOMEM;goto done;}
    run=kmalloc(bytes,0);
    if (!run) {status=K_ENOMEM;goto done;}
    *run=(control_text_run_t){.count=count,.glyph_count=unique,.width=width,.height=height,.bytes=bytes};
    atomic_init(&run->references,1U);
    run->cells=(void *)(run+1);run->glyphs=(void *)((uint8_t *)run->cells+cells_bytes);
    run->pixels=(void *)(run->glyphs+unique);
    memcpy(run->cells,cells,cells_bytes);memcpy(run->glyphs,glyphs,(size_t)unique*sizeof(*glyphs));
    for (uint32_t i=0;i<unique;++i) {
        control_text_glyph_t glyph=glyphs[i];size_t size=(size_t)glyph.width*height;
        if (!ui_font_copy_glyph(glyph.code,glyph.width,height,run->pixels+glyph.offset,size) &&
            !ui_font_copy_glyph('?',glyph.width,height,run->pixels+glyph.offset,size)) {
            kfree(run);status=K_EIO;goto done;
        }
    }
    document->runs[index]=run;document->run_bytes+=bytes;
done:
    kfree(temporary);return status;
}

static Rect control_pointer_bounds(const control_tree_box_t *box) {
    Rect bounds = box->bounds;
    if (bounds.x0 < box->clip.x0) bounds.x0 = box->clip.x0;
    if (bounds.y0 < box->clip.y0) bounds.y0 = box->clip.y0;
    if (bounds.x1 > box->clip.x1) bounds.x1 = box->clip.x1;
    if (bounds.y1 > box->clip.y1) bounds.y1 = box->clip.y1;
    return rect_is_empty(bounds) ? (Rect){0} : bounds;
}

/* 桌面弹层和应用文档使用相同的 UTF-8 字形准备，调用方必须在窗口锁外执行。 */
window_control_document_t *window_control_prepare_overlay(window_control_document_t *previous,
    const os_control_node_t *nodes,uint32_t count,const char *text,uint32_t text_size,
    const control_tree_box_t *boxes) {
    if (!nodes || !count || count>OS_CONTROL_TREE_MAX_NODES || !boxes ||
        !text || text_size>OS_CONTROL_TREE_MAX_TEXT) return 0;
    for (uint32_t i=0;i<count;++i)
        if (nodes[i].text_offset>text_size || nodes[i].text_length>text_size-nodes[i].text_offset) return 0;
    bool same=previous && previous->count==count && previous->text_size==text_size &&
        !memcmp(previous->text,text,text_size);
    for (uint32_t i=0;same && i<count;++i)
        same=previous->nodes[i].identifier==nodes[i].identifier &&
            previous->nodes[i].text_offset==nodes[i].text_offset &&
            previous->nodes[i].text_length==nodes[i].text_length &&
            previous->boxes[i].style.font_size==boxes[i].style.font_size;
    if (same) {window_control_document_get(previous);return previous;}
    window_control_document_t *document=kzalloc(sizeof(*document),0);
    if (!document) return 0;
    atomic_init(&document->references,1U);document->count=count;document->text_size=text_size;
    document->nodes=kmalloc((size_t)count*sizeof(*nodes),0);
    document->boxes=kmalloc((size_t)count*sizeof(*boxes),0);
    document->text=kmalloc(text_size?text_size:1U,0);
    /* 复用字形的标识索引；弹层节点数很小，无须单独建立排序索引。 */
    if (!document->nodes || !document->boxes || !document->text) goto fail_overlay;
    memcpy(document->nodes,nodes,(size_t)count*sizeof(*nodes));
    memcpy(document->boxes,boxes,(size_t)count*sizeof(*boxes));
    memcpy(document->text,text,text_size);
    for (uint32_t i=0;i<count;++i)
        if (nodes[i].text_length && control_text_prepare(document,0,i)!=K_OK) goto fail_overlay;
    return document;
fail_overlay:
    window_control_document_put(document);return 0;
}

static Rect control_button_damage(const os_control_node_t *node, const control_tree_box_t *box) {
    return os_control_is_interactive(node->kind) ? control_pointer_bounds(box) : (Rect){0};
}

/* 在窗口锁外比较不可变的交互含义；别处数字刷新不能取消一个稳定按钮的回车。
 * 文字、关联对象、类型或状态改变则取消，避免把旧界面的激活带入新的动作。 */
static bool control_keyboard_target_unchanged(const window_control_document_t *previous,
    const window_control_document_t *next, uint32_t target) {
    if (!previous || !next || previous->nodes[0].identifier!=next->nodes[0].identifier ||
        !(previous->nodes[0].flags&OS_CONTROL_MANAGED_FOCUS) ||
        !(next->nodes[0].flags&OS_CONTROL_MANAGED_FOCUS)) return false;
    uint32_t old_index=control_target_index(previous,target),new_index=control_target_index(next,target);
    if (old_index==OS_CONTROL_NO_PARENT || new_index==OS_CONTROL_NO_PARENT) return false;
    const os_control_node_t *old=previous->nodes+old_index,*node=next->nodes+new_index;
    const control_tree_box_t *box=next->boxes+new_index;
    return os_control_is_interactive(node->kind) && old->kind==node->kind &&
        old->user_data==node->user_data && old->value==node->value &&
        !((old->flags^node->flags)&(OS_CONTROL_CHECKED|OS_CONTROL_SYMBOL)) &&
        box->visible && box->enabled && !rect_is_empty(control_button_damage(node,box)) &&
        old->text_length==node->text_length && (!node->text_length ||
            !memcmp(previous->text+old->text_offset,next->text+node->text_offset,node->text_length));
}

static void control_mark_damage_locked(window_server_window_t *window, Rect bounds) {
    if (bounds.y0 < 0 && window->controls &&
        (window->controls->nodes[0].flags & OS_CONTROL_WINDOW_FRAME)) {
        Rect viewport = control_tree_window_viewport(window->controls->nodes, window->controls->count,
            window->width, window->height, window->flags);
        if (bounds.x0 < 0) bounds.x0 = 0;
        if (bounds.y0 < viewport.y0) bounds.y0 = viewport.y0;
        if (bounds.x1 > viewport.x1) bounds.x1 = viewport.x1;
        if (bounds.y1 > viewport.y1) bounds.y1 = viewport.y1;
        int64_t x = (int64_t)window->x+window_client_offset_x(window->flags)+bounds.x0;
        int64_t y = (int64_t)window->y+window_client_offset_y(window->flags)+bounds.y0;
        if (!rect_is_empty(bounds)) {
            if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX)
                window_mark_window_locked(window);
            else window_mark_rect_locked((int32_t)x, (int32_t)y,
                (uint32_t)(bounds.x1-bounds.x0), (uint32_t)(bounds.y1-bounds.y0));
        }
        return;
    }
    /* 输入缓存可能早于尺寸变化，必须重新裁剪，不能把旧局部范围扩展到窗口外。 */
    Rect clipped;
    if (rect_clip_to_extent(bounds.x0, bounds.y0, bounds.x1, bounds.y1,
        window->width, window->height, &clipped))
        window_mark_surface_locked(window, clipped.x0, clipped.y0,
            (uint32_t)(clipped.x1-clipped.x0), (uint32_t)(clipped.y1-clipped.y0));
}

typedef struct control_surface_damage_context {
    window_server_window_t *window;
    Rect visible;
    bool marked;
} control_surface_damage_context_t;

static void control_surface_damage_emit(void *raw, Rect damage) {
    control_surface_damage_context_t *context = raw;
    if (damage.x0 < context->visible.x0) damage.x0 = context->visible.x0;
    if (damage.y0 < context->visible.y0) damage.y0 = context->visible.y0;
    if (damage.x1 > context->visible.x1) damage.x1 = context->visible.x1;
    if (damage.y1 > context->visible.y1) damage.y1 = context->visible.y1;
    if (rect_is_empty(damage)) return;
    control_mark_damage_locked(context->window, damage);
    context->marked = true;
}

void window_control_surface_damage_locked(window_server_window_t *window,
    int32_t x, int32_t y, uint32_t width, uint32_t height) {
    window_control_document_t *document = window->controls;
    int64_t origin_x = (int64_t)window->x + window_client_offset_x(window->flags);
    int64_t origin_y = (int64_t)window->y + window_client_offset_y(window->flags);
    control_surface_damage_context_t context = {.window=window};
    if (!document || !rect_clip_to_extent(-origin_x, -origin_y,
        (int64_t)g_window_server.display_width-origin_x,
        (int64_t)g_window_server.display_height-origin_y,
        window->width, window->height, &context.visible)) {
        window_mark_window_locked(window);
        return;
    }
    Rect source;
    if (rect_clip_to_extent(x, y, (int64_t)x+width, (int64_t)y+height,
        window->width, window->height, &source) &&
        !control_tree_surface_damage(document->nodes, document->count,
            control_tree_window_viewport(document->nodes, document->count,
                window->width, window->height, window->flags),
            1U, window->width, window->height, source, control_surface_damage_emit, &context)) {
        window_mark_window_locked(window);
        return;
    }
    if (!context.marked) {
        /* 不可见内容也要让提交进入快照，确保 ACK 有序推进；只恢复一个可见像素，
         * 不能直接确认序列，也不能在窗口左上角已移出屏幕时标记不可见位置。 */
        control_mark_damage_locked(window, (Rect){context.visible.x0, context.visible.y0,
            context.visible.x0+1, context.visible.y0+1});
    }
}

static uint32_t control_find_button(window_server_window_t *window, uint32_t identifier,
    control_tree_box_t *box) {
    if (!identifier) return OS_CONTROL_NO_PARENT;
    window_control_document_t *document = window->controls;
    uint32_t candidate=control_target_index(document,identifier);
    if (candidate==OS_CONTROL_NO_PARENT || !os_control_is_interactive(document->nodes[candidate].kind))
        return OS_CONTROL_NO_PARENT;
    uint32_t index = control_tree_find(document->nodes, document->count,
        control_tree_window_viewport(document->nodes, document->count,
            window->width, window->height, window->flags), identifier, box);
    if (index == OS_CONTROL_NO_PARENT || !os_control_is_interactive(document->nodes[index].kind) ||
        !box->visible || !box->enabled || rect_is_empty(control_button_damage(document->nodes+index, box)))
        return OS_CONTROL_NO_PARENT;
    return index;
}

static void control_damage_identifier(window_server_window_t *window, uint32_t identifier) {
    control_tree_box_t box;
    uint32_t index = control_find_button(window, identifier, &box);
    if (index != OS_CONTROL_NO_PARENT)
        control_mark_damage_locked(window, control_button_damage(window->controls->nodes+index, &box));
}

bool window_control_cancel_pointer_press_locked(window_server_window_t *window) {
    if (!window) return false;
    uint32_t pressed = window->control_pressed;
    window->client_button_capture = false;
    window->control_text_drag=CONTROL_TEXT_IDLE;
    if (!pressed) return false;
    window->control_pressed = 0U;
    /* 使用当前布局裁剪原按压控件，不复用可能因缩放失效的悬停范围。
     * 只取消鼠标激活，物理悬停、键盘焦点和按键配对保持不变。 */
    control_damage_identifier(window, pressed);
    return true;
}

void window_control_keyboard_blur_locked(window_server_window_t *window) {
    if (!window || !window->controls) return;
    /* 修饰键可能在另一窗口释放；失去键盘所有权时取消激活，不能留下粘滞按压。 */
    if (window->control_key_target) control_damage_identifier(window, window->control_key_target);
    window->control_key_target = window->control_key_code = window->control_shift = 0U;
    window->control_text_ctrl=0U;
    if(window->control_text_drag)(void)window_control_cancel_pointer_press_locked(window);
}

typedef struct control_focus_damage_query {
    uint32_t targets[3];
    Rect bounds[3];
} control_focus_damage_query_t;

static void control_focus_damage_visit(void *raw, uint32_t index,
    const os_control_node_t *node, const control_tree_box_t *box) {
    (void)index;
    control_focus_damage_query_t *query=raw;
    if (!box->visible || !box->enabled || !os_control_is_interactive(node->kind)) return;
    for (uint32_t i=0;i<3U;++i)
        if (query->targets[i]==node->identifier) query->bounds[i]=control_button_damage(node,box);
}

/* 旧按压与旧、新焦点共享一次即时布局；查询完整成功后才发布损坏区域。 */
static void control_mark_focus_change_locked(window_server_window_t *window, uint32_t next) {
    window_control_document_t *document=window->controls;
    control_focus_damage_query_t query={.targets={window->control_key_target,window->control_focus,next}};
    bool needed=false;
    for (uint32_t i=0;i<3U;++i) {
        uint32_t index=control_target_index(document,query.targets[i]);
        if (index==OS_CONTROL_NO_PARENT || !os_control_is_interactive(document->nodes[index].kind))
            query.targets[i]=0U;
        for (uint32_t j=0;j<i;++j)
            if (query.targets[j]==query.targets[i]) query.targets[i]=0U;
        needed|=query.targets[i]!=0U;
    }
    if (!needed || !control_tree_visit_validated(document->nodes,document->count,
        control_tree_window_viewport(document->nodes,document->count,
            window->width,window->height,window->flags),control_focus_damage_visit,&query)) return;
    for (uint32_t i=0;i<3U;++i) control_mark_damage_locked(window,query.bounds[i]);
}

static void control_commit_focus_locked(window_server_window_t *window, const os_input_event_t *input,
    uint32_t identifier, int32_t x, int32_t y) {
    uint32_t previous = window->control_focus;
    if (previous == identifier) return;
    window->control_key_target = window->control_key_code = 0U;
    window->control_focus = identifier;
    if (previous) {
        window_enqueue_control_event_locked(window, input, previous, OS_CONTROL_BLUR, x, y);
    }
    if (identifier) {
        window_enqueue_control_event_locked(window, input, identifier, OS_CONTROL_FOCUS, x, y);
    }
}

static void control_set_focus(window_server_window_t *window, const os_input_event_t *input,
    uint32_t identifier, int32_t x, int32_t y) {
    if (window->control_focus == identifier) return;
    control_mark_focus_change_locked(window, identifier);
    control_commit_focus_locked(window, input, identifier, x, y);
}

static void control_advance_focus_locked(window_server_window_t *window,
    const os_input_event_t *input, bool reverse, int32_t x, int32_t y) {
    window_control_document_t *document = window->controls;
    control_focus_damage_query_t query = {
        .targets={window->control_key_target, window->control_focus, 0U}};
    control_tree_box_t box;
    uint32_t next;
    /* 导航时同时收集旧键盘按压与旧焦点范围，新焦点直接使用导航返回的布局。
     * 完整布局失败时不更新焦点、不发事件，也不发布半份损坏区域。 */
    if (!control_tree_focus_next_query(document->nodes, document->count,
        control_tree_window_viewport(document->nodes, document->count,
            window->width, window->height, window->flags),
        window->control_focus, reverse, &next, &box, control_focus_damage_visit, &query)) return;
    uint32_t identifier = next == OS_CONTROL_NO_PARENT ? 0U : document->nodes[next].identifier;
    if (identifier == window->control_focus) return;
    if (identifier) {
        query.targets[2] = identifier;
        query.bounds[2] = control_button_damage(document->nodes+next, &box);
    }
    for (uint32_t i=0U; i<3U; ++i) {
        bool duplicate = !query.targets[i];
        for (uint32_t j=0U; j<i; ++j)
            duplicate |= query.targets[j] == query.targets[i];
        if (!duplicate) control_mark_damage_locked(window, query.bounds[i]);
    }
    control_commit_focus_locked(window, input, identifier, x, y);
}

static void control_change(window_server_window_t *window, const os_input_event_t *input,
    const os_control_node_t *node, uint32_t value, int32_t x, int32_t y) {
    uint32_t current = os_control_is_choice(node->kind) ?
        (node->flags & OS_CONTROL_CHECKED) != 0U : node->value;
    /* 拖回已发布值也必须发送，前一条建议值可能还在应用队列中等待处理。 */
    if (value == current && !os_control_is_range(node->kind)) return;
    os_input_event_t change = *input;
    change.value = (int32_t)value;
    window_enqueue_control_event_locked(window, &change, node->identifier, OS_CONTROL_CHANGE, x, y);
}

static void control_activate(window_server_window_t *window, const os_input_event_t *input,
    const os_control_node_t *node, int32_t x, int32_t y) {
    /* 调用者在同一窗口锁内完成命中或焦点校验；事件入队不会替换当前文档。 */
    if (node->kind==OS_CONTROL_WINDOW_BUTTON) {
        window_control_caption_action_locked(window,node->value);
        return;
    }
    window_enqueue_control_event_locked(window, input, node->identifier, OS_CONTROL_CLICK, x, y);
    if (os_control_is_choice(node->kind))
        control_change(window, input, node, node->kind == OS_CONTROL_RADIO ? 1U :
            !(node->flags & OS_CONTROL_CHECKED), x, y);
}

/* 拖动在原控件上捕获，越界位置钳位到端点；不写入不可变的文档快照。 */
static void control_range_cache_store(window_server_window_t *window, uint32_t target, Rect bounds) {
    window->control_range_target = target;
    window->control_range_bounds = bounds;
    window->control_range_width = window->width;
    window->control_range_height = window->height;
    window->control_range_flags = window->flags;
    window->control_range_revision = window->control_revision;
}

static void control_range_pointer(window_server_window_t *window, const os_input_event_t *input,
    uint32_t target, int32_t x, int32_t y) {
    uint32_t candidate=control_target_index(window->controls,target);
    if (candidate==OS_CONTROL_NO_PARENT || !os_control_is_range(window->controls->nodes[candidate].kind))
        return;
    Rect bounds;
    if (window->control_range_target == target &&
        window->control_range_revision == window->control_revision &&
        window->control_range_width == window->width && window->control_range_height == window->height &&
        window->control_range_flags == window->flags) {
        bounds = window->control_range_bounds;
    } else {
        control_tree_box_t box;
        if (control_find_button(window, target, &box) == OS_CONTROL_NO_PARENT) return;
        bounds = box.bounds;
        control_range_cache_store(window, target, bounds);
    }
    const os_control_node_t *node = window->controls->nodes+candidate;
    bool vertical = node->flags & OS_CONTROL_VERTICAL;
    int64_t start = vertical ? bounds.y0 : bounds.x0;
    int64_t size = vertical ? (int64_t)bounds.y1-start : (int64_t)bounds.x1-start;
    int64_t cross = vertical ? (int64_t)bounds.x1-bounds.x0 :
        (int64_t)bounds.y1-bounds.y0;
    int64_t knob = os_control_range_thumb(node,(uint32_t)size,(uint32_t)cross);
    int64_t travel = size-knob;
    int64_t offset = (vertical ? y : x)-start-knob/2;
    if (offset < 0) offset = 0;
    if (offset > travel) offset = travel;
    control_change(window, input, node, travel > 0 ?
        (uint32_t)(offset*OS_CONTROL_VALUE_MAX/travel) : 0U, x, y);
}

static bool control_route_key(window_server_window_t *window, const os_input_event_t *input,
    int32_t x, int32_t y) {
    window_control_document_t *document = window->controls;
    if (!(document->nodes[0].flags & OS_CONTROL_MANAGED_FOCUS)) return false;
    uint32_t code = input->code;
    if (code == 0xe1U || code == 0xe5U) {
        uint32_t bit = code == 0xe1U ? 1U : 2U;
        if (input->value == OS_INPUT_VALUE_RELEASE) window->control_shift &= ~bit;
        else window->control_shift |= bit;
        return false;
    }
    /* 普通文字键不需要几何查询；方向键也只对确实为范围控件的焦点计算布局。 */
    bool range_key=code==0x4aU || code==0x4dU || (code>=0x4fU && code<=0x52U);
    uint32_t focused=range_key?control_target_index(document,window->control_focus):OS_CONTROL_NO_PARENT;
    control_tree_box_t focus_box;
    if (range_key && focused!=OS_CONTROL_NO_PARENT && os_control_is_range(document->nodes[focused].kind) &&
        control_find_button(window,window->control_focus,&focus_box)!=OS_CONTROL_NO_PARENT) {
        const os_control_node_t *node = document->nodes+focused;
        if (input->value != OS_INPUT_VALUE_RELEASE) {
            int32_t value = (int32_t)node->value;
            if (code == 0x4aU) value = 0;
            else if (code == 0x4dU) value = OS_CONTROL_VALUE_MAX;
            else value += (code == 0x4fU || code == 0x51U) ? 100 : -100;
            if (value < 0) value = 0;
            if (value > (int32_t)OS_CONTROL_VALUE_MAX) value = OS_CONTROL_VALUE_MAX;
            control_change(window, input, node, (uint32_t)value, x, y);
        }
        return true;
    }
    bool arrow = (document->nodes[0].flags & OS_CONTROL_FOCUS_ARROWS) &&
        (code == 0x4fU || code == 0x50U);
    if (code == 0x2bU || arrow) {
        if (input->value != OS_INPUT_VALUE_RELEASE) {
            control_advance_focus_locked(window, input,
                arrow ? code == 0x50U : window->control_shift != 0U, x, y);
        }
        return true;
    }
    if (code != 0x28U && code != 0x58U && code != 0x2cU) return false;
    /* 重复按下、自动重复和未配对的释放不改变按钮状态，不为它们重新查询布局。 */
    if (input->value == OS_INPUT_VALUE_PRESS) {
        if (window->control_key_target) return true;
    } else if (input->value != OS_INPUT_VALUE_RELEASE || window->control_key_code != code) return true;
    control_tree_box_t box;
    uint32_t index = control_find_button(window, window->control_focus, &box);
    if (input->value == OS_INPUT_VALUE_PRESS) {
        if (index != OS_CONTROL_NO_PARENT) {
            window->control_key_target = window->control_focus;
            window->control_key_code = code;
            control_mark_damage_locked(window,control_button_damage(document->nodes+index,&box));
        }
    } else {
        uint32_t target = window->control_key_target;
        window->control_key_target = window->control_key_code = 0U;
        if (target && target==window->control_focus && index!=OS_CONTROL_NO_PARENT)
            control_mark_damage_locked(window,control_button_damage(document->nodes+index,&box));
        else control_damage_identifier(window,target);
        if (target && target == window->control_focus && index != OS_CONTROL_NO_PARENT)
            control_activate(window, input, document->nodes+index, x, y);
    }
    return true;
}

kstatus_t window_server_control_tree(process_t *owner, os_window_control_tree_t *request) {
    window_control_document_t *document = 0;
    uint32_t default_focus=0U;
    kstatus_t status = K_OK;
    if (!owner || !request || !request->identifier || request->count > OS_CONTROL_TREE_MAX_NODES ||
        request->text_size > OS_CONTROL_TREE_MAX_TEXT || (!request->count && request->text_size) ||
        request->reserved || (request->count && !request->nodes) ||
        (request->text_size && !request->text))
        return K_EINVAL;
    /* 先校验所有权，避免无权调用者通过无效窗口触发大块复制和分配。 */
    window_lock();
    window_server_window_t *window = window_scene_find_locked(request->identifier);
    bool owned = window && window->owner == owner;
    uint32_t width = owned ? window->width : 0U;
    uint32_t height = owned ? window->height : 0U;
    uint32_t flags = owned ? window->flags : 0U;
    uint32_t key_target=owned?window->control_key_target:0U;
    uint32_t key_code=owned?window->control_key_code:0U;
    /* 旧文档引用跨越准备阶段，重复提交可复用只读字形，不在窗口锁内生成或复制缓存。 */
    window_control_document_t *font_source = owned && request->count ? window->controls : 0;
    window_control_document_get(font_source);
    window_unlock();
    if (!owned) return K_EPERM;
    if (request->count) {
        document = kzalloc(sizeof(*document), 0);
        if (!document) { status = K_ENOMEM; goto fail; }
        atomic_init(&document->references, 1U);
        document->count = request->count; document->text_size = request->text_size;
        document->nodes = kmalloc((size_t)request->count *
            (sizeof(*document->nodes)+sizeof(*document->targets)), 0);
        document->targets = document->nodes ? (uint16_t *)(document->nodes+request->count) : 0;
        if (request->text_size) document->text = kmalloc(request->text_size, 0);
        if (!document->nodes || (request->text_size && !document->text)) {
            status = K_ENOMEM; goto fail;
        }
        status = copy_from_user(document->nodes,
            (const void __user *)(uintptr_t)request->nodes,
            (size_t)request->count * sizeof(*document->nodes));
        if (status != K_OK) goto fail;
        if (request->text_size) {
            status = copy_from_user(document->text,
                (const void __user *)(uintptr_t)request->text, request->text_size);
            if (status != K_OK) goto fail;
        }
        /* 完全相同的文档是幂等提交：不改变版本，不中断按压，也不制造整窗损坏。
         * 大块比较在锁外完成；提交前重新核对所有权、当前文档和视口，避免并发替换竞态。 */
        if (font_source && font_source->count==document->count &&
            font_source->text_size==document->text_size &&
            !memcmp(font_source->nodes,document->nodes,(size_t)document->count*sizeof(*document->nodes)) &&
            (!document->text_size || !memcmp(font_source->text,document->text,document->text_size))) {
            window_lock();
            window=window_scene_find_locked(request->identifier);
            if (!window || window->owner!=owner) {
                window_unlock();status=K_EPERM;goto fail;
            }
            bool unchanged=window->controls==font_source &&
                font_source->prepared_width==window->width && font_source->prepared_height==window->height &&
                font_source->prepared_flags==window->flags;
            if (unchanged) request->revision=font_source->revision;
            window_unlock();
            if (unchanged) {
                window_control_document_put(document);
                window_control_document_put(font_source);
                return K_OK;
            }
        }
        if (!os_control_tree_validate_indexed(document->nodes,document->count,document->text_size,
                                              document->targets)) {
            status = K_EINVAL; goto fail;
        }
        document->has_caption=(document->nodes[0].flags&OS_CONTROL_WINDOW_FRAME)!=0U;
        /* 客户端装饰有独立坐标契约，不允许同时占用服务端标题栏。 */
        if ((document->nodes[0].flags & OS_CONTROL_WINDOW_FRAME) &&
            ((flags & OS_WINDOW_CLIENT_DECORATIONS) || !(flags & OS_WINDOW_STANDARD_CAPTION_CONTROLS))) {
            status = K_EINVAL; goto fail;
        }
        for (uint32_t i = 0U; i < document->count; ++i) {
            const os_control_node_t *node = document->nodes + i;

            /* 首个宿主资源为所属窗口的现有 Surface，绝不解析外部窗口或用户地址。 */
            if ((node->kind == OS_CONTROL_SURFACE && node->surface != 1U) ||
                node->font_size > 96U || node->border_width > 4096U || node->radius > 4096U) {
                status = K_EINVAL; goto fail;
            }
        }
        /* 完成幂等判断和节点校验后才分配布局，重复刷新与无效提交不占用整树几何内存。 */
        document->boxes = kmalloc((size_t)document->count * sizeof(*document->boxes), 0);
        if (!document->boxes) { status = K_ENOMEM; goto fail; }
        /* 在替换前验证布局；无效坐标不能使已显示的旧树消失。 */
        if (width > INT32_MAX || height > INT32_MAX ||
            !control_tree_layout_validated(document->nodes, document->count,
                control_tree_window_viewport(document->nodes, document->count,
                    width, height, flags), document->boxes)) {
            status = K_EINVAL; goto fail;
        }
        document->prepared_width = width; document->prepared_height = height;
        document->prepared_flags = flags;
        document->layout_valid = true;
        document->layout_width = width; document->layout_height = height;
        document->layout_flags = flags;
        for (uint32_t i = 0U; i < document->count; ++i) {
            const os_control_node_t *node = document->nodes + i;
            /* 选择能力随只读文档准备，普通点击不再为了无可选文字的树重复布局。 */
            if(control_text_selectable(node) && document->boxes[i].visible &&
                !document->boxes[i].inert)
                document->has_selectable_text=true;
            /* 复用准备阶段的遍历选出默认焦点，不在发布锁内重新扫描整棵树。 */
            if (!default_focus && (document->nodes[0].flags & OS_CONTROL_MANAGED_FOCUS) &&
                (node->flags & OS_CONTROL_DEFAULT_FOCUS) && document->boxes[i].visible &&
                document->boxes[i].enabled &&
                !rect_is_empty(control_button_damage(node,document->boxes+i)))
                default_focus=node->identifier;
            if (node->text_length) {
                status=control_text_prepare(document,font_source,i);
                if (status!=K_OK) goto fail;
                if (document->runs && document->runs[i]) continue;
            }
            uint32_t size = document->boxes[i].style.font_size;
            if (!node->text_length || !size || size == ascii_font_height() || document->fonts[size]) continue;
            uint32_t font_width = os_control_text_cell_width(' ',size);
            if (!font_width) continue;
            control_font_cache_t *font = font_source ? font_source->fonts[size] : 0;
            if (font && font->width == font_width && font->height == size) {
                atomic_fetch_add_explicit(&font->references, 1U, memory_order_relaxed);
                document->fonts[size] = font;
                continue;
            }
            size_t bytes = (size_t)font_width * size * (ASCII_FONT_LAST - ASCII_FONT_FIRST + 1U);
            font = kmalloc(sizeof(*font) + bytes, 0);
            if (!font) { status = K_ENOMEM; goto fail; }
            atomic_init(&font->references, 1U);
            font->width = font_width; font->height = size;
            document->fonts[size] = font;
            if (!ascii_font_copy_cache(font_width, size, font->pixels, bytes)) {
                status = K_EIO; goto fail;
            }
        }
    }
    bool keep_key=key_target && control_keyboard_target_unchanged(font_source,document,key_target);
    window_lock();
    window = window_scene_find_locked(request->identifier);
    if (!window || window->owner != owner) {
        window_unlock(); status = K_EPERM; goto fail;
    }
    window_control_document_t *previous = window->controls;
    /* 自动刷新不应清除其他控件的选区；只有文字、身份或可交互性改变才取消。 */
    bool keep_selection=false;
    if(document && previous && window->control_text_target &&
        document->nodes[0].identifier==previous->nodes[0].identifier &&
        document->prepared_width==window->width && document->prepared_height==window->height &&
        document->prepared_flags==window->flags) {
        uint32_t old_index=control_target_index(previous,window->control_text_target);
        uint32_t new_index=control_target_index(document,window->control_text_target);
        if(old_index!=OS_CONTROL_NO_PARENT && new_index!=OS_CONTROL_NO_PARENT) {
            const os_control_node_t *old=previous->nodes+old_index,*next=document->nodes+new_index;
            const control_tree_box_t *box=document->boxes+new_index;
            keep_selection=old->kind==next->kind && old->user_data==next->user_data &&
                control_text_selectable(next) && box->visible && !box->inert &&
                old->text_length==next->text_length &&
                window->control_text_anchor<=next->text_length && window->control_text_end<=next->text_length &&
                !memcmp(previous->text+old->text_offset,document->text+next->text_offset,next->text_length);
            if(keep_selection) {
                window->control_text_font=box->style.font_size?box->style.font_size:ascii_font_height();
                window->control_text_bounds=control_document_text_bounds(document,new_index,box,window->control_text_font);
            }
        }
    }
    if(!keep_selection) {
        if(window->control_text_drag && !window->control_pressed)window->client_button_capture=false;
        window->control_text_target=window->control_text_anchor=window->control_text_end=0U;
        window->control_text_drag=CONTROL_TEXT_IDLE;
    }
    ++window->control_revision;
    if (!window->control_revision) ++window->control_revision;
    if (document) document->revision = window->control_revision;
    request->revision = window->control_revision;
    if (!document || !previous ||
        ((document->nodes[0].flags ^ previous->nodes[0].flags) & OS_CONTROL_MANAGED_FOCUS))
        window->control_shift = 0U;
    uint32_t next_focus = 0U;
    if (document) {
        uint32_t focus=control_target_index(document,window->control_focus);
        if (focus!=OS_CONTROL_NO_PARENT && document->boxes[focus].visible && document->boxes[focus].enabled &&
            (!(document->nodes[0].flags & OS_CONTROL_MANAGED_FOCUS) ||
                (os_control_is_interactive(document->nodes[focus].kind) &&
                 !rect_is_empty(control_button_damage(document->nodes+focus,document->boxes+focus)))))
            next_focus=window->control_focus;
        if (!next_focus) next_focus=default_focus;
    }
    uint32_t next_pressed = 0U, next_range = OS_CONTROL_NO_PARENT;
    if (document && previous && window->control_pressed && window->client_button_capture &&
        document->nodes[0].identifier == previous->nodes[0].identifier &&
        !((document->nodes[0].flags ^ previous->nodes[0].flags) & OS_CONTROL_MANAGED_FOCUS)) {
        uint32_t old_index=control_target_index(previous,window->control_pressed);
        uint32_t new_index=control_target_index(document,window->control_pressed);
        if (old_index!=OS_CONTROL_NO_PARENT && new_index!=OS_CONTROL_NO_PARENT &&
            document->nodes[new_index].kind==previous->nodes[old_index].kind &&
            document->nodes[new_index].user_data==previous->nodes[old_index].user_data &&
            (document->nodes[new_index].kind!=OS_CONTROL_WINDOW_BUTTON ||
             document->nodes[new_index].value==previous->nodes[old_index].value) &&
            !(document->nodes[new_index].flags & OS_CONTROL_POINTER_PASSTHROUGH) &&
            document->boxes[new_index].visible && document->boxes[new_index].enabled &&
            !document->boxes[new_index].inert &&
            !rect_is_empty(control_pointer_bounds(document->boxes+new_index))) {
            next_pressed=window->control_pressed;
            if (os_control_is_range(document->nodes[new_index].kind)) next_range=new_index;
        }
    }
    if (window->control_pressed && !next_pressed) window->client_button_capture = false;
    /* 文本容器也需要捕获来结束拖选；稳定控件重绘时保留按压，删除、禁用或换代则取消。 */
    window->control_pressed = next_pressed;
    window->control_range_target = 0U;
    /* 候选布局尚未发布，只在准备视口仍匹配时刷新拖动几何。 */
    if (next_range != OS_CONTROL_NO_PARENT && document->layout_width == window->width &&
        document->layout_height == window->height && document->layout_flags == window->flags)
        control_range_cache_store(window, next_pressed, document->boxes[next_range].bounds);
    window->control_hover = 0U;
    window->control_hover_damage = (Rect){0};
    if (!keep_key || previous!=font_source || next_focus!=key_target ||
        window->control_key_target!=key_target || window->control_key_code!=key_code ||
        document->layout_width!=window->width || document->layout_height!=window->height ||
        document->layout_flags!=window->flags)
        window->control_key_target = window->control_key_code = 0U;
    window->control_input_width = window->width;
    window->control_input_height = window->height;
    window->control_focus = next_focus;
    if (document && window_scene_pointer_window_locked() == window) {
        int64_t x = (int64_t)g_window_server.pointer_x - window->x - window_client_offset_x(window->flags);
        int64_t y = (int64_t)g_window_server.pointer_y - window->y - window_client_offset_y(window->flags);
        Rect viewport = control_tree_window_viewport(document->nodes, document->count,
            window->width, window->height, window->flags);
        if (x >= 0 && y >= viewport.y0 && x < window->width && y < window->height) {
            control_tree_box_t box;
            uint32_t hit;
            /* 候选文档尚未发布，布局数组只属于当前提交线程，可安全复用。
             * 若准备期间发生调整大小，必须改用当前视口查询，不能读取过期几何。 */
            if (document->layout_width==window->width && document->layout_height==window->height &&
                document->layout_flags==window->flags) {
                hit=control_tree_hit_test(document->nodes,document->count,document->boxes,(int32_t)x,(int32_t)y);
                if (hit!=OS_CONTROL_NO_PARENT) box=document->boxes[hit];
            } else hit=control_tree_locate(document->nodes,document->count,
                viewport,(int32_t)x,(int32_t)y,&box);
            if (hit != OS_CONTROL_NO_PARENT &&
                (!(document->nodes[0].flags & OS_CONTROL_WINDOW_FRAME) || y >= 0 ||
                 os_control_is_interactive(document->nodes[hit].kind))) {
                window->control_hover = document->nodes[hit].identifier;
                window->control_hover_damage = control_button_damage(document->nodes+hit, &box);
            }
        }
    }
    window->controls = document;
    window->dirty = true;
    window_mark_window_locked(window);
    window_unlock();
    window_control_document_put(font_source);
    window_control_document_put(previous);
    window_server_notify_worker();
    return K_OK;
fail:
    window_control_document_put(font_source);
    window_control_document_put(document);
    return status;
}

bool window_control_caption_hit_locked(window_server_window_t *window, int32_t screen_x, int32_t screen_y) {
    if (!window || !window->controls ||
        !(window->controls->nodes[0].flags & OS_CONTROL_WINDOW_FRAME)) return false;
    int64_t x = (int64_t)screen_x-window->x-window_client_offset_x(window->flags);
    int64_t y = (int64_t)screen_y-window->y-window_client_offset_y(window->flags);
    Rect viewport = control_tree_window_viewport(window->controls->nodes, window->controls->count,
        window->width, window->height, window->flags);
    if (x < 0 || y < viewport.y0 || y >= 0) return false;
    control_tree_box_t box;
    uint32_t index = control_tree_locate(window->controls->nodes, window->controls->count,
        viewport, (int32_t)x, (int32_t)y, &box);
    return index != OS_CONTROL_NO_PARENT && box.enabled &&
        os_control_is_interactive(window->controls->nodes[index].kind);
}

typedef struct control_pointer_query {
    bool valid;
    uint32_t index;
    control_tree_box_t box;
} control_pointer_query_t;

static bool control_route_pointer_locked(window_server_window_t *window,
    const os_input_event_t *input, int32_t x, int32_t y,
    bool pointer_inside, bool hover_only, bool hover_prepared, const control_pointer_query_t *query) {
    window_control_document_t *document = window->controls;
    if (input->type != OS_INPUT_EVENT_BUTTON && input->type != OS_INPUT_EVENT_RELATIVE &&
        input->type != OS_INPUT_EVENT_ABSOLUTE) return true;
    control_tree_box_t box = {0};
    /* 场景已处理本次悬停时复用目标索引，不对同一物理事件重复执行控件布局。 */
    uint32_t index = OS_CONTROL_NO_PARENT;
    if (pointer_inside) {
        index = hover_prepared ? control_target_index(document, window->control_hover) :
            query && query->valid ? query->index : control_tree_locate(document->nodes, document->count,
                control_tree_window_viewport(document->nodes, document->count,
                    window->width, window->height, window->flags), x, y, &box);
    }
    if (!hover_prepared && query && query->valid) box=query->box;
    /* 标题空白交给窗口拖动；客户区容器仍接收点击，供菜单遮罩等处理关闭。 */
    if (y < 0 && (document->nodes[0].flags & OS_CONTROL_WINDOW_FRAME) &&
        (index == OS_CONTROL_NO_PARENT || !os_control_is_interactive(document->nodes[index].kind))) {
        index = OS_CONTROL_NO_PARENT;
    }
    bool passthrough = (document->nodes[0].flags & OS_CONTROL_WINDOW_FRAME) &&
        index == OS_CONTROL_NO_PARENT && !window->control_pressed;
    uint32_t target = index == OS_CONTROL_NO_PARENT ? 0U : document->nodes[index].identifier;
    if (!hover_prepared && index != OS_CONTROL_NO_PARENT && os_control_is_range(document->nodes[index].kind) &&
        (!window->control_pressed || window->control_pressed == target))
        control_range_cache_store(window, target, box.bounds);
    uint32_t old_hover = window->control_hover, old_pressed = window->control_pressed;
    Rect old_damage = window->control_hover_damage;
    Rect new_damage = index == OS_CONTROL_NO_PARENT ? (Rect){0} :
        hover_prepared ? window->control_hover_damage : control_button_damage(document->nodes+index, &box);
    bool geometry_changed = window->control_input_width != window->width ||
        window->control_input_height != window->height;
    window->control_hover_damage = new_damage;
    window->control_input_width = window->width; window->control_input_height = window->height;
    if (old_hover != target) {
        if (old_hover) window_enqueue_control_event_locked(window, input, old_hover, OS_CONTROL_LEAVE, x, y);
        if (target) window_enqueue_control_event_locked(window, input, target, OS_CONTROL_ENTER, x, y);
        window->control_hover = target;
    }
    /* 悬停分派只更新进入、离开和装饰状态；捕获仍决定谁接收实际移动及释放。 */
    if (!hover_only && !passthrough) {
        if (input->type == OS_INPUT_EVENT_BUTTON) {
            if (input->value == OS_INPUT_VALUE_PRESS) {
                if (input->code == OS_INPUT_BUTTON_LEFT) {
                    window->control_pressed = target;
                    if (document->nodes[0].flags & OS_CONTROL_MANAGED_FOCUS) {
                        uint32_t focus = index != OS_CONTROL_NO_PARENT &&
                            os_control_is_interactive(document->nodes[index].kind) ? target : 0U;
                        /* 背景没有可接收键盘的目标，不能让一次空白点击吞掉对话框焦点。 */
                        if (focus) control_set_focus(window, input, focus, x, y);
                    } else window->control_focus = target;
                    /* 复用窗口捕获，释放或离开时仍由同一个控件接收收尾事件。 */
                    if (target) window->client_button_capture = true;
                }
                if (target) window_enqueue_control_event_locked(window, input, target, OS_CONTROL_DOWN, x, y);
                if (input->code == OS_INPUT_BUTTON_LEFT && target)
                    control_range_pointer(window, input, target, x, y);
            } else if (input->value == OS_INPUT_VALUE_RELEASE) {
                uint32_t pressed = input->code == OS_INPUT_BUTTON_LEFT ? old_pressed : target;
                if (input->code == OS_INPUT_BUTTON_LEFT) {
                    if (pressed) control_range_pointer(window, input, pressed, x, y);
                    window->control_pressed = 0U;
                    window->client_button_capture = false;
                }
                if (pressed) window_enqueue_control_event_locked(window, input, pressed, OS_CONTROL_UP, x, y);
                if (pressed && pressed == target && input->code == OS_INPUT_BUTTON_LEFT)
                    control_activate(window, input, document->nodes+index, x, y);
            }
        } else {
            bool wheel = input->type == OS_INPUT_EVENT_RELATIVE && input->code == OS_INPUT_REL_WHEEL;
            /* 拖动中的移动继续送给按下目标，移出窗口也不能中断文本选择。
             * 悬停仍按当前位置更新，滚轮也保持命中目标的滚动语义。 */
            uint32_t move_target = !wheel && old_pressed && window->client_button_capture ?
                old_pressed : target;
            if (!wheel && old_pressed && window->client_button_capture)
                control_range_pointer(window, input, move_target, x, y);
            if (move_target)
                window_enqueue_control_event_locked(window, input, move_target,
                    wheel ? OS_CONTROL_WHEEL : OS_CONTROL_MOVE, x, y);
        }
    }
    if ((old_hover != window->control_hover || old_pressed != window->control_pressed) &&
        (!rect_is_empty(old_damage) || !rect_is_empty(new_damage))) {
        /* 普通悬停/按压只重绘旧、新按钮；几何变化后旧缓存失效才退回整窗恢复。 */
        if (geometry_changed) window_mark_window_locked(window);
        else {
            control_mark_damage_locked(window, old_damage);
            if (new_damage.x0 != old_damage.x0 || new_damage.y0 != old_damage.y0 ||
                new_damage.x1 != old_damage.x1 || new_damage.y1 != old_damage.y1)
                control_mark_damage_locked(window, new_damage);
        }
    }
    return !passthrough;
}


typedef struct control_text_query {
    int32_t x,y;
    uint32_t index;
    control_tree_box_t box;
} control_text_query_t;

/* 只读文本允许拖选，按钮保留单击行为，拖出非空选区后才取消激活。 */
static bool control_text_selectable(const os_control_node_t *node) {
    return node->text_length && !(node->flags&OS_CONTROL_SYMBOL) &&
        !(node->kind==OS_CONTROL_TEXT && (node->value&OS_CONTROL_TEXT_APP_SELECTION));
}
static void control_text_query_visit(void *raw,uint32_t index,const os_control_node_t *node,
    const control_tree_box_t *box) {
    control_text_query_t *q=raw;
    if(!box->visible ||
        q->x<box->bounds.x0 || q->x>=box->bounds.x1 || q->y<box->bounds.y0 || q->y>=box->bounds.y1 ||
        q->x<box->clip.x0 || q->x>=box->clip.x1 || q->y<box->clip.y0 || q->y>=box->clip.y1)return;
    if(!(node->flags&OS_CONTROL_POINTER_PASSTHROUGH))q->index=OS_CONTROL_NO_PARENT;
    if(box->inert || !control_text_selectable(node))return;
    /* 遍历只保留最上层候选，遮挡下的文字不计算宽度或重复查询标识索引。 */
    q->index=index;q->box.bounds=box->bounds;
    q->box.style.font_size=box->style.font_size;q->box.style.padding=box->style.padding;
}
/* 选区变化只重画两个端点之间的文字，不让长标签的拖动反复损坏整行。 */
static void control_text_damage_selection(window_server_window_t *window,uint32_t index,
    uint32_t first,uint32_t last) {
    if(first==last)return;
    if(first>last){uint32_t swap=first;first=last;last=swap;}
    window_control_document_t *document=window->controls;
    if(index==OS_CONTROL_NO_PARENT)return;
    const os_control_node_t *node=document->nodes+index;
    if(last>node->text_length)return;
    const control_text_run_t *run=document->runs?document->runs[index]:0;
    const char *text=document->text+node->text_offset;
    uint32_t font=window->control_text_font;
    uint32_t start=control_selection_prepared_prefix(run,text,first,font);
    uint32_t end=control_selection_prepared_prefix(run,text,last,font);
    Rect bounds=window->control_text_bounds;
    int64_t x0=(int64_t)bounds.x0+start,x1=(int64_t)bounds.x0+end;
    if(x0<INT32_MIN || x1>INT32_MAX){window_mark_window_locked(window);return;}
    bounds.x0=(int32_t)x0;bounds.x1=(int32_t)x1;
    control_mark_damage_locked(window,bounds);
}

/* 标题栏拖动等非客户区操作也要取消选区，不能只依赖控件收到按下事件。 */
void window_control_clear_text_selection_locked(window_server_window_t *window) {
    if(!window || !window->controls || !window->control_text_target)return;
    if(window->control_text_anchor!=window->control_text_end)
        control_text_damage_selection(window,control_target_index(window->controls,window->control_text_target),
            window->control_text_anchor,window->control_text_end);
    window->control_text_target=window->control_text_anchor=window->control_text_end=0U;
    window->control_text_drag=CONTROL_TEXT_IDLE;
}

window_control_copy_request_t window_control_copy_selection_locked(const os_input_event_t *event) {
    window_control_copy_request_t request={0};
    if(event->type!=OS_INPUT_EVENT_KEY || event->value==OS_INPUT_VALUE_RELEASE ||
        (event->code!=0x06U && event->code!='C'))return request;
    window_server_window_t *window=window_scene_find_locked(g_window_server.focused_identifier);
    if(!window || !window->controls || !window->control_text_ctrl ||
        !window->control_text_target || window->control_text_anchor==window->control_text_end)return request;
    window_control_document_t *document=window->controls;
    uint32_t index=control_target_index(document,window->control_text_target);
    if(index==OS_CONTROL_NO_PARENT)return request;
    uint32_t first=window->control_text_anchor,last=window->control_text_end;
    if(first>last){uint32_t swap=first;first=last;last=swap;}
    const os_control_node_t *node=document->nodes+index;
    if(last>node->text_length)return request;
    window_control_document_get(document);
    return (window_control_copy_request_t){document,node->text_offset+first,last-first};
}

void window_control_copy_complete(window_control_copy_request_t *request) {
    window_control_document_t *document=request->document;
    if(!document)return;
    request->document=0;
    (void)clipboard_set_text(document->text+request->first,request->size);
    window_control_document_put(document);
}

static bool control_text_input(window_server_window_t *window,const os_input_event_t *input,
    int32_t x,int32_t y,bool inside,control_pointer_query_t *pointer) {
    window_control_document_t *document=window->controls;
    if(input->type==OS_INPUT_EVENT_KEY) {
        uint32_t code=input->code;
        if(code==0xe0U || code==0xe4U) {
            uint32_t bit=code==0xe0U?1U:2U;
            if(input->value==OS_INPUT_VALUE_RELEASE)window->control_text_ctrl&=~bit;
            else window->control_text_ctrl|=bit;
        }
        if(window->control_text_ctrl && (code==0x06U || code=='C') &&
            window->control_text_target && window->control_text_anchor!=window->control_text_end) {
            return true;
        }
        return false;
    }
    bool left=input->type==OS_INPUT_EVENT_BUTTON && input->code==OS_INPUT_BUTTON_LEFT;
    if(left && input->value==OS_INPUT_VALUE_PRESS) {
        window_control_clear_text_selection_locked(window);
        if(!inside || !document->has_selectable_text)return false;
        control_text_query_t q={.x=x,.y=y,.index=OS_CONTROL_NO_PARENT};
        /* 文字选择和普通点击共享当前视口的一次遍历，不读取可变的绘制缓存。 */
        pointer->valid=control_tree_locate_query(document->nodes,document->count,
            control_tree_window_viewport(document->nodes,document->count,window->width,window->height,window->flags),
            x,y,&pointer->index,&pointer->box,control_text_query_visit,&q);
        if(!pointer->valid || q.index==OS_CONTROL_NO_PARENT)return false;
        uint32_t index=q.index;
        const os_control_node_t *node=document->nodes+index;
        uint32_t font=q.box.style.font_size?q.box.style.font_size:ascii_font_height();
        Rect bounds=control_document_text_bounds(document,index,&q.box,font);
        /* 控件整块区域都可起选；留白按文字首尾夹紧，拖动不能误变为点击。 */
        window->control_text_target=node->identifier;window->control_text_bounds=bounds;window->control_text_font=font;
        window->control_text_anchor=window->control_text_end=control_selection_prepared_offset(document->runs?document->runs[index]:0,
            document->text+node->text_offset,node->text_length,font,(int64_t)x-bounds.x0);
        window->control_text_drag=CONTROL_TEXT_PRESSED;
        window->client_button_capture=true;
        return false;
    }
    if(!window->control_text_drag)return false;
    bool motion=input->type==OS_INPUT_EVENT_ABSOLUTE || (input->type==OS_INPUT_EVENT_RELATIVE &&
        input->code!=OS_INPUT_REL_WHEEL);
    if(!motion && !(left && input->value==OS_INPUT_VALUE_RELEASE))return false;
    uint32_t index=control_target_index(document,window->control_text_target);
    if(index==OS_CONTROL_NO_PARENT){window->control_text_drag=CONTROL_TEXT_IDLE;return false;}
    const os_control_node_t *node=document->nodes+index;
    uint32_t end=control_selection_prepared_offset(document->runs?document->runs[index]:0,
        document->text+node->text_offset,node->text_length,
        window->control_text_font,(int64_t)x-window->control_text_bounds.x0);
    if(end!=window->control_text_end) {
        /* 移动已定位目标，损坏范围直接复用索引，不在窗口锁内重复查找。 */
        control_text_damage_selection(window,index,window->control_text_end,end);
        window->control_text_end=end;window->control_text_drag=CONTROL_TEXT_DRAGGED;
    }
    bool selected=window->control_text_drag==CONTROL_TEXT_DRAGGED;
    if(left) {
        window->control_text_drag=CONTROL_TEXT_IDLE;
        window->client_button_capture=false;
        if(selected) {
            (void)window_control_cancel_pointer_press_locked(window);
            window->client_button_capture=false;return true;
        }
    }
    return selected;
}

bool window_control_route_locked(window_server_window_t *window, const os_input_event_t *input,
    int32_t x, int32_t y, bool pointer_inside, bool hover_prepared) {
    window_control_document_t *document = window->controls;
    if (!document) return false;
    control_pointer_query_t query={0};
    if (control_text_input(window,input,x,y,pointer_inside,&query))return true;
    if (input->type == OS_INPUT_EVENT_KEY) {
        if ((document->nodes[0].flags & OS_CONTROL_WINDOW_FRAME) &&
            !(document->nodes[0].flags & OS_CONTROL_MANAGED_FOCUS)) return false;
        if (control_route_key(window, input, x, y)) return true;
        uint32_t target = window->control_focus ? window->control_focus : document->nodes[0].identifier;
        window_enqueue_control_event_locked(window, input, target, OS_CONTROL_KEY, x, y);
        return true;
    }
    return control_route_pointer_locked(window, input, x, y, pointer_inside, false, hover_prepared, &query);
}

void window_control_pointer_hover_locked(window_server_window_t *window,
    const os_input_event_t *input, bool pointer_inside) {
    if (!window || !window->controls) return;
    int64_t x = (int64_t)g_window_server.pointer_x - window->x - window_client_offset_x(window->flags);
    int64_t y = (int64_t)g_window_server.pointer_y - window->y - window_client_offset_y(window->flags);
    int32_t local_x = x < INT32_MIN ? INT32_MIN : x > INT32_MAX ? INT32_MAX : (int32_t)x;
    int32_t local_y = y < INT32_MIN ? INT32_MIN : y > INT32_MAX ? INT32_MAX : (int32_t)y;
    (void)control_route_pointer_locked(window, input, local_x, local_y, pointer_inside, true, false, 0);
}

typedef struct control_paint_context {
    const compositor_window_view_t *view;
    window_control_document_t *document;
    int64_t x, y;
} control_paint_context_t;

static void control_paint_surface(control_paint_context_t *context,
    const control_tree_box_t *box, Rect damage) {
    const compositor_window_view_t *view = context->view;
    compositor_surface_source_context_t source;
    compositor_surface_source_prepare(view, &source);
    uint64_t cached_page = UINT64_MAX;
    uint8_t *cached_base = 0;
    /* 第一版宿主按原始像素裁剪，不隐式缩放共享 Surface，也不改变其行跨度。 */
    int64_t right = (int64_t)box->bounds.x0 + view->width;
    int64_t bottom = (int64_t)box->bounds.y0 + view->height;
    if (damage.x1 > right) damage.x1 = (int32_t)right;
    if (damage.y1 > bottom) damage.y1 = (int32_t)bottom;
    for (int32_t y = damage.y0; y < damage.y1; ++y) {
        uint32_t first = (uint32_t)(damage.x0 - box->bounds.x0);
        uint32_t last = (uint32_t)(damage.x1 - box->bounds.x0);
        if (first >= last) continue;
        uint64_t destination = (uint64_t)(context->y + y) * g_window_server.display_stride +
            (uint32_t)(context->x + damage.x0);
        uint64_t row = (uint64_t)(y - box->bounds.y0) * source.row_bytes;
        uint32_t *wb = g_window_server.composite_framebuffer != g_window_server.framebuffer ?
            (uint32_t *)(uintptr_t)(g_window_server.composite_framebuffer + destination) : 0;
        compositor_copy_surface_span(view, &source, row, first, last, wb,
            wb ? 0 : g_window_server.framebuffer + destination, &cached_page, &cached_base);
    }
}

static void control_fill(int64_t x, int64_t y, uint32_t w, uint32_t h,
    uint32_t radius, uint32_t color) {
    if (!radius && x >= INT32_MIN && x <= INT32_MAX && y >= INT32_MIN && y <= INT32_MAX) {
        compositor_fill_rounded_locked((int32_t)x, (int32_t)y, w, h, radius, color);
        return;
    }
    color=os_theme_resolve(color,g_compositor_snapshot.theme);
    /* 先裁剪宽坐标再收窄，背景、透明背景和边环使用同一个逐行轮廓。 */
    Rect clip = g_compositor_snapshot.damage_bounds;
    int64_t right = x + w, bottom = y + h;
    int64_t left = x > clip.x0 ? x : clip.x0;
    int64_t top = y > clip.y0 ? y : clip.y0;
    if (right > clip.x1) right = clip.x1;
    if (bottom > clip.y1) bottom = clip.y1;
    if (left >= right || top >= bottom) return;
    radius=control_round_radius(radius,w,h);
    for (int64_t py=top; py<bottom; ++py) {
        uint32_t row=(uint32_t)(py-y), rows=1U;
        /* 圆角以外的平直中段一次填充，不为相同跨度逐行提交。 */
        if (row>=radius && row<h-radius) {
            int64_t end=y+h-radius<bottom?y+h-radius:bottom;
            rows=(uint32_t)(end-py);
        }
        uint32_t inset=control_round_inset(row, w, h, radius);
        int64_t first=left>x+inset?left:x+inset;
        int64_t last=right<x+w-inset?right:x+w-inset;
        if (first < last)
            compositor_fill_locked((int32_t)first, (int32_t)py,
                (uint32_t)(last-first), rows, color);
        py+=rows-1U;
    }
}

static void control_border_emit(void *context, int64_t x, int64_t y,
    uint32_t width, uint32_t height, uint32_t color) {
    (void)context;
    control_fill(x, y, width, height, 0U, color);
}

/* 底纹锚定到控件原点，局部损坏和窗口移动都不能改变格子的相位。 */
static void control_checkerboard(int64_t x, int64_t y, uint32_t w, uint32_t h,
    uint32_t radius, uint32_t light, uint32_t dark) {
    Rect clip=g_compositor_snapshot.damage_bounds;
    int64_t top=y>clip.y0?y:clip.y0,bottom=y+h<clip.y1?y+h:clip.y1;
    for (int64_t py=top;py<bottom;) {
        uint32_t rows=radius?1U:16U-(uint32_t)(py-y)%16U;
        if ((int64_t)rows>bottom-py) rows=(uint32_t)(bottom-py);
        uint32_t inset=radius?control_round_inset((uint32_t)(py-y),w,h,radius):0U;
        int64_t first=x+inset>clip.x0?x+inset:clip.x0;
        int64_t last=x+w-inset<clip.x1?x+w-inset:clip.x1;
        while (first<last) {
            uint32_t span=16U-(uint32_t)(first-x)%16U;
            if ((int64_t)span>last-first) span=(uint32_t)(last-first);
            uint32_t color=(((uint64_t)(first-x)/16U+(uint64_t)(py-y)/16U)&1U)?dark:light;
            control_fill(first,py,span,rows,0,color);
            first+=span;
        }
        py+=rows;
    }
}

static void control_border(int64_t x, int64_t y, uint32_t w, uint32_t h,
    uint32_t radius, uint32_t border, uint32_t color) {
    control_raster_border_spans(g_compositor_snapshot.damage_bounds, x, y,
        w, h, radius, border, color, control_border_emit, 0);
}

static void control_fill_alpha(int64_t x, int64_t y, uint32_t w, uint32_t h,
    uint32_t radius, uint32_t color) {
    control_raster_target_t target = {g_window_server.composite_framebuffer,
        g_window_server.display_stride, g_compositor_snapshot.damage_bounds};
    control_raster_fill_alpha(&target, x, y, w, h, radius,
        os_theme_resolve(color,g_compositor_snapshot.theme));
}

static void control_paint_text(const os_control_node_t *node, const char *text,
    int64_t text_x, int64_t text_y, const uint8_t *cache) {
    if (!text || !node->text_length) return;
    uint32_t font_w = ascii_font_width(), font_h = ascii_font_height();
    uint32_t target_h = node->font_size ? node->font_size : font_h;
    uint32_t target_w = os_control_text_cell_width(' ',target_h);
    if (!font_w || !font_h || !target_w || !target_h) return;
    Rect clip = g_compositor_snapshot.damage_bounds;
    if (rect_is_empty(clip) || text_y >= clip.y1 || text_y + target_h <= clip.y0) return;
    /* 先按半开裁剪范围跳过整枚字形，再限制字形内的行列，不能逐像素检查整段文本。
     * 所有偏移先使用宽整数，负滚动原点和居中的超长文字都不能回绕。 */
    int64_t first = clip.x0 > text_x ? ((int64_t)clip.x0 - text_x) / target_w : 0;
    int64_t last = ((int64_t)clip.x1 - text_x + target_w - 1) / target_w;
    if (last > node->text_length) last = node->text_length;
    if (first >= last) return;
    uint32_t first_row = text_y < clip.y0 ? (uint32_t)((int64_t)clip.y0-text_y) : 0U;
    uint32_t last_row = text_y + target_h > clip.y1 ? (uint32_t)((int64_t)clip.y1-text_y) : target_h;
    for (int64_t i = first; i < last; ++i) {
        int64_t glyph_x = text_x + i * target_w;
        uint32_t first_col = glyph_x < clip.x0 ? (uint32_t)((int64_t)clip.x0-glyph_x) : 0U;
        uint32_t last_col = glyph_x + target_w > clip.x1 ? (uint32_t)((int64_t)clip.x1-glyph_x) : target_w;
        unsigned char character = (unsigned char)text[i];
        if (character < ASCII_FONT_FIRST || character > ASCII_FONT_LAST) character = '?';
        const uint8_t *glyph = cache ? cache + (size_t)(character-ASCII_FONT_FIRST) * target_w * target_h :
            ascii_font_glyph(character);
        if (!glyph) continue;
        for (uint32_t row = first_row; row < last_row; ++row) {
            uint64_t source_row = cache ? (uint64_t)row * target_w :
                (uint64_t)(row * font_h / target_h) * font_w;
            uint64_t offset = (uint64_t)(text_y+row) * g_window_server.display_stride + (uint32_t)(glyph_x+first_col);
            if (cache) {
                control_text_blend_span(g_window_server.composite_framebuffer+offset,
                    glyph+source_row+first_col, last_col-first_col, node->foreground);
                continue;
            }
            for (uint32_t col = first_col; col < last_col; ++col, ++offset) {
                uint32_t alpha = glyph[source_row + col * font_w / target_w];
                if (!alpha) continue;
                uint32_t color = (node->foreground & 0x00ffffffU) | (alpha << 24U);
                g_window_server.composite_framebuffer[offset] = control_blend_argb(
                    g_window_server.composite_framebuffer[offset], color);
            }
        }
    }
}

static inline uint32_t control_button_background(uint32_t background, bool pressed) {
    uint32_t rgb = background & 0x00ffffffU;
    if (rgb == OS_THEME_SURFACE || rgb == OS_THEME_FIELD || rgb == OS_THEME_TOOLBAR)
        return (background & 0xff000000U) | (pressed ? OS_THEME_SELECTED : OS_THEME_HOVER);
    uint32_t brightness = (background & 255U) + ((background >> 8U) & 255U) +
        ((background >> 16U) & 255U);
    bool light = brightness >= 3U * 192U;
    uint32_t weight = !pressed && light ? 15U : 7U;
    uint32_t tint = pressed || light ? 0U : 255U;
    uint32_t result = background & 0xff000000U;
    /* 白色背景不能再向白色增亮；浅色悬停轻微变暗，按压使用更明显的反馈。 */
    for (uint32_t shift = 0U; shift < 24U; shift += 8U)
        result |= (((((background >> shift) & 255U) * weight + tint) / (weight+1U)) << shift);
    return result;
}

static void control_paint_node(void *raw, const os_control_node_t *node,
    const control_tree_box_t *box, Rect damage) {
    /* 框架根节点不能覆盖统一的聚焦/失焦底色；子控件照常绘制。 */
    if (node->flags & OS_CONTROL_WINDOW_FRAME) return;
    /* 所有绘制分支使用同一份计算样式，源节点保持不变。 */
    const os_control_node_t *source_node=node;
    os_control_node_t resolved = *node;
    resolved.background = box->style.background;
    resolved.foreground = box->style.foreground;
    resolved.font_size = box->style.font_size;
    resolved.border_color = box->style.border_color;
    resolved.border_width = box->style.border_width;
    resolved.radius = box->style.radius;
    resolved.padding = box->style.padding;
    resolved.flags = (resolved.flags & ~OS_CONTROL_BACKGROUND_ALPHA) |
        (box->style.background_alpha ? OS_CONTROL_BACKGROUND_ALPHA : 0U);
    node = &resolved;
    control_paint_context_t *context = raw;
    Rect saved = g_compositor_snapshot.damage_bounds;
    g_compositor_snapshot.damage_bounds = (Rect){
        (int32_t)(context->x + damage.x0), (int32_t)(context->y + damage.y0),
        (int32_t)(context->x + damage.x1), (int32_t)(context->y + damage.y1)};
    int64_t x = context->x + box->bounds.x0;
    int64_t y = context->y + box->bounds.y0;
    uint32_t w = (uint32_t)((int64_t)box->bounds.x1 - box->bounds.x0);
    uint32_t h = (uint32_t)((int64_t)box->bounds.y1 - box->bounds.y0);
    uint32_t background = node->background;
    bool appearance_enabled=box->enabled || box->inert;
    if (os_control_is_interactive(node->kind) && !appearance_enabled)
        background = (background & 0xff000000U) | OS_THEME_TOOLBAR;
    if ((node->kind == OS_CONTROL_LIST_ITEM || node->kind == OS_CONTROL_TAB) &&
        (node->flags & OS_CONTROL_CHECKED) && appearance_enabled) background = OS_THEME_SELECTED;
    bool keyboard_pressed = node->identifier == context->view->control_key_target;
    if (os_control_is_interactive(node->kind) && box->enabled &&
        (keyboard_pressed || (node->identifier == context->view->control_hover &&
            (!(node->flags & OS_CONTROL_NO_HOVER) || node->identifier == context->view->control_pressed)))) {
        background = control_button_background(node->background,
            keyboard_pressed || node->identifier == context->view->control_pressed);
        if ((node->flags & OS_CONTROL_BACKGROUND_ALPHA) && !(node->background >> 24U))
            background = keyboard_pressed || node->identifier == context->view->control_pressed ?
                0x22000000U : 0x11000000U;
    }
    if (node->kind==OS_CONTROL_WINDOW_BUTTON) {
        bool pressed=keyboard_pressed || node->identifier==context->view->control_pressed;
        bool hover=node->identifier==context->view->control_hover;
        uint32_t side=w<h?w:h;
        uint32_t diameter=side*7U/8U;
        if (box->enabled && (pressed || hover))
            control_fill_alpha(x+(w-diameter)/2U,y+(h-diameter)/2U,
                diameter,diameter,diameter/2U,pressed?0x22000000U:0x14000000U);
        compositor_control_caption_icon(x,y,w,h,node->value,context->view->maximized,
            appearance_enabled && (hover || context->view->identifier==g_compositor_snapshot.focused_identifier));
        g_compositor_snapshot.damage_bounds=saved;
        return;
    }
    if (node->flags & OS_CONTROL_CHECKERBOARD)
        control_checkerboard(x,y,w,h,node->radius,background,box->style.foreground);
    else if (node->flags & OS_CONTROL_BACKGROUND_ALPHA)
        control_fill_alpha(x, y, w, h, node->radius, background);
    else control_fill(x, y, w, h, node->radius, background);
    if (node->kind == OS_CONTROL_SURFACE) control_paint_surface(context, box, damage);
    control_border(x, y, w, h, node->radius, node->border_width, node->border_color);
    if (node->flags & OS_CONTROL_SYMBOL) {
        uint32_t side=node->icon_size ? node->icon_size : node->font_size ? node->font_size : 16U;
        if (side>w) side=w;
        if (side>h) side=h;
        int64_t left=x+(w-side)/2U, top=y+(h-side)/2U;
        Rect clip=g_compositor_snapshot.damage_bounds;
        int64_t first_x=left>clip.x0 ? left : clip.x0, first_y=top>clip.y0 ? top : clip.y0;
        int64_t last_x=left+side<clip.x1 ? left+side : clip.x1;
        int64_t last_y=top+side<clip.y1 ? top+side : clip.y1;
        uint32_t color=os_theme_resolve(appearance_enabled ? node->foreground : OS_THEME_MUTED,
            g_compositor_snapshot.theme);
        /* 裁剪后的起点只计算一次采样商余数；行内及换行都精确递推。
         * 不生成中间位图，主题、禁用状态和透明混合仍由统一绘制路径处理。 */
        control_icon_axis_t columns,rows;
        if (first_x<last_x && first_y<last_y &&
            control_icon_axis_start(&columns,(uint32_t)(first_x-left),side) &&
            control_icon_axis_start(&rows,(uint32_t)(first_y-top),side)) {
            for (int64_t py=first_y; py<last_y; ++py) {
                control_icon_axis_t cursor=columns;
                for (int64_t px=first_x; px<last_x; ++px) {
                    uint32_t alpha=control_icon_samples_alpha(node->value,cursor.samples,rows.samples);
                    if (alpha) {
                        uint64_t offset=(uint64_t)py*g_window_server.display_stride+(uint32_t)px;
                        g_window_server.composite_framebuffer[offset]=control_blend_argb(
                            g_window_server.composite_framebuffer[offset], (color&0xffffffU)|(alpha<<24U));
                    }
                    control_icon_axis_next(&cursor);
                }
                control_icon_axis_next(&rows);
            }
        }
    }
    uint32_t accent = appearance_enabled ? OS_THEME_ACCENT : OS_THEME_MUTED;
    if (os_control_is_choice(node->kind)) {
        uint32_t side = node->font_size ? node->font_size : 16U;
        if (side > h) side = h;
        uint32_t available = node->kind == OS_CONTROL_SWITCH ? w/2U : w;
        if (side > available) side = available;
        uint32_t mark_w = node->kind == OS_CONTROL_SWITCH ? side*2U : side;
        if (mark_w > w) mark_w = w;
        int64_t mark_y = y+(h-side)/2U;
        bool checked = node->flags & OS_CONTROL_CHECKED;
        uint32_t radius = node->kind == OS_CONTROL_CHECKBOX ? 3U : side/2U;
        control_fill(x, mark_y, mark_w, side, radius, checked ? accent : OS_THEME_FIELD);
        control_border(x, mark_y, mark_w, side, radius, 1U, checked ? accent : OS_THEME_BORDER);
        if (node->kind == OS_CONTROL_SWITCH && side > 4U) {
            uint32_t knob = side-4U;
            control_fill(x+2+(checked ? mark_w-side : 0U), mark_y+2,
                knob, knob, knob/2U, OS_THEME_SURFACE);
        } else if (checked && side >= 8U) {
            if (node->kind == OS_CONTROL_RADIO)
                control_fill(x+side/3U, mark_y+side/3U, side-2U*(side/3U),
                    side-2U*(side/3U), side/2U, OS_THEME_SURFACE);
            else {
                /* 勾号按控件实际字号栅格化，不依赖应用字体是否包含符号。 */
                for (uint32_t i=0; i<side/2U; ++i) {
                    int64_t cy = i<side/5U ? mark_y+side/2U+i :
                        mark_y+side/2U+2U*(side/5U)-i;
                    control_fill(x+side/4U+i, cy, 2U, 2U, 0U, OS_THEME_ON_ACCENT);
                }
            }
        }
    } else if (node->kind == OS_CONTROL_PROGRESS || os_control_is_range(node->kind)) {
        bool vertical = node->flags & OS_CONTROL_VERTICAL;
        uint32_t length = vertical ? h : w, cross = vertical ? w : h;
        uint32_t thickness = cross < 6U ? cross : 6U;
        int64_t tx = x+(vertical ? (cross-thickness)/2U : 0U);
        int64_t ty = y+(vertical ? 0U : (cross-thickness)/2U);
        control_fill(tx, ty, vertical ? thickness : length,
            vertical ? length : thickness, thickness/2U, OS_THEME_BORDER);
        if (node->kind == OS_CONTROL_PROGRESS) {
            uint32_t filled = (uint32_t)((uint64_t)length*node->value/OS_CONTROL_VALUE_MAX);
            control_fill(tx, ty, vertical ? thickness : filled,
                vertical ? filled : thickness, thickness/2U, accent);
        } else {
            uint32_t knob = os_control_range_thumb(node,length,cross);
            uint32_t position = (uint32_t)((uint64_t)(length-knob)*node->value/OS_CONTROL_VALUE_MAX);
            uint32_t breadth = node->kind == OS_CONTROL_SCROLLBAR ? thickness : cross;
            control_fill(x+(vertical ? (cross-breadth)/2U : position),
                y+(vertical ? position : (cross-breadth)/2U),
                vertical ? breadth : knob, vertical ? knob : breadth,
                breadth/2U, accent);
        }
    } else if (node->kind == OS_CONTROL_SEPARATOR) {
        bool vertical = node->flags & OS_CONTROL_VERTICAL;
        control_fill(x+(vertical ? w/2U : 0U), y+(vertical ? 0U : h/2U),
            vertical ? (w ? 1U : 0U) : w, vertical ? h : (h ? 1U : 0U), 0U, node->border_color);
    }
    if ((context->document->nodes[0].flags & OS_CONTROL_MANAGED_FOCUS) &&
        os_control_is_interactive(node->kind) && box->enabled && node->identifier == context->view->control_focus &&
        w > 10U && h > 10U) {
        /* 焦点框也是声明式控件的样式，不要求应用把边线画进 Surface。 */
        uint32_t focus = node->background == OS_THEME_ACCENT ? OS_THEME_ON_ACCENT : OS_THEME_FOCUS;
        control_fill(x+4, y+4, w-8, 1, 0U, focus);
        control_fill(x+4, y+h-5, w-8, 1, 0U, focus);
        control_fill(x+4, y+4, 1, h-8, 0U, focus);
        control_fill(x+w-5, y+4, 1, h-8, 0U, focus);
    }
    /* 无文字节点的背景、图标、边框和焦点框已绘制，无需访问文字或字体缓存。 */
    if (!node->text_length) {
        g_compositor_snapshot.damage_bounds = saved;
        return;
    }
    /* 文字由合成器绘制，Surface 不保存预先栅格化的按钮或标签。 */
    os_control_node_t styled = *node;
    if (!appearance_enabled && os_control_is_interactive(node->kind)) styled.foreground = OS_THEME_MUTED;
    /* 强调色按钮上的白字保持对比，不能当成窗口底色解析。 */
    if (node->background==OS_THEME_ACCENT && styled.foreground==OS_THEME_ON_ACCENT)
        styled.foreground=(g_compositor_snapshot.theme>>8U)==3U?0x202125U:0xFFFFFFU;
    else styled.foreground=os_theme_resolve(styled.foreground,g_compositor_snapshot.theme);
    /* 控件文字始终受自身包围盒裁剪，不能因长标签覆盖相邻控件。 */
    Rect text_clip = g_compositor_snapshot.damage_bounds;
    if (text_clip.x0 < x) text_clip.x0 = (int32_t)x;
    if (text_clip.y0 < y) text_clip.y0 = (int32_t)y;
    if (text_clip.x1 > x+w) text_clip.x1 = (int32_t)(x+w);
    if (text_clip.y1 > y+h) text_clip.y1 = (int32_t)(y+h);
    if (rect_is_empty(text_clip)) {
        g_compositor_snapshot.damage_bounds = saved;
        return;
    }
    g_compositor_snapshot.damage_bounds = text_clip;
    control_text_run_t *run=context->document->runs ?
        context->document->runs[source_node-context->document->nodes] : 0;
    const char *text=context->document->text?context->document->text+node->text_offset:0;
    uint32_t font=run?run->height:styled.font_size?styled.font_size:ascii_font_height();
    uint32_t text_width=control_selection_prepared_width(run,text,node->text_length,font);
    control_text_position_t position=control_text_position(node,box,text_width,font);
    int64_t text_x=context->x+position.x,text_y=context->y+position.y;
    if (context->view->control_text_target==node->identifier &&
        context->view->control_text_anchor!=context->view->control_text_end) {
        uint32_t first=context->view->control_text_anchor,last=context->view->control_text_end;
        if(first>last){uint32_t swap=first;first=last;last=swap;}
        if(last<=node->text_length) {
            uint32_t start=control_selection_prepared_prefix(run,text,first,font);
            uint32_t end=control_selection_prepared_prefix(run,text,last,font);
            control_fill(text_x+start,text_y,end-start,font,0U,OS_THEME_TEXT_SELECTION);
        }
    }
    if (run) {
        control_raster_target_t target={g_window_server.composite_framebuffer,
            g_window_server.display_stride,text_clip};
        control_text_draw(&target,run,text_x,text_y,styled.foreground);
    } else control_paint_text(&styled,text,text_x,text_y,
        context->document->fonts[styled.font_size] ? context->document->fonts[styled.font_size]->pixels : 0);
    g_compositor_snapshot.damage_bounds = saved;
}

void compositor_control_tree(const compositor_window_view_t *view) {
    window_control_document_t *document = view->controls;
    if (!document || !view->width || !view->height) return;
    if (!document->layout_valid || document->layout_width != view->width ||
        document->layout_height != view->height || document->layout_flags != view->flags) {
        document->layout_valid = control_tree_layout_validated(document->nodes, document->count,
            control_tree_window_viewport(document->nodes, document->count,
                view->width, view->height, view->flags), document->boxes);
        document->layout_width = view->width; document->layout_height = view->height;
        document->layout_flags = view->flags;
    }
    control_paint_context_t context = {.view = view, .document = document,
        .x = (int64_t)view->x + window_client_offset_x(view->flags),
        .y = (int64_t)view->y + window_client_offset_y(view->flags)};
    if (!document->layout_valid) {
        compositor_fill_locked((int32_t)context.x, (int32_t)context.y, view->width, view->height, view->background);
        return;
    }
    Rect screen = g_compositor_snapshot.damage_bounds;
    if (screen.x0 < 0) screen.x0 = 0;
    if (screen.y0 < 0) screen.y0 = 0;
    if ((uint32_t)screen.x1 > g_window_server.display_width) screen.x1 = (int32_t)g_window_server.display_width;
    if ((uint32_t)screen.y1 > g_window_server.display_height) screen.y1 = (int32_t)g_window_server.display_height;
    int64_t left = (int64_t)screen.x0 - context.x, top = (int64_t)screen.y0 - context.y;
    int64_t right = (int64_t)screen.x1 - context.x, bottom = (int64_t)screen.y1 - context.y;
    if (left < 0) left = 0;
    Rect viewport = control_tree_window_viewport(document->nodes, document->count,
        view->width, view->height, view->flags);
    if (top < viewport.y0) top = viewport.y0;
    if (right > view->width) right = view->width;
    if (bottom > view->height) bottom = view->height;
    /* 声明窗口操作的树占有整个标题栏，绘制和命中使用相同裁剪边界。 */
    if (left < right && top < 0 && bottom > viewport.y0) {
        int64_t caption_right = right;
        if (left < caption_right)
            control_tree_paint(document->nodes, document->count, document->boxes,
                (Rect){(int32_t)left, (int32_t)top, (int32_t)caption_right,
                    bottom < 0 ? (int32_t)bottom : 0}, control_paint_node, &context);
        top = 0;
    }
    if (left < right && top < bottom)
        control_tree_paint(document->nodes, document->count, document->boxes,
            (Rect){(int32_t)left, (int32_t)top, (int32_t)right, (int32_t)bottom}, control_paint_node, &context);
}

/* 内核桌面弹出层复用同一个绘制器；调用方提供已验证的纯控件布局，不携带 Surface。 */
void compositor_control_nodes(const os_control_node_t *nodes, uint32_t count,
    const control_tree_box_t *boxes, const char *text, uint32_t hover, uint32_t pressed,
    const window_control_document_t *prepared) {
    if (!nodes || !count || !boxes) return;
    /* 公共绘制器还会读取根节点的焦点策略；临时文档必须携带完整节点视图。 */
    window_control_document_t document = {.nodes = (os_control_node_t *)nodes,
        .count = count, .text = (char *)text,
        .runs = prepared && prepared->count==count ? prepared->runs : 0};
    compositor_window_view_t view = {.control_hover = hover, .control_pressed = pressed};
    control_paint_context_t context = {.view = &view, .document = &document};
    control_tree_paint(nodes, count, boxes, g_compositor_snapshot.damage_bounds,
        control_paint_node, &context);
}
