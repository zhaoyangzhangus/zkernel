#pragma once

/* 应用提交语义颜色，合成器在绘制时统一解析主题；图片和 Surface 保持原色。 */
#define OS_THEME_HEADER           0x00D3E3FDU
#define OS_THEME_HEADER_INACTIVE  0x00E8EAEDU
#define OS_THEME_SURFACE          0x00FFFFFFU
#define OS_THEME_TOOLBAR          0x00F1F3F4U
#define OS_THEME_FIELD            0x00EAF1FBU
#define OS_THEME_BORDER           0x00DADCE0U
#define OS_THEME_TEXT             0x00202124U
#define OS_THEME_MUTED            0x005F6368U
#define OS_THEME_ACCENT           0x001A73E8U
#define OS_THEME_SELECTED         0x00D3E3FDU
/* 文字选区必须区别于按钮按压和列表选中背景。 */
#define OS_THEME_TEXT_SELECTION   0x00A8C7FAU
#define OS_THEME_HOVER            0x00E8F0FEU
#define OS_THEME_ON_ACCENT        OS_THEME_SURFACE
#define OS_THEME_FOCUS            OS_THEME_ACCENT
#define OS_THEME_DANGER           0x00B3261EU
/* 模态层只拦截输入，不使底下的窗口变灰。 */
#define OS_THEME_SCRIM            0x00000000U
#define OS_THEME_RADIUS           8U


#include "abi.h"
#define OS_THEME_BLUE 0U
#define OS_THEME_PURPLE 1U
#define OS_THEME_GREEN 2U
#define OS_THEME_COUNT 3U
#define OS_THEME_BACKGROUND_COUNT 4U
#define OS_THEME_READ 0U
#define OS_THEME_WRITE 1U
typedef struct os_theme_settings {
    os_versioned_header_t hdr;
    uint32_t operation;
    uint32_t theme;
    uint32_t background;
    uint32_t reserved;
} os_theme_settings_t;

/* 快照编码同时携带配色和背景，一帧内保持一致。 */
static inline uint32_t os_theme_resolve(uint32_t color,uint32_t palette) {
    uint32_t theme=palette&255U,background=palette>>8U;
    uint32_t rgb=color&0xffffffU,alpha=color&0xff000000U;
    if (background==3U) {
        if (rgb==OS_THEME_SURFACE) rgb=0x202125U;
        else if (rgb==OS_THEME_TOOLBAR || rgb==OS_THEME_HEADER_INACTIVE) rgb=0x292B30U;
        else if (rgb==OS_THEME_BORDER) rgb=0x50535AU;
        else if (rgb==OS_THEME_TEXT) rgb=0xF1F3F5U;
        else if (rgb==OS_THEME_MUTED) rgb=0xBDC1C6U;
        else if (rgb==OS_THEME_HEADER || rgb==OS_THEME_SELECTED)
            rgb=theme==1U?0x49365CU:theme==2U?0x254A3AU:0x2C405CU;
        else if (rgb==OS_THEME_FIELD || rgb==OS_THEME_HOVER)
            rgb=theme==1U?0x382D45U:theme==2U?0x253C32U:0x293749U;
        else if (rgb==OS_THEME_ACCENT)
            rgb=theme==1U?0xCBA7EDU:theme==2U?0x81C995U:0x8AB4F8U;
        else if (rgb==OS_THEME_TEXT_SELECTION)
            rgb=theme==1U?0x604577U:theme==2U?0x34654DU:0x405B80U;
        return alpha|rgb;
    }
    if (background==1U) {
        if (rgb==OS_THEME_SURFACE) rgb=0xFFF8EFU;
        else if (rgb==OS_THEME_TOOLBAR) rgb=0xF4ECE1U;
        else if (rgb==OS_THEME_HEADER_INACTIVE) rgb=0xEEE5D8U;
        else if (rgb==OS_THEME_BORDER) rgb=0xDCD0BFU;
    } else if (background==2U) {
        if (rgb==OS_THEME_SURFACE) rgb=0xECEEF2U;
        else if (rgb==OS_THEME_TOOLBAR) rgb=0xDFE3EAU;
        else if (rgb==OS_THEME_HEADER_INACTIVE) rgb=0xD7DCE5U;
        else if (rgb==OS_THEME_BORDER) rgb=0xC2C9D4U;
    }
    if (theme==OS_THEME_PURPLE) {
        if (rgb==OS_THEME_HEADER) rgb=0xE9DDFBU;
        else if (rgb==OS_THEME_FIELD) rgb=0xF3ECFAU;
        else if (rgb==OS_THEME_ACCENT) rgb=0x7946BAU;
        else if (rgb==OS_THEME_TEXT_SELECTION) rgb=0xD3B7EFU;
        else if (rgb==OS_THEME_HOVER) rgb=0xF1E8FAU;
    } else if (theme==OS_THEME_GREEN) {
        if (rgb==OS_THEME_HEADER) rgb=0xCDEBDDU;
        else if (rgb==OS_THEME_FIELD) rgb=0xE8F5EFU;
        else if (rgb==OS_THEME_ACCENT) rgb=0x167349U;
        else if (rgb==OS_THEME_TEXT_SELECTION) rgb=0xA4D9BDU;
        else if (rgb==OS_THEME_HOVER) rgb=0xE5F3EBU;
    }
    return alpha|rgb;
}
