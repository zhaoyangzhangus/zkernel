#ifndef KERNEL_WINDOW_SURFACE_EXCHANGE_H
#define KERNEL_WINDOW_SURFACE_EXCHANGE_H

#include <stdbool.h>
#include <stdint.h>

/* 此结构仅保存元数据：调用方在每次操作期间持有窗口锁，但渲染期间不持锁。
 * 快照引用会单独固定后备分配区。旧帧的所有快照结束前，不得复用其后缓冲区。 */
typedef struct window_surface_exchange {
    uint32_t readers[2];
    uint32_t front;
    uint32_t writer_slot;
    uint64_t serial;
    uint64_t writer_token;
    bool writing;
    bool closed;
} window_surface_exchange_t;

static inline bool window_surface_write_begin(window_surface_exchange_t *state,
                                              uint32_t *slot,
                                              uint64_t *token) {
    uint32_t back = state->front ^ 1U;
    if (state->closed || state->writing || state->readers[back] != 0U ||
        state->serial == UINT64_MAX) return false;
    state->writing = true;
    state->writer_slot = back;
    state->writer_token = ++state->serial;
    *slot = back;
    *token = state->writer_token;
    return true;
}

/* 必须先成功完成 GPU/CPU 操作，再提交。中止时仍显示上一完整帧，
 * 即使失败前已部分覆盖后缓冲区也一样。 */
static inline bool window_surface_write_end(window_surface_exchange_t *state,
                                            uint64_t token, bool commit) {
    if (!state->writing || token != state->writer_token) return false;
    if (commit && state->closed) return false;
    if (commit) state->front = state->writer_slot;
    state->writing = false;
    return true;
}

static inline bool window_surface_read_begin(window_surface_exchange_t *state,
                                             uint32_t *slot) {
    if (state->closed || state->readers[state->front] == UINT32_MAX) return false;
    *slot = state->front;
    ++state->readers[*slot];
    return true;
}

static inline bool window_surface_read_end(window_surface_exchange_t *state,
                                           uint32_t slot) {
    if (slot > 1U || state->readers[slot] == 0U) return false;
    --state->readers[slot];
    return true;
}

static inline void window_surface_exchange_close(window_surface_exchange_t *state) {
    state->closed = true;
    state->writing = false;
}

#endif
