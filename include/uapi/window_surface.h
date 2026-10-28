#pragma once
#include <stdint.h>

/* Opt-in surface exchange. The mapped allocation contains two equal,
 * page-aligned slots; applications only write the slot granted by BEGIN.
 * COMMIT publishes a complete frame, CANCEL keeps the previous frame. */
enum os_surface_operation {
    OS_SURFACE_BEGIN = 1U,
    OS_SURFACE_COMMIT = 2U,
    OS_SURFACE_CANCEL = 3U,
};
typedef struct os_surface_frame {
    uint64_t token;
    uint64_t address;
    uint64_t capacity;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t reserved;
    /* 完整帧中的变化区域；首次显示及尺寸改变由服务端提升为整窗损坏。 */
    int32_t damage_x, damage_y;
    uint32_t damage_width, damage_height;
} os_surface_frame_t;
typedef struct os_surface_command {
    uint32_t operation;
    uint32_t reserved;
    os_surface_frame_t frame;
} os_surface_command_t;
_Static_assert(sizeof(os_surface_command_t) == 64U, "surface command ABI");
