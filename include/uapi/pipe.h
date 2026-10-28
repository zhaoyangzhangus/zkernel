#pragma once

#include "abi.h"

/* PIPE_CREATE 标志刻意与 libc 的 O_* 取值相互独立。 */
#define OS_PIPE_FLAG_CLOEXEC  (1U << 0)
#define OS_PIPE_FLAG_NONBLOCK (1U << 1)
#define OS_PIPE_FLAG_MASK     (OS_PIPE_FLAG_CLOEXEC | OS_PIPE_FLAG_NONBLOCK)

/* 不超过此大小的写入保证以原子方式提交。 */
#define OS_PIPE_BUF            4096U
#define OS_PIPE_DEFAULT_SIZE   65536U

/* Result bits returned by WAIT_ONE/WAIT_MANY for a pipe endpoint. */
#define OS_PIPE_WAIT_READABLE  (1U << 0)
#define OS_PIPE_WAIT_HUP       (1U << 1)
#define OS_PIPE_WAIT_WRITABLE  (1U << 2)
#define OS_PIPE_WAIT_ERROR     (1U << 3)

typedef struct os_pipe_create {
    os_versioned_header_t hdr;
    uint32_t flags;
    uint32_t reserved;
    os_handle_t read_handle;
    os_handle_t write_handle;
} os_pipe_create_t;
