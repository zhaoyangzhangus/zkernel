#pragma once

#include "abi.h"

/* 物理内存统计以基础页为单位。free_pages 包含分配器每 CPU 缓存中的页面，
 * 因为这些页面仍可立即使用。 */
typedef struct os_memory_info {
    os_versioned_header_t hdr;
    uint64_t page_size;
    uint64_t total_pages;
    uint64_t free_pages;
} os_memory_info_t;
