#include <arch/x86_64/cpu.h>
#include <kernel/completion_port.h>
#include <kernel/deferred.h>
#include <kernel/kmem.h>
#include <kernel/net_core.h>
#include <kernel/socket.h>
#include <kernel/wait.h>

#include "socket_internal.h"
#include "socket_model.h"
#include "tcp_output.h"
#include "tcp_retry.h"

/* REFACTOR_P8_SOCKET_IO_OWNER：socket 发送、接收和异步 I/O。 */

static kstatus_t socket_send_stream(socket_t *socket,
                                    socket_private_t *private,
                                    const void *buffer,
                                    size_t length,
                                    uint64_t *bytes) {
    socket_t *peer;
    socket_tcp_ipv4_output_fn output4 = 0;
    socket_tcp_ipv6_output_fn output6 = 0;
    void *output4_context = 0;
    void *output6_context = 0;
    bool wire_output;

    if (length > SOCKET_STREAM_BUFFER_SIZE) return K_EAGAIN;
    socket_tcp_output_get(&output4, &output4_context);
    socket_tcp6_output_get(&output6, &output6_context);
    socket_lock(&socket->lock);

    if (atomic_load_explicit(&private->closed, memory_order_acquire)) {
        socket_unlock(&socket->lock);
        *bytes = 0U;
        return K_EDEVREMOVED;
    }

    if (atomic_load_explicit(&private->send_shutdown, memory_order_acquire)) {
        socket_unlock(&socket->lock);
        *bytes = 0U;
        return K_EPIPE;
    }

    if ((socket->family == OS_AF_INET4 ||
         socket->family == OS_AF_INET6) &&
        private->peer == 0 &&
        private->tcp_state != SOCKET_TCP_ESTABLISHED) {
        kstatus_t status;

        if (private->tcp_send_failed) {
            status = K_EIO;
            int reset = atomic_load_explicit(&private->tcp_error, memory_order_acquire);
            if (reset != 0) status = reset;
        } else if (private->tcp_state == SOCKET_TCP_SYN_SENT ||
                   private->tcp_state == SOCKET_TCP_SYN_RECEIVED) {
            status = K_EAGAIN;
        } else {
            status = K_EDEVREMOVED;
        }

        socket_unlock(&socket->lock);
        *bytes = 0U;
        return status;
    }

    wire_output =
        (socket->family == OS_AF_INET4 ||
         socket->family == OS_AF_INET6) &&
        private->peer == 0 &&
        private->tcp_state == SOCKET_TCP_ESTABLISHED;

    if (wire_output) {
        if (length == 0U) {
            socket_unlock(&socket->lock);
            *bytes = 0U;
            return K_OK;
        }
        /* 在现有重传缓冲区中保留有界的在途数据。TLS 写入无需每发送一个 MSS
         * 都等待一次往返。 */
        uint16_t peer_mss = private->tcp_peer_mss;
        size_t room = socket_tcp_send_room_locked(socket->family, private);
        if (length > room) length = room;
        if (length == 0U) {
            kstatus_t status = private->tcp_send_failed ? K_EIO : K_EAGAIN;
            if (!private->tcp_send_failed && private->tcp_peer_window == 0U)
                socket_tcp_persist_arm_locked(private, x86_read_tsc());
            socket_unlock(&socket->lock);
            *bytes = 0U;
            return status;
        }

        if ((socket->family == OS_AF_INET4 && output4 == 0) ||
            (socket->family == OS_AF_INET6 && output6 == 0)) {
            socket_unlock(&socket->lock);
            *bytes = 0U;
            return K_EAGAIN;
        }

        uint32_t sequence_wire = private->tcp_send_next_sequence;
        size_t retained = private->tcp_send_pending ? private->tcp_send_length : 0U;
        if (!private->tcp_send_pending) {
            private->tcp_send_unacknowledged = sequence_wire;
            private->tcp_send_retransmits = 0U;
            private->tcp_local_retry_tsc = 0U;
            private->tcp_send_deadline_tsc =
                socket_tcp_retransmit_deadline(x86_read_tsc());
        }
        private->tcp_send_next_sequence += (uint32_t)length;
        private->tcp_send_length = (uint16_t)(retained + length);
        private->tcp_send_flags = NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH;
        private->tcp_send_pending = true;
        socket_copy(private->tcp_send_payload + retained, buffer, length);

        uint16_t source_port_wire = private->local_port;
        uint16_t destination_port_wire = private->peer_port;
        uint32_t acknowledgement_wire = private->tcp_next_sequence;
        uint16_t window_wire = socket_tcp_receive_window(private, false);
        uint32_t source_address_wire = private->local_address;
        uint32_t destination_address_wire = private->peer_address;
        uint8_t source_address6_wire[16];
        uint8_t destination_address6_wire[16];
        socket_copy(source_address6_wire, private->local_address6, 16U);
        socket_copy(destination_address6_wire, private->peer_address6, 16U);
        socket_unlock(&socket->lock);

        kstatus_t output_status;
        if (socket->family == OS_AF_INET4) {
            output_status = socket_tcp_output4_batch(output4,
                output4_context, source_address_wire, source_port_wire,
                destination_address_wire, destination_port_wire,
                sequence_wire, acknowledgement_wire,
                NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH, window_wire,
                buffer, length, peer_mss);
        } else {
            output_status = socket_tcp_output6_batch(output6,
                output6_context, source_address6_wire, source_port_wire,
                destination_address6_wire, destination_port_wire,
                sequence_wire, acknowledgement_wire,
                NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH, window_wire,
                buffer, length, peer_mss);
        }

        socket_lock(&socket->lock);
        bool failed = socket_tcp_local_output_locked(private, sequence_wire, sequence_wire + (uint32_t)length,
                                                      output_status, x86_read_tsc());
        int error = atomic_load_explicit(&private->tcp_error, memory_order_acquire);
        socket_unlock(&socket->lock);
        if (failed) {
            (void)wake_all(&socket->waitq);
            object_notify_changed(socket);
        }

        /* SND.NXT 和保留的重传负载已经接管此次写入。ARP/TX 背压会将其留在队列中，
         * 由 socket_tcp_poll 处理；若报告接受字节数为零，调用方会在排队写入完成后
         * 使用新的序列号重发相同字节。 */
        if (error != 0) {
            *bytes = 0U;
            return error;
        }

        *bytes = length;
        return K_OK;
    }

    peer = private->peer;
    if (peer != 0 && !object_try_get(peer)) peer = 0;
    uint32_t source_address = private->local_address;
    uint16_t source_port = private->local_port;
    socket_unlock(&socket->lock);

    if (peer == 0) {
        *bytes = 0U;
        return K_EDEVREMOVED;
    }

    socket_private_t *peer_private = socket_private(peer);
    if (peer_private == 0) {
        object_put(peer);
        *bytes = 0U;
        return K_EIO;
    }

    socket_lock(&peer->lock);
    uint32_t count = atomic_load_explicit(
        &peer_private->stream_count, memory_order_relaxed);
    if (atomic_load_explicit(&peer_private->closed, memory_order_acquire) ||
        atomic_load_explicit(&peer_private->receive_shutdown, memory_order_acquire)) {
        socket_unlock(&peer->lock);
        object_put(peer);
        *bytes = 0U;
        return K_EPIPE;
    }
    if (length > socket_stream_capacity(peer_private) ||
        count > socket_stream_capacity(peer_private) - length) {
        socket_unlock(&peer->lock);
        object_put(peer);
        *bytes = 0U;
        return K_EAGAIN;
    }

    peer_private->stream_write = socket_stream_write_locked(peer_private, buffer, length);
    atomic_store_explicit(&peer_private->stream_count,
                          count + (uint32_t)length,
                          memory_order_release);
    peer_private->peer_address = source_address;
    peer_private->peer_port = source_port;
    socket_unlock(&peer->lock);
    (void)wake_all(&peer->waitq);
    object_notify_signaled(peer);
    object_put(peer);
    *bytes = length;
    return K_OK;
}

