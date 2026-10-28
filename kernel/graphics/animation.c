#include <arch/x86_64/cpu.h>
#include <kernel/control_raster.h>
#include "internal.h"

#define WINDOW_ANIMATION_NS 300000000ULL
#define WINDOW_ANIMATION_STEP_NS 16000000ULL
#define WINDOW_ANIMATION_FULL 10000U
#define WINDOW_ANIMATION_CLOSE 1U
#define WINDOW_ANIMATION_MINIMIZE 2U
#define WINDOW_ANIMATION_RESTORE 3U

/* 退场只延长既有文档和前台槽位的租约，不复制窗口像素，不延迟进程退出。 */
static struct {
    bool active;
    uint64_t start;
    int32_t target_dx, target_dy;
    compositor_window_view_t view;
} g_window_departures[WINDOW_SERVER_MAX_WINDOWS];
static atomic_uint_fast64_t g_window_animation_deadline;

static uint32_t window_animation_progress(uint64_t start, uint64_t now) {
    uint64_t elapsed = x86_tsc_to_ns(now-start);
    if (elapsed >= WINDOW_ANIMATION_NS) return WINDOW_ANIMATION_FULL;
    uint64_t remaining = WINDOW_ANIMATION_FULL-elapsed*WINDOW_ANIMATION_FULL/WINDOW_ANIMATION_NS;
    /* 三次缓出；整数计算不依赖浮点状态，也不要求固定帧率。 */
    return WINDOW_ANIMATION_FULL-(uint32_t)(remaining*remaining*remaining/100000000ULL);
}

static void window_animation_damage(const compositor_window_view_t *view) {
    window_mark_rect_locked(view->x,view->y,window_outer_width(view->width,view->flags),
        window_outer_height(view->height,view->flags));
}

bool window_animation_ready(void) {
    uint64_t deadline=atomic_load_explicit(&g_window_animation_deadline,memory_order_acquire);
    return deadline && (int64_t)(x86_read_tsc()-deadline)>=0;
}

/* 保存恢复起点，绘制时只平移快照，不改变应用的窗口位置或 Surface。 */
void window_animation_restore_locked(window_server_window_t *window) {
    if (!window || !window->minimized) return;
    control_tree_box_t box;
    window->animation_dx=window->animation_dy=0;
    window->animation_restore=desktop_icon_box_locked(window->desktop_app,&box);
    if (window->animation_restore) {
        window->animation_dx=(int32_t)(((int64_t)box.bounds.x0+box.bounds.x1)/2-
            ((int64_t)window->x+window_outer_width(window->width,window->flags)/2));
        window->animation_dy=(int32_t)(((int64_t)box.bounds.y0+box.bounds.y1)/2-
            ((int64_t)window->y+window_outer_height(window->height,window->flags)/2));
    }
    window->animation_seen=false;
    window->animation_start=0;
}

uint64_t window_animation_wait_ns(void) {
    return atomic_load_explicit(&g_window_animation_deadline,memory_order_acquire) ?
        WINDOW_ANIMATION_STEP_NS : UINT64_MAX;
}

void window_animation_depart_locked(window_server_window_t *window, bool minimize) {
    if (!window || window->minimized || window->client_show_inactive ||
        !(window->flags&OS_WINDOW_VISIBLE) || !window->compositor_presented_valid ||
        window->identifier!=g_window_server.focused_identifier ||
        g_window_server.composite_framebuffer==g_window_server.framebuffer) return;
    for (uint32_t i=0;i<WINDOW_SERVER_MAX_WINDOWS;++i) {
        if (g_window_departures[i].active) continue;
        compositor_capture_window_locked(window,&g_window_departures[i].view);
        g_window_departures[i].view.animation_kind=minimize ? WINDOW_ANIMATION_MINIMIZE : WINDOW_ANIMATION_CLOSE;
        g_window_departures[i].target_dx=g_window_departures[i].target_dy=0;
        if (minimize) {
            control_tree_box_t box;
            if (desktop_icon_box_locked(window->desktop_app,&box)) {
                g_window_departures[i].target_dx=(int32_t)(((int64_t)box.bounds.x0+box.bounds.x1)/2-
                    ((int64_t)window->x+window_outer_width(window->width,window->flags)/2));
                g_window_departures[i].target_dy=(int32_t)(((int64_t)box.bounds.y0+box.bounds.y1)/2-
                    ((int64_t)window->y+window_outer_height(window->height,window->flags)/2));
            }
        }
        g_window_departures[i].view.animation_progress=window->animation_start ?
            window_animation_progress(window->animation_start,x86_read_tsc()) : WINDOW_ANIMATION_FULL;
        g_window_departures[i].start=x86_read_tsc();
        g_window_departures[i].active=true;
        atomic_store_explicit(&g_window_animation_deadline,x86_read_tsc(),memory_order_release);
        window_animation_damage(&g_window_departures[i].view);
        return;
    }
    /* 大量窗口同时退出时直接关闭；不能等待动画槽位而阻塞退出或输入。 */
}

