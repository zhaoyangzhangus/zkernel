#pragma once
#include "control_tree.h"

static inline bool os_control_identifier_greater(const os_control_node_t *nodes,uint16_t a,uint16_t b) {
    return nodes[a].identifier>nodes[b].identifier;
}
static inline void os_control_identifier_sift(const os_control_node_t *nodes,uint16_t *order,
                                             uint32_t root,uint32_t count) {
    while (root<count/2U) {
        uint32_t child=root*2U+1U;
        if (child+1U<count && os_control_identifier_greater(nodes,order[child+1U],order[child])) ++child;
        if (!os_control_identifier_greater(nodes,order[child],order[root])) break;
        uint16_t swap=order[root];order[root]=order[child];order[child]=swap;
        root=child;
    }
}
static inline bool os_control_identifiers_prepare(const os_control_node_t *nodes,uint32_t count,
                                                 uint16_t *order,bool ordered) {
    for (uint32_t i=0;i<count;++i) order[i]=(uint16_t)i;
    if (ordered) return true;
    for (uint32_t i=count/2U;i>0;--i) os_control_identifier_sift(nodes,order,i-1U,count);
    for (uint32_t end=count;end>1U;) {
        uint16_t swap=order[0];order[0]=order[--end];order[end]=swap;
        os_control_identifier_sift(nodes,order,0,end);
    }
    for (uint32_t i=1;i<count;++i)
        if (nodes[order[i-1U]].identifier==nodes[order[i]].identifier) return false;
    return true;
}
static inline bool os_control_identifiers_unique(const os_control_node_t *nodes,uint32_t count) {
    /* 无需持久索引的调用者只使用有界临时空间；内核文档复用已有索引存储。 */
    _Static_assert(OS_CONTROL_TREE_MAX_NODES<=UINT16_MAX,"control index width");
    uint16_t order[OS_CONTROL_TREE_MAX_NODES];
    return os_control_identifiers_prepare(nodes,count,order,false);
}

