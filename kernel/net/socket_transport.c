#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/net_core.h>
#include <kernel/socket.h>

#include "socket_internal.h"
#include "socket_model.h"
#include "tcp_output.h"
#include "tcp_retry.h"

/* REFACTOR_P8_SOCKET_TRANSPORT_OWNER：TCP 重传轮询。 */

#if OS_DEBUG_SERIAL
static uint32_t g_debug_tcp_poll_logs;
#endif

static void socket_tcp_keepalive_output_failed_locked(
    socket_private_t *private, uint64_t deadline, uint64_t now_tsc) {
    // 本地 TX 背压不能证明对端已失效。不要撤销已重置此代数的并发 ACK/选项变更。
    if (!atomic_load_explicit(&private->closed, memory_order_acquire) &&
        private->tcp_state == SOCKET_TCP_ESTABLISHED &&
        private->tcp_keep_deadline_tsc == deadline &&
        private->tcp_keep_probes != 0U) {
        --private->tcp_keep_probes;
        uint64_t ticks = x86_timeout_ns_to_tsc(1000000000ULL);
        private->tcp_keep_deadline_tsc = ticks > UINT64_MAX - now_tsc ?
                                       UINT64_MAX : now_tsc + ticks;
    }
}

/* 在一次注册表保护锁下跳过空槽。轮询期间只固定一个 socket；无需复制表快照或分配内存。
 * 插入到游标之后的连接会在下一次常规轮询中处理。 */
static socket_t *socket_tcp_poll_next(uint32_t *cursor, bool ipv6,
                                      uint32_t *pass_limit) {
    socket_t *target = 0;
    socket_lock(socket_binding_lock());
    uint32_t capacity = socket_registry_capacity_locked();
    /* 在首次常规加锁时为两个地址族捕获同一个边界。本轮不会因扩容而无限延长；
     * 缩容时仍会按当前后备存储限制读取范围。新增的高位槽会在下一次轮询处理。 */
    if (*pass_limit == 0U) *pass_limit = capacity;
    uint32_t limit = capacity < *pass_limit ? capacity : *pass_limit;
    while (*cursor < limit) {
        uint32_t index = (*cursor)++;
        if (ipv6) {
            const socket_tcp6_connection_t *connection = socket_tcp6_connection_at(index);
            target = connection != 0 ? connection->socket : 0;
        } else {
            const socket_tcp_connection_t *connection = socket_tcp_connection_at(index);
            target = connection != 0 ? connection->socket : 0;
        }
        if (target != 0 && object_try_get(target)) break;
        target = 0;
    }
    socket_unlock(socket_binding_lock());
    return target;
}

/* 每次处理一个已固定的 socket，每轮最多发送一个延迟的纯 ACK。关闭操作会注销 socket；
 * 已固定的扫描在此检查 closed 状态，不会重新启用它。输出前在保护锁下清除标志：
 * 旧的成功结果不能抹掉解锁输出期间新排队的失败。数据在途定时器不受影响。
 * 调用和返回时均持有目标锁；仅在实际外发 ACK 时释放并重新获取。
 * 空闲连接和未到期 ACK 直接复用本轮发送状态检查的保护，避免重复加锁。 */
static void socket_tcp_poll_ack_locked(socket_t *target, socket_private_t *private,
    uint64_t now, bool ipv6, socket_tcp_ipv4_output_fn output4, void *context4,
    socket_tcp_ipv6_output_fn output6, void *context6) {
    uint32_t local4, peer4, sequence, acknowledgement;
    uint8_t local6[16], peer6[16];
    uint16_t local_port, peer_port, window;
    if (atomic_load_explicit(&private->closed, memory_order_acquire) ||
        private->tcp_state != SOCKET_TCP_ESTABLISHED || private->peer != 0) {
        private->tcp_ack_pending = false;
        private->tcp_ack_retry_tsc = 0U;
    }
    if (!private->tcp_ack_pending ||
        (int64_t)(now - private->tcp_ack_retry_tsc) < 0) {
        return;
    }
    private->tcp_ack_pending = false;
    private->tcp_ack_retry_tsc = 0U;
    sequence = private->tcp_send_next_sequence;
    acknowledgement = private->tcp_next_sequence;
    window = socket_tcp_receive_window(private, false);
    local_port = private->local_port;
    peer_port = private->peer_port;
    local4 = private->local_address;
    peer4 = private->peer_address;
    if (ipv6) {
        socket_copy(local6, private->local_address6, sizeof(local6));
        socket_copy(peer6, private->peer_address6, sizeof(peer6));
    }
    socket_unlock(&target->lock);
    kstatus_t status = K_ENOENT;
    if (ipv6 && output6 != 0)
        status = output6(context6, local6, local_port, peer6, peer_port,
                         sequence, acknowledgement, NET_TCP_FLAG_ACK, window, 0, 0U);
    else if (!ipv6 && output4 != 0)
        status = output4(context4, local4, local_port, peer4, peer_port,
                         sequence, acknowledgement, NET_TCP_FLAG_ACK, window, 0, 0U);
    socket_lock(&target->lock);
    if (status == K_EAGAIN) {
        socket_tcp_ack_retry_locked(private, status, x86_read_tsc(),
                                    x86_timeout_ns_to_tsc(20000000ULL));
    }
}

