#pragma once
#include <kernel/base.h>

/* 反射式 IEEE CRC32，不是 x86 crc32 操作码实现的 CRC32C。调用方从 UINT32_MAX 开始，
 * 所有片段处理完后再执行一次取反。 */
static inline uint32_t crc32_ieee_byte(uint32_t crc, uint8_t byte) {
    static const uint32_t table[16] = {
        0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
        0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
        0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
        0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU,
    };
    crc ^= byte;
    crc = (crc >> 4U) ^ table[crc & 15U];
    return (crc >> 4U) ^ table[crc & 15U];
}

static inline uint32_t crc32_ieee_update(uint32_t crc, const void *data,
                                        size_t length) {
    const uint8_t *bytes = data;
    for (size_t i = 0; i < length; ++i) crc = crc32_ieee_byte(crc, bytes[i]);
    return crc;
}

/* 文件系统记录会在校验范围内保存四字节校验和。计算时不得修改记录，也不得创建临时副本。 */
static inline uint32_t crc32_ieee_zeroed(const uint8_t *bytes, size_t length,
                                        size_t checksum_offset) {
    size_t prefix = checksum_offset < length ? checksum_offset : length;
    uint32_t crc = crc32_ieee_update(UINT32_MAX, bytes, prefix);
    size_t zeros = length - prefix;
    if (zeros > sizeof(uint32_t)) zeros = sizeof(uint32_t);
    for (size_t i = 0; i < zeros; ++i) crc = crc32_ieee_byte(crc, 0U);
    size_t suffix = prefix + zeros;
    if (suffix < length) {
        crc = crc32_ieee_update(crc, bytes + suffix, length - suffix);
    }
    return ~crc;
}
