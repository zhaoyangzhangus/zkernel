#pragma once
#include <kernel/control_tree.h>
#include <kernel/control_text.h>
#include <uapi/utf8.h>

/* 选区始终使用 UTF-8 字节边界，命中宽度与系统字形布局一致。 */
static inline uint32_t control_selection_width(const char *text,uint32_t length,uint32_t font) {
    uint32_t width=0;
    for(uint32_t i=0;i<length;) {
        size_t used;uint32_t code=os_utf8_next(text+i,length-i,&used);
        if(!used){used=1;code=0xfffdU;}
        width+=os_control_text_cell_width(code,font);i+=(uint32_t)used;
    }
    return width;
}
static inline uint32_t control_selection_offset(const char *text,uint32_t length,uint32_t font,int64_t x) {
    int64_t width=0;
    for(uint32_t i=0;i<length;) {
        size_t used;uint32_t code=os_utf8_next(text+i,length-i,&used);
        if(!used){used=1;code=0xfffdU;}
        uint32_t advance=os_control_text_cell_width(code,font);
        if(x<width+(int64_t)advance/2)return i;
        width+=advance;i+=(uint32_t)used;
    }
    return length;
}

/* 已准备的中文使用字形宽度，ASCII 等宽文字直接计算；字体变化时才重新测量。 */
static inline uint32_t control_selection_prepared_width(const control_text_run_t *run,
    const char *text,uint32_t length,uint32_t font) {
    if(!run)return length*os_control_text_cell_width(' ',font);
    return run->height==font ? run->width : control_selection_width(text,length,font);
}

/* 等宽 ASCII 直接换算；中文按字符中心二分，返回原始 UTF-8 字节起点。 */
static inline uint32_t control_selection_prepared_offset(const control_text_run_t *run,
    const char *text,uint32_t length,uint32_t font,int64_t x) {
    if(!run) {
        if(x<0)return 0;
        uint32_t advance=os_control_text_cell_width(' ',font);
        if(!advance || (uint64_t)x>=(uint64_t)length*advance)return length;
        uint32_t offset=(uint32_t)((x+advance-advance/2U)/advance);
        return offset<length?offset:length;
    }
    if(run->height!=font)return control_selection_offset(text,length,font,x);
    uint32_t first=0,last=run->count;
    while(first<last) {
        uint32_t middle=first+(last-first)/2U;
        uint32_t left=middle?run->cells[middle-1U].end:0U;
        uint32_t advance=run->cells[middle].end-left;
        if(x<(int64_t)left+advance/2U)last=middle;
        else first=middle+1U;
    }
    return first<run->count?run->cells[first].byte_offset:length;
}

/* 输入产生的选区始终落在字符边界，前缀宽度可直接查询同一份字符索引。 */
static inline uint32_t control_selection_prepared_prefix(const control_text_run_t *run,
    const char *text,uint32_t bytes,uint32_t font) {
    if(!run)return bytes*os_control_text_cell_width(' ',font);
    if(run->height!=font)return control_selection_width(text,bytes,font);
    uint32_t first=0,last=run->count;
    while(first<last) {
        uint32_t middle=first+(last-first)/2U;
        if(run->cells[middle].byte_offset<bytes)first=middle+1U;
        else last=middle;
    }
    return first?run->cells[first-1U].end:0U;
}

typedef struct control_text_position { int64_t x,y; } control_text_position_t;

/* 绘制和文字命中共用定位；超长居中文字先保留宽坐标，绘制时再裁剪。 */
static inline control_text_position_t control_text_position(const os_control_node_t *node,
    const control_tree_box_t *box,uint32_t width,uint32_t font) {
    control_text_position_t position={box->bounds.x0,box->bounds.y0};
    int64_t w=(int64_t)box->bounds.x1-box->bounds.x0,h=(int64_t)box->bounds.y1-box->bounds.y0;
    if(os_control_is_choice(node->kind)) {
        uint32_t side=font<h?font:(uint32_t)h;
        uint32_t available=node->kind==OS_CONTROL_SWITCH?(uint32_t)w/2U:(uint32_t)w;
        if(side>available)side=available;
        uint32_t mark=node->kind==OS_CONTROL_SWITCH?side*2U:side;
        uint32_t offset=mark<w?mark+8U:(uint32_t)w;
        if(offset>w)offset=(uint32_t)w;
        position.x+=offset;
    }
    if(node->kind==OS_CONTROL_BUTTON || node->kind==OS_CONTROL_TAB) {
        position.x+=(w-width)/2;
        position.y+=(h-font)/2;
    } else {
        position.x+=box->style.padding;
        position.y+=node->kind==OS_CONTROL_LIST_ITEM || os_control_is_choice(node->kind) ?
            (h-font)/2 : (int64_t)box->style.padding;
    }
    return position;
}

static inline Rect control_text_bounds_width(const os_control_node_t *node,const control_tree_box_t *box,
    uint32_t width,uint32_t font) {
    control_text_position_t position=control_text_position(node,box,width,font);
    return (Rect){(int32_t)position.x,(int32_t)position.y,
        (int32_t)(position.x+width),(int32_t)(position.y+font)};
}
