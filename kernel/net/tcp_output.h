#pragma once

#include <kernel/net_core.h>
#include <kernel/socket.h>
#include "socket_model.h"

/* 保留的在途数据可以包含多个段，但每次设备回调仍遵循协商出的 MSS。
 * 重传时也使用同一辅助函数。拥塞控制尚未使当前单个在途数据继续增长前，
 * 初始在途数据最多包含三个段。不额外分配负载缓冲区。 */
static inline size_t socket_tcp_flight_limit(uint16_t family, uint16_t mss) {
    size_t limit = (size_t)(mss ? mss : (family == OS_AF_INET4 ? 536U : 1220U)) * 3U;
    return limit < SOCKET_MAX_PAYLOAD ? limit : SOCKET_MAX_PAYLOAD;
}

/* 调用方持有 socket 保护锁。通告窗口从 SND.UNA 开始，因此尚未确认的字节
 * 同时占用窗口和保留在途数据的空间。就绪状态和发送准入必须使用相同的额度计算。 */
static inline size_t socket_tcp_send_room_locked(uint16_t family,
                                                  const socket_private_t *private) {
    if (private->tcp_send_failed || private->tcp_fin_pending || private->tcp_fin_sent ||
        (private->tcp_send_flags & (NET_TCP_FLAG_SYN | NET_TCP_FLAG_FIN)) != 0U)
        return 0U;
    size_t retained = private->tcp_send_pending ? private->tcp_send_length : 0U;
    if (private->tcp_send_pending &&
        private->tcp_send_next_sequence - private->tcp_send_unacknowledged != retained)
        return 0U;
    size_t limit = socket_tcp_flight_limit(family, private->tcp_peer_mss);
    size_t window = private->tcp_peer_window;
    if (retained >= limit || retained >= window) return 0U;
    size_t room = limit - retained;
    return room < window - retained ? room : window - retained;
}

#define SOCKET_TCP_BATCH_BODY(INVOKE) \
    size_t offset = 0U; \
    if (mss == 0U) mss = fallback; \
    do { \
        size_t chunk = length - offset; \
        if (chunk > mss) chunk = mss; \
        uint8_t segment_flags = flags; \
        if (offset + chunk < length) \
            segment_flags &= (uint8_t)~(NET_TCP_FLAG_PSH | NET_TCP_FLAG_FIN); \
        kstatus_t status = (INVOKE); \
        if (status != K_OK) return status; \
        offset += chunk; \
    } while (offset < length); \
    return K_OK

static inline kstatus_t socket_tcp_output4_batch(
    socket_tcp_ipv4_output_fn output, void *context,
    uint32_t source, uint16_t source_port, uint32_t destination, uint16_t destination_port,
    uint32_t sequence, uint32_t acknowledgement, uint8_t flags, uint16_t window,
    const void *payload, size_t length, uint16_t mss) {
    const uint16_t fallback = 536U;
    SOCKET_TCP_BATCH_BODY(output(context, source, source_port, destination,
        destination_port, sequence + (uint32_t)offset, acknowledgement,
        segment_flags, window, length ? (const uint8_t *)payload + offset : payload, chunk));
}

static inline kstatus_t socket_tcp_output6_batch(
    socket_tcp_ipv6_output_fn output, void *context,
    const uint8_t source[16], uint16_t source_port,
    const uint8_t destination[16], uint16_t destination_port,
    uint32_t sequence, uint32_t acknowledgement, uint8_t flags, uint16_t window,
    const void *payload, size_t length, uint16_t mss) {
    const uint16_t fallback = 1220U;
    SOCKET_TCP_BATCH_BODY(output(context, source, source_port, destination,
        destination_port, sequence + (uint32_t)offset, acknowledgement,
        segment_flags, window, length ? (const uint8_t *)payload + offset : payload, chunk));
}

#undef SOCKET_TCP_BATCH_BODY
