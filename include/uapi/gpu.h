#pragma once
#pragma once
#include "abi.h"

typedef uint64_t os_gpu_va_t;

typedef struct os_gpu_create_context {
    os_versioned_header_t hdr;
    os_handle_t device;
    uint64_t flags;
} os_gpu_create_context_t;

typedef struct os_gpu_alloc {
    os_versioned_header_t hdr;
    uint64_t size;
    uint32_t flags;
    uint32_t reserved;
} os_gpu_alloc_t;

enum os_gpu_map_flags {
    OS_GPU_MAP_FIXED = 1u << 0,
};

typedef struct os_gpu_map {
    os_versioned_header_t hdr;
    os_handle_t allocation;
    uint64_t address;
    uint64_t offset;
    uint64_t length;
    uint32_t prot;
    uint32_t flags;
} os_gpu_map_t;

typedef struct os_gpu_submit {
    os_versioned_header_t hdr;
    os_handle_t context;
    os_handle_t command_buffer;
    os_handle_t signal_fence;
    uint64_t signal_value;
    uint64_t command_offset;
    uint64_t command_length;
} os_gpu_submit_t;

/* GPU_SUBMIT 在 signal_fence 为无效句柄时创建 fence 并原位写回句柄。 */

enum os_gpu_device_state {
    OS_GPU_DEVICE_DISCOVERED,
    OS_GPU_DEVICE_ACTIVE,
    OS_GPU_DEVICE_SUSPENDED,
    OS_GPU_DEVICE_RECOVERING,
    OS_GPU_DEVICE_REMOVED,
    OS_GPU_DEVICE_FAILED,
};

/* 枚举已发现的 PCI 显示设备；不触发驱动初始化，也不暴露 BAR 地址。 */
typedef struct os_gpu_info {
    os_versioned_header_t hdr;
    uint32_t index,count;
    uint32_t vendor_id,device_id;
    uint32_t segment,bus,slot,function;
    uint32_t state,reserved;
    uint64_t vram_bytes; /* 驱动确认的物理显存字节数；零表示尚不可用。 */
} os_gpu_info_t;