kstatus_t socket_send(socket_t *socket, const void *buffer, size_t length,
                      uint32_t address, uint16_t port, uint64_t *bytes) {
    socket_private_t *private = socket_private(socket);
    socket_udp_ipv4_output_fn output = 0;
    void *output_context = 0;

    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET4 || private == 0 ||
        bytes == 0 || (buffer == 0 && length != 0U) ||
        length > SOCKET_MAX_PAYLOAD) return K_EINVAL;
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) {
        *bytes = 0U;
        return K_EDEVREMOVED;
    }
    if (atomic_load_explicit(&private->send_shutdown, memory_order_acquire)) {
        *bytes = 0U;
        return K_EPIPE;
    }
    if (socket->type == OS_SOCK_STREAM) {
        return socket_send_stream(socket, private, buffer, length, bytes);
    }

    socket_lock(&socket->lock);
    if (address == 0U && port == 0U && private->connected) {
        address = private->peer_address;
        port = private->peer_port;
    }
    uint32_t source_address = private->bound ?
        private->local_address : 0x7F000001U;
    uint16_t source_port = private->bound ? private->local_port : 0U;
    socket_unlock(&socket->lock);

    if (source_port == 0U) {
        *bytes = 0U;
        return K_EINVAL;
    }
    if (address == 0x7F000001U || address == source_address) {
        kstatus_t status = socket_inject_udp(
            source_address, source_port, address, port, buffer, length);
        *bytes = status == K_OK ? length : 0U;
        return status;
    }

    socket_udp4_output_get(&output, &output_context);
    if (output != 0) {
        kstatus_t output_status = output(
            output_context, source_address, source_port,
            address, port, buffer, length);
        *bytes = output_status == K_OK ? length : 0U;
        return output_status;
    }

    kstatus_t status = socket_inject_udp(
        source_address, source_port, address, port, buffer, length);
    *bytes = status == K_OK ? length : 0U;
    return status;
}

