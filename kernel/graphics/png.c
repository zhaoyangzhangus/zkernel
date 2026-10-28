#include <kernel/kmem.h>

#include <stdint.h>
#include <stdbool.h>

#include "internal.h"
#include "png_internal.h"

/* PNG decoding is shared by the cold desktop-asset worker and bounded user
 * image requests; both paths use the same 16 MiB output limit. */
#define PNG_MAX_OUTPUT_BYTES     (16ULL * 1024ULL * 1024ULL)
#define PNG_HUFFMAN_MAX_NODES    640U
#define PNG_MAX_CODE_LENGTH      15U

typedef struct png_bit_reader {
    const uint8_t *data;
    size_t size;
    size_t bit_position;
} png_bit_reader_t;

typedef struct png_huffman_node {
    int16_t child[2];
    int16_t symbol;
} png_huffman_node_t;

typedef struct png_huffman {
    png_huffman_node_t nodes[PNG_HUFFMAN_MAX_NODES];
    uint16_t node_count;
} png_huffman_t;

static const uint32_t png_length_base[29] = {
    3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U,
    11U, 13U, 15U, 17U, 19U, 23U, 27U, 31U,
    35U, 43U, 51U, 59U, 67U, 83U, 99U, 115U,
    131U, 163U, 195U, 227U, 258U,
};

static const uint8_t png_length_extra[29] = {
    0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
    1U, 1U, 1U, 1U, 2U, 2U, 2U, 2U,
    3U, 3U, 3U, 3U, 4U, 4U, 4U, 4U,
    5U, 5U, 5U, 5U, 0U,
};

static const uint32_t png_distance_base[30] = {
    1U, 2U, 3U, 4U, 5U, 7U, 9U, 13U,
    17U, 25U, 33U, 49U, 65U, 97U, 129U, 193U,
    257U, 385U, 513U, 769U, 1025U, 1537U, 2049U, 3073U,
    4097U, 6145U, 8193U, 12289U, 16385U, 24577U,
};

static const uint8_t png_distance_extra[30] = {
    0U, 0U, 0U, 0U, 1U, 1U, 2U, 2U,
    3U, 3U, 4U, 4U, 5U, 5U, 6U, 6U,
    7U, 7U, 8U, 8U, 9U, 9U, 10U, 10U,
    11U, 11U, 12U, 12U, 13U, 13U,
};

static const uint8_t png_code_length_order[19] = {
    16U, 17U, 18U, 0U, 8U, 7U, 9U, 6U, 10U, 5U,
    11U, 4U, 12U, 3U, 13U, 2U, 14U, 1U, 15U,
};

static bool png_read_bits(png_bit_reader_t *reader, uint32_t count,
                          uint32_t *value) {
    uint32_t result = 0U;
    size_t total_bits;

    if (reader == 0 || value == 0 || count > 24U ||
        reader->size > SIZE_MAX / 8U) {
        return false;
    }
    total_bits = reader->size * 8U;
    if (reader->bit_position > total_bits ||
        count > total_bits - reader->bit_position) return false;
    for (uint32_t bit = 0U; bit < count; ++bit) {
        size_t byte_position = reader->bit_position >> 3U;
        uint32_t bit_position = (uint32_t)(reader->bit_position & 7U);
        result |= ((reader->data[byte_position] >> bit_position) & 1U) << bit;
        ++reader->bit_position;
    }
    *value = result;
    return true;
}

static void png_align_bits(png_bit_reader_t *reader) {
    reader->bit_position = (reader->bit_position + 7U) & ~(size_t)7U;
}

static uint16_t png_reverse_bits(uint16_t value, uint32_t count) {
    uint16_t result = 0U;

    for (uint32_t index = 0U; index < count; ++index) {
        result = (uint16_t)((result << 1U) | (value & 1U));
        value >>= 1U;
    }
    return result;
}

static void png_huffman_reset(png_huffman_t *table) {
    table->node_count = 1U;
    table->nodes[0].child[0] = -1;
    table->nodes[0].child[1] = -1;
    table->nodes[0].symbol = -1;
}

