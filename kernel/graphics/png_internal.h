#pragma once

#include <stdbool.h>
#include <uapi/abi.h>

/* PNG 容器模块和像素解码模块共用的私有接口。 */
#define PNG_SIGNATURE_SIZE 8U

typedef struct png_image_info {
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint8_t bit_depth;
    uint8_t color_type;
    bool has_ihdr;
    bool has_idat;
    bool has_iend;
    uint8_t palette[256U * 3U];
    uint8_t palette_alpha[256U];
    uint32_t palette_entries;
    bool has_transparent_gray;
    uint16_t transparent_gray;
    bool has_transparent_rgb;
    uint16_t transparent_red;
    uint16_t transparent_green;
    uint16_t transparent_blue;
    size_t idat_size;
} png_image_info_t;

static inline uint32_t png_read_u32_be(const uint8_t *data) {
    return ((uint32_t)data[0] << 24U) |
           ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) |
           (uint32_t)data[3];
}

bool png_parse_chunks(const uint8_t *encoded, size_t encoded_size,
                      png_image_info_t *info, uint8_t *idat);
