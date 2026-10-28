#pragma once

#include <string.h>
#include <uapi/control_validation.h>

static inline bool ui_modal_ranges_overlap(uintptr_t a,size_t size_a,uintptr_t b,size_t size_b) {
    if(!size_a || !size_b)return false;
    return a<=b ? b-a<size_a : a-b<size_b;
}

/* 公共弹窗的第二个节点是底层占位，原地换成应用子树，其他节点保留稳定标识。
 * 输入与输出缓冲不得重叠；所有校验先完成，失败不改变正文、节点或计数。
 * 绘制和命中继续由合成器处理，不建立额外 Surface 或兼容绘制路径。 */
static inline bool ui_modal_tree_attach(os_control_node_t *nodes,uint32_t *count,uint32_t capacity,
    char *text,uint32_t *text_size,uint32_t text_capacity,const os_window_control_tree_t *modal) {
    if(!nodes || !count || !text || !text_size || !modal || !modal->nodes ||
        !*count || *count>capacity || *text_size>text_capacity || modal->count<2U ||
        modal->count>OS_CONTROL_TREE_MAX_NODES || *count>OS_CONTROL_TREE_MAX_NODES ||
        modal->count-1U>capacity-*count || modal->count-1U>OS_CONTROL_TREE_MAX_NODES-*count ||
        modal->text_size>text_capacity-*text_size || *text_size>OS_CONTROL_TREE_MAX_TEXT ||
        modal->text_size>OS_CONTROL_TREE_MAX_TEXT-*text_size || (modal->text_size && !modal->text))return false;
    const os_control_node_t *overlay=(const void *)(uintptr_t)modal->nodes;
    const char *overlay_text=(const void *)(uintptr_t)modal->text;
    uint32_t base_count=*count,base_text=*text_size,total=base_count+modal->count-1U;
    if(ui_modal_ranges_overlap((uintptr_t)nodes,(size_t)total*sizeof(*nodes),
            (uintptr_t)overlay,(size_t)modal->count*sizeof(*overlay)) ||
        ui_modal_ranges_overlap((uintptr_t)text,(size_t)base_text+modal->text_size,
            (uintptr_t)overlay_text,modal->text_size))return false;
    /* 复用唯一性校验的索引，后续冲突查询不再重复排序应用控件树。 */
    uint16_t order[OS_CONTROL_TREE_MAX_NODES];
    if(overlay[0].kind!=OS_CONTROL_CONTAINER || overlay[1].kind!=OS_CONTROL_SURFACE ||
        overlay[1].parent!=0U || !os_control_tree_validate_indexed(nodes,base_count,base_text,order) ||
        !os_control_tree_validate(overlay,modal->count,modal->text_size))return false;
    uint8_t depth[OS_CONTROL_TREE_MAX_NODES];depth[0]=0;
    /* 原树向下移一层，不能超过公共深度上限；原有默认焦点随后会被解除。 */
    for(uint32_t i=1;i<base_count;++i) {
        uint32_t next=(uint32_t)depth[nodes[i].parent]+1U;
        if(next>=OS_CONTROL_TREE_MAX_DEPTH-1U)return false;
        depth[i]=(uint8_t)next;
    }
    for(uint32_t i=0;i<modal->count;++i) {
        if(i==1U)continue;
        if(i>1U && overlay[i].parent==1U)return false;
        uint32_t low=0,high=base_count;
        while(low<high) {
            uint32_t middle=low+(high-low)/2U;
            if(nodes[order[middle]].identifier<overlay[i].identifier)low=middle+1U;
            else high=middle;
        }
        if(low<base_count && nodes[order[low]].identifier==overlay[i].identifier)return false;
    }
    memmove(nodes+1,nodes,(size_t)base_count*sizeof(*nodes));
    nodes[0]=overlay[0];nodes[0].text_offset+=base_text;
    nodes[0].flags|=nodes[1].flags&OS_CONTROL_WINDOW_FRAME;
    if(nodes[1].flags&OS_CONTROL_WINDOW_FRAME) {
        nodes[1].flags|=OS_CONTROL_BACKGROUND_ALPHA;nodes[1].background=0;
    }
    nodes[1].parent=0;nodes[1].flags|=OS_CONTROL_INERT;
    nodes[1].flags&=~(OS_CONTROL_WINDOW_FRAME|OS_CONTROL_MANAGED_FOCUS|OS_CONTROL_FOCUS_ARROWS);
    nodes[1].x=nodes[1].y=0;nodes[1].width=nodes[1].height=OS_CONTROL_FILL;
    for(uint32_t i=1;i<=base_count;++i) {
        nodes[i].flags&=~OS_CONTROL_DEFAULT_FOCUS;
        if(i>1U)++nodes[i].parent;
    }
    uint32_t appended=base_count+1U;
    for(uint32_t i=2;i<modal->count;++i) {
        os_control_node_t node=overlay[i];
        node.parent=node.parent ? base_count+node.parent-1U : 0U;
        node.text_offset+=base_text;nodes[appended++]=node;
    }
    if(modal->text_size)memcpy(text+base_text,overlay_text,modal->text_size);
    *count=total;*text_size=base_text+modal->text_size;return true;
}