static bool png_huffman_build(png_huffman_t *table,
                              const uint8_t *lengths, uint32_t count) {
    uint16_t counts[PNG_MAX_CODE_LENGTH + 1U] = {0};
    uint16_t next_code[PNG_MAX_CODE_LENGTH + 1U] = {0};
    uint32_t code = 0U;

    if (table == 0 || lengths == 0 || count == 0U || count > 288U) {
        return false;
    }
    png_huffman_reset(table);
    for (uint32_t symbol = 0U; symbol < count; ++symbol) {
        if (lengths[symbol] > PNG_MAX_CODE_LENGTH) return false;
        if (lengths[symbol] != 0U) ++counts[lengths[symbol]];
    }
    for (uint32_t bits = 1U; bits <= PNG_MAX_CODE_LENGTH; ++bits) {
        code = (code + counts[bits - 1U]) << 1U;
        if (code + counts[bits] > (1U << bits)) return false;
        next_code[bits] = (uint16_t)code;
    }

    for (uint32_t symbol = 0U; symbol < count; ++symbol) {
        uint32_t length = lengths[symbol];
        uint16_t reversed;
        uint16_t node = 0U;

        if (length == 0U) continue;
        reversed = png_reverse_bits(next_code[length]++, length);
        for (uint32_t bit = 0U; bit < length; ++bit) {
            uint32_t branch = (reversed >> bit) & 1U;
            int16_t child = table->nodes[node].child[branch];
            if (table->nodes[node].symbol >= 0) return false;
            if (child < 0) {
                if (table->node_count >= PNG_HUFFMAN_MAX_NODES) return false;
                child = (int16_t)table->node_count++;
                table->nodes[child].child[0] = -1;
                table->nodes[child].child[1] = -1;
                table->nodes[child].symbol = -1;
                table->nodes[node].child[branch] = child;
            }
            node = (uint16_t)child;
        }
        if (table->nodes[node].symbol >= 0) return false;
        if (table->nodes[node].child[0] >= 0 ||
            table->nodes[node].child[1] >= 0) return false;
        table->nodes[node].symbol = (int16_t)symbol;
    }
    return true;
}

static bool png_huffman_decode(png_bit_reader_t *reader,
                               const png_huffman_t *table,
                               uint32_t *symbol) {
    int16_t node = 0;

    if (reader == 0 || table == 0 || symbol == 0) return false;
    for (uint32_t depth = 0U; depth < PNG_MAX_CODE_LENGTH; ++depth) {
        uint32_t bit;
        if (!png_read_bits(reader, 1U, &bit)) return false;
        node = table->nodes[node].child[bit];
        if (node < 0 || node >= (int16_t)table->node_count) return false;
        if (table->nodes[node].symbol >= 0) {
            *symbol = (uint32_t)table->nodes[node].symbol;
            return true;
        }
    }
    return false;
}

static bool png_build_fixed_tables(png_huffman_t *literal_length,
                                   png_huffman_t *distance) {
    uint8_t literal_lengths[288] = {0};
    uint8_t distance_lengths[32] = {0};

    for (uint32_t symbol = 0U; symbol <= 143U; ++symbol) {
        literal_lengths[symbol] = 8U;
    }
    for (uint32_t symbol = 144U; symbol <= 255U; ++symbol) {
        literal_lengths[symbol] = 9U;
    }
    for (uint32_t symbol = 256U; symbol <= 279U; ++symbol) {
        literal_lengths[symbol] = 7U;
    }
    for (uint32_t symbol = 280U; symbol < 288U; ++symbol) {
        literal_lengths[symbol] = 8U;
    }
    for (uint32_t symbol = 0U; symbol < 32U; ++symbol) {
        distance_lengths[symbol] = 5U;
    }
    return png_huffman_build(literal_length, literal_lengths, 288U) &&
           png_huffman_build(distance, distance_lengths, 32U);
}

static bool png_build_dynamic_tables(png_bit_reader_t *reader,
                                     png_huffman_t *literal_length,
                                     png_huffman_t *distance) {
    uint32_t value;
    uint32_t literal_count;
    uint32_t distance_count;
    uint32_t code_length_count;
    uint8_t code_length_lengths[19] = {0};
    uint8_t lengths[288U + 32U] = {0};
    png_huffman_t code_length;
    uint32_t index = 0U;

    if (!png_read_bits(reader, 5U, &value)) return false;
    literal_count = value + 257U;
    if (!png_read_bits(reader, 5U, &value)) return false;
    distance_count = value + 1U;
    if (!png_read_bits(reader, 4U, &value)) return false;
    code_length_count = value + 4U;
    if (literal_count > 288U || distance_count > 32U) return false;

    for (uint32_t order = 0U; order < code_length_count; ++order) {
        if (!png_read_bits(reader, 3U, &value)) return false;
        code_length_lengths[png_code_length_order[order]] = (uint8_t)value;
    }
    if (!png_huffman_build(&code_length, code_length_lengths, 19U)) {
        return false;
    }

    while (index < literal_count + distance_count) {
        uint32_t symbol;
        if (!png_huffman_decode(reader, &code_length, &symbol)) return false;
        if (symbol <= 15U) {
            lengths[index++] = (uint8_t)symbol;
        } else if (symbol == 16U) {
            uint32_t repeat;
            uint8_t previous;
            if (index == 0U || !png_read_bits(reader, 2U, &value)) {
                return false;
            }
            repeat = value + 3U;
            previous = lengths[index - 1U];
            if (repeat > literal_count + distance_count - index) return false;
            while (repeat-- != 0U) lengths[index++] = previous;
        } else if (symbol == 17U || symbol == 18U) {
            uint32_t repeat;
            uint32_t extra_bits = symbol == 17U ? 3U : 7U;
            uint32_t base = symbol == 17U ? 3U : 11U;
            if (!png_read_bits(reader, extra_bits, &value)) return false;
            repeat = value + base;
            if (repeat > literal_count + distance_count - index) return false;
            while (repeat-- != 0U) lengths[index++] = 0U;
        } else {
            return false;
        }
    }

    return png_huffman_build(literal_length, lengths, literal_count) &&
           png_huffman_build(distance, lengths + literal_count,
                             distance_count);
}