/* 第一帧渲染和发布可能比动画本身还久；从实际发布起计时，不能在后台耗尽打开动画。 */
void window_animation_presented_locked(const compositor_window_view_t *view) {
    if (view->reference && view->reference->animation_start && !view->animation_progress)
        view->reference->animation_start=x86_read_tsc();
}

static void window_departure_damage(uint32_t index) {
    compositor_window_view_t *view=&g_window_departures[index].view;
    int64_t dx=g_window_departures[index].target_dx,dy=g_window_departures[index].target_dy;
    int64_t x=view->x+(dx<0?dx:0),y=view->y+(dy<0?dy:0);
    window_mark_rect_locked((int32_t)x,(int32_t)y,
        window_outer_width(view->width,view->flags)+(uint32_t)(dx<0?-dx:dx),
        window_outer_height(view->height,view->flags)+(uint32_t)(dy<0?-dy:dy));
}

void window_animation_snapshot_locked(void) {
    g_compositor_snapshot.animation_time=x86_read_tsc();
    g_compositor_snapshot.animation_layers=0;
    g_compositor_snapshot.animations=false;
    for (uint32_t i=0;i<WINDOW_SERVER_MAX_WINDOWS;++i) {
        if (!g_window_departures[i].active) continue;
        g_compositor_snapshot.animation_layers |= 1ULL<<i;
        g_compositor_snapshot.animations=true;
    }
    if (g_compositor_snapshot.animations) g_compositor_snapshot.drag_blit_valid=false;
}

void window_animation_capture_locked(compositor_window_view_t *view) {
    window_server_window_t *window=view->reference;
    view->animation_progress=WINDOW_ANIMATION_FULL;
    view->animation_kind=window->animation_restore ? WINDOW_ANIMATION_RESTORE : 0;
    view->animation_dx=window->animation_dx;
    view->animation_dy=window->animation_dy;
    if (!window->animation_seen && !window->client_show_inactive &&
        (window->controls || window->present_sequence || window->surface_content_ready) &&
        g_window_server.composite_framebuffer!=g_window_server.framebuffer) {
        window->animation_seen=true;
        window->animation_start=x86_read_tsc();
        atomic_store_explicit(&g_window_animation_deadline,x86_read_tsc(),memory_order_release);
    }
    if (!window->animation_start) return;
    view->animation_progress=window_animation_progress(window->animation_start,
        g_compositor_snapshot.animation_time < window->animation_start ? window->animation_start :
        g_compositor_snapshot.animation_time);
    if (view->animation_progress<WINDOW_ANIMATION_FULL) {
        g_compositor_snapshot.animations=true;
        g_compositor_snapshot.drag_blit_valid=false;
    }
}