kstatus_t socket_send_ipv6(socket_t *socket, const void *buffer, size_t length,
                           const uint8_t address[16], uint16_t port,
                           uint64_t *bytes) {
    socket_private_t *private = socket_private(socket);
    socket_udp_ipv6_output_fn output = 0;
    void *output_context = 0;
    uint8_t destination[16];
    uint16_t destination_port = port;
    static const uint8_t loopback6[16] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 1,
    };

    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET6 || private == 0 || bytes == 0 ||
        (buffer == 0 && length != 0U) || length > SOCKET_MAX_PAYLOAD) {
        return K_EINVAL;
    }
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) {
        *bytes = 0U;
        return K_EDEVREMOVED;
    }
    if (atomic_load_explicit(&private->send_shutdown, memory_order_acquire)) {
        *bytes = 0U;
        return K_EPIPE;
    }
    if (socket->type == OS_SOCK_STREAM) {
        if ((address != 0 && port != 0U) &&
            (!private->connected ||
             !socket_address6_equal(address, private->peer_address6) ||
             port != private->peer_port)) {
            *bytes = 0U;
            return K_EINVAL;
        }
        return socket_send_stream(socket, private, buffer, length, bytes);
    }

    socket_lock(&socket->lock);
    if (address == 0 && port == 0U && private->connected) {
        socket_copy(destination, private->peer_address6, 16U);
        destination_port = private->peer_port;
    } else if (address != 0 && port != 0U) {
        socket_copy(destination, address, 16U);
    } else {
        socket_unlock(&socket->lock);
        *bytes = 0U;
        return K_EINVAL;
    }
    uint8_t source[16];
    if (private->bound) {
        socket_copy(source, private->local_address6, 16U);
    } else {
        socket_copy(source, loopback6, 16U);
    }
    uint16_t source_port = private->bound ? private->local_port : 0U;
    socket_unlock(&socket->lock);

    if (source_port == 0U) {
        *bytes = 0U;
        return K_EINVAL;
    }
    if (socket_address6_equal(destination, loopback6) ||
        socket_address6_equal(destination, source)) {
        kstatus_t status = socket_inject_udp_ipv6(
            source, source_port, destination, destination_port,
            buffer, length);
        *bytes = status == K_OK ? length : 0U;
        return status;
    }

    socket_udp6_output_get(&output, &output_context);
    if (output != 0) {
        kstatus_t output_status = output(
            output_context, source, source_port, destination,
            destination_port, buffer, length);
        *bytes = output_status == K_OK ? length : 0U;
        return output_status;
    }

    kstatus_t status = socket_inject_udp_ipv6(
        source, source_port, destination, destination_port, buffer, length);
    *bytes = status == K_OK ? length : 0U;
    return status;
}