static bool png_inflate_huffman_block(png_bit_reader_t *reader,
                                      const png_huffman_t *literal_length,
                                      const png_huffman_t *distance,
                                      uint8_t *output, size_t capacity,
                                      size_t *output_size) {
    while (true) {
        uint32_t symbol;
        if (!png_huffman_decode(reader, literal_length, &symbol)) return false;
        if (symbol < 256U) {
            if (*output_size >= capacity) return false;
            output[(*output_size)++] = (uint8_t)symbol;
        } else if (symbol == 256U) {
            return true;
        } else if (symbol >= 257U && symbol <= 285U) {
            uint32_t length_index = symbol - 257U;
            uint32_t length = png_length_base[length_index];
            uint32_t distance_symbol;
            uint32_t distance_value;
            uint32_t distance_length;

            if (png_length_extra[length_index] != 0U &&
                !png_read_bits(reader, png_length_extra[length_index],
                               &distance_value)) {
                return false;
            }
            if (png_length_extra[length_index] != 0U) {
                length += distance_value;
            }
            if (!png_huffman_decode(reader, distance, &distance_symbol) ||
                distance_symbol >= 30U) {
                return false;
            }
            distance_length = png_distance_base[distance_symbol];
            if (png_distance_extra[distance_symbol] != 0U &&
                !png_read_bits(reader, png_distance_extra[distance_symbol],
                               &distance_value)) {
                return false;
            }
            if (png_distance_extra[distance_symbol] != 0U) {
                distance_length += distance_value;
            }
            if (distance_length > *output_size ||
                length > capacity - *output_size) {
                return false;
            }
            for (uint32_t index = 0U; index < length; ++index) {
                output[*output_size] =
                    output[*output_size - distance_length];
                ++*output_size;
            }
        } else {
            return false;
        }
    }
}

static uint32_t png_adler32(const uint8_t *data, size_t size) {
    uint32_t low = 1U;
    uint32_t high = 0U;

    for (size_t index = 0U; index < size; ++index) {
        low += data[index];
        if (low >= 65521U) low -= 65521U;
        high += low;
        if (high >= 65521U) high -= 65521U;
    }
    return (high << 16U) | low;
}

