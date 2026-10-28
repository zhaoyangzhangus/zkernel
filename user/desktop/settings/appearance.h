#pragma once

static os_theme_settings_t g_appearance;
static bool g_appearance_valid,g_appearance_error;
static void settings_appearance_refresh(void) {
    g_appearance=(os_theme_settings_t){.hdr={sizeof(g_appearance),OS_SYSCALL_ABI_VERSION}};
    g_appearance_valid=settings_syscall_one(OS_SYS_THEME_SETTINGS,(uintptr_t)&g_appearance)==0;
}
static void settings_appearance_content(uint32_t parent) {
    uint32_t height=12U*ui_text_line_height()+settings_scale(200U);
    uint32_t reserved=user_caption_height()+settings_scale(36U);
    uint32_t visible=g_window.height>reserved?g_window.height-reserved:0U;
    g_scroll_limit=height>visible?height-visible:0U;
    if (g_scroll>g_scroll_limit) g_scroll=g_scroll_limit;
    g_nodes[parent].scroll_y=(int32_t)g_scroll;
    uint32_t card=settings_card(parent,12U);
    g_nodes[card].height=(int32_t)height;
    settings_label(card,"标题栏颜色",SETTINGS_SECTION_TEXT);
    if (!g_appearance_valid) {
        settings_label(card,"无法读取外观设置",SETTINGS_ERROR_TEXT);return;
    }
    const char *names[]={"蓝色（默认）","紫色","绿色"};
    for (uint32_t i=0;i<OS_THEME_COUNT;++i)
        settings_navigation(card,names[i],1200U+i,g_appearance.theme==i);
    settings_label(card,"背景色",SETTINGS_SECTION_TEXT);
    const char *backgrounds[]={"纯白","暖白","浅灰","深色"};
    for (uint32_t i=0;i<OS_THEME_BACKGROUND_COUNT;++i)
        settings_navigation(card,backgrounds[i],1300U+i,g_appearance.background==i);
    settings_label(card,"立即应用于系统标题栏、菜单和控件",SETTINGS_HINT_TEXT);
    settings_label(card,"图片和视频保持原色；重启后恢复默认外观",SETTINGS_HINT_TEXT);
    if (g_appearance_error) settings_label(card,"外观设置失败，请重试",SETTINGS_ERROR_TEXT);
}
static bool settings_appearance_click(uint32_t id) {
    bool accent=id>=1200U && id<1200U+OS_THEME_COUNT;
    bool background=id>=1300U && id<1300U+OS_THEME_BACKGROUND_COUNT;
    if (!accent && !background) return false;
    if (!g_appearance_valid) return true;
    os_theme_settings_t request={.hdr={sizeof(request),OS_SYSCALL_ABI_VERSION},
        .operation=OS_THEME_WRITE,.theme=accent?id-1200U:g_appearance.theme,
        .background=background?id-1300U:g_appearance.background};
    g_appearance_error=settings_syscall_one(OS_SYS_THEME_SETTINGS,(uintptr_t)&request)!=0;
    settings_appearance_refresh();return true;
}
