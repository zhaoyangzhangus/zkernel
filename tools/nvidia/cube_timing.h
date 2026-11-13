#ifndef CUBE_TIMING_H
#define CUBE_TIMING_H

#include <stdint.h>

typedef struct {
    uint64_t elapsed_ns;
    uint32_t frames;
    uint64_t fps;
} cube_fps_counter_t;

static inline uint64_t cube_fps_update(cube_fps_counter_t *counter,
                                     uint32_t completed_frames,
                                     uint64_t elapsed_ns) {
    if (elapsed_ns < counter->elapsed_ns) {
        *counter = (cube_fps_counter_t){.elapsed_ns = elapsed_ns,
                                      .frames = completed_frames};
        return 0U;
    }
    uint64_t interval = elapsed_ns - counter->elapsed_ns;
    /* 只统计已完成提交的帧，每秒更新；卡顿耗时必须计入，不能继续显示历史平均值。
     * 无符号差值允许帧计数跨越 UINT32_MAX，不引入限帧或额外时钟调用。 */
    if (interval >= 1000000000ULL) {
        uint32_t frames = completed_frames - counter->frames;
        counter->fps = ((uint64_t)frames * 1000000000ULL) / interval;
        counter->frames = completed_frames;
        counter->elapsed_ns = elapsed_ns;
    }
    return counter->fps;
}

typedef struct {
    uint64_t readback_ns;
    uint64_t publish_ns;
    int valid;
} cube_timing_sample_t;

typedef struct {
    uint64_t frames;
    uint64_t readback_ns;
    uint64_t publish_ns;
    uint64_t readback_max_ns;
    uint64_t publish_max_ns;
} cube_timing_totals_t;

static inline void cube_timing_add(cube_timing_totals_t *totals,
                                   const cube_timing_sample_t *sample) {
    if (!sample->valid) return;
    ++totals->frames;
    totals->readback_ns += sample->readback_ns;
    totals->publish_ns += sample->publish_ns;
    if (sample->readback_ns > totals->readback_max_ns)
        totals->readback_max_ns = sample->readback_ns;
    if (sample->publish_ns > totals->publish_max_ns)
        totals->publish_max_ns = sample->publish_ns;
}

#endif