static bool png_inflate_zlib(const uint8_t *encoded, size_t encoded_size,
                             uint8_t *output, size_t output_capacity) {
    png_bit_reader_t reader;
    png_huffman_t literal_length;
    png_huffman_t distance;
    size_t output_size = 0U;
    uint32_t header;
    bool final = false;

    if (encoded == 0 || output == 0 || encoded_size < 6U) return false;
    header = ((uint32_t)encoded[0] << 8U) | encoded[1];
    if ((encoded[0] & 0x0FU) != 8U || (header % 31U) != 0U ||
        (encoded[1] & 0x20U) != 0U) {
        return false;
    }
    reader.data = encoded;
    /* Keep the Adler-32 bytes addressable for the final checksum check. */
    reader.size = encoded_size;
    reader.bit_position = 16U;

    while (!final) {
        uint32_t block_final;
        uint32_t block_type;
        if (!png_read_bits(&reader, 1U, &block_final) ||
            !png_read_bits(&reader, 2U, &block_type)) {
            return false;
        }
        final = block_final != 0U;
        if (block_type == 0U) {
            uint32_t length;
            uint32_t inverse_length;
            size_t byte_position;

            png_align_bits(&reader);
            byte_position = reader.bit_position >> 3U;
            if (byte_position > reader.size || reader.size - byte_position < 4U) {
                return false;
            }
            length = (uint32_t)reader.data[byte_position] |
                     ((uint32_t)reader.data[byte_position + 1U] << 8U);
            inverse_length = (uint32_t)reader.data[byte_position + 2U] |
                             ((uint32_t)reader.data[byte_position + 3U] << 8U);
            if ((length ^ 0xFFFFU) != inverse_length ||
                length > output_capacity - output_size ||
                length > reader.size - byte_position - 4U) {
                return false;
            }
            for (uint32_t index = 0U; index < length; ++index) {
                output[output_size++] =
                    reader.data[byte_position + 4U + index];
            }
            reader.bit_position = (byte_position + 4U + length) * 8U;
        } else if (block_type == 1U || block_type == 2U) {
            bool tables_ok;
            if (block_type == 1U) {
                tables_ok = png_build_fixed_tables(&literal_length, &distance);
            } else {
                tables_ok = png_build_dynamic_tables(&reader,
                                                      &literal_length,
                                                      &distance);
            }
            if (!tables_ok ||
                !png_inflate_huffman_block(&reader, &literal_length, &distance,
                                           output, output_capacity,
                                           &output_size)) {
                return false;
            }
        } else {
            return false;
        }
    }

    png_align_bits(&reader);
    if ((reader.bit_position >> 3U) > encoded_size - 4U ||
        encoded_size - (reader.bit_position >> 3U) < 4U ||
        output_size != output_capacity ||
        png_adler32(output, output_size) !=
            png_read_u32_be(reader.data + (reader.bit_position >> 3U))) {
        return false;
    }
    return true;
}

static uint8_t png_paeth(uint8_t left, uint8_t up, uint8_t up_left) {
    int32_t estimate = (int32_t)left + (int32_t)up - (int32_t)up_left;
    int32_t left_distance = estimate - (int32_t)left;
    int32_t up_distance = estimate - (int32_t)up;
    int32_t up_left_distance = estimate - (int32_t)up_left;

    if (left_distance < 0) left_distance = -left_distance;
    if (up_distance < 0) up_distance = -up_distance;
    if (up_left_distance < 0) up_left_distance = -up_left_distance;
    if (left_distance <= up_distance && left_distance <= up_left_distance) {
        return left;
    }
    return up_distance <= up_left_distance ? up : up_left;
}

static bool png_unfilter(uint8_t *scanlines, uint32_t height,
                         size_t row_bytes, uint32_t bytes_per_pixel) {
    size_t row_stride = row_bytes + 1U;

    for (uint32_t row = 0U; row < height; ++row) {
        uint8_t *current = scanlines + (size_t)row * row_stride + 1U;
        const uint8_t *previous = row == 0U ? 0 :
            scanlines + (size_t)(row - 1U) * row_stride + 1U;
        uint8_t filter = scanlines[(size_t)row * row_stride];

        /* 过滤类型整行不变；无过滤行不扫描，首行也不反复判断上一行是否存在。 */
        if (filter == 0U || (filter == 2U && !previous)) continue;
        size_t prefix = row_bytes < bytes_per_pixel ? row_bytes : bytes_per_pixel;
        if (filter == 1U || (filter == 4U && !previous)) {
            for (size_t column = prefix; column < row_bytes; ++column)
                current[column] = (uint8_t)(current[column] + current[column-bytes_per_pixel]);
        } else if (filter == 2U) {
            for (size_t column = 0U; column < row_bytes; ++column)
                current[column] = (uint8_t)(current[column] + previous[column]);
        } else if (filter == 3U) {
            if (previous) {
                for (size_t column = 0U; column < prefix; ++column)
                    current[column] = (uint8_t)(current[column] + previous[column]/2U);
                for (size_t column = prefix; column < row_bytes; ++column)
                    current[column] = (uint8_t)(current[column] +
                        ((uint32_t)current[column-bytes_per_pixel]+previous[column])/2U);
            } else {
                for (size_t column = prefix; column < row_bytes; ++column)
                    current[column] = (uint8_t)(current[column] + current[column-bytes_per_pixel]/2U);
            }
        } else if (filter == 4U) {
            for (size_t column = 0U; column < prefix; ++column)
                current[column] = (uint8_t)(current[column] + previous[column]);
            for (size_t column = prefix; column < row_bytes; ++column)
                current[column] = (uint8_t)(current[column] + png_paeth(
                    current[column-bytes_per_pixel],previous[column],previous[column-bytes_per_pixel]));
        } else return false;
    }
    return true;
}

