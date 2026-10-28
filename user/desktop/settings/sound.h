#pragma once

static os_audio_settings_t g_sound;
static bool g_sound_valid;
static bool g_sound_error;

static void settings_sound_refresh(void) {
    g_sound=(os_audio_settings_t){.hdr={sizeof(g_sound),OS_SYSCALL_ABI_VERSION}};
    g_sound_valid=settings_syscall_one(OS_SYS_AUDIO_SETTINGS,(uintptr_t)&g_sound)==0;
}

static void settings_sound_button(uint32_t parent,const char *text,uint32_t id,bool selected) {
    settings_navigation(parent,text,id,selected);
    os_control_node_t *node=g_nodes+g_node_count-1U;
    /* 设备项保留列表选择语义；普通按钮不能携带列表的选中标记。 */
    if (id>1103U) {
        node->kind=OS_CONTROL_BUTTON;
        node->flags&=~OS_CONTROL_CHECKED;
        if (selected) node->background=OS_THEME_SELECTED;
    }
}

static void settings_sound_content(uint32_t parent) {
    uint32_t total=16U*ui_text_line_height()+settings_scale(76U);
    uint32_t reserved=user_caption_height()+settings_scale(36U);
    uint32_t visible=g_window.height>reserved?g_window.height-reserved:0U;
    g_scroll_limit=total>visible?total-visible:0U;
    if (g_scroll>g_scroll_limit) g_scroll=g_scroll_limit;
    g_nodes[parent].scroll_y=(int32_t)g_scroll;
    if (!g_sound_valid) {
        settings_label(parent,"声音服务不可用",SETTINGS_ERROR_TEXT);
        return;
    }
    uint32_t card=settings_card(parent,10U);
    settings_label(card,"声音输出",SETTINGS_SECTION_TEXT);
    settings_sound_button(card,"自动选择输出设备",1100U,g_sound.backend==OS_AUDIO_BACKEND_AUTO);
    if (g_sound.available&OS_AUDIO_DEVICE_HDA)
        settings_sound_button(card,"NVIDIA 5070 · HDMI / DisplayPort",1101U,g_sound.backend==OS_AUDIO_BACKEND_HDA);
    if (g_sound.available&OS_AUDIO_DEVICE_USB)
        settings_sound_button(card,"主板 USB 声卡 · 耳机 / 音箱",1102U,g_sound.backend==OS_AUDIO_BACKEND_USB);
    if (g_sound.available&OS_AUDIO_DEVICE_USB_HEADSET)
        settings_sound_button(card,"USB 耳机 · EarPods",1103U,g_sound.backend==OS_AUDIO_BACKEND_USB_HEADSET);
    if (!g_sound.available) settings_label(card,"未检测到可用声卡",SETTINGS_HINT_TEXT);
    settings_label(card,"Chrome 和系统应用跟随此设置",SETTINGS_HINT_TEXT);
    card=settings_card(parent,6U);
    char text[48]={0};append_decimal(text,sizeof(text),g_sound.volume);
    append_text(text,sizeof(text)," %");
    settings_value(card,"输出音量  ",text);
    uint32_t row=settings_box(card,OS_CONTROL_CONTAINER,OS_CONTROL_ROW,
        OS_CONTROL_FILL,(int32_t)(ui_text_line_height()+settings_scale(16U)),SETTINGS_CARD_BACKGROUND);
    g_nodes[row].gap=settings_scale(12U);
    settings_sound_button(row,"−",1110U,false);
    g_nodes[g_node_count-1U].width=(int32_t)settings_scale(64U);
    settings_sound_button(row,"+",1111U,false);
    g_nodes[g_node_count-1U].width=(int32_t)settings_scale(64U);
    settings_sound_button(card,g_sound.muted?"取消静音":"静音",1112U,g_sound.muted!=0U);
    if (g_sound_error) settings_label(card,"未能保存声音设置，请重试",SETTINGS_ERROR_TEXT);
}

static bool settings_sound_click(uint32_t id) {
    if (id<1100U || id>1112U) return false;
    if (!g_sound_valid) return true;
    os_audio_settings_t next=g_sound;
    next.operation=OS_AUDIO_SETTINGS_WRITE;
    if (id<=1103U) next.backend=id-1100U;
    else if (id==1110U) next.volume=next.volume>=10U?next.volume-10U:0U;
    else if (id==1111U) next.volume=next.volume<=90U?next.volume+10U:100U;
    else if (id==1112U) next.muted=!next.muted;
    else return false;
    g_sound_error=settings_syscall_one(OS_SYS_AUDIO_SETTINGS,(uintptr_t)&next)!=0;
    settings_sound_refresh();
    return true;
}