void socket_tcp_poll(uint64_t now_tsc) {
    socket_tcp_ipv4_output_fn output4 = 0;
    socket_tcp_ipv6_output_fn output6 = 0;
    void *context4 = 0;
    void *context6 = 0;
    socket_initialize_globals();
    socket_tcp_output_get(&output4, &context4);
    socket_tcp6_output_get(&output6, &context6);
    uint32_t pass_limit = 0U;

    for (unsigned family = 0U; family < 2U; ++family) {
        bool ipv6 = family != 0U;
        bool output_available = ipv6 ? output6 != 0 : output4 != 0;
        for (uint32_t cursor = 0U;;) {
            socket_t *target = socket_tcp_poll_next(&cursor, ipv6, &pass_limit);
            if (target == 0) break;
            socket_private_t *private = socket_private(target);
            if (private == 0) {
                object_put(target);
                continue;
            }
            uint8_t payload[SOCKET_MAX_PAYLOAD];
            uint8_t source6[16], destination6[16];
            uint32_t source4 = 0U, destination4 = 0U;
            uint16_t source_port = 0U, destination_port = 0U;
            uint16_t payload_length = 0U, window = 0U;
            uint32_t sequence = 0U, acknowledgement = 0U;
            uint32_t send_end_sequence = 0U;
            uint8_t flags = 0U;
            uint64_t keep_deadline = 0U;
            bool transmit = false, failed = false, keepalive = false, persist = false;

            socket_lock(&target->lock);
            socket_tcp_poll_ack_locked(target, private, now_tsc, ipv6,
                                       output4, context4, output6, context6);
            if (atomic_load_explicit(&private->closed, memory_order_acquire)) {
                socket_unlock(&target->lock);
                object_put(target);
                continue;
            }
            bool persist_wait = socket_tcp_persist_wait_locked(private, now_tsc, &persist);
            if (persist) {
                flags = NET_TCP_FLAG_ACK;
                sequence = private->tcp_send_next_sequence - 1U;
                if (private->tcp_send_pending && private->tcp_send_length != 0U) {
                    /* 重传一个保留字节。重新打开的接收端可通过常规部分 ACK 路径确认它。 */
                    sequence = private->tcp_send_unacknowledged;
                    payload[0] = private->tcp_send_payload[0];
                    payload_length = 1U;
                }
                transmit = true;
            } else if (!persist_wait && private->tcp_state == SOCKET_TCP_ESTABLISHED &&
                private->peer == 0 && private->tcp_fin_pending &&
                !private->tcp_fin_sent && !private->tcp_send_pending) {
                /* FIN 占用一个序列号；IPv4 和 IPv6 均使用常规待处理发送重试路径。 */
                private->tcp_fin_pending = false;
                private->tcp_fin_sent = true;
                private->tcp_send_pending = true;
                private->tcp_local_retry_tsc = 0U;
                private->tcp_send_unacknowledged = private->tcp_send_next_sequence++;
                private->tcp_send_length = 0U;
                private->tcp_send_flags = NET_TCP_FLAG_ACK | NET_TCP_FLAG_FIN;
                private->tcp_send_retransmits = 0U;
                private->tcp_send_deadline_tsc = socket_tcp_retransmit_deadline(now_tsc);
                transmit = true;
            } else if (!persist_wait && (private->tcp_state == SOCKET_TCP_SYN_SENT ||
                        private->tcp_state == SOCKET_TCP_SYN_RECEIVED ||
                        private->tcp_state == SOCKET_TCP_ESTABLISHED) &&
                       private->tcp_send_pending && private->tcp_send_deadline_tsc != 0U) {
                bool network_due = (int64_t)(now_tsc - private->tcp_send_deadline_tsc) >= 0;
                if (network_due &&
                    private->tcp_send_retransmits >= SOCKET_TCP_MAX_RETRANSMITS) {
                    socket_tcp_fail_locked(private, K_ETIMEDOUT);
                    failed = true;
                } else if (network_due || socket_tcp_local_retry_due(private, now_tsc)) {
                    if (network_due) {
                        ++private->tcp_send_retransmits;
                        private->tcp_send_deadline_tsc = socket_tcp_retransmit_deadline(now_tsc);
                    }
                    private->tcp_local_retry_tsc = 0U;
                    transmit = true;
                }
            }
            if (!persist_wait && !transmit && !failed &&
                socket_tcp_keepalive_poll_locked(private, now_tsc, &failed)) {
                /* 在 SND.NXT 之前探测，不消耗序列空间。 */
                flags = NET_TCP_FLAG_ACK;
                sequence = private->tcp_send_next_sequence - 1U;
                keepalive = transmit = true;
                keep_deadline = private->tcp_keep_deadline_tsc;
            }
            if (transmit) {
                source_port = private->local_port;
                destination_port = private->peer_port;
                if (ipv6) {
                    socket_copy(source6, private->local_address6, sizeof(source6));
                    socket_copy(destination6, private->peer_address6, sizeof(destination6));
                } else {
                    source4 = private->local_address;
                    destination4 = private->peer_address;
                }
                acknowledgement = private->tcp_next_sequence;
                window = socket_tcp_receive_window(private, false);
                if (!keepalive && !persist) {
                    send_end_sequence = private->tcp_send_next_sequence;
                    flags = private->tcp_send_flags;
                    sequence = private->tcp_send_unacknowledged;
                    payload_length = private->tcp_send_length;
                    socket_copy(payload, private->tcp_send_payload, payload_length);
                    if (private->tcp_state == SOCKET_TCP_SYN_SENT) {
                        acknowledgement = 0U;
                        window = socket_tcp_receive_window(private, true);
                    } else if (private->tcp_state == SOCKET_TCP_SYN_RECEIVED) {
                        acknowledgement = private->tcp_peer_initial_sequence + 1U;
                        window = socket_tcp_receive_window(private, true);
                    }
                }
            }
            uint16_t peer_mss = private->tcp_peer_mss;
#if OS_DEBUG_SERIAL
            uint32_t retries = private->tcp_send_retransmits;
#endif
            socket_unlock(&target->lock);

            if (failed) {
                (void)wake_all(&target->waitq);
                object_notify_changed(target);
            } else if (transmit && output_available) {
                kstatus_t status = ipv6 ?
                    socket_tcp_output6_batch(output6, context6, source6, source_port,
                        destination6, destination_port, sequence, acknowledgement,
                        flags, window, payload, payload_length, peer_mss) :
                    socket_tcp_output4_batch(output4, context4, source4, source_port,
                        destination4, destination_port, sequence, acknowledgement,
                        flags, window, payload, payload_length, peer_mss);
                /* 探测输出失败不代表数据发送确认丢失。保留退避后的截止时间；不得立即重试，也不能让过期的
                 * 完成事件消费或污染保留的发送内容。 */
                if (!persist && (!keepalive || status != K_OK)) {
                    bool output_failed = false;
                    socket_lock(&target->lock);
                    if (keepalive) {
                        socket_tcp_keepalive_output_failed_locked(private, keep_deadline, now_tsc);
                    } else {
                        output_failed = socket_tcp_local_output_locked(private, sequence, send_end_sequence, status, now_tsc);
                    }
                    socket_unlock(&target->lock);
                    if (output_failed) {
                        (void)wake_all(&target->waitq);
                        object_notify_changed(target);
                    }
                }
#if OS_DEBUG_SERIAL
                if (!ipv6 && g_debug_tcp_poll_logs < 32U) {
                    kernel_serial_printf_serial_only(
                        "OS_TCP_POLL src=%x:%u dst=%x:%u flags=%x retry=%u status=%lld\r\n",
                        source4, source_port, destination4, destination_port,
                        flags, retries, (long long)status);
                    ++g_debug_tcp_poll_logs;
                }
#endif
            } else if (keepalive) {
                socket_lock(&target->lock);
                socket_tcp_keepalive_output_failed_locked(private, keep_deadline, now_tsc);
                socket_unlock(&target->lock);
            }
            object_put(target);
        }
    }
}
