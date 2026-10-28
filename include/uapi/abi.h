#pragma once
#pragma once
#include <stdint.h>
#if !defined(__MINGW32__)
#include <stddef.h>
#else
/* freestanding MinGW 不应引入依赖 32 位宿主配置的 CRT stddef。 */
typedef __SIZE_TYPE__ size_t;
#endif

typedef uint64_t os_handle_t;
typedef int64_t os_status_t;

#define OS_INVALID_HANDLE ((os_handle_t)0)

/* 每个句柄各自拥有的标志。fork 时复制，exec 时使用。 */
#define OS_HANDLE_FLAG_CLOEXEC (1U << 0)

typedef struct os_versioned_header {
    uint32_t size;
    uint16_t version;
    uint16_t flags;
} os_versioned_header_t;

typedef struct os_timespec {
    int64_t seconds;
    int32_t nanoseconds;
    int32_t reserved;
} os_timespec_t;
