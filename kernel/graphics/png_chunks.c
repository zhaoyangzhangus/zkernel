#include "png_internal.h"
#include <kernel/crc32.h>

/* REFACTOR_P7A_PNG_CHUNK_OWNER：PNG 容器和数据块校验、IDAT 收集逻辑与 DEFLATE
 * 解压及像素转换单元分开。 */

static uint16_t png_read_u16_be(const uint8_t *data) {
    return (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
}

static bool png_type_is(const uint8_t *type, char a, char b, char c, char d) {
    return type[0] == (uint8_t)a && type[1] == (uint8_t)b &&
           type[2] == (uint8_t)c && type[3] == (uint8_t)d;
}

static uint32_t png_crc32(const uint8_t *type, const uint8_t *data,
                          size_t length) {
    uint32_t crc = crc32_ieee_update(UINT32_MAX, type, 4U);
    return ~crc32_ieee_update(crc, data, length);
}

static bool png_signature_valid(const uint8_t *data, size_t size) {
    static const uint8_t signature[PNG_SIGNATURE_SIZE] = {
        0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU,
    };

    if (size < sizeof(signature)) return false;
    for (uint32_t index = 0U; index < sizeof(signature); ++index) {
        if (data[index] != signature[index]) return false;
    }
    return true;
}

bool png_parse_chunks(const uint8_t *encoded, size_t encoded_size,
                      png_image_info_t *info, uint8_t *idat) {
    size_t position = PNG_SIGNATURE_SIZE;
    size_t idat_position = 0U;

    if (!png_signature_valid(encoded, encoded_size) || info == 0) {
        return false;
    }
    for (uint32_t index = 0U; index < 256U; ++index) {
        info->palette_alpha[index] = 255U;
    }

    while (position < encoded_size) {
        uint32_t length;
        const uint8_t *type;
        const uint8_t *data;
        uint32_t expected_crc;
        uint32_t actual_crc;

        if (encoded_size - position < 12U) return false;
        length = png_read_u32_be(encoded + position);
        if ((uint64_t)length > encoded_size - position - 12U) return false;
        type = encoded + position + 4U;
        data = encoded + position + 8U;
        expected_crc = png_read_u32_be(data + length);
        actual_crc = png_crc32(type, data, length);
        if (expected_crc != actual_crc) return false;

        if (png_type_is(type, 'I', 'H', 'D', 'R')) {
            if (info->has_ihdr || length != 13U) return false;
            info->width = png_read_u32_be(data);
            info->height = png_read_u32_be(data + 4U);
            info->bit_depth = data[8];
            info->color_type = data[9];
            if (info->width == 0U || info->height == 0U ||
                info->bit_depth != 8U || data[10] != 0U || data[11] != 0U ||
                data[12] != 0U) {
                return false;
            }
            if (info->color_type == 0U || info->color_type == 4U) {
                info->channels = info->color_type == 0U ? 1U : 2U;
            } else if (info->color_type == 2U || info->color_type == 3U) {
                info->channels = info->color_type == 2U ? 3U : 1U;
            } else if (info->color_type == 6U) {
                info->channels = 4U;
            } else {
                return false;
            }
            info->has_ihdr = true;
        } else if (png_type_is(type, 'P', 'L', 'T', 'E')) {
            if (!info->has_ihdr || info->color_type != 3U ||
                length == 0U || length > sizeof(info->palette) ||
                (length % 3U) != 0U) {
                return false;
            }
            for (uint32_t index = 0U; index < length; ++index) {
                info->palette[index] = data[index];
            }
            info->palette_entries = length / 3U;
        } else if (png_type_is(type, 't', 'R', 'N', 'S')) {
            if (!info->has_ihdr) return false;
            if (info->color_type == 3U) {
                if (length > 256U) return false;
                for (uint32_t index = 0U; index < length; ++index) {
                    info->palette_alpha[index] = data[index];
                }
            } else if (info->color_type == 0U && length == 2U) {
                info->has_transparent_gray = true;
                info->transparent_gray = png_read_u16_be(data);
            } else if (info->color_type == 2U && length == 6U) {
                info->has_transparent_rgb = true;
                info->transparent_red = png_read_u16_be(data);
                info->transparent_green = png_read_u16_be(data + 2U);
                info->transparent_blue = png_read_u16_be(data + 4U);
            } else {
                return false;
            }
        } else if (png_type_is(type, 'I', 'D', 'A', 'T')) {
            if (!info->has_ihdr || length == 0U ||
                info->idat_size > SIZE_MAX - length) {
                return false;
            }
            if (idat != 0) {
                for (uint32_t index = 0U; index < length; ++index) {
                    idat[idat_position + index] = data[index];
                }
                idat_position += length;
            }
            info->idat_size += length;
            info->has_idat = true;
        } else if (png_type_is(type, 'I', 'E', 'N', 'D')) {
            if (length != 0U || !info->has_ihdr || !info->has_idat) {
                return false;
            }
            info->has_iend = true;
            position += (size_t)length + 12U;
            break;
        }

        position += (size_t)length + 12U;
    }

    return info->has_ihdr && info->has_idat && info->has_iend &&
           (info->color_type != 3U || info->palette_entries != 0U) &&
           (idat == 0 || idat_position == info->idat_size);
}
