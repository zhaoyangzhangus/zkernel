#pragma once

#include "abi.h"

#define OS_AUDIO_MAX_CHANNELS 8U
#define OS_AUDIO_MIN_RATE 8000U
#define OS_AUDIO_MAX_RATE 192000U
#define OS_AUDIO_MIN_PERIOD_FRAMES 64U
#define OS_AUDIO_MAX_PERIOD_FRAMES 4096U
#define OS_AUDIO_MAX_PERIOD_COUNT 32U

enum os_audio_sample_format {
    OS_AUDIO_SAMPLE_S16_LE = 1,
    OS_AUDIO_SAMPLE_S24_LE = 2,
    OS_AUDIO_SAMPLE_S32_LE = 3,
    OS_AUDIO_SAMPLE_F32_LE = 4,
};

enum os_audio_direction {
    OS_AUDIO_PLAYBACK = 0,
    OS_AUDIO_CAPTURE = 1,
};

typedef struct os_audio_stream_config {
    os_versioned_header_t hdr;
    uint32_t direction;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t sample_format;
    uint32_t period_frames;
    uint32_t period_count;
} os_audio_stream_config_t;

typedef struct os_audio_stream_stats {
    os_versioned_header_t hdr;
    uint64_t queued_frames;
    uint64_t mixed_frames;
    uint64_t underruns;
    uint64_t overruns;
    uint64_t device_generation;
    uint32_t state;
    uint32_t reserved;
} os_audio_stream_stats_t;

/* 音频 syscall 的流句柄和控制请求。数据通过受检用户态拷贝进入 DMA 周期。 */
enum os_audio_backend {
    OS_AUDIO_BACKEND_AUTO = 0,
    OS_AUDIO_BACKEND_HDA = 1,
    OS_AUDIO_BACKEND_USB = 2,
    OS_AUDIO_BACKEND_USB_HEADSET = 3,
};

typedef struct os_audio_open {
    os_versioned_header_t hdr;
    os_audio_stream_config_t config;
    os_handle_t handle;
    uint32_t backend;
} os_audio_open_t;

enum os_audio_control_code {
    OS_AUDIO_CONTROL_CONFIGURE = 1,
    OS_AUDIO_CONTROL_START = 2,
    OS_AUDIO_CONTROL_STOP = 3,
    OS_AUDIO_CONTROL_QUEUE = 4,
    OS_AUDIO_CONTROL_COMPLETE = 5,
    OS_AUDIO_CONTROL_RECOVER = 6,
    OS_AUDIO_CONTROL_RESET = 7,
    OS_AUDIO_CONTROL_DISCONNECT = 8,
    OS_AUDIO_CONTROL_GET_STATS = 9,
};

typedef struct os_audio_control {
    os_versioned_header_t hdr;
    uint32_t code;
    uint32_t period;
    uint32_t frames;
    uint32_t flags;
    uint64_t buffer;
    uint64_t buffer_size;
    uint64_t bytes_returned;
} os_audio_control_t;

#define OS_AUDIO_STATE_READY        0U
#define OS_AUDIO_STATE_RUNNING      1U
#define OS_AUDIO_STATE_DISCONNECTED 2U

/* 系统输出策略；应用默认跟随此设置，不自行保存声卡和主音量。 */
#define OS_AUDIO_DEVICE_HDA (1U << OS_AUDIO_BACKEND_HDA)
#define OS_AUDIO_DEVICE_USB (1U << OS_AUDIO_BACKEND_USB)
#define OS_AUDIO_DEVICE_USB_HEADSET (1U << OS_AUDIO_BACKEND_USB_HEADSET)
#define OS_AUDIO_SETTINGS_READ 0U
#define OS_AUDIO_SETTINGS_WRITE 1U
typedef struct os_audio_settings {
    os_versioned_header_t hdr;
    uint32_t operation;
    uint32_t backend;
    uint32_t volume;
    uint32_t muted;
    uint32_t available;
    uint32_t effective_backend;
    uint64_t generation;
} os_audio_settings_t;