/* 只有合成所有者在两帧之间回收退场视图；销毁与槽位唤醒必须在窗口锁外。 */
void window_animation_tick(void) {
    if (!window_animation_ready()) return;
    for (uint32_t i=0;i<WINDOW_SERVER_MAX_WINDOWS;++i) {
        compositor_window_view_t retired={0};
        window_lock();
        if (g_window_server.composing) {window_unlock();return;}
        if (g_window_departures[i].active) {
            window_departure_damage(i);
            window_server_window_t *restored=window_scene_find_locked(g_window_departures[i].view.identifier);
            if ((restored==g_window_departures[i].view.reference && !restored->minimized) ||
                window_animation_progress(g_window_departures[i].start,x86_read_tsc())==WINDOW_ANIMATION_FULL) {
                retired=g_window_departures[i].view;
                g_window_departures[i].active=false;
            }
        }
        window_unlock();
        if (retired.reference) {
            window_surface_snapshot_release(&retired);
            window_control_document_put(retired.controls);
            caption_text_put(retired.caption);
            object_put(retired.reference);
        }
    }
    window_lock();
    bool active=false;
    uint64_t now=x86_read_tsc();
    for (uint32_t i=0;i<g_window_server.scene.count;++i) {
        window_server_window_t *window=g_window_server.scene.children[i];
        if (!window || !window->animation_start) continue;
        window_mark_window_locked(window);
        if (window->animation_restore) {
            int64_t dx=window->animation_dx,dy=window->animation_dy;
            window_mark_rect_locked(window->x+(int32_t)(dx<0?dx:0),window->y+(int32_t)(dy<0?dy:0),
                window_outer_width(window->width,window->flags)+(uint32_t)(dx<0?-dx:dx),
                window_outer_height(window->height,window->flags)+(uint32_t)(dy<0?-dy:dy));
        }
        if (window_animation_progress(window->animation_start,now)==WINDOW_ANIMATION_FULL) {
            window->animation_start=0;
            window->animation_restore=false;
        } else active=true;
    }
    for (uint32_t i=0;i<WINDOW_SERVER_MAX_WINDOWS;++i) active |= g_window_departures[i].active;
    atomic_store_explicit(&g_window_animation_deadline,active ?
        now+x86_timeout_ns_to_tsc(WINDOW_ANIMATION_STEP_NS) : 0,memory_order_release);
    window_unlock();
}

static void window_animation_span(const compositor_window_view_t *view,Rect span) {
    Rect saved=g_compositor_snapshot.damage_bounds;
    if (span.x0<saved.x0) span.x0=saved.x0;
    if (span.y0<saved.y0) span.y0=saved.y0;
    if (span.x1>saved.x1) span.x1=saved.x1;
    if (span.y1>saved.y1) span.y1=saved.y1;
    if (rect_is_empty(span)) return;
    g_compositor_snapshot.damage_bounds=span;
    compositor_draw_window_locked(view);
    g_compositor_snapshot.damage_bounds=saved;
}

/* 合成器单所有者的有界条带暂存，仅供关闭淡出保留底图；不复制整张 Surface。 */
static uint32_t g_window_fade_background[8192];

static void compositor_animation_fade(const compositor_window_view_t *view) {
    Rect saved=g_compositor_snapshot.damage_bounds,clip;
    if (!rect_clip_to_extent(view->x,view->y,
            (int64_t)view->x+window_outer_width(view->width,view->flags),
            (int64_t)view->y+window_outer_height(view->height,view->flags),
            g_window_server.display_width,g_window_server.display_height,&clip)) return;
    if (clip.x0<saved.x0) clip.x0=saved.x0;
    if (clip.y0<saved.y0) clip.y0=saved.y0;
    if (clip.x1>saved.x1) clip.x1=saved.x1;
    if (clip.y1>saved.y1) clip.y1=saved.y1;
    if (rect_is_empty(clip)) return;
    uint32_t alpha=view->animation_progress*255U/WINDOW_ANIMATION_FULL;
    for (int32_t left=clip.x0;left<clip.x1;) {
        uint32_t width=(uint32_t)(clip.x1-left);
        if (width>8192U) width=8192U;
        uint32_t rows=8192U/width;
        for (int32_t top=clip.y0;top<clip.y1;) {
            uint32_t height=(uint32_t)(clip.y1-top);
            if (height>rows) height=rows;
            for (uint32_t y=0;y<height;++y)
                for (uint32_t x=0;x<width;++x)
                    g_window_fade_background[y*width+x]=g_window_server.composite_framebuffer[
                        (uint64_t)(top+y)*g_window_server.display_stride+left+x];
            g_compositor_snapshot.damage_bounds=(Rect){left,top,left+(int32_t)width,top+(int32_t)height};
            compositor_draw_window_locked(view);
            for (uint32_t y=0;y<height;++y)
                for (uint32_t x=0;x<width;++x) {
                    volatile uint32_t *pixel=g_window_server.composite_framebuffer+
                        (uint64_t)(top+y)*g_window_server.display_stride+left+x;
                    *pixel=control_blend_argb(g_window_fade_background[y*width+x],
                        (alpha<<24)|(*pixel&0xffffffU));
                }
            top+=(int32_t)height;
        }
        left+=(int32_t)width;
    }
    g_compositor_snapshot.damage_bounds=saved;
}

