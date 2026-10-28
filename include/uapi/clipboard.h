#pragma once
#include "abi.h"

#define OS_CLIPBOARD_MAX_BYTES (1024U*1024U)
/* UTF-8 文本不含结尾零字节。读取容量为零时仅查询长度。 */
typedef struct os_clipboard_text {
    os_versioned_header_t hdr;
    uint64_t data;
    uint64_t size;
} os_clipboard_text_t;
