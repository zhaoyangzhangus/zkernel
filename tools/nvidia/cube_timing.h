#ifndef CUBE_TIMING_H
#define CUBE_TIMING_H

#include <stdint.h>

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