/* 合成器以圆角展开/收拢揭示真实窗口内容；不重分配 Surface，也不向应用发送伪造的 resize。 */
void compositor_animation_window(const compositor_window_view_t *view) {
    uint32_t progress=view->animation_progress;
    compositor_window_view_t restored;
    if (view->animation_kind==WINDOW_ANIMATION_RESTORE) {
        restored=*view;
        restored.x+=(int32_t)((int64_t)view->animation_dx*(WINDOW_ANIMATION_FULL-progress)/WINDOW_ANIMATION_FULL);
        restored.y+=(int32_t)((int64_t)view->animation_dy*(WINDOW_ANIMATION_FULL-progress)/WINDOW_ANIMATION_FULL);
        view=&restored;
    }
    if (progress>=WINDOW_ANIMATION_FULL) {compositor_draw_window_locked(view);return;}
    if (!progress) return;
    if (view->animation_kind==WINDOW_ANIMATION_CLOSE) {compositor_animation_fade(view);return;}
    uint32_t outer_w=window_outer_width(view->width,view->flags);
    uint32_t outer_h=window_outer_height(view->height,view->flags);
    uint32_t w=(uint32_t)((uint64_t)outer_w*progress/WINDOW_ANIMATION_FULL);
    uint32_t h=(uint32_t)((uint64_t)outer_h*progress/WINDOW_ANIMATION_FULL);
    if (!w || !h) return;
    int64_t x=(int64_t)view->x+(outer_w-w)/2U, y=(int64_t)view->y+(outer_h-h)/2U;
    Rect clip;
    if (!rect_clip_to_extent(x,y,x+w,y+h,g_window_server.display_width,g_window_server.display_height,&clip)) return;
    uint32_t radius=control_round_radius(8,w,h);
    for (int32_t row=clip.y0;row<clip.y1;) {
        uint32_t inset=control_round_inset((uint32_t)(row-y),w,h,radius);
        int32_t end=row+1;
        if ((int64_t)row>=y+radius && (int64_t)row<y+h-radius)
            end=clip.y1<y+h-radius?clip.y1:(int32_t)(y+h-radius);
        Rect span;
        if (rect_clip_to_extent(x+inset,row,x+w-inset,end,g_window_server.display_width,
                g_window_server.display_height,&span)) window_animation_span(view,span);
        row=end;
    }
}

void compositor_animation_layers(void) {
    uint64_t layers=g_compositor_snapshot.animation_layers;
    while (layers) {
        uint32_t index=(uint32_t)__builtin_ctzll(layers);
        layers &= layers-1U;
        compositor_window_view_t view=g_window_departures[index].view;
        uint32_t elapsed=window_animation_progress(g_window_departures[index].start,g_compositor_snapshot.animation_time);
        view.animation_progress=(uint32_t)((uint64_t)view.animation_progress*(WINDOW_ANIMATION_FULL-elapsed)/WINDOW_ANIMATION_FULL);
        if (view.animation_kind==WINDOW_ANIMATION_MINIMIZE) {
            view.x+=(int32_t)((int64_t)g_window_departures[index].target_dx*elapsed/WINDOW_ANIMATION_FULL);
            view.y+=(int32_t)((int64_t)g_window_departures[index].target_dy*elapsed/WINDOW_ANIMATION_FULL);
        }
        compositor_animation_window(&view);
    }
}
