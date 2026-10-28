#pragma once
#include "abi.h"
/* 设置壁纸在资源线程完成解码、缩放及配置保存，失败保留当前桌面。 */
typedef struct os_desktop_wallpaper {
    os_versioned_header_t hdr;
    char path[256];
} os_desktop_wallpaper_t;