static void png_write_pixel(const png_image_info_t *info,
                            const uint8_t *source, uint8_t *destination) {
    uint8_t alpha = 255U;

    if (info->color_type == 0U) {
        destination[0] = source[0];
        destination[1] = source[0];
        destination[2] = source[0];
        if (info->has_transparent_gray &&
            info->transparent_gray == source[0]) {
            alpha = 0U;
        }
    } else if (info->color_type == 2U) {
        destination[0] = source[0];
        destination[1] = source[1];
        destination[2] = source[2];
        if (info->has_transparent_rgb &&
            info->transparent_red == source[0] &&
            info->transparent_green == source[1] &&
            info->transparent_blue == source[2]) {
            alpha = 0U;
        }
    } else if (info->color_type == 3U) {
        uint32_t palette_index = source[0];
        if (palette_index >= info->palette_entries) {
            destination[0] = 0U;
            destination[1] = 0U;
            destination[2] = 0U;
            alpha = 0U;
        } else {
            destination[0] = info->palette[palette_index * 3U];
            destination[1] = info->palette[palette_index * 3U + 1U];
            destination[2] = info->palette[palette_index * 3U + 2U];
            alpha = info->palette_alpha[palette_index];
        }
    } else if (info->color_type == 4U) {
        destination[0] = source[0];
        destination[1] = source[0];
        destination[2] = source[0];
        alpha = source[1];
    } else {
        destination[0] = source[0];
        destination[1] = source[1];
        destination[2] = source[2];
        alpha = source[3];
    }
    destination[3] = alpha;
}

bool desktop_png_decode(const uint8_t *encoded, size_t encoded_size,
                        desktop_asset_image_t *asset) {
    png_image_info_t info = {0};
    png_image_info_t verified = {0};
    uint8_t *idat = 0;
    uint8_t *scanlines = 0;
    uint8_t *pixels = 0;
    uint64_t row_bytes_u64;
    uint64_t scanline_bytes_u64;
    uint64_t pixel_bytes_u64;
    size_t row_bytes;
    size_t scanline_bytes;
    size_t pixel_bytes;

    if (encoded == 0 || asset == 0 ||
        !png_parse_chunks(encoded, encoded_size, &info, 0)) {
        return false;
    }
    if (info.idat_size == 0U || info.idat_size > encoded_size ||
        info.channels == 0U) {
        return false;
    }

    row_bytes_u64 = (uint64_t)info.width * info.channels;
    scanline_bytes_u64 = (row_bytes_u64 + 1U) * info.height;
    pixel_bytes_u64 = (uint64_t)info.width * info.height * 4U;
    if (row_bytes_u64 == 0U || scanline_bytes_u64 > SIZE_MAX ||
        pixel_bytes_u64 > SIZE_MAX || scanline_bytes_u64 > PNG_MAX_OUTPUT_BYTES ||
        pixel_bytes_u64 > PNG_MAX_OUTPUT_BYTES) {
        return false;
    }
    row_bytes = (size_t)row_bytes_u64;
    scanline_bytes = (size_t)scanline_bytes_u64;
    pixel_bytes = (size_t)pixel_bytes_u64;

    idat = (uint8_t *)kmalloc(info.idat_size, 0U);
    scanlines = (uint8_t *)kmalloc(scanline_bytes, 0U);
    pixels = (uint8_t *)kmalloc(pixel_bytes, 0U);
    if (idat == 0 || scanlines == 0 || pixels == 0 ||
        !png_parse_chunks(encoded, encoded_size, &verified, idat) ||
        verified.idat_size != info.idat_size) {
        if (idat != 0) kfree(idat);
        if (scanlines != 0) kfree(scanlines);
        if (pixels != 0) kfree(pixels);
        return false;
    }
    if (!png_inflate_zlib(idat, info.idat_size, scanlines, scanline_bytes)) {
        kfree(idat);
        kfree(scanlines);
        kfree(pixels);
        return false;
    }
    if (!png_unfilter(scanlines, info.height, row_bytes, info.channels)) {
        kfree(idat);
        kfree(scanlines);
        kfree(pixels);
        return false;
    }

    for (uint32_t row = 0U; row < info.height; ++row) {
        const uint8_t *source = scanlines + (size_t)row * (row_bytes + 1U) + 1U;
        uint8_t *destination = pixels + (size_t)row * info.width * 4U;
        for (uint32_t column = 0U; column < info.width; ++column) {
            png_write_pixel(&info,
                            source + (size_t)column * info.channels,
                            destination + (size_t)column * 4U);
        }
    }

    kfree(idat);
    kfree(scanlines);
    asset->storage = pixels;
    asset->pixels = pixels;
    asset->width = info.width;
    asset->height = info.height;
    asset->stride = info.width * 4U;
    return true;
}
