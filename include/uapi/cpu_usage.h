#pragma once
#include "abi.h"
/* 所有在线 CPU 的累计忙碌/空闲时间；使用两次快照的差值计算整机占用。 */
typedef struct os_cpu_usage {
    os_versioned_header_t hdr;
    uint64_t busy_ns,idle_ns;
    uint32_t cpu_count,reserved;
} os_cpu_usage_t;