/* 结构与唯一性分阶段，已复制到内核的节点只在提交时进行一次完整校验。 */
static inline bool os_control_tree_validate_structure(const os_control_node_t *nodes,uint32_t count,
                                                     uint32_t text_size,bool *increasing) {
    uint32_t ancestors[OS_CONTROL_TREE_MAX_DEPTH],depth=0U;
    bool has_default=false,ordered=true;
    if(!nodes||!count||count>OS_CONTROL_TREE_MAX_NODES||text_size>OS_CONTROL_TREE_MAX_TEXT)return false;
    for(uint32_t i=0;i<count;++i){
        const os_control_node_t *node=nodes+i;
        if (node->icon_size>4096U || (node->icon_size && !(node->flags&OS_CONTROL_SYMBOL))) return false;
        if (node->page_size>OS_CONTROL_VALUE_MAX ||
            (node->page_size && node->kind!=OS_CONTROL_SCROLLBAR)) return false;
        if(!node->identifier||node->kind>OS_CONTROL_WINDOW_BUTTON||node->layout>OS_CONTROL_COLUMN||
            (node->flags&~(OS_CONTROL_HIDDEN|OS_CONTROL_DISABLED|OS_CONTROL_CLIP_CHILDREN|
                OS_CONTROL_POINTER_PASSTHROUGH|OS_CONTROL_BACKGROUND_ALPHA|OS_CONTROL_MANAGED_FOCUS|
                OS_CONTROL_DEFAULT_FOCUS|OS_CONTROL_SKIP_TAB|OS_CONTROL_FOCUS_ARROWS|
                OS_CONTROL_INHERIT_STYLE|OS_CONTROL_FLEX_GROW|OS_CONTROL_JUSTIFY_CENTER|
                OS_CONTROL_JUSTIFY_END|OS_CONTROL_JUSTIFY_BETWEEN|OS_CONTROL_ALIGN_CENTER|
                OS_CONTROL_ALIGN_END|OS_CONTROL_ALIGN_STRETCH|OS_CONTROL_CHECKED|OS_CONTROL_VERTICAL|
                OS_CONTROL_WINDOW_FRAME|OS_CONTROL_SYMBOL|OS_CONTROL_NO_HOVER|OS_CONTROL_CHECKERBOARD|
                OS_CONTROL_MENU_PANEL|OS_CONTROL_DIALOG_PANEL|
                OS_CONTROL_DIALOG_ACTIONS|OS_CONTROL_DIALOG_CLOSE))||
            ((node->flags&(OS_CONTROL_MENU_PANEL|OS_CONTROL_DIALOG_PANEL))&&
                ((!i&&(node->flags&OS_CONTROL_DIALOG_PANEL))||node->kind!=OS_CONTROL_CONTAINER||
                 (node->flags&(OS_CONTROL_MENU_PANEL|OS_CONTROL_DIALOG_PANEL))==
                    (OS_CONTROL_MENU_PANEL|OS_CONTROL_DIALOG_PANEL)))||
            ((node->flags&(OS_CONTROL_DIALOG_ACTIONS|OS_CONTROL_DIALOG_CLOSE))&&
                (!i||node->parent>=i||!(nodes[node->parent].flags&OS_CONTROL_DIALOG_PANEL)||
                 (node->flags&(OS_CONTROL_DIALOG_ACTIONS|OS_CONTROL_DIALOG_CLOSE))==
                    (OS_CONTROL_DIALOG_ACTIONS|OS_CONTROL_DIALOG_CLOSE)||
                 ((node->flags&OS_CONTROL_DIALOG_ACTIONS)&&node->kind!=OS_CONTROL_CONTAINER)||
                 ((node->flags&OS_CONTROL_DIALOG_CLOSE)&&node->kind!=OS_CONTROL_BUTTON)))||
            ((node->flags&OS_CONTROL_CHECKERBOARD)&&node->kind!=OS_CONTROL_CONTAINER)||
            ((node->flags&OS_CONTROL_NO_HOVER)&&!os_control_is_interactive(node->kind))||
            ((node->flags&OS_CONTROL_SYMBOL)&&(node->kind!=OS_CONTROL_BUTTON||node->text_length||
                node->value<=OS_CONTROL_SYMBOL_NONE||node->value>OS_CONTROL_SYMBOL_CLOSE))||
            ((node->flags&(OS_CONTROL_MANAGED_FOCUS|OS_CONTROL_FOCUS_ARROWS|OS_CONTROL_WINDOW_FRAME))&&i)||
            ((node->flags&(OS_CONTROL_DEFAULT_FOCUS|OS_CONTROL_SKIP_TAB))&&!os_control_is_interactive(node->kind))||
            ((node->flags&OS_CONTROL_CHECKED)&&!os_control_is_choice(node->kind)&&
                node->kind!=OS_CONTROL_LIST_ITEM&&node->kind!=OS_CONTROL_TAB)||
            ((node->flags&OS_CONTROL_VERTICAL)&&!os_control_is_range(node->kind)&&
                node->kind!=OS_CONTROL_PROGRESS&&node->kind!=OS_CONTROL_SEPARATOR)||
            ((os_control_is_range(node->kind)||node->kind==OS_CONTROL_PROGRESS)&&node->value>OS_CONTROL_VALUE_MAX)||
            ((node->flags&OS_CONTROL_FOCUS_ARROWS)&&!(node->flags&OS_CONTROL_MANAGED_FOCUS))||
            ((node->flags&OS_CONTROL_DEFAULT_FOCUS)&&(has_default||(node->flags&OS_CONTROL_SKIP_TAB)))||
            node->width<OS_CONTROL_FILL||node->height<OS_CONTROL_FILL||node->padding>INT32_MAX||node->gap>INT32_MAX||
            node->text_offset>text_size||node->text_length>text_size-node->text_offset)return false;
        if (node->kind==OS_CONTROL_WINDOW_BUTTON &&
            (!i || !(nodes[0].flags&OS_CONTROL_WINDOW_FRAME) ||
             node->value<OS_CONTROL_WINDOW_MINIMIZE || node->value>OS_CONTROL_WINDOW_CLOSE ||
             node->text_length)) return false;
        uint32_t justify=node->flags&(OS_CONTROL_JUSTIFY_CENTER|OS_CONTROL_JUSTIFY_END|OS_CONTROL_JUSTIFY_BETWEEN);
        uint32_t align=node->flags&(OS_CONTROL_ALIGN_CENTER|OS_CONTROL_ALIGN_END|OS_CONTROL_ALIGN_STRETCH);
        if((justify&&(justify&(justify-1U)))||(align&&(align&(align-1U)))||
            ((justify||align)&&(node->kind!=OS_CONTROL_CONTAINER||node->layout==OS_CONTROL_ABSOLUTE)))return false;
        if(node->flags&OS_CONTROL_DEFAULT_FOCUS)has_default=true;
        if (i && nodes[i-1U].identifier==node->identifier) return false;
        if (i && nodes[i-1U].identifier>node->identifier) ordered=false;
        if(!i){if(node->parent!=OS_CONTROL_NO_PARENT||node->kind!=OS_CONTROL_CONTAINER)return false;}
        else{
            /* 列表卡片可包含图标和标签；子项仍通过父卡片统一命中与裁剪。 */
            if(node->parent>=i||(nodes[node->parent].kind!=OS_CONTROL_CONTAINER&&
                nodes[node->parent].kind!=OS_CONTROL_LIST_ITEM))return false;
            while(depth&&ancestors[depth-1U]!=node->parent)--depth;
            if(!depth)return false;
        }
        if((node->flags&OS_CONTROL_FLEX_GROW)&&(!i||nodes[node->parent].layout==OS_CONTROL_ABSOLUTE))return false;
        if(depth==OS_CONTROL_TREE_MAX_DEPTH)return false;
        ancestors[depth++]=i;
    }
    *increasing=ordered;
    return true;
}
/* 索引只在返回成功时有效；失败调用者必须丢弃候选文档。 */
static inline bool os_control_tree_validate_indexed(const os_control_node_t *nodes,uint32_t count,
                                                   uint32_t text_size,uint16_t *order) {
    bool ordered;
    return order && os_control_tree_validate_structure(nodes,count,text_size,&ordered) &&
        os_control_identifiers_prepare(nodes,count,order,ordered);
}
/* 文件加载器和内核采用同一套约束；用户态通过不意味着内核可以省略验证。 */
static inline bool os_control_tree_validate(const os_control_node_t *nodes,uint32_t count,uint32_t text_size) {
    bool ordered;
    return os_control_tree_validate_structure(nodes,count,text_size,&ordered) &&
        (ordered || os_control_identifiers_unique(nodes,count));
}