static void socket_async_send_work(void *argument) {
    socket_async_send_t *request = (socket_async_send_t *)argument;
    os_completion_entry_t completion = {0};
    uint64_t bytes = 0U;
    kstatus_t status;

    if (request == 0) return;
    status = request->ipv6 ?
        socket_send_ipv6(request->socket, request->payload, request->length,
                         request->address6, request->port, &bytes) :
        socket_send(request->socket, request->payload, request->length,
                    request->address, request->port, &bytes);
    completion.user_key = request->user_key;
    completion.status = status;
    completion.bytes_done = bytes;
    completion.request_id = request->request_id;
    (void)completion_port_post(request->completion_port, &completion);
    object_put(request->completion_port);
    object_put(request->socket);
    kfree(request);
}

static kstatus_t socket_send_async_common(socket_t *socket, const void *buffer, size_t length,
                            uint32_t address, const uint8_t address6[16], uint16_t port,
                            struct completion_port *completion_port,
                            uint64_t user_key, uint64_t *request_id) {
    socket_async_send_t *request;
    socket_private_t *private;
    bool socket_ref;
    bool port_ref;

    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        (buffer == 0 && length != 0U) || length > SOCKET_MAX_PAYLOAD ||
        completion_port == 0 || request_id == 0 ||
        ((completion_port_t *)completion_port)->object.type !=
            KOBJECT_TYPE_COMPLETION_PORT) return K_EINVAL;
    private = socket_private(socket);
    if (private == 0 ||
        atomic_load_explicit(&private->closed, memory_order_acquire)) {
        return K_EDEVREMOVED;
    }
    if (atomic_load_explicit(&private->send_shutdown, memory_order_acquire)) {
        return K_EPIPE;
    }

    /* length is bounded by SOCKET_MAX_PAYLOAD above. Do not allocate or clear
     * the unused maximum payload for every small asynchronous send. */
    request = (socket_async_send_t *)kmalloc(sizeof(*request) + length, 0);
    if (request == 0) return K_ENOMEM;
    socket_zero(request, sizeof(*request));
    socket_ref = object_try_get(socket);
    port_ref = object_try_get(completion_port);
    if (!socket_ref || !port_ref) {
        if (port_ref) object_put(completion_port);
        if (socket_ref) object_put(socket);
        kfree(request);
        return K_ECANCELED;
    }
    request->socket = socket;
    request->completion_port = (completion_port_t *)completion_port;
    request->length = length;
    request->address = address;
    request->ipv6 = address6 != 0;
    if (address6 != 0) socket_copy(request->address6, address6, 16U);
    request->port = port;
    request->user_key = user_key;
    request->request_id = socket_next_async_request_id();
    socket_copy(request->payload, buffer, length);
    /* The worker may finish and free request immediately after publication. */
    uint64_t assigned_id = request->request_id;
    if (!deferred_schedule(socket_async_send_work, request)) {
        object_put(completion_port);
        object_put(socket);
        kfree(request);
        return K_EBUSY;
    }
    *request_id = assigned_id;
    return K_OK;
}

kstatus_t socket_send_async(socket_t *socket, const void *buffer, size_t length,
                            uint32_t address, uint16_t port,
                            struct completion_port *completion_port,
                            uint64_t user_key, uint64_t *request_id) {
    return socket_send_async_common(socket, buffer, length, address, 0, port,
                                    completion_port, user_key, request_id);
}

kstatus_t socket_send_async_ipv6(socket_t *socket, const void *buffer,
                                 size_t length, const uint8_t address[16],
                                 uint16_t port,
                                 struct completion_port *completion_port,
                                 uint64_t user_key, uint64_t *request_id) {
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET6 || socket->type != OS_SOCK_DGRAM ||
        address == 0 || port == 0U) return K_EINVAL;
    return socket_send_async_common(socket, buffer, length, 0U, address, port,
                                    completion_port, user_key, request_id);
}

/* 消费回环流后恢复对端写额度；调用方在接收锁内保留引用，解锁后通知。 */
static void socket_receive_notify_peer(socket_t *peer) {
    if (peer == 0) return;
    (void)wake_all(&peer->waitq);
    object_notify_changed(peer);
    object_put(peer);
}

/* 调用方持有接收 socket 的锁。两种地址族共用消费、PEEK 和写额度唤醒事务；
 * 对端引用在锁内固定，通知和释放由调用方解锁后执行。 */
