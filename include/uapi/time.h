#pragma once

#include "abi.h"

/* 由内核和 freestanding libc 共用的稳定时钟标识。 */
#define OS_CLOCK_REALTIME  0U
#define OS_CLOCK_MONOTONIC 1U

typedef struct os_clock_set {
    os_versioned_header_t hdr;
    uint32_t clock_id;
    uint32_t reserved;
    os_timespec_t value;
} os_clock_set_t;
