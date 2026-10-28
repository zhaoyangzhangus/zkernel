#pragma once
#pragma once

#include "abi.h"
#include <stdbool.h>
#include "input.h"
#include "control_tree.h"

/* 窗口由应用拥有内容缓冲，由 Ring0 Window Server 负责摆放和合成。 */
enum os_window_flags {
    OS_WINDOW_VISIBLE             = 1u << 0,
    OS_WINDOW_RESIZABLE           = 1u << 1,
    /* Ring3 拥有完整的 surface 窗口装饰，并接收所有客户端输入。 */
    OS_WINDOW_CLIENT_DECORATIONS  = 1u << 2,
    /* Surface 像素采用预乘 ARGB8888，而非不透明的 XRGB8888。 */
    OS_WINDOW_PREMULTIPLIED_ALPHA = 1u << 3,
    /* 内置客户端装饰将标题栏控件交由服务器处理。 */
    OS_WINDOW_STANDARD_CAPTION_CONTROLS = 1u << 4,
    OS_WINDOW_SURFACE_EXCHANGE = 1u << 5,
    /* 客户端命中测试需显式请求交互式移动；不可根据 surface 顶部几行
     * 猜测可拖动区域。 */
    OS_WINDOW_EXPLICIT_MOVE_RESIZE = 1u << 6,
    /* 深色文件管理器装饰；按钮命中、圆角和绘制仍由合成器管理。 */
    OS_WINDOW_DARK_CAPTION = 1u << 7,
};

/* 可选的 WINDOW_UPDATE 元数据。零表示沿用旧版 40px 标题栏。
 * surface ABI 大小保持不变。只有标准客户端标题栏接受 [40,128] 范围内的高度；
 * 服务器会将其用于绘制和命中测试。 */
#define OS_WINDOW_CAPTION_HEIGHT_MASK (0xFFU << 16U)
#define OS_WINDOW_CAPTION_HEIGHT(height) ((uint32_t)(height) << 16U)
static inline uint32_t os_window_caption_height(uint32_t flags) {
    uint32_t height = (flags & OS_WINDOW_CAPTION_HEIGHT_MASK) >> 16U;
    return height >= 40U && height <= 128U ? height : 40U;
}

/* 自绘窗口沿用应用度量；标准装饰的按钮随标题栏整体缩放。 */
static inline uint32_t os_window_caption_button_width(uint32_t flags) {
    return flags & OS_WINDOW_CLIENT_DECORATIONS ? 32U :
        (32U*os_window_caption_height(flags)+20U)/40U;
}

/* 创建时即可声明标准标题栏度量，避免首帧发布后才改变按钮绘制及命中范围。 */
static inline bool os_window_caption_create_valid(uint32_t flags, uint32_t height) {
    uint32_t caption = (flags & OS_WINDOW_CAPTION_HEIGHT_MASK) >> 16U;
    return caption == 0U || ((flags & OS_WINDOW_STANDARD_CAPTION_CONTROLS) &&
        caption >= 40U && caption <= 128U && caption <= height);
}

typedef struct os_window_create {
    os_versioned_header_t hdr;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint32_t background;
    char title[32];
    os_handle_t window;
    uint32_t identifier;
    uint32_t reserved;
    uint64_t address;
    uint64_t buffer_size;
} os_window_create_t;

typedef struct os_window_info {
    uint32_t identifier;
    uint32_t owner_pid;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t visible;
    uint32_t focused;
    uint32_t z_order;
    uint32_t reserved;
    uint64_t buffer_size;
    char title[32];
} os_window_info_t;

typedef struct os_window_enumerate {
    os_versioned_header_t hdr;
    uint32_t index;
    uint32_t reserved;
    os_window_info_t info;
} os_window_enumerate_t;

typedef struct os_window_map {
    os_versioned_header_t hdr;
    uint32_t identifier;
    uint32_t reserved;
    uint64_t address;
    uint64_t length;
} os_window_map_t;

/* 用户完成 surface 绘制后通知内核窗口服务器重新合成。 */
typedef struct os_window_update {
    os_versioned_header_t hdr;
    uint32_t identifier;
    uint32_t flags;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} os_window_update_t;

/* 仅包含元数据；标题栏光栅化和命中测试由合成器负责。 */
typedef struct os_window_title {
    os_versioned_header_t hdr;
    uint32_t identifier;
    uint32_t reserved;
    char title[32]; /* 必须包含结尾的 NUL */
} os_window_title_t;

typedef struct os_window_set {
    os_versioned_header_t hdr;
    uint32_t identifier;
    uint32_t visible;
    int32_t x;
    int32_t y;
    uint32_t reserved;
} os_window_set_t;

typedef struct os_window_focus {
    os_versioned_header_t hdr;
    uint32_t identifier;
    uint32_t reserved;
} os_window_focus_t;

enum os_window_event_type {
    /* INPUT 保持为零，使旧版零初始化事件槽继续保持原有含义。 */
    OS_WINDOW_EVENT_INPUT         = 0u,
    OS_WINDOW_EVENT_RESIZE        = 1u,

    /*
    * Ring0 装饰会请求所属应用关闭此窗口。
     *
    * 这只是请求，不会强制销毁窗口。应用可以保存状态、拒绝/延迟关闭，
    * 或正常退出。
     */
    OS_WINDOW_EVENT_CLOSE_REQUEST = 2u,
    OS_WINDOW_EVENT_MINIMIZED = 3u,
    OS_WINDOW_EVENT_MOVED = 4u,
    OS_WINDOW_EVENT_CONTROL = 5u,
};

typedef struct os_window_resize_event {
    uint32_t width;
    uint32_t height;
    uint64_t buffer_size;
    uint64_t reserved;
} os_window_resize_event_t;

typedef struct os_window_minimized_event {
    uint32_t minimized;
    uint32_t reserved;
    uint64_t reserved2;
    uint64_t reserved3;
} os_window_minimized_event_t;

/* 客户端的绝对原点，而不是外层装饰的原点；尺寸不变。 */
typedef struct os_window_moved_event {
    int32_t x;
    int32_t y;
    uint64_t reserved;
    uint64_t reserved2;
} os_window_moved_event_t;

typedef struct os_window_event {
    uint32_t identifier;
    uint32_t type;
    union {
        os_input_event_t input;
        os_control_event_t control;
        os_window_resize_event_t resize;
        os_window_minimized_event_t minimized;
        os_window_moved_event_t moved;
    };

    /*
    * 指针相对于 Ring3 客户端 surface 的位置。
     *
    * 仅对 OS_WINDOW_EVENT_INPUT 有效。指针位于 Ring0 所有的装饰区域时，坐标可能为负值。
     */
    int32_t pointer_x;
    int32_t pointer_y;
} os_window_event_t;

typedef struct os_window_event_read {
    os_versioned_header_t hdr;
    uint32_t identifier;
    uint32_t reserved;
    uint64_t timeout_ns;
    os_window_event_t event;
} os_window_event_read_t;

typedef struct os_window_input_dispatch {
    os_versioned_header_t hdr;
    uint32_t identifier;
    uint32_t reserved;
    os_input_event_t event;
} os_window_input_dispatch_t;
