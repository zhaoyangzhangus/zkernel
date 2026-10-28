#pragma once

/* 导航控件避让真实标题和服务端按钮；布局结果同时用于绘制及命中。 */
static void fm4_caption_layout(os_control_node_t *nodes, uint32_t width, uint32_t caption, uint32_t sidebar) {
    uint32_t font=caption>24U ? caption-24U : 16U;
    if (font>96U) font=96U;
    int32_t gap=(int32_t)((font+3U)/4U);
    int32_t size=(int32_t)os_window_caption_button_width(OS_WINDOW_CAPTION_HEIGHT(caption));
    int32_t left=(int32_t)sidebar+gap;
    int32_t right=(int32_t)width-3*size-gap;
    int32_t location=right-left-6*(size+gap);
    bool full=location>=(int32_t)(3U*font);
    bool navigation=right-left-3*(size+gap)>=(int32_t)(3U*font);
    bool address=right-left-(size+gap)>=(int32_t)(3U*font);
    if (!full) location=right-left-(navigation ? 3 : 1)*(size+gap);
    int32_t cursor=left;
    for (uint32_t i=0;i<7U;++i) {
        os_control_node_t *node=nodes+i;
        node->x=cursor;node->y=4;
        node->width=i==2U ? (address ? location : 0) : size;
        node->height=(int32_t)caption-8;
        node->radius=(uint32_t)gap;
        if ((!full && i>=3U && i<=5U) || (!navigation && i<2U) ||
            (!address && i==2U) || (i==6U && right-size<left)) node->flags|=OS_CONTROL_HIDDEN;
        if (!(node->flags&OS_CONTROL_HIDDEN)) cursor+=node->width+gap;
    }
}
