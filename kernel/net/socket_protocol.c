#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/net_core.h>
#include <kernel/object.h>
#include <kernel/socket.h>
#include <kernel/wait.h>

#include "socket_model.h"
#include "tcp_reassembly.h"

#if OS_DEBUG_SERIAL
static uint32_t g_debug_udp_inject_logs;
static uint32_t g_debug_tcp_proxy_rx_logs;
#endif

/* REFACTOR_P8_SOCKET_PROTOCOL_OWNER：数据包注入和 TCP 状态转换。 */

/* 查找期间只持有注册表保护锁。释放该锁前先固定对象引用，之后再独立获取 socket
 * 保护锁；输出操作不会在任一保护锁下执行。 */
static void socket_tcp_ack_output_target(socket_t *target, kstatus_t status) {
    if (target == 0) return;
    socket_private_t *private = socket_private(target);
    if (private != 0) {
        socket_lock(&target->lock);
        socket_tcp_ack_retry_locked(private, status, x86_read_tsc(),
                                    x86_timeout_ns_to_tsc(20000000ULL));
        socket_unlock(&target->lock);
    }
    object_put(target);
}

void socket_tcp_ack_output_ipv4(uint32_t local, uint16_t local_port,
                                uint32_t peer, uint16_t peer_port,
                                uint8_t flags, kstatus_t status) {
    if (status != K_EAGAIN || flags != NET_TCP_FLAG_ACK) return;
    socket_t *target = 0;
    socket_initialize_globals();
    socket_lock(socket_binding_lock());
    int32_t index = socket_find_tcp_locked(peer, peer_port, local, local_port);
    if (index >= 0) {
        target = socket_tcp_connection_at((uint32_t)index)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
    socket_tcp_ack_output_target(target, status);
}

void socket_tcp_ack_output_ipv6(const uint8_t local[16], uint16_t local_port,
                                const uint8_t peer[16], uint16_t peer_port,
                                uint8_t flags, kstatus_t status) {
    if (status != K_EAGAIN || flags != NET_TCP_FLAG_ACK ||
        local == 0 || peer == 0) return;
    socket_t *target = 0;
    socket_initialize_globals();
    socket_lock(socket_binding_lock());
    int32_t index = socket_find_tcp6_locked(peer, peer_port, local, local_port);
    if (index >= 0) {
        target = socket_tcp6_connection_at((uint32_t)index)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
    socket_tcp_ack_output_target(target, status);
}

/* 调用方持有 socket 锁。先处理 RST，再处理 ACK/窗口/数据，避免拒绝连接时
 * 完成握手或确认在途数据。SYN-SENT 状态会验证 ACK；已同步的 socket 要求
 * RCV.NXT 完全匹配。窗口内但不完全匹配的复位会收到 challenge ACK
 *（RFC 9293 第 3.10.7 节）。 */
static bool socket_tcp_reset_locked(socket_private_t *private, uint32_t sequence,
                                    uint32_t acknowledgement, uint8_t flags,
                                    socket_tcp_reply_t *reply) {
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) return false;
    bool opening = private->tcp_state == SOCKET_TCP_SYN_SENT;
    if (opening) {
        if ((flags & NET_TCP_FLAG_ACK) == 0U ||
            acknowledgement != private->tcp_send_next_sequence) return false;
    } else {
        if (private->tcp_state != SOCKET_TCP_ESTABLISHED &&
            private->tcp_state != SOCKET_TCP_SYN_RECEIVED) return false;
        if (sequence != private->tcp_next_sequence) {
            uint32_t available = socket_stream_capacity(private) -
                atomic_load_explicit(&private->stream_count, memory_order_relaxed);
            if ((uint32_t)(sequence - private->tcp_next_sequence) < available && reply != 0) {
                reply->valid = true;
                reply->flags = NET_TCP_FLAG_ACK;
                reply->sequence = private->tcp_send_next_sequence;
                reply->acknowledgement = private->tcp_next_sequence;
                reply->window = socket_tcp_receive_window(private, false);
            }
            return false;
        }
    }
    socket_tcp_fail_locked(private, opening ? K_ECONNREFUSED : K_ECONNRESET);
    return true;
}

/* 重传数据可能与已交付字节重叠。仅保留新后缀，包括尚未消费的 FIN。
 * 无符号距离可处理回绕；在小于 2^31 的 TCP 接收窗口内，有符号比较顺序有效。 */
static kstatus_t socket_tcp_trim_overlap(uint32_t *sequence, uint32_t expected,
                                         const void **buffer, size_t *length,
                                         uint8_t flags) {
    if (*sequence == expected) return K_OK;
    if ((int32_t)(*sequence - expected) > 0) return K_EAGAIN;
    uint32_t consumed = expected - *sequence;
    if (consumed > *length ||
        (consumed == *length && (flags & NET_TCP_FLAG_FIN) == 0U)) return K_ENOENT;
    if (consumed != 0U) *buffer = (const uint8_t *)*buffer + consumed;
    *length -= consumed;
    *sequence = expected;
    return K_OK;
}

static void socket_tcp_apply_mss(socket_t *target, uint32_t sequence,
                                  uint32_t acknowledgement, uint8_t flags,
                                  uint16_t mss) {
    if (target == 0) return;
    socket_private_t *private = socket_private(target);
    if (private != 0) {
        socket_lock(&target->lock);
        bool syn_ack = (flags & (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK | NET_TCP_FLAG_RST)) ==
                        (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK);
        bool syn = (flags & (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK | NET_TCP_FLAG_RST)) ==
                    NET_TCP_FLAG_SYN;
        if (!atomic_load_explicit(&private->closed, memory_order_acquire) &&
            ((syn_ack && private->tcp_state == SOCKET_TCP_SYN_SENT &&
              acknowledgement == private->tcp_send_next_sequence) ||
             (syn && private->tcp_state == SOCKET_TCP_SYN_RECEIVED &&
              sequence == private->tcp_peer_initial_sequence))) {
            uint16_t maximum = target->family == OS_AF_INET4 ? 1460U : 1440U;
            if (mss == 0U) mss = target->family == OS_AF_INET4 ? 536U : 1220U;
            private->tcp_peer_mss = mss < maximum ? mss : maximum;
        }
        socket_unlock(&target->lock);
    }
    object_put(target);
}

void socket_tcp_mss_ipv4(uint32_t source, uint16_t source_port,
                         uint32_t destination, uint16_t destination_port,
                         uint32_t sequence, uint32_t acknowledgement,
                         uint8_t flags, uint16_t mss) {
    socket_t *target = 0;
    socket_initialize_globals();
    socket_lock(socket_binding_lock());
    int32_t index = socket_find_tcp_locked(source, source_port, destination, destination_port);
    if (index >= 0) {
        target = socket_tcp_connection_at((uint32_t)index)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
    socket_tcp_apply_mss(target, sequence, acknowledgement, flags, mss);
}

void socket_tcp_mss_ipv6(const uint8_t source[16], uint16_t source_port,
                         const uint8_t destination[16], uint16_t destination_port,
                         uint32_t sequence, uint32_t acknowledgement,
                         uint8_t flags, uint16_t mss) {
    socket_t *target = 0;
    socket_initialize_globals();
    socket_lock(socket_binding_lock());
    int32_t index = socket_find_tcp6_locked(source, source_port, destination, destination_port);
    if (index >= 0) {
        target = socket_tcp6_connection_at((uint32_t)index)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
    socket_tcp_apply_mss(target, sequence, acknowledgement, flags, mss);
}

static socket_t *socket_tcp_option_target4(uint32_t source, uint16_t source_port,
    uint32_t destination, uint16_t destination_port) {
    socket_t *target = 0;
    socket_initialize_globals();
    socket_lock(socket_binding_lock());
    int32_t index = socket_find_tcp_locked(source, source_port, destination, destination_port);
    if (index >= 0) {
        target = socket_tcp_connection_at((uint32_t)index)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
    return target;
}

static socket_t *socket_tcp_option_target6(const uint8_t source[16], uint16_t source_port,
    const uint8_t destination[16], uint16_t destination_port) {
    if (source == 0 || destination == 0) return 0;
    socket_t *target = 0;
    socket_initialize_globals();
    socket_lock(socket_binding_lock());
    int32_t index = socket_find_tcp6_locked(source, source_port, destination, destination_port);
    if (index >= 0) {
        target = socket_tcp6_connection_at((uint32_t)index)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
    return target;
}

static void socket_tcp_apply_scale(socket_t *target, uint32_t sequence,
    uint32_t acknowledgement, uint8_t flags, int scale) {
    if (target == 0) return;
    socket_private_t *private = socket_private(target);
    if (private != 0 && scale >= -1 && scale <= 14) {
        socket_lock(&target->lock);
        uint8_t control = flags & (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK |
                                   NET_TCP_FLAG_RST | NET_TCP_FLAG_FIN);
        bool active = private->tcp_state == SOCKET_TCP_SYN_SENT &&
            control == (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK) &&
            acknowledgement == private->tcp_send_next_sequence;
        bool passive = private->tcp_state == SOCKET_TCP_SYN_RECEIVED &&
            control == NET_TCP_FLAG_SYN && sequence == private->tcp_peer_initial_sequence;
        if (!atomic_load_explicit(&private->closed, memory_order_acquire) &&
            !private->tcp_scale_seen && (active || passive)) {
            private->tcp_scale_seen = true;
            if (passive) private->tcp_scale_offered = scale >= 0 &&
                socket_stream_capacity(private) > SOCKET_STREAM_BUFFER_SIZE;
            private->tcp_scale_negotiated = private->tcp_scale_offered && scale >= 0;
            private->tcp_peer_scale = private->tcp_scale_negotiated ? (uint8_t)scale : 0U;
        }
        socket_unlock(&target->lock);
    }
    object_put(target);
}

static int socket_tcp_syn_scale_target(socket_t *target, uint32_t sequence,
                                        uint32_t acknowledgement) {
    if (target == 0) return -1;
    int scale = -1;
    socket_private_t *private = socket_private(target);
    if (private != 0) {
        socket_lock(&target->lock);
        bool active = private->tcp_state == SOCKET_TCP_SYN_SENT && acknowledgement == 0U;
        bool passive = private->tcp_state == SOCKET_TCP_SYN_RECEIVED &&
            acknowledgement == private->tcp_peer_initial_sequence + 1U;
        if (!atomic_load_explicit(&private->closed, memory_order_acquire) &&
            private->tcp_scale_offered && sequence == private->tcp_initial_sequence &&
            (active || passive)) scale = private->tcp_receive_scale;
        socket_unlock(&target->lock);
    }
    object_put(target);
    return scale;
}

void socket_tcp_scale_ipv4(uint32_t source, uint16_t source_port,
    uint32_t destination, uint16_t destination_port, uint32_t sequence,
    uint32_t acknowledgement, uint8_t flags, int scale) {
    socket_tcp_apply_scale(socket_tcp_option_target4(source, source_port, destination,
        destination_port), sequence, acknowledgement, flags, scale);
}
void socket_tcp_scale_ipv6(const uint8_t source[16], uint16_t source_port,
    const uint8_t destination[16], uint16_t destination_port, uint32_t sequence,
    uint32_t acknowledgement, uint8_t flags, int scale) {
    socket_tcp_apply_scale(socket_tcp_option_target6(source, source_port, destination,
        destination_port), sequence, acknowledgement, flags, scale);
}
int socket_tcp_syn_scale_ipv4(uint32_t local, uint16_t local_port,
    uint32_t peer, uint16_t peer_port, uint32_t sequence, uint32_t acknowledgement) {
    return socket_tcp_syn_scale_target(socket_tcp_option_target4(peer, peer_port,
        local, local_port), sequence, acknowledgement);
}
int socket_tcp_syn_scale_ipv6(const uint8_t local[16], uint16_t local_port,
    const uint8_t peer[16], uint16_t peer_port, uint32_t sequence, uint32_t acknowledgement) {
    return socket_tcp_syn_scale_target(socket_tcp_option_target6(peer, peer_port,
        local, local_port), sequence, acknowledgement);
}

kstatus_t socket_inject_udp(uint32_t source_address, uint16_t source_port,
                            uint32_t destination_address, uint16_t destination_port,
                            const void *buffer, size_t length) {
    socket_t *target = 0;
    socket_private_t *private;
    if (source_address == 0U || source_port == 0U || destination_port == 0U ||
        (buffer == 0 && length != 0) || length > SOCKET_MAX_PAYLOAD) return K_EINVAL;
    socket_initialize_globals();
    socket_lock(socket_binding_lock());
    int32_t binding = socket_find_binding_locked(destination_address, destination_port,
                                                 OS_SOCK_DGRAM, false);
    if (binding >= 0) {
        target = socket_binding_at((uint32_t)binding)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
    if (target == 0) {
#if OS_DEBUG_SERIAL
        if (g_debug_udp_inject_logs < 128U) {
            kernel_serial_printf_serial_only(
                "OS_UDP_INJECT dst=%x:%u status=%d\r\n",
                destination_address, destination_port, K_ENOENT);
            ++g_debug_udp_inject_logs;
        }
#endif
        return K_ENOENT;
    }
    private = socket_private(target);
    if (private == 0) {
        object_put(target);
        return K_EIO;
    }
    socket_lock(&target->lock);
    uint32_t count = atomic_load_explicit(&private->datagram_count, memory_order_relaxed);
    if (atomic_load_explicit(&private->closed, memory_order_acquire) ||
        atomic_load_explicit(&private->receive_shutdown, memory_order_acquire)) {
        socket_unlock(&target->lock);
        object_put(target);
        return K_OK;
    }
    if (count >= SOCKET_RX_QUEUE_DEPTH) {
        socket_unlock(&target->lock);
        object_put(target);
        return K_EAGAIN;
    }
    socket_datagram_t *datagram = &private->datagrams[private->datagram_write];
    datagram->family = OS_AF_INET4;
    datagram->source_address = source_address;
    datagram->source_port = source_port;
    datagram->destination_address = destination_address;
    datagram->destination_port = destination_port;
    datagram->length = (uint16_t)length;
    if (length != 0) socket_copy(datagram->payload, buffer, length);
    private->datagram_write = (private->datagram_write + 1U) % SOCKET_RX_QUEUE_DEPTH;
    atomic_store_explicit(&private->datagram_count, count + 1U, memory_order_release);
    socket_unlock(&target->lock);
    (void)wake_all(&target->waitq);
    object_notify_signaled(target);
#if OS_DEBUG_SERIAL
    if (g_debug_udp_inject_logs < 128U) {
        kernel_serial_printf_serial_only(
            "OS_UDP_INJECT dst=%x:%u status=%d count=%u\r\n",
            destination_address, destination_port, K_OK, count + 1U);
        ++g_debug_udp_inject_logs;
    }
#endif
    object_put(target);
    return K_OK;
}

kstatus_t socket_inject_udp_ipv6(const uint8_t source_address[16], uint16_t source_port,
                                 const uint8_t destination_address[16],
                                 uint16_t destination_port,
                                 const void *buffer, size_t length) {
    socket_t *target = 0;
    socket_private_t *private;
    if (source_address == 0 || destination_address == 0 || source_port == 0U ||
        destination_port == 0U || (buffer == 0 && length != 0U) ||
        length > SOCKET_MAX_PAYLOAD) return K_EINVAL;
    socket_initialize_globals();
    socket_lock(socket_binding_lock());
    int32_t binding = socket_find_binding6_locked(destination_address, destination_port,
                                                  OS_SOCK_DGRAM, false);
    if (binding >= 0) {
        target = socket_binding_at((uint32_t)binding)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
    if (target == 0) return K_ENOENT;
    private = socket_private(target);
    if (private == 0) {
        object_put(target);
        return K_EIO;
    }
    socket_lock(&target->lock);
    uint32_t count = atomic_load_explicit(&private->datagram_count, memory_order_relaxed);
    if (atomic_load_explicit(&private->closed, memory_order_acquire) ||
        atomic_load_explicit(&private->receive_shutdown, memory_order_acquire)) {
        socket_unlock(&target->lock);
        object_put(target);
        return K_OK;
    }
    if (count >= SOCKET_RX_QUEUE_DEPTH) {
        socket_unlock(&target->lock);
        object_put(target);
        return K_EAGAIN;
    }
    socket_datagram_t *datagram = &private->datagrams[private->datagram_write];
    datagram->family = OS_AF_INET6;
    socket_copy(datagram->source_address6, source_address, 16U);
    socket_copy(datagram->destination_address6, destination_address, 16U);
    datagram->source_port = source_port;
    datagram->destination_port = destination_port;
    datagram->length = (uint16_t)length;
    if (length != 0U) socket_copy(datagram->payload, buffer, length);
    private->datagram_write = (private->datagram_write + 1U) % SOCKET_RX_QUEUE_DEPTH;
    atomic_store_explicit(&private->datagram_count, count + 1U, memory_order_release);
    socket_unlock(&target->lock);
    (void)wake_all(&target->waitq);
    object_notify_signaled(target);
    object_put(target);
    return K_OK;
}

kstatus_t socket_inject_tcp_ipv4_reply(uint32_t source_address, uint16_t source_port,
                                       uint32_t destination_address,
                                       uint16_t destination_port, uint32_t sequence,
                                       uint8_t flags, const void *buffer, size_t length,
                                       socket_tcp_reply_t *reply) {
    socket_t *target = 0;
    socket_private_t *private;
    bool notify = false;
    bool discard_receive = false;
    if (reply != 0) socket_zero(reply, sizeof(*reply));
    if (source_address == 0U || source_port == 0U || destination_port == 0U ||
        (buffer == 0 && length != 0U) || length > SOCKET_STREAM_BUFFER_SIZE ||
        (flags & (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK)) != NET_TCP_FLAG_ACK) {
        return K_EINVAL;
    }
    socket_initialize_globals();
    socket_lock(socket_binding_lock());
    int32_t connection = socket_find_tcp_locked(source_address, source_port,
                                                 destination_address, destination_port);
    if (connection >= 0) {
        target = socket_tcp_connection_at((uint32_t)connection)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
    if (target == 0) {
#if OS_DEBUG_SERIAL
        if (source_port == 7890U && g_debug_tcp_proxy_rx_logs < 128U) {
            kernel_serial_printf_serial_only(
                "OS_TCP_PROXY_RX unmatched src=%x:%u dst=%x:%u "
                "seq=%u flags=%x len=%u\r\n",
                source_address, source_port, destination_address,
                destination_port, sequence, flags, (unsigned)length);
            ++g_debug_tcp_proxy_rx_logs;
        }
#endif
        return K_ENOENT;
    }
    private = socket_private(target);
    if (private == 0) {
        object_put(target);
        return K_EIO;
    }
    socket_lock(&target->lock);
    if (private->tcp_state != SOCKET_TCP_ESTABLISHED) {
        socket_unlock(&target->lock);
        object_put(target);
        return K_EAGAIN;
    }
    uint32_t count = atomic_load_explicit(&private->stream_count, memory_order_relaxed);
    discard_receive = atomic_load_explicit(&private->receive_shutdown,
                                            memory_order_acquire);
    uint32_t expected = private->tcp_sequence_valid ? private->tcp_next_sequence : sequence;
    if (reply != 0) {
        reply->valid = true;
        reply->flags = NET_TCP_FLAG_ACK;
        reply->sequence = private->tcp_send_next_sequence;
        reply->acknowledgement = expected;
        reply->window = socket_tcp_window_for_count(private, count, false);
    }
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) {
        socket_unlock(&target->lock);
        object_put(target);
        return K_EDEVREMOVED;
    }
    kstatus_t overlap = socket_tcp_trim_overlap(&sequence, expected, &buffer, &length, flags);
    if (overlap != K_OK && !(overlap == K_EAGAIN &&
        private->tcp_rx_present != 0 && !discard_receive)) {
#if OS_DEBUG_SERIAL
        if (source_port == 7890U && g_debug_tcp_proxy_rx_logs < 128U) {
            kernel_serial_printf_serial_only(
                "OS_TCP_PROXY_RX sequence src=%x:%u dst=%x:%u "
                "seq=%u expected=%u flags=%x len=%u buffered=%u\r\n",
                source_address, source_port, destination_address,
                destination_port, sequence, expected, flags,
                (unsigned)length, count);
            ++g_debug_tcp_proxy_rx_logs;
        }
#endif
        /* 瑜版挸澧?RX 缁愭褰涢崣顏呭复閸欐ぞ绗呮稉鈧稉顏囩箾缂侇厼绨崣鍑ょ幢闁插秴顦?娑斿崬绨崠鍛暠娑撳﹤鐪伴柌宥勭炊閺堝搫鍩楁径鍕倞閵?*/
        socket_unlock(&target->lock);
        object_put(target);
        return overlap == K_ENOENT ? K_OK : overlap;
    }
    kstatus_t reassembly = socket_tcp_reassemble_locked(
        private, sequence, expected, flags, buffer, length, &notify);
    if (reassembly != K_OK) {
        socket_unlock(&target->lock);
        object_put(target);
        return reassembly;
    }
    if (reply != 0) {
        reply->acknowledgement = private->tcp_next_sequence;
        reply->window = socket_tcp_receive_window(private, false);
    }
#if OS_DEBUG_SERIAL
    if (source_port == 7890U && g_debug_tcp_proxy_rx_logs < 128U) {
        kernel_serial_printf_serial_only(
            "OS_TCP_PROXY_RX accepted src=%x:%u dst=%x:%u "
            "seq=%u flags=%x len=%u buffered=%u->%u\r\n",
            source_address, source_port, destination_address,
            destination_port, sequence, flags, (unsigned)length, count,
            discard_receive ? count : count + (uint32_t)length);
        ++g_debug_tcp_proxy_rx_logs;
    }
#endif
    socket_unlock(&target->lock);
    if (notify) {
        (void)wake_all(&target->waitq);
        object_notify_changed(target);
    }
    object_put(target);
    return K_OK;
}

kstatus_t socket_inject_tcp_ipv6_reply(const uint8_t source_address[16],
                                       uint16_t source_port,
                                       const uint8_t destination_address[16],
                                       uint16_t destination_port, uint32_t sequence,
                                       uint8_t flags, const void *buffer, size_t length,
                                       socket_tcp_reply_t *reply) {
    socket_t *target = 0;
    socket_private_t *private;
    bool notify = false;
    bool discard_receive = false;
    if (reply != 0) socket_zero(reply, sizeof(*reply));
    if (source_address == 0 || destination_address == 0 ||
        socket_address6_zero(source_address) || socket_address6_zero(destination_address) ||
        source_port == 0U || destination_port == 0U ||
        (buffer == 0 && length != 0U) || length > SOCKET_STREAM_BUFFER_SIZE ||
        (flags & (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK)) != NET_TCP_FLAG_ACK) {
        return K_EINVAL;
    }
    socket_initialize_globals();
    socket_lock(socket_binding_lock());
    int32_t connection = socket_find_tcp6_locked(source_address, source_port,
                                                 destination_address, destination_port);
    if (connection >= 0) {
        target = socket_tcp6_connection_at((uint32_t)connection)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
    if (target == 0) return K_ENOENT;
    private = socket_private(target);
    if (private == 0) {
        object_put(target);
        return K_EIO;
    }
    socket_lock(&target->lock);
    if (private->tcp_state != SOCKET_TCP_ESTABLISHED) {
        socket_unlock(&target->lock);
        object_put(target);
        return K_EAGAIN;
    }
    uint32_t count = atomic_load_explicit(&private->stream_count, memory_order_relaxed);
    discard_receive = atomic_load_explicit(&private->receive_shutdown,
                                            memory_order_acquire);
    uint32_t expected = private->tcp_sequence_valid ? private->tcp_next_sequence : sequence;
    if (reply != 0) {
        reply->valid = true;
        reply->flags = NET_TCP_FLAG_ACK;
        reply->sequence = private->tcp_send_next_sequence;
        reply->acknowledgement = expected;
        reply->window = socket_tcp_window_for_count(private, count, false);
    }
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) {
        socket_unlock(&target->lock);
        object_put(target);
        return K_EDEVREMOVED;
    }
    kstatus_t overlap = socket_tcp_trim_overlap(&sequence, expected, &buffer, &length, flags);
    if (overlap != K_OK && !(overlap == K_EAGAIN &&
        private->tcp_rx_present != 0 && !discard_receive)) {
        socket_unlock(&target->lock);
        object_put(target);
        return overlap == K_ENOENT ? K_OK : overlap;
    }
    kstatus_t reassembly = socket_tcp_reassemble_locked(
        private, sequence, expected, flags, buffer, length, &notify);
    if (reassembly != K_OK) {
        socket_unlock(&target->lock);
        object_put(target);
        return reassembly;
    }
    if (reply != 0) {
        reply->acknowledgement = private->tcp_next_sequence;
        reply->window = socket_tcp_receive_window(private, false);
    }
    socket_unlock(&target->lock);
    if (notify) {
        (void)wake_all(&target->waitq);
        object_notify_signaled(target);
    }
    object_put(target);
    return K_OK;
}

kstatus_t socket_inject_tcp_ipv4(uint32_t source_address, uint16_t source_port,
                                 uint32_t destination_address,
                                 uint16_t destination_port, uint32_t sequence,
                                 uint8_t flags, const void *buffer, size_t length) {
    return socket_inject_tcp_ipv4_reply(source_address, source_port,
                                        destination_address, destination_port,
                                        sequence, flags, buffer, length, 0);
}

kstatus_t socket_inject_tcp_syn_ipv4(uint32_t source_address, uint16_t source_port,
                                     uint32_t destination_address,
                                     uint16_t destination_port, uint32_t sequence,
                                     socket_tcp_reply_t *reply) {
    return socket_inject_tcp_syn_ipv4_scaled(source_address, source_port,
        destination_address, destination_port, sequence, -1, reply);
}

kstatus_t socket_inject_tcp_syn_ipv4_scaled(uint32_t source_address, uint16_t source_port,
    uint32_t destination_address, uint16_t destination_port, uint32_t sequence,
    int peer_scale, socket_tcp_reply_t *reply) {
    socket_t *listener = 0;
    socket_t *child = 0;
    socket_private_t *listener_private;
    socket_private_t *child_private;
    if (source_address == 0U || source_port == 0U || destination_address == 0U ||
        destination_port == 0U || reply == 0 || peer_scale < -1 || peer_scale > 14) return K_EINVAL;
    socket_zero(reply, sizeof(*reply));
    socket_initialize_globals();
    socket_lock(socket_binding_lock());
    int32_t binding = socket_find_binding_locked(destination_address, destination_port,
                                                 OS_SOCK_STREAM, true);
    if (binding >= 0) {
        listener = socket_binding_at((uint32_t)binding)->socket;
        if (!object_try_get(listener)) listener = 0;
    }
    socket_unlock(socket_binding_lock());
    if (listener == 0) return K_ENOENT;
    listener_private = socket_private(listener);
    if (listener_private == 0) {
        object_put(listener);
        return K_EIO;
    }
    socket_lock(socket_binding_lock());
    int32_t existing = socket_find_tcp_locked(source_address, source_port,
                                               destination_address, destination_port);
    socket_t *existing_socket = existing >= 0 ?
        socket_tcp_connection_at((uint32_t)existing)->socket : 0;
    if (existing_socket != 0 && !object_try_get(existing_socket)) existing_socket = 0;
    socket_unlock(socket_binding_lock());
    if (existing_socket != 0) {
        socket_private_t *existing_private = socket_private(existing_socket);
        socket_lock(&existing_socket->lock);
        if (existing_private != 0 &&
            !atomic_load_explicit(&existing_private->closed, memory_order_acquire) &&
            existing_private->tcp_state == SOCKET_TCP_SYN_RECEIVED &&
            sequence == existing_private->tcp_peer_initial_sequence) {
            reply->valid = true;
            reply->flags = NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK;
            reply->window = socket_tcp_receive_window(existing_private, true);
            reply->sequence = existing_private->tcp_initial_sequence;
            reply->acknowledgement = sequence + 1U;
            socket_unlock(&existing_socket->lock);
            object_put(existing_socket);
            object_put(listener);
            return K_OK;
        }
        socket_unlock(&existing_socket->lock);
        object_put(existing_socket);
        object_put(listener);
        return K_EAGAIN;
    }
    socket_lock(&listener->lock);
    if (!listener_private->listening ||
        atomic_load_explicit(&listener_private->closed, memory_order_acquire) ||
        atomic_load_explicit(&listener_private->accept_count, memory_order_relaxed) >=
            SOCKET_LISTEN_BACKLOG) {
        socket_unlock(&listener->lock);
        object_put(listener);
        return K_EAGAIN;
    }
    socket_unlock(&listener->lock);
    socket_lock(socket_binding_lock());
    bool available = socket_tcp_slots_available_locked(1U);
    socket_unlock(socket_binding_lock());
    if (!available || socket_allocate(OS_AF_INET4, OS_SOCK_STREAM,
                                      SOCKET_PROTOCOL_TCP, &child) != K_OK) {
        object_put(listener);
        return K_ENOMEM;
    }
    if (peer_scale >= 0) (void)socket_stream_reserve(child, SOCKET_STREAM_TCP_CAPACITY);
    child_private = socket_private(child);
    socket_tcp_prepare_scale(child_private, false);
    child_private->tcp_scale_seen = true;
    child_private->tcp_scale_offered = peer_scale >= 0 &&
        socket_stream_capacity(child_private) > SOCKET_STREAM_BUFFER_SIZE;
    child_private->tcp_scale_negotiated = child_private->tcp_scale_offered;
    child_private->tcp_peer_scale = child_private->tcp_scale_offered ? (uint8_t)peer_scale : 0U;
    socket_lock(&listener->lock);
    child_private->tcp_keepalive = listener_private->tcp_keepalive;
    child_private->tcp_keepidle = listener_private->tcp_keepidle;
    child_private->tcp_keepintvl = listener_private->tcp_keepintvl;
    child_private->tcp_keepcnt = listener_private->tcp_keepcnt;
    socket_unlock(&listener->lock);
    child_private->bound = true;
    child_private->connected = true;
    child_private->local_address = destination_address;
    child_private->local_port = destination_port;
    child_private->peer_address = source_address;
    child_private->peer_port = source_port;
    child_private->tcp_peer_initial_sequence = sequence;
    child_private->tcp_initial_sequence = 0x10000000U + source_port;
    child_private->tcp_next_sequence = child_private->tcp_initial_sequence + 1U;
    child_private->tcp_send_next_sequence = child_private->tcp_initial_sequence + 1U;
    child_private->tcp_send_unacknowledged = child_private->tcp_initial_sequence;
    child_private->tcp_send_length = 0U;
    child_private->tcp_send_flags = NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK;
    child_private->tcp_send_pending = true;
    child_private->tcp_send_retransmits = 0U;
    child_private->tcp_send_deadline_tsc = socket_tcp_retransmit_deadline(x86_read_tsc());
    child_private->tcp_peer_window = SOCKET_STREAM_BUFFER_SIZE;
    child_private->tcp_sequence_valid = true;
    child_private->tcp_state = SOCKET_TCP_SYN_RECEIVED;
    child_private->peer = 0;
    child_private->listener = listener;
    object_get(listener);

    socket_lock(&listener->lock);
    if (atomic_load_explicit(&listener_private->accept_count, memory_order_relaxed) >=
        SOCKET_LISTEN_BACKLOG) {
        socket_unlock(&listener->lock);
        socket_abort_child(child);
        object_put(listener);
        return K_EAGAIN;
    }
    listener_private->accept_queue[listener_private->accept_write] = child;
    listener_private->accept_write =
        (listener_private->accept_write + 1U) % SOCKET_LISTEN_BACKLOG;
    atomic_fetch_add_explicit(&listener_private->accept_count, 1U, memory_order_release);
    socket_unlock(&listener->lock);
    (void)wake_all(&listener->waitq);
    object_notify_signaled(listener);
    object_put(listener);

    socket_lock(socket_binding_lock());
    bool registered = socket_register_tcp_locked(child, child_private);
    socket_unlock(socket_binding_lock());
    if (!registered) {
        socket_abort_child(child);
        return K_ENOMEM;
    }
    reply->valid = true;
    reply->flags = NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK;
    reply->window = socket_tcp_receive_window(child_private, true);
    reply->sequence = child_private->tcp_initial_sequence;
    reply->acknowledgement = sequence + 1U;
    return K_OK;
}

kstatus_t socket_inject_tcp_syn_ipv6(const uint8_t source_address[16],
                                     uint16_t source_port,
                                     const uint8_t destination_address[16],
                                     uint16_t destination_port, uint32_t sequence,
                                     socket_tcp_reply_t *reply) {
    return socket_inject_tcp_syn_ipv6_scaled(source_address, source_port,
        destination_address, destination_port, sequence, -1, reply);
}

kstatus_t socket_inject_tcp_syn_ipv6_scaled(const uint8_t source_address[16],
    uint16_t source_port, const uint8_t destination_address[16], uint16_t destination_port,
    uint32_t sequence, int peer_scale, socket_tcp_reply_t *reply) {
    socket_t *listener = 0;
    socket_t *child = 0;
    socket_private_t *listener_private;
    socket_private_t *child_private;
    if (source_address == 0 || destination_address == 0 ||
        socket_address6_zero(source_address) || socket_address6_zero(destination_address) ||
        source_port == 0U || destination_port == 0U || reply == 0 ||
        peer_scale < -1 || peer_scale > 14) return K_EINVAL;
    socket_zero(reply, sizeof(*reply));
    socket_initialize_globals();
    socket_lock(socket_binding_lock());
    int32_t binding = socket_find_binding6_locked(destination_address, destination_port,
                                                  OS_SOCK_STREAM, true);
    if (binding >= 0) {
        listener = socket_binding_at((uint32_t)binding)->socket;
        if (!object_try_get(listener)) listener = 0;
    }
    socket_unlock(socket_binding_lock());
    if (listener == 0) return K_ENOENT;
    listener_private = socket_private(listener);
    if (listener_private == 0) {
        object_put(listener);
        return K_EIO;
    }
    socket_lock(socket_binding_lock());
    int32_t existing = socket_find_tcp6_locked(source_address, source_port,
                                                destination_address, destination_port);
    socket_t *existing_socket = existing >= 0 ?
        socket_tcp6_connection_at((uint32_t)existing)->socket : 0;
    if (existing_socket != 0 && !object_try_get(existing_socket)) existing_socket = 0;
    socket_unlock(socket_binding_lock());
    if (existing_socket != 0) {
        socket_private_t *existing_private = socket_private(existing_socket);
        socket_lock(&existing_socket->lock);
        if (existing_private != 0 &&
            !atomic_load_explicit(&existing_private->closed, memory_order_acquire) &&
            existing_private->tcp_state == SOCKET_TCP_SYN_RECEIVED &&
            sequence == existing_private->tcp_peer_initial_sequence) {
            reply->valid = true;
            reply->flags = NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK;
            reply->window = socket_tcp_receive_window(existing_private, true);
            reply->sequence = existing_private->tcp_initial_sequence;
            reply->acknowledgement = sequence + 1U;
            socket_unlock(&existing_socket->lock);
            object_put(existing_socket);
            object_put(listener);
            return K_OK;
        }
        socket_unlock(&existing_socket->lock);
        object_put(existing_socket);
        object_put(listener);
        return K_EAGAIN;
    }
    socket_lock(&listener->lock);
    if (!listener_private->listening ||
        atomic_load_explicit(&listener_private->closed, memory_order_acquire) ||
        atomic_load_explicit(&listener_private->accept_count, memory_order_relaxed) >=
            SOCKET_LISTEN_BACKLOG) {
        socket_unlock(&listener->lock);
        object_put(listener);
        return K_EAGAIN;
    }
    socket_unlock(&listener->lock);
    socket_lock(socket_binding_lock());
    bool available = socket_tcp6_slots_available_locked(1U);
    socket_unlock(socket_binding_lock());
    if (!available || socket_allocate(OS_AF_INET6, OS_SOCK_STREAM,
                                      SOCKET_PROTOCOL_TCP, &child) != K_OK) {
        object_put(listener);
        return K_ENOMEM;
    }
    if (peer_scale >= 0) (void)socket_stream_reserve(child, SOCKET_STREAM_TCP_CAPACITY);
    child_private = socket_private(child);
    socket_tcp_prepare_scale(child_private, false);
    child_private->tcp_scale_seen = true;
    child_private->tcp_scale_offered = peer_scale >= 0 &&
        socket_stream_capacity(child_private) > SOCKET_STREAM_BUFFER_SIZE;
    child_private->tcp_scale_negotiated = child_private->tcp_scale_offered;
    child_private->tcp_peer_scale = child_private->tcp_scale_offered ? (uint8_t)peer_scale : 0U;
    socket_lock(&listener->lock);
    child_private->tcp_keepalive = listener_private->tcp_keepalive;
    child_private->tcp_keepidle = listener_private->tcp_keepidle;
    child_private->tcp_keepintvl = listener_private->tcp_keepintvl;
    child_private->tcp_keepcnt = listener_private->tcp_keepcnt;
    socket_unlock(&listener->lock);
    child_private->bound = true;
    child_private->connected = true;
    socket_copy(child_private->local_address6, destination_address, 16U);
    child_private->local_port = destination_port;
    socket_copy(child_private->peer_address6, source_address, 16U);
    child_private->peer_port = source_port;
    child_private->tcp_peer_initial_sequence = sequence;
    child_private->tcp_initial_sequence = 0x11000000U + source_port;
    child_private->tcp_next_sequence = child_private->tcp_initial_sequence + 1U;
    child_private->tcp_send_next_sequence = child_private->tcp_initial_sequence + 1U;
    child_private->tcp_send_unacknowledged = child_private->tcp_initial_sequence;
    child_private->tcp_send_length = 0U;
    child_private->tcp_send_flags = NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK;
    child_private->tcp_send_pending = true;
    child_private->tcp_send_retransmits = 0U;
    child_private->tcp_send_deadline_tsc = socket_tcp_retransmit_deadline(x86_read_tsc());
    child_private->tcp_peer_window = SOCKET_STREAM_BUFFER_SIZE;
    child_private->tcp_sequence_valid = true;
    child_private->tcp_state = SOCKET_TCP_SYN_RECEIVED;
    child_private->peer = 0;
    child_private->listener = listener;
    object_get(listener);

    socket_lock(&listener->lock);
    if (atomic_load_explicit(&listener_private->accept_count, memory_order_relaxed) >=
        SOCKET_LISTEN_BACKLOG) {
        socket_unlock(&listener->lock);
        socket_abort_child(child);
        object_put(listener);
        return K_EAGAIN;
    }
    listener_private->accept_queue[listener_private->accept_write] = child;
    listener_private->accept_write =
        (listener_private->accept_write + 1U) % SOCKET_LISTEN_BACKLOG;
    atomic_fetch_add_explicit(&listener_private->accept_count, 1U, memory_order_release);
    socket_unlock(&listener->lock);
    (void)wake_all(&listener->waitq);
    object_notify_signaled(listener);
    object_put(listener);

    socket_lock(socket_binding_lock());
    bool registered = socket_register_tcp_locked(child, child_private);
    socket_unlock(socket_binding_lock());
    if (!registered) {
        socket_abort_child(child);
        return K_ENOMEM;
    }
    reply->valid = true;
    reply->flags = NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK;
    reply->window = socket_tcp_receive_window(child_private, true);
    reply->sequence = child_private->tcp_initial_sequence;
    reply->acknowledgement = sequence + 1U;
    return K_OK;
}

/* 消费查找引用；线上地址族验证和查找仍由入口函数负责。状态转换与唤醒
 * 共用同一个锁边界。 */
static kstatus_t socket_tcp_ack_target(
    socket_t *target, uint32_t sequence, uint32_t acknowledgement,
    uint8_t flags, uint16_t window, socket_tcp_reply_t *reply) {
    socket_private_t *private;
    bool notify = false;
    private = socket_private(target);
    if (private == 0) {
        object_put(target);
        return K_EIO;
    }
    socket_lock(&target->lock);
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) {
        socket_unlock(&target->lock);
        object_put(target);
        return K_OK;
    }
    if ((flags & NET_TCP_FLAG_RST) != 0U) {
        bool reset = socket_tcp_reset_locked(private, sequence, acknowledgement, flags, reply);
        socket_unlock(&target->lock);
        if (reset) {
            (void)wake_all(&target->waitq);
            object_notify_changed(target);
        }
        object_put(target);
        return K_OK;
    }
    if (private->tcp_state == SOCKET_TCP_SYN_RECEIVED &&
        acknowledgement == private->tcp_initial_sequence + 1U &&
        sequence == private->tcp_peer_initial_sequence + 1U) {
        private->tcp_state = SOCKET_TCP_ESTABLISHED;
        private->tcp_sequence_valid = true;
        private->tcp_next_sequence = sequence;
        private->tcp_peer_window = socket_tcp_decode_window(private, window, false);
        private->tcp_window_sequence = sequence;
        private->tcp_window_acknowledgement = acknowledgement;
        private->tcp_send_unacknowledged = acknowledgement;
        private->tcp_send_pending = false;
        private->tcp_send_length = 0U;
        private->tcp_send_flags = 0U;
        private->tcp_send_retransmits = 0U;
        private->tcp_send_deadline_tsc = 0U;
        socket_t *listener = private->listener;
        private->listener = 0;
        socket_tcp_keepalive_activity_locked(private, x86_read_tsc());
        socket_unlock(&target->lock);
        if (listener != 0) {
            (void)wake_all(&listener->waitq);
            object_notify_signaled(listener);
            object_put(listener);
        }
        object_put(target);
        return K_OK;
    }
    if (private->tcp_state == SOCKET_TCP_SYN_SENT &&
        (flags & NET_TCP_FLAG_SYN) != 0U &&
        acknowledgement == private->tcp_initial_sequence + 1U) {
        private->tcp_peer_initial_sequence = sequence;
        private->tcp_next_sequence = sequence + 1U;
        private->tcp_sequence_valid = true;
        private->tcp_state = SOCKET_TCP_ESTABLISHED;
        private->tcp_peer_window = window;
        private->tcp_window_sequence = sequence;
        private->tcp_window_acknowledgement = acknowledgement;
        private->tcp_send_unacknowledged = acknowledgement;
        private->tcp_send_pending = false;
        private->tcp_send_length = 0U;
        private->tcp_send_flags = 0U;
        private->tcp_send_retransmits = 0U;
        private->tcp_send_deadline_tsc = 0U;
        if (reply != 0) {
            reply->valid = true;
            reply->flags = NET_TCP_FLAG_ACK;
            reply->sequence = private->tcp_send_next_sequence;
            reply->acknowledgement = private->tcp_next_sequence;
            reply->window = socket_tcp_receive_window(private, false);
        }
        socket_tcp_keepalive_activity_locked(private, x86_read_tsc());
        socket_unlock(&target->lock);
        (void)wake_all(&target->waitq);
        object_notify_changed(target);
        object_put(target);
        return K_OK;
    }
    /* RFC 9293 第 3.10.7.4 节：旧 ACK 或未来 ACK 不能改变 SND.WND。对于可接受的 ACK，
     * SND.WL1/WL2 可防止重排的数据段覆盖更新的窗口通告。有符号序列差值可处理回绕；
     * 保留的在途数据始终小于序列空间的一半。 */
    if (private->tcp_state == SOCKET_TCP_ESTABLISHED &&
        (flags & NET_TCP_FLAG_ACK) != 0U &&
        acknowledgement - private->tcp_send_unacknowledged <=
            private->tcp_send_next_sequence - private->tcp_send_unacknowledged &&
        ((int32_t)(sequence - private->tcp_window_sequence) > 0 ||
         (sequence == private->tcp_window_sequence &&
          (int32_t)(acknowledgement - private->tcp_window_acknowledgement) >= 0))) {
        uint32_t decoded = socket_tcp_decode_window(private, window,
                                                     (flags & NET_TCP_FLAG_SYN) != 0U);
        if (decoded > private->tcp_peer_window) notify = true;
        private->tcp_peer_window = decoded;
        private->tcp_window_sequence = sequence;
        private->tcp_window_acknowledgement = acknowledgement;
    }
    /* 仅保留一个发送批次：只有 SND.NXT 才表示完整确认。相等性检查会拒绝未来 ACK，且序列号回绕时仍有效。 */
    if (private->tcp_state == SOCKET_TCP_ESTABLISHED &&
        private->tcp_send_pending &&
        acknowledgement == private->tcp_send_next_sequence) {
        private->tcp_send_pending = false;
        private->tcp_send_length = 0U;
        private->tcp_send_flags = 0U;
        private->tcp_send_unacknowledged = acknowledgement;
        private->tcp_send_retransmits = 0U;
        private->tcp_send_deadline_tsc = 0U;
        private->tcp_local_retry_tsc = 0U;
        notify = true;
    } else if (private->tcp_state == SOCKET_TCP_ESTABLISHED &&
               private->tcp_send_pending &&
               (private->tcp_send_flags & (NET_TCP_FLAG_SYN | NET_TCP_FLAG_FIN)) == 0U) {
        uint32_t outstanding = private->tcp_send_next_sequence -
                               private->tcp_send_unacknowledged;
        uint32_t acknowledged = acknowledgement - private->tcp_send_unacknowledged;
        /* 只允许回收此保留发送批次拥有的数据。无符号差值可在序列号回绕时拒绝重复/过期/未来 ACK。
         * SYN/FIN 会占用序列空间，但不拥有负载字节。 */
        if (outstanding == private->tcp_send_length &&
            acknowledged != 0U && acknowledged < outstanding) {
            private->tcp_send_length -= (uint16_t)acknowledged;
            socket_copy(private->tcp_send_payload,
                        private->tcp_send_payload + acknowledged,
                        private->tcp_send_length);
            private->tcp_send_unacknowledged = acknowledgement;
            private->tcp_send_retransmits = 0U;
            private->tcp_send_deadline_tsc = socket_tcp_retransmit_deadline(x86_read_tsc());
            /* 保留较早的本地重试状态：未确认后缀可能包含被设备输出回调拒绝的报文段。 */
            notify = true;
        }
    }
    bool established = private->tcp_state == SOCKET_TCP_ESTABLISHED;
    /* 零长度保活/窗口探测使用 RCV.NXT-1。为避免 ACK 循环，RCV.NXT 处的普通 ACK 保持静默；即使接收
     * 窗口仍为零，也要响应探测（RFC 9293 3.8.4/3.8.6.1）。 */
    if (established && private->tcp_sequence_valid &&
        flags == NET_TCP_FLAG_ACK &&
        sequence == private->tcp_next_sequence - 1U && reply != 0) {
        reply->valid = true;
        reply->flags = NET_TCP_FLAG_ACK;
        reply->sequence = private->tcp_send_next_sequence;
        reply->acknowledgement = private->tcp_next_sequence;
        reply->window = socket_tcp_receive_window(private, false);
    }
    if (established && sequence == private->tcp_next_sequence &&
        acknowledgement == private->tcp_send_next_sequence) {
        socket_tcp_keepalive_activity_locked(private, x86_read_tsc());
    }
    socket_unlock(&target->lock);
    if (notify) {
        (void)wake_all(&target->waitq);
        object_notify_changed(target);
    }
    object_put(target);
    return established ? K_OK : K_EAGAIN;
}

kstatus_t socket_inject_tcp_ack_ipv4_reply(uint32_t source_address,
                                           uint16_t source_port,
                                           uint32_t destination_address,
                                           uint16_t destination_port,
                                           uint32_t sequence,
                                           uint32_t acknowledgement,
                                           uint8_t flags, uint16_t window,
                                           socket_tcp_reply_t *reply) {
    socket_t *target = 0;
    if (source_address == 0U || source_port == 0U || destination_address == 0U ||
        destination_port == 0U || (flags & (NET_TCP_FLAG_ACK | NET_TCP_FLAG_RST)) == 0U) return K_EINVAL;
    if (reply != 0) socket_zero(reply, sizeof(*reply));
    socket_initialize_globals();
    socket_lock(socket_binding_lock());
    int32_t connection = socket_find_tcp_locked(source_address, source_port,
                                                 destination_address, destination_port);
    if (connection >= 0) {
        target = socket_tcp_connection_at((uint32_t)connection)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
    if (target == 0) return K_ENOENT;
    return socket_tcp_ack_target(target, sequence, acknowledgement, flags, window, reply);
}

kstatus_t socket_inject_tcp_ack_ipv6_reply(const uint8_t source_address[16],
                                           uint16_t source_port,
                                           const uint8_t destination_address[16],
                                           uint16_t destination_port,
                                           uint32_t sequence,
                                           uint32_t acknowledgement,
                                           uint8_t flags, uint16_t window,
                                           socket_tcp_reply_t *reply) {
    socket_t *target = 0;
    if (source_address == 0 || destination_address == 0 ||
        socket_address6_zero(source_address) || socket_address6_zero(destination_address) ||
        source_port == 0U || destination_port == 0U ||
        (flags & (NET_TCP_FLAG_ACK | NET_TCP_FLAG_RST)) == 0U) return K_EINVAL;
    if (reply != 0) socket_zero(reply, sizeof(*reply));
    socket_initialize_globals();
    socket_lock(socket_binding_lock());
    int32_t connection = socket_find_tcp6_locked(source_address, source_port,
                                                 destination_address, destination_port);
    if (connection >= 0) {
        target = socket_tcp6_connection_at((uint32_t)connection)->socket;
        if (!object_try_get(target)) target = 0;
    }
    socket_unlock(socket_binding_lock());
    if (target == 0) return K_ENOENT;
    return socket_tcp_ack_target(target, sequence, acknowledgement, flags, window, reply);
}

kstatus_t socket_inject_tcp_ack_ipv4_window(uint32_t source_address,
                                            uint16_t source_port,
                                            uint32_t destination_address,
                                            uint16_t destination_port,
                                            uint32_t sequence,
                                            uint32_t acknowledgement,
                                            uint8_t flags, uint16_t window) {
    return socket_inject_tcp_ack_ipv4_reply(source_address, source_port,
                                            destination_address, destination_port,
                                            sequence, acknowledgement, flags,
                                            window, 0);
}

kstatus_t socket_inject_tcp_ack_ipv6_window(const uint8_t source_address[16],
                                            uint16_t source_port,
                                            const uint8_t destination_address[16],
                                            uint16_t destination_port,
                                            uint32_t sequence,
                                            uint32_t acknowledgement,
                                            uint8_t flags, uint16_t window) {
    return socket_inject_tcp_ack_ipv6_reply(source_address, source_port,
                                            destination_address, destination_port,
                                            sequence, acknowledgement, flags,
                                            window, 0);
}
