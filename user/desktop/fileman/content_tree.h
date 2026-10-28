#pragma once

/* 卡片与子控件只提交数据，应用不再接触窗口像素。 */
static bool fm4_content_node(os_control_node_t *nodes,char *text,uint32_t *count,
                              uint32_t *text_size,os_control_node_t node,const char *label) {
    if (*count>=FM4_FRAME_NODE_COUNT) return false;
    if (label && !fm4_frame_text(&node,text,text_size,label)) return false;
    nodes[(*count)++]=node;
    return true;
}
static bool fm4_content_text(os_control_node_t *nodes,char *text,uint32_t *count,
                              uint32_t *text_size,uint32_t parent,uint32_t x,uint32_t y,
                              uint32_t width,const char *label,uint32_t color) {
    return fm4_content_node(nodes,text,count,text_size,(os_control_node_t){
        .identifier=0x20000000U+*count,.parent=parent,.kind=OS_CONTROL_TEXT,
        .flags=OS_CONTROL_POINTER_PASSTHROUGH|OS_CONTROL_BACKGROUND_ALPHA,
        .x=(int32_t)x,.y=(int32_t)y,.width=(int32_t)width,.height=(int32_t)ui_text_size(),
        .font_size=ui_text_size(),.foreground=color},label);
}
static bool fm4_append_content(os_control_node_t *nodes,char *text,uint32_t *count,uint32_t *text_size) {
    uint32_t caption=user_caption_height();
    if (!g_entry_count) {
        uint32_t width=g_window.width>FM4_SIDEBAR_WIDTH ? g_window.width-FM4_SIDEBAR_WIDTH : 0U;
        const char *label=g_query[0] ? "未找到匹配的文件" : "文件夹为空";
        uint32_t measured=fm4_measure(label);
        return fm4_content_text(nodes,text,count,text_size,0U,
            FM4_SIDEBAR_WIDTH+(width>measured ? (width-measured)/2U : 0U),
            caption+fm4_grid_top()+16U,width,label,OS_THEME_MUTED);
    }
    for (uint32_t index=g_first_entry;index<g_entry_count;++index) {
        uint32_t x,y;
        if (!fm4_item_rect(index,&x,&y)) break;
        uint32_t card=*count,width=FM4_GRID_CARD_WIDTH,height=FM4_GRID_CARD_HEIGHT;
        if (!fm4_content_node(nodes,text,count,text_size,(os_control_node_t){
            .identifier=FM4_ITEM_ID_BASE+index,.parent=0,.kind=OS_CONTROL_LIST_ITEM,
            .flags=OS_CONTROL_CLIP_CHILDREN|(g_selection_valid&&g_selected==index ? OS_CONTROL_CHECKED : 0U),
            .x=(int32_t)x,.y=(int32_t)(caption+y),.width=(int32_t)width,.height=(int32_t)height,
            .background=OS_THEME_SURFACE,.foreground=OS_THEME_TEXT,
            .font_size=ui_text_size(),.radius=8U,.user_data=index},NULL)) return false;
        const os_file_info_t *info=&g_entries[index]->info;
        if (g_list_view) {
            char name[OS_FILE_NAME_MAX],details[64]="";
            uint32_t available=width>140U ? width-140U : width>24U ? width-24U : 0U;
            copy_text(name,sizeof(name),info->name);fm4_fit_text(name,sizeof(name),available);
            uint32_t top=height>ui_text_size() ? (height-ui_text_size())/2U : 0U;
            if (!fm4_content_text(nodes,text,count,text_size,card,12U,top,available,name,OS_THEME_TEXT)) return false;
            if (width>140U) {
                if (info->type==OS_FILE_TYPE_DIRECTORY) copy_text(details,sizeof(details),"文件夹");
                else {append_decimal(details,sizeof(details),info->size);append_text(details,sizeof(details)," B");}
                if (!fm4_content_text(nodes,text,count,text_size,card,width-120U,top,116U,details,OS_THEME_MUTED)) return false;
            }
        } else {
            uint32_t icon=FM4_GRID_ICON_SIZE;
            if (icon>width) icon=width;
            if (icon>height) icon=height;
            if (!fm4_content_node(nodes,text,count,text_size,(os_control_node_t){
                .identifier=0x20000000U+*count,.parent=card,.kind=OS_CONTROL_BUTTON,
                .flags=OS_CONTROL_SYMBOL|OS_CONTROL_BACKGROUND_ALPHA|OS_CONTROL_POINTER_PASSTHROUGH,
                .x=width>icon ? (int32_t)((width-icon)/2U) : 0,.y=(int32_t)FM4_SCALE(4U),
                .width=(int32_t)icon,.height=(int32_t)icon,.font_size=ui_text_size(),.icon_size=icon,
                .foreground=info->type==OS_FILE_TYPE_DIRECTORY ? 0x005BC5EBU : OS_THEME_MUTED,
                .value=info->type==OS_FILE_TYPE_DIRECTORY ? OS_CONTROL_SYMBOL_FOLDER : OS_CONTROL_SYMBOL_FILE},NULL)) return false;
            char first[OS_FILE_NAME_MAX],second[OS_FILE_NAME_MAX];
            uint32_t available=width>12U ? width-12U : 0U;
            size_t bytes=strlen(info->name),offset=0;
            uint32_t line_width=0,font=ui_text_size();
            /* 换行按字符累计，不能为每个候选断点重新测量整个前缀。 */
            while (offset<bytes) {
                size_t used;uint32_t code=os_utf8_next(info->name+offset,bytes-offset,&used);
                size_t next=offset+(used?used:1U);
                uint32_t advance=os_control_text_cell_width(code,font);
                if (next>=sizeof(first) || advance>available-line_width) break;
                line_width+=advance;offset=next;
            }
            memcpy(first,info->name,offset);first[offset]=0;
            copy_text(second,sizeof(second),info->name+offset);fm4_fit_text(second,sizeof(second),available);
            const char *lines[]={first,second};
            for (uint32_t line=0;line<2U;++line) {
                if (line && !second[0]) break;
                uint32_t measured=fm4_measure(lines[line]);
                if (!fm4_content_text(nodes,text,count,text_size,card,width>measured ? (width-measured)/2U : 0U,
                    FM4_GRID_LABEL_TOP+line*ui_text_line_height(),width,lines[line],OS_THEME_TEXT)) return false;
            }
        }
    }
    return true;
}
