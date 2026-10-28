#pragma once

#include "abi.h"

/* 限制用户态图像传输大小；内核解码器使用相同上限。 */
#define OS_IMAGE_MAX_ENCODED_BYTES (16ULL * 1024ULL * 1024ULL)
#define OS_IMAGE_MAX_PIXEL_BYTES   (16ULL * 1024ULL * 1024ULL)

enum os_image_pixel_format {
    OS_IMAGE_PIXEL_RGBA8888 = 1U,
};

/* IMAGE_INFO 验证 PNG，并返回 RGBA 输出的大小。 */
typedef struct os_image_info {
    os_versioned_header_t hdr;
    uint64_t encoded;
    uint64_t encoded_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t pixel_bytes;
} os_image_info_t;

/* IMAGE_DECODE 将解码后的 RGBA 像素复制到调用方拥有的缓冲区。 */
typedef struct os_image_decode {
    os_versioned_header_t hdr;
    uint64_t encoded;
    uint64_t encoded_size;
    uint64_t pixels;
    uint64_t capacity;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t bytes_written;
} os_image_decode_t;
