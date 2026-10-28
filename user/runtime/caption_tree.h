#pragma once
#include <string.h>
#include <uapi/control_tree.h>
#include <uapi/window.h>
#include <uapi/window_theme.h>

/* 标题栏属于应用的声明式界面。左右位置、标题和预留空间都由调用者选择。 */
typedef struct user_caption_layout {
    const char *title;
    uint32_t width,height,font,title_width;
    bool buttons_left;
    /* 全零采用默认顺序；非零时必须完整指定三个不同操作。 */
    uint32_t order[3];
} user_caption_layout_t;

static inline bool user_caption_tree(os_control_node_t *nodes,uint32_t *count,uint32_t capacity,
    char *text,uint32_t *used,uint32_t text_capacity,user_caption_layout_t layout) {
    if (!nodes || !count || !*count || !used || *used>text_capacity) return false;
    layout.height=os_window_caption_height(OS_WINDOW_CAPTION_HEIGHT(layout.height));
    if (!layout.font) layout.font=16U;
    uint32_t order[3]={OS_CONTROL_WINDOW_MINIMIZE,OS_CONTROL_WINDOW_MAXIMIZE,OS_CONTROL_WINDOW_CLOSE};
    if (layout.order[0] || layout.order[1] || layout.order[2]) {
        uint32_t seen=0;
        for (uint32_t i=0;i<3U;++i) {
            uint32_t action=layout.order[i];
            if (action<OS_CONTROL_WINDOW_MINIMIZE || action>OS_CONTROL_WINDOW_CLOSE || (seen&(1U<<action))) return false;
            seen|=1U<<action;order[i]=action;
        }
    }
    bool framed=(nodes[0].flags&OS_CONTROL_WINDOW_FRAME)!=0U;
    uint32_t length=layout.title?(uint32_t)strlen(layout.title):0U;
    uint32_t extra=3U+(length?1U:0U)+(framed?0U:1U);
    if (*count>capacity || extra>capacity-*count || length>text_capacity-*used) return false;
    if (!framed) {
        os_control_node_t root=nodes[0];
        for (uint32_t i=*count;i>0;--i) {
            nodes[i]=nodes[i-1U];
            nodes[i].parent=i==1U?0U:nodes[i].parent+1U;
        }
        nodes[1].identifier=0x7fffffe0U;
        nodes[1].flags&=~(OS_CONTROL_MANAGED_FOCUS|OS_CONTROL_FOCUS_ARROWS);
        nodes[1].y=(int32_t)layout.height;
        nodes[1].width=nodes[1].height=OS_CONTROL_FILL;
        nodes[0]=(os_control_node_t){.identifier=root.identifier,.parent=OS_CONTROL_NO_PARENT,
            .kind=OS_CONTROL_CONTAINER,.flags=OS_CONTROL_WINDOW_FRAME|OS_CONTROL_CLIP_CHILDREN|
                OS_CONTROL_POINTER_PASSTHROUGH|(root.flags&(OS_CONTROL_MANAGED_FOCUS|OS_CONTROL_FOCUS_ARROWS)),
            .width=OS_CONTROL_FILL,.height=OS_CONTROL_FILL,.font_size=root.font_size,
            .foreground=root.foreground,.background=root.background};
        ++*count;
    }
    uint32_t side=os_window_caption_button_width(OS_WINDOW_CAPTION_HEIGHT(layout.height));
    /* 极窄窗口压缩按钮而非移出窗口，命中范围与显示范围始终一致。 */
    if (side>layout.width/3U) side=layout.width/3U;
    uint32_t buttons=side*3U;
    if (length) {
        uint32_t x=layout.buttons_left?buttons+12U:12U;
        uint32_t available=layout.width>buttons+24U?layout.width-buttons-24U:0U;
        if (layout.title_width && available>layout.title_width) available=layout.title_width;
        nodes[(*count)++]=(os_control_node_t){.identifier=0x7fffffe1U,.parent=0,
            .kind=OS_CONTROL_TEXT,.flags=OS_CONTROL_BACKGROUND_ALPHA|OS_CONTROL_POINTER_PASSTHROUGH,
            .x=(int32_t)x,.y=(int32_t)((layout.height>layout.font?layout.height-layout.font:0U)/2U),
            .width=(int32_t)available,.height=(int32_t)layout.font,.font_size=layout.font,
            .foreground=OS_THEME_TEXT,.text_offset=*used,.text_length=length};
        memcpy(text+*used,layout.title,length);*used+=length;
    }
    for (uint32_t i=0;i<3U;++i) {
        nodes[(*count)++]=(os_control_node_t){.identifier=0x7fffffe1U+order[i],.parent=0,
            .kind=OS_CONTROL_WINDOW_BUTTON,.flags=OS_CONTROL_SKIP_TAB|OS_CONTROL_BACKGROUND_ALPHA,
            .x=(int32_t)((layout.buttons_left?0U:layout.width-buttons)+i*side),
            .width=(int32_t)side,.height=(int32_t)layout.height,
            .value=order[i]};
    }
    return true;
}
