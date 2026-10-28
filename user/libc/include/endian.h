#pragma once

#include <stdint.h>

/* System libc 在受支持的 x86_64 目标上采用小端序。提供标准字节序转换宏，
 * 供可移植 POSIX 代码使用，不依赖宿主 libc 头文件。 */
#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN 1234
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN 4321
#endif
#ifndef __PDP_ENDIAN
#define __PDP_ENDIAN 3412
#endif
#ifndef __BYTE_ORDER
#define __BYTE_ORDER __LITTLE_ENDIAN
#endif
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN __BIG_ENDIAN
#endif
#ifndef PDP_ENDIAN
#define PDP_ENDIAN __PDP_ENDIAN
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER __BYTE_ORDER
#endif

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LIBC_ENDIAN_LITTLE 1
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define LIBC_ENDIAN_BIG 1
#else
#error "Unsupported System byte order"
#endif

#ifndef htobe16
#if defined(LIBC_ENDIAN_LITTLE)
#define htobe16(value) __builtin_bswap16((uint16_t)(value))
#define htobe32(value) __builtin_bswap32((uint32_t)(value))
#define htobe64(value) __builtin_bswap64((uint64_t)(value))
#define be16toh(value) __builtin_bswap16((uint16_t)(value))
#define be32toh(value) __builtin_bswap32((uint32_t)(value))
#define be64toh(value) __builtin_bswap64((uint64_t)(value))
#define htole16(value) ((uint16_t)(value))
#define htole32(value) ((uint32_t)(value))
#define htole64(value) ((uint64_t)(value))
#define le16toh(value) ((uint16_t)(value))
#define le32toh(value) ((uint32_t)(value))
#define le64toh(value) ((uint64_t)(value))
#else
#define htobe16(value) ((uint16_t)(value))
#define htobe32(value) ((uint32_t)(value))
#define htobe64(value) ((uint64_t)(value))
#define be16toh(value) ((uint16_t)(value))
#define be32toh(value) ((uint32_t)(value))
#define be64toh(value) ((uint64_t)(value))
#define htole16(value) __builtin_bswap16((uint16_t)(value))
#define htole32(value) __builtin_bswap32((uint32_t)(value))
#define htole64(value) __builtin_bswap64((uint64_t)(value))
#define le16toh(value) __builtin_bswap16((uint16_t)(value))
#define le32toh(value) __builtin_bswap32((uint32_t)(value))
#define le64toh(value) __builtin_bswap64((uint64_t)(value))
#endif
#endif