static size_t socket_receive_stream_locked(socket_private_t *private,
                                           void *buffer, size_t length,
                                           uint32_t count, bool peek,
                                           socket_t **notify_peer,
                                           bool *window_update) {
    size_t copied = count < length ? count : length;
    uint32_t read_index = socket_stream_read_locked(private, buffer, copied);
    *notify_peer = 0;
    *window_update = !peek && private->peer == 0 &&
        private->tcp_state == SOCKET_TCP_ESTABLISHED &&
        private->tcp_sequence_valid && copied != 0U &&
        count >= socket_stream_capacity(private) * 3U / 4U;
    if (!peek) {
        private->stream_read = read_index;
        atomic_store_explicit(&private->stream_count, count - (uint32_t)copied,
                              memory_order_release);
        if (copied != 0U && private->peer != 0 && object_try_get(private->peer))
            *notify_peer = private->peer;
    }
    return copied;
}

static bool socket_receive_ready(void *context) {
    socket_wait_context_t *wait = (socket_wait_context_t *)context;
    if (wait == 0 || wait->private == 0) return false;
    return atomic_load_explicit(&wait->private->datagram_count,
                                memory_order_acquire) != 0U ||
           atomic_load_explicit(&wait->private->stream_count,
                                memory_order_acquire) != 0U ||
           atomic_load_explicit(&wait->private->receive_shutdown,
                                memory_order_acquire) ||
           atomic_load_explicit(&wait->private->closed,
                                memory_order_acquire) ||
           atomic_load_explicit(&wait->private->peer_closed,
                                memory_order_acquire) ||
           atomic_load_explicit(&wait->private->tcp_error, memory_order_acquire) != 0;
}

static kstatus_t socket_recv_ipv4_impl(socket_t *socket, void *buffer,
                                        size_t length,
                                        socket_ipv4_endpoint_t *source,
                                        uint64_t timeout_ns,
                                        uint64_t *bytes, bool peek) {
    socket_private_t *private = socket_private(socket);

    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET4 || private == 0 || source == 0 ||
        bytes == 0 || (buffer == 0 && length != 0U)) return K_EINVAL;

    socket_wait_context_t wait = {.socket = socket, .private = private};
    for (;;) {
        if (atomic_load_explicit(&private->receive_shutdown, memory_order_acquire)) {
            *bytes = 0U;
            source->address = 0U;
            source->port = 0U;
            source->reserved = 0U;
            return K_OK;
        }
        kstatus_t status = wait_on_queue(&socket->waitq,
                                         socket_receive_ready,
                                         &wait, timeout_ns);
        if (status != K_OK) return status;
        socket_lock(&socket->lock);

        if (atomic_load_explicit(&private->receive_shutdown, memory_order_acquire)) {
            private->datagram_read = private->datagram_write = 0U;
            private->stream_read = private->stream_write = 0U;
            atomic_store_explicit(&private->datagram_count, 0U,
                                  memory_order_release);
            atomic_store_explicit(&private->stream_count, 0U,
                                  memory_order_release);
            source->address = 0U;
            source->port = 0U;
            source->reserved = 0U;
            socket_unlock(&socket->lock);
            *bytes = 0U;
            return K_OK;
        }

        if (socket->type == OS_SOCK_STREAM) {
            uint32_t count = atomic_load_explicit(
                &private->stream_count, memory_order_relaxed);
            if (count != 0U) {
                socket_t *notify_peer = 0;
                bool window_update;
                size_t copy_length = socket_receive_stream_locked(
                    private, buffer, length, count, peek, &notify_peer, &window_update);
                source->address = private->peer_address;
                source->port = private->peer_port;
                source->reserved = 0U;
                *bytes = copy_length;

                uint32_t update_source = private->local_address;
                uint32_t update_destination = private->peer_address;
                uint16_t update_source_port = private->local_port;
                uint16_t update_destination_port = private->peer_port;
                uint32_t update_sequence = private->tcp_send_next_sequence;
                uint32_t update_acknowledgement = private->tcp_next_sequence;
                uint16_t update_window = window_update ? socket_tcp_window_for_count(
                    private, count - (uint32_t)copy_length, false) : 0U;
                socket_unlock(&socket->lock);
                socket_receive_notify_peer(notify_peer);

                if (window_update) {
                    socket_tcp_ipv4_output_fn output = 0;
                    void *output_context = 0;
                    socket_tcp_output_get(&output, &output_context);
                    if (output != 0) {
                        kstatus_t output_status = output(output_context, update_source,
                                     update_source_port, update_destination,
                                     update_destination_port, update_sequence,
                                     update_acknowledgement, NET_TCP_FLAG_ACK,
                                     update_window, 0, 0U);
                        socket_tcp_ack_output_ipv4(update_source, update_source_port,
                            update_destination, update_destination_port,
                            NET_TCP_FLAG_ACK, output_status);
                    }
                }
                return K_OK;
            }

            int error = atomic_load_explicit(&private->tcp_error, memory_order_acquire);
            if (error != 0) {
                socket_unlock(&socket->lock);
                return error;
            }
            bool closed =
                atomic_load_explicit(&private->closed,
                                     memory_order_acquire) ||
                atomic_load_explicit(&private->peer_closed,
                                     memory_order_acquire);
            socket_unlock(&socket->lock);
            if (closed) {
                *bytes = 0U;
                source->address = private->peer_address;
                source->port = private->peer_port;
                source->reserved = 0U;
                return K_OK;
            }
            continue;
        }

        uint32_t count = atomic_load_explicit(
            &private->datagram_count, memory_order_relaxed);
        if (count != 0U) {
            socket_datagram_t *datagram =
                &private->datagrams[private->datagram_read];
            uint16_t datagram_length = datagram->length;
            size_t copy_length = datagram_length > length ?
                length : datagram_length;
            if (copy_length != 0U) {
                socket_copy(buffer, datagram->payload, copy_length);
            }
            source->address = datagram->source_address;
            source->port = datagram->source_port;
            source->reserved = 0U;
            *bytes = copy_length;
            if (!peek) {
                private->datagram_read =
                    (private->datagram_read + 1U) % SOCKET_RX_QUEUE_DEPTH;
                atomic_store_explicit(&private->datagram_count, count - 1U,
                                      memory_order_release);
            }
            socket_unlock(&socket->lock);
            return datagram_length > length ? K_EAGAIN : K_OK;
        }

        bool closed = atomic_load_explicit(&private->closed,
                                           memory_order_acquire);
        socket_unlock(&socket->lock);
        if (closed) return K_EDEVREMOVED;
    }
}

