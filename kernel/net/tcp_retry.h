#pragma once

#include <arch/x86_64/cpu.h>
#include "socket_model.h"

/* 对端关闭窗口属于流量控制，不是丢包。使用有界指数退避进行探测，
 * 不要消耗普通丢包重试额度。截止时间使用模运算计算 TSC 差值，包括零点回绕。 */
static inline void socket_tcp_persist_schedule_locked(socket_private_t *private,
                                                      uint64_t now) {
    uint64_t ticks = x86_timeout_ns_to_tsc(
        (uint64_t)private->tcp_persist_seconds * 1000000000ULL);
    if (ticks == 0U) ticks = 1U;
    private->tcp_persist_deadline_tsc = now + ticks;
    if (private->tcp_persist_deadline_tsc == 0U)
        private->tcp_persist_deadline_tsc = 1U;
}

static inline void socket_tcp_persist_arm_locked(socket_private_t *private,
                                                 uint64_t now) {
    if (private->tcp_persist_deadline_tsc != 0U) return;
    private->tcp_persist_seconds = 1U;
    socket_tcp_persist_schedule_locked(private, now);
}

static inline bool socket_tcp_persist_wait_locked(socket_private_t *private,
                                                  uint64_t now, bool *probe) {
    *probe = false;
    bool waiting = private->tcp_state == SOCKET_TCP_ESTABLISHED &&
        private->peer == 0 && private->tcp_peer_window == 0U &&
        (private->tcp_send_pending || private->tcp_fin_pending ||
         private->tcp_persist_deadline_tsc != 0U);
    if (!waiting) {
        if (private->tcp_persist_deadline_tsc != 0U &&
            private->tcp_state == SOCKET_TCP_ESTABLISHED && private->tcp_send_pending) {
            /* 恢复时启动新的丢包定时器，而非使用接收方主动暂停额度期间已过期的 RTO。 */
            private->tcp_send_deadline_tsc = socket_tcp_retransmit_deadline(now);
            private->tcp_send_retransmits = 0U;
            private->tcp_local_retry_tsc = now != 0U ? now : 1U;
        }
        private->tcp_persist_deadline_tsc = 0U;
        private->tcp_persist_seconds = 0U;
        return false;
    }
    if (private->tcp_persist_deadline_tsc == 0U) {
        socket_tcp_persist_arm_locked(private, now);
        return true;
    }
    if ((int64_t)(now - private->tcp_persist_deadline_tsc) < 0) return true;
    *probe = true;
    uint32_t seconds = private->tcp_persist_seconds;
    private->tcp_persist_seconds = seconds < 30U ? seconds * 2U : 60U;
    socket_tcp_persist_schedule_locked(private, now);
    return true;
}

/* 调用方持有 socket 保护锁。延迟的输出完成事件不能重新调度已确认的输出区间
 * 或后续写入。完成令牌使用原始的起始/排他结束范围，而非不断变化的 SND.UNA。
 * 即使本地邻居解析始终失败，RTO 仍受有界丢包额度约束。 */
static inline bool socket_tcp_local_output_locked(socket_private_t *private,
    uint32_t start_sequence, uint32_t end_sequence, kstatus_t status, uint64_t now) {
    if (atomic_load_explicit(&private->closed, memory_order_acquire) ||
        !private->tcp_send_pending) return false;
    uint32_t remaining = end_sequence - private->tcp_send_unacknowledged;
    uint32_t outstanding = private->tcp_send_next_sequence - private->tcp_send_unacknowledged;
    if (remaining == 0U || remaining > outstanding) return false;
    if (status != K_OK && status != K_EAGAIN) {
        socket_tcp_fail_locked(private, status);
        return true; /* Caller notifies waiters after unlocking. */
    }
    if (status != K_EAGAIN) {
        /* An append ending at SND.NXT does not cover an older failed prefix.
         * Only success covering the entire remaining range clears its retry. */
        if (end_sequence == private->tcp_send_next_sequence &&
            (uint32_t)(private->tcp_send_unacknowledged - start_sequence) <
                (uint32_t)(end_sequence - start_sequence))
            private->tcp_local_retry_tsc = 0U;
        return false;
    }
    uint64_t interval = x86_timeout_ns_to_tsc(20000000ULL);
    if (interval == 0U) interval = 1U;
    uint64_t next = interval > UINT64_MAX - now ? UINT64_MAX : now + interval;
    if (private->tcp_send_deadline_tsc != 0U &&
        next > private->tcp_send_deadline_tsc)
        next = private->tcp_send_deadline_tsc;
    if (private->tcp_local_retry_tsc == 0U ||
        (int64_t)(now - private->tcp_local_retry_tsc) >= 0 ||
        next < private->tcp_local_retry_tsc)
        private->tcp_local_retry_tsc = next;
    return false;
}

static inline bool socket_tcp_local_retry_due(const socket_private_t *private,
                                              uint64_t now) {
    return private->tcp_local_retry_tsc != 0U &&
           (int64_t)(now - private->tcp_local_retry_tsc) >= 0;
}
