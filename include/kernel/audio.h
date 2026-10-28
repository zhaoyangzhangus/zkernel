#pragma once
#pragma once

#include "base.h"
#include "mm.h"
#include "object.h"

struct device;

#define KOBJECT_TYPE_AUDIO_STREAM 0x0113U

#define AUDIO_STREAM_RIGHT_CONTROL (1U << 0)
#define AUDIO_STREAM_RIGHT_QUERY   (1U << 1)
#define AUDIO_STREAM_RIGHT_ALL     (AUDIO_STREAM_RIGHT_CONTROL | \
                                    AUDIO_STREAM_RIGHT_QUERY)

/* 音频流方向由用户态音频服务器选择，内核只负责 DMA 周期和状态。 */
typedef enum audio_direction {
    AUDIO_PLAYBACK = 0,
    AUDIO_CAPTURE = 1,
} audio_direction_t;

typedef enum audio_sample_format {
    AUDIO_SAMPLE_S16_LE = 1,
    AUDIO_SAMPLE_S24_LE = 2,
    AUDIO_SAMPLE_S32_LE = 3,
    AUDIO_SAMPLE_F32_LE = 4,
} audio_sample_format_t;

typedef enum audio_stream_state {
    AUDIO_STREAM_NEW = 0,
    AUDIO_STREAM_CONFIGURED,
    AUDIO_STREAM_RUNNING,
    AUDIO_STREAM_XRUN,
    AUDIO_STREAM_DISCONNECTED,
} audio_stream_state_t;

typedef struct audio_format {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t sample_format;
    uint32_t period_frames;
    uint32_t period_count;
} audio_format_t;

typedef struct audio_stream_stats {
    uint64_t queued_frames;
    uint64_t queued_periods;
    uint64_t completed_periods;
    uint64_t completed_frames;
    uint64_t underruns;
    uint64_t overruns;
    uint64_t controller_resets;
    uint32_t active_period;
    uint32_t queued_count;
    audio_stream_state_t state;
    uint64_t device_generation;
} audio_stream_stats_t;

typedef struct audio_stream audio_stream_t;

bool audio_core_init(void);
bool audio_stream_is_valid(const audio_stream_t *stream);
struct device *audio_stream_device(const audio_stream_t *stream);
kstatus_t audio_stream_create(struct device *device, audio_direction_t direction,
                              audio_stream_t **out);
audio_direction_t audio_stream_direction(const audio_stream_t *stream);
void audio_stream_destroy(audio_stream_t *stream);
kstatus_t audio_stream_configure(audio_stream_t *stream,
                                 const audio_format_t *format);
kstatus_t audio_stream_start(audio_stream_t *stream);
kstatus_t audio_stream_stop(audio_stream_t *stream);
kstatus_t audio_stream_queue(audio_stream_t *stream, uint32_t period,
                             uint32_t frames);
kstatus_t audio_stream_complete(audio_stream_t *stream, uint32_t period,
                                uint32_t frames);
kstatus_t audio_stream_recover(audio_stream_t *stream);
kstatus_t audio_stream_controller_reset(audio_stream_t *stream);
kstatus_t audio_stream_disconnect(audio_stream_t *stream);
void *audio_stream_period_address(audio_stream_t *stream, uint32_t period);
uint32_t audio_stream_period_page_count(audio_stream_t *stream, uint32_t period);
void *audio_stream_period_page_address(audio_stream_t *stream, uint32_t period,
                                       uint32_t page);
uint64_t audio_stream_period_bytes(const audio_stream_t *stream);
uint64_t audio_stream_bytes_for_frames(const audio_stream_t *stream,
                                       uint32_t frames);
void audio_stream_get_format(const audio_stream_t *stream, audio_format_t *format);
uint32_t audio_stream_period_dma_segment_count(const audio_stream_t *stream,
                                               uint32_t period);
uint64_t audio_stream_period_dma_address(const audio_stream_t *stream,
                                         uint32_t period, uint32_t segment);
uint64_t audio_stream_period_dma_length(const audio_stream_t *stream,
                                        uint32_t period, uint32_t segment);
kstatus_t audio_stream_period_write(audio_stream_t *stream, uint32_t period,
                                    const void *buffer, uint64_t bytes);
kstatus_t audio_stream_period_read(audio_stream_t *stream, uint32_t period,
                                   void *buffer, uint64_t bytes);
/* 一次性提交播放数据，写入与入队共同持有流锁。 */
kstatus_t audio_stream_submit(audio_stream_t *stream, uint32_t period,
                               const void *buffer, uint64_t bytes,
                               uint32_t frames, uint64_t *submission);
/* 仅撤销尚未完成且标识匹配的提交，供后端提交失败时回滚。 */
kstatus_t audio_stream_cancel_submit(audio_stream_t *stream, uint32_t period,
                                     uint64_t submission);
void audio_stream_get_stats(audio_stream_t *stream, audio_stream_stats_t *stats);
bool audio_core_self_test(void);