kstatus_t socket_recv(socket_t *socket, void *buffer, size_t length,
                      socket_ipv4_endpoint_t *source, uint64_t timeout_ns,
                      uint64_t *bytes) {
    return socket_recv_ipv4_impl(socket, buffer, length, source, timeout_ns,
                                 bytes, false);
}

kstatus_t socket_recv_peek(socket_t *socket, void *buffer, size_t length,
                           socket_ipv4_endpoint_t *source,
                           uint64_t timeout_ns, uint64_t *bytes) {
    return socket_recv_ipv4_impl(socket, buffer, length, source, timeout_ns,
                                 bytes, true);
}

static kstatus_t socket_recv_ipv6_impl(socket_t *socket, void *buffer,
                                       size_t length,
                                       socket_ipv6_endpoint_t *source,
                                       uint64_t timeout_ns,
                                       uint64_t *bytes, bool peek) {
    socket_private_t *private = socket_private(socket);

    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        private == 0 || socket->family != OS_AF_INET6 || source == 0 ||
        bytes == 0 || (buffer == 0 && length != 0U)) return K_EINVAL;

    socket_wait_context_t wait = {.socket = socket, .private = private};
    for (;;) {
        if (atomic_load_explicit(&private->receive_shutdown, memory_order_acquire)) {
            socket_copy(source->address, private->peer_address6, 16U);
            source->port = 0U;
            source->reserved = 0U;
            *bytes = 0U;
            return K_OK;
        }
        kstatus_t status = wait_on_queue(&socket->waitq,
                                         socket_receive_ready,
                                         &wait, timeout_ns);
        if (status != K_OK) return status;
        socket_lock(&socket->lock);

        if (atomic_load_explicit(&private->receive_shutdown, memory_order_acquire)) {
            private->datagram_read = private->datagram_write = 0U;
            private->stream_read = private->stream_write = 0U;
            atomic_store_explicit(&private->datagram_count, 0U,
                                  memory_order_release);
            atomic_store_explicit(&private->stream_count, 0U,
                                  memory_order_release);
            socket_copy(source->address, private->peer_address6, 16U);
            source->port = 0U;
            source->reserved = 0U;
            socket_unlock(&socket->lock);
            *bytes = 0U;
            return K_OK;
        }

        if (socket->type == OS_SOCK_STREAM) {
            uint32_t stream_count = atomic_load_explicit(
                &private->stream_count, memory_order_relaxed);
            if (stream_count != 0U) {
                socket_t *notify_peer = 0;
                bool window_update;
                size_t copy_length = socket_receive_stream_locked(
                    private, buffer, length, stream_count, peek, &notify_peer, &window_update);
                socket_copy(source->address, private->peer_address6, 16U);
                source->port = private->peer_port;
                source->reserved = 0U;
                *bytes = copy_length;

                uint8_t update_source[16];
                uint8_t update_destination[16];
                if (window_update) {
                    socket_copy(update_source, private->local_address6, 16U);
                    socket_copy(update_destination, private->peer_address6, 16U);
                }
                uint16_t update_source_port = private->local_port;
                uint16_t update_destination_port = private->peer_port;
                uint32_t update_sequence = private->tcp_send_next_sequence;
                uint32_t update_acknowledgement = private->tcp_next_sequence;
                uint16_t update_window = window_update ? socket_tcp_window_for_count(
                    private, stream_count - (uint32_t)copy_length, false) : 0U;
                socket_unlock(&socket->lock);
                socket_receive_notify_peer(notify_peer);

                if (window_update) {
                    socket_tcp_ipv6_output_fn output = 0;
                    void *output_context = 0;
                    socket_tcp6_output_get(&output, &output_context);
                    if (output != 0) {
                        kstatus_t output_status = output(output_context, update_source,
                                     update_source_port, update_destination,
                                     update_destination_port, update_sequence,
                                     update_acknowledgement, NET_TCP_FLAG_ACK,
                                     update_window, 0, 0U);
                        socket_tcp_ack_output_ipv6(update_source, update_source_port,
                            update_destination, update_destination_port,
                            NET_TCP_FLAG_ACK, output_status);
                    }
                }
                return K_OK;
            }

            int error = atomic_load_explicit(&private->tcp_error, memory_order_acquire);
            if (error != 0) {
                socket_unlock(&socket->lock);
                return error;
            }
            bool stream_closed =
                atomic_load_explicit(&private->closed,
                                     memory_order_acquire) ||
                atomic_load_explicit(&private->peer_closed,
                                     memory_order_acquire);
            socket_unlock(&socket->lock);
            if (stream_closed) {
                socket_copy(source->address, private->peer_address6, 16U);
                source->port = private->peer_port;
                source->reserved = 0U;
                *bytes = 0U;
                return K_OK;
            }
            continue;
        }

        uint32_t count = atomic_load_explicit(
            &private->datagram_count, memory_order_relaxed);
        if (count != 0U) {
            socket_datagram_t *datagram =
                &private->datagrams[private->datagram_read];
            uint16_t datagram_length = datagram->length;
            size_t copy_length = datagram_length > length ?
                length : datagram_length;
            if (datagram->family != OS_AF_INET6) {
                socket_unlock(&socket->lock);
                return K_EIO;
            }
            if (copy_length != 0U) {
                socket_copy(buffer, datagram->payload, copy_length);
            }
            socket_copy(source->address, datagram->source_address6, 16U);
            source->port = datagram->source_port;
            source->reserved = 0U;
            *bytes = copy_length;
            if (!peek) {
                private->datagram_read =
                    (private->datagram_read + 1U) % SOCKET_RX_QUEUE_DEPTH;
                atomic_store_explicit(&private->datagram_count, count - 1U,
                                      memory_order_release);
            }
            socket_unlock(&socket->lock);
            return datagram_length > length ? K_EAGAIN : K_OK;
        }

        bool closed = atomic_load_explicit(&private->closed,
                                           memory_order_acquire);
        socket_unlock(&socket->lock);
        if (closed) return K_EDEVREMOVED;
    }
}

kstatus_t socket_recv_ipv6(socket_t *socket, void *buffer, size_t length,
                           socket_ipv6_endpoint_t *source,
                           uint64_t timeout_ns, uint64_t *bytes) {
    return socket_recv_ipv6_impl(socket, buffer, length, source, timeout_ns,
                                 bytes, false);
}

kstatus_t socket_recv_ipv6_peek(socket_t *socket, void *buffer, size_t length,
                                socket_ipv6_endpoint_t *source,
                                uint64_t timeout_ns, uint64_t *bytes) {
    return socket_recv_ipv6_impl(socket, buffer, length, source, timeout_ns,
                                 bytes, true);
}
