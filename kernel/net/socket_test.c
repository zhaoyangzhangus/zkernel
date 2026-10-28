/* REFACTOR_P8_SOCKET_TEST_OWNER: hostable socket protocol/runtime fixture. */

#include <arch/x86_64/cpu.h>
#include <kernel/completion_port.h>
#include <kernel/deferred.h>
#include <kernel/net_core.h>
#include <kernel/socket.h>

#include "socket_internal.h"
#include "socket_model.h"

#define SOCKET_LONG_TEST_CHUNK 1024U
#define SOCKET_LONG_TEST_BATCH 8U
#define SOCKET_LONG_TEST_ROUNDS 8U

/* Synthetic peers go through the capture callback, not the loopback path.
 * TEST-NET-1 addresses cannot collide with an in-kernel local listener. */
#define SOCKET_TEST_WIRE_PEER_IPV4 0xC0000202U
#define SOCKET_TEST_TIMEOUT_PEER_IPV4 0xC0000203U

static void socket_test_zero(void *memory, size_t length) {
    uint8_t *bytes = (uint8_t *)memory;
    while (length-- != 0U) *bytes++ = 0U;
}

static void socket_test_copy(void *destination, const void *source, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    while (length-- != 0U) *out++ = *in++;
}

static bool socket_test_address6_equal(const uint8_t left[16],
                                       const uint8_t right[16]) {
    uint8_t difference = 0U;
    for (uint32_t index = 0U; index < 16U; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}
static bool socket_check_payload(const uint8_t *actual, const uint8_t *expected, size_t length) {
    for (size_t index = 0; index < length; ++index) {
        if (actual[index] != expected[index]) return false;
    }
    return true;
}

typedef struct socket_test_tcp_output {
    uint32_t count;
    uint32_t source_address;
    uint16_t source_port;
    uint32_t destination_address;
    uint16_t destination_port;
    uint32_t sequence;
    uint32_t acknowledgement;
    uint8_t flags;
    uint16_t window;
    size_t payload_length;
    uint8_t payload[SOCKET_MAX_PAYLOAD];
} socket_test_tcp_output_t;

typedef struct socket_test_tcp6_output {
    uint32_t count;
    uint8_t source_address[16];
    uint16_t source_port;
    uint8_t destination_address[16];
    uint16_t destination_port;
    uint32_t sequence;
    uint32_t acknowledgement;
    uint8_t flags;
    uint16_t window;
    size_t payload_length;
    uint8_t payload[SOCKET_MAX_PAYLOAD];
} socket_test_tcp6_output_t;

static kstatus_t socket_test_tcp_output(
    void *context, uint32_t source_address, uint16_t source_port,
    uint32_t destination_address, uint16_t destination_port,
    uint32_t sequence, uint32_t acknowledgement, uint8_t flags,
    uint16_t window, const void *payload, size_t payload_length) {
    socket_test_tcp_output_t *capture = (socket_test_tcp_output_t *)context;
    if (capture == 0 || payload_length > sizeof(capture->payload) ||
        (payload == 0 && payload_length != 0U)) return K_EINVAL;
    ++capture->count;
    capture->source_address = source_address;
    capture->source_port = source_port;
    capture->destination_address = destination_address;
    capture->destination_port = destination_port;
    capture->sequence = sequence;
    capture->acknowledgement = acknowledgement;
    capture->flags = flags;
    capture->window = window;
    capture->payload_length = payload_length;
    if (payload_length != 0U) socket_test_copy(capture->payload, payload, payload_length);
    return K_OK;
}

static kstatus_t socket_test_tcp6_output(
    void *context, const uint8_t source_address[16], uint16_t source_port,
    const uint8_t destination_address[16], uint16_t destination_port,
    uint32_t sequence, uint32_t acknowledgement, uint8_t flags,
    uint16_t window, const void *payload, size_t payload_length) {
    socket_test_tcp6_output_t *capture = (socket_test_tcp6_output_t *)context;
    if (capture == 0 || source_address == 0 || destination_address == 0 ||
        payload_length > sizeof(capture->payload) ||
        (payload == 0 && payload_length != 0U)) return K_EINVAL;
    ++capture->count;
    socket_test_copy(capture->source_address, source_address, 16U);
    capture->source_port = source_port;
    socket_test_copy(capture->destination_address, destination_address, 16U);
    capture->destination_port = destination_port;
    capture->sequence = sequence;
    capture->acknowledgement = acknowledgement;
    capture->flags = flags;
    capture->window = window;
    capture->payload_length = payload_length;
    if (payload_length != 0U) socket_test_copy(capture->payload, payload, payload_length);
    return K_OK;
}

static void socket_test_release(socket_t *socket) {
    if (socket == 0) return;
    (void)socket_close(socket);
    object_put(socket);
}

static bool socket_test_closed_loopback(void) {
    static const uint32_t addresses[] = {
        0x7F000001U, 0x7F000002U, 0x7F123456U, 0x7FFFFFFFU
    };
    for (size_t i = 0U; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
        socket_t *probe = 0;
        if (socket_create(OS_AF_INET4, OS_SOCK_STREAM, 0, &probe) != K_OK)
            return false;
        kstatus_t status = socket_connect(probe, addresses[i], 17011U);
        socket_test_release(probe);
        if (status != K_ECONNREFUSED) return false;
    }
    return true;
}

static bool socket_test_open_local_pair(uint16_t listener_port,
                                        uint16_t client_port,
                                        socket_t **listener_out,
                                        socket_t **client_out,
                                        socket_t **accepted_out) {
    socket_t *listener = 0;
    socket_t *client = 0;
    socket_t *accepted = 0;

    if (listener_out == 0 || client_out == 0 || accepted_out == 0 ||
        socket_create(OS_AF_INET4, OS_SOCK_STREAM, 0, &listener) != K_OK ||
        socket_create(OS_AF_INET4, OS_SOCK_STREAM, 0, &client) != K_OK ||
        socket_bind(listener, 0x7F000001U, listener_port) != K_OK ||
        socket_listen(listener, 1U) != K_OK ||
        socket_bind(client, 0x7F000001U, client_port) != K_OK ||
        socket_connect(client, 0x7F000001U, listener_port) != K_OK ||
        socket_accept(listener, 1000000000ULL, &accepted) != K_OK) {
        socket_test_release(accepted);
        socket_test_release(client);
        socket_test_release(listener);
        return false;
    }
    *listener_out = listener;
    *client_out = client;
    *accepted_out = accepted;
    return true;
}

static bool socket_test_peek_local(void) {
    socket_t *listener = 0;
    socket_t *client = 0;
    socket_t *accepted = 0;
    socket_ipv4_endpoint_t source = {0};
    static const uint8_t payload[] = {'a', 'b', 'c'};
    uint8_t received[sizeof(payload)] = {0};
    uint64_t bytes = 0U;
    bool success = false;

    if (!socket_test_open_local_pair(17006U, 17007U, &listener, &client,
                                     &accepted) ||
        socket_send(client, payload, sizeof(payload), 0U, 0U, &bytes) != K_OK ||
        bytes != sizeof(payload)) goto cleanup;
    for (uint32_t attempt = 0U; attempt < 2U; ++attempt) {
        if (socket_recv_peek(accepted, received, 1U, &source,
                             1000000000ULL, &bytes) != K_OK ||
            bytes != 1U || received[0] != payload[0]) goto cleanup;
    }
    if (socket_recv(accepted, received, sizeof(received), &source,
                    1000000000ULL, &bytes) != K_OK ||
        bytes != sizeof(payload) ||
        !socket_check_payload(received, payload, sizeof(payload)) ||
        (socket_poll_state(accepted) & SOCKET_POLL_READ) != 0U) goto cleanup;
    success = true;
cleanup:
    socket_test_release(accepted);
    socket_test_release(client);
    socket_test_release(listener);
    return success;
}

static bool socket_test_shutdown_local(void) {
    socket_t *probe = 0;
    socket_t *listener = 0;
    socket_t *client = 0;
    socket_t *accepted = 0;
    socket_ipv4_endpoint_t source = {0};
    uint8_t byte = 0x5AU;
    uint64_t bytes = 0U;
    bool success = false;

    /* The socket API must reject values outside the ABI enum. */
    if (socket_create(OS_AF_INET4, OS_SOCK_STREAM, 0, &probe) != K_OK ||
        socket_shutdown(probe, OS_SOCKET_SHUT_BOTH + 1U) != K_EINVAL) goto cleanup;
    socket_test_release(probe);
    probe = 0;

    /* SHUT_WR publishes EOF to the peer and rejects subsequent writes. */
    if (!socket_test_open_local_pair(17000U, 17001U, &listener, &client, &accepted) ||
        socket_shutdown(client, OS_SOCKET_SHUT_WRITE) != K_OK ||
        socket_send(client, &byte, sizeof(byte), 0U, 0U, &bytes) != K_EPIPE ||
        bytes != 0U ||
        socket_recv(accepted, &byte, sizeof(byte), &source, 0U, &bytes) != K_OK ||
        bytes != 0U ||
        socket_send(accepted, &byte, sizeof(byte), 0U, 0U, &bytes) != K_OK ||
        bytes != sizeof(byte) ||
        socket_recv(client, &byte, sizeof(byte), &source, 0U, &bytes) != K_OK ||
        bytes != sizeof(byte) ||
        socket_shutdown(client, OS_SOCKET_SHUT_WRITE) != K_OK) goto cleanup;
    socket_test_release(accepted);
    socket_test_release(client);
    socket_test_release(listener);
    accepted = 0;
    client = 0;
    listener = 0;

    /* SHUT_RD discards queued data and makes the peer's send fail with EPIPE. */
    if (!socket_test_open_local_pair(17002U, 17003U, &listener, &client, &accepted) ||
        socket_send(accepted, &byte, sizeof(byte), 0U, 0U, &bytes) != K_OK ||
        bytes != sizeof(byte) ||
        socket_shutdown(client, OS_SOCKET_SHUT_READ) != K_OK ||
        socket_recv(client, &byte, sizeof(byte), &source, 0U, &bytes) != K_OK ||
        bytes != 0U ||
        socket_send(accepted, &byte, sizeof(byte), 0U, 0U, &bytes) != K_EPIPE ||
        bytes != 0U ||
        socket_send(client, &byte, sizeof(byte), 0U, 0U, &bytes) != K_OK ||
        bytes != sizeof(byte) ||
        socket_recv(accepted, &byte, sizeof(byte), &source, 0U, &bytes) != K_OK ||
        bytes != sizeof(byte)) goto cleanup;
    socket_test_release(accepted);
    socket_test_release(client);
    socket_test_release(listener);
    accepted = 0;
    client = 0;
    listener = 0;

    /* SHUT_RDWR applies both local directions and signals both peer paths. */
    if (!socket_test_open_local_pair(17004U, 17005U, &listener, &client, &accepted) ||
        socket_shutdown(client, OS_SOCKET_SHUT_BOTH) != K_OK ||
        socket_send(client, &byte, sizeof(byte), 0U, 0U, &bytes) != K_EPIPE ||
        bytes != 0U ||
        socket_recv(client, &byte, sizeof(byte), &source, 0U, &bytes) != K_OK ||
        bytes != 0U ||
        socket_send(accepted, &byte, sizeof(byte), 0U, 0U, &bytes) != K_EPIPE ||
        bytes != 0U ||
        socket_recv(accepted, &byte, sizeof(byte), &source, 0U, &bytes) != K_OK ||
        bytes != 0U ||
        socket_shutdown(client, OS_SOCKET_SHUT_BOTH) != K_OK) goto cleanup;
    success = true;

cleanup:
    socket_test_release(accepted);
    socket_test_release(client);
    socket_test_release(listener);
    socket_test_release(probe);
    return success;
}

static bool socket_test_shutdown_wire(void) {
    socket_t *client = 0;
    socket_test_tcp_output_t capture;
    socket_tcp_ipv4_output_fn saved_output = 0;
    void *saved_context = 0;
    socket_tcp_reply_t handshake = {0};
    socket_tcp_reply_t fin_reply = {0};
    socket_tcp_reply_t duplicate_reply = {0};
    socket_ipv4_endpoint_t source = {0};
    uint8_t byte = 0U;
    uint64_t bytes = 0U;
    uint32_t output_count;
    bool output_installed = false;
    bool success = false;

    socket_test_zero(&capture, sizeof(capture));
    socket_tcp_output_get(&saved_output, &saved_context);
    socket_set_tcp_ipv4_output(socket_test_tcp_output, &capture);
    output_installed = true;

    /* Complete an active handshake through the test wire and capture a FIN. */
    if (socket_create(OS_AF_INET4, OS_SOCK_STREAM, 0, &client) != K_OK ||
        socket_connect(client, SOCKET_TEST_WIRE_PEER_IPV4, 17010U) != K_OK ||
        capture.count != 1U || capture.flags != NET_TCP_FLAG_SYN ||
        socket_inject_tcp_ack_ipv4_reply(
            capture.destination_address, capture.destination_port,
            capture.source_address, capture.source_port, 400U,
            capture.sequence + 1U, NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK,
            SOCKET_STREAM_BUFFER_SIZE, &handshake) != K_OK ||
        !handshake.valid || handshake.flags != NET_TCP_FLAG_ACK) goto cleanup;

    socket_test_zero(&capture, sizeof(capture));
    if (socket_shutdown(client, OS_SOCKET_SHUT_WRITE) != K_OK ||
        capture.count != 1U ||
        capture.flags != (NET_TCP_FLAG_ACK | NET_TCP_FLAG_FIN) ||
        capture.payload_length != 0U) goto cleanup;

    uint32_t fin_sequence = capture.sequence;
    uint32_t peer_sequence = capture.acknowledgement;
    uint32_t local_address = capture.source_address;
    uint16_t local_port = capture.source_port;
    uint32_t peer_address = capture.destination_address;
    uint16_t peer_port = capture.destination_port;

    /* The FIN consumes one sequence number and its ACK stops retransmission. */
    if (socket_inject_tcp_ack_ipv4_reply(
            peer_address, peer_port, local_address, local_port,
            peer_sequence, fin_sequence + 1U, NET_TCP_FLAG_ACK,
            SOCKET_STREAM_BUFFER_SIZE, 0) != K_OK) goto cleanup;
    output_count = capture.count;
    socket_tcp_poll(x86_read_tsc() +
                    x86_timeout_ns_to_tsc(SOCKET_TCP_RETRANSMIT_TIMEOUT_NS) + 1U);
    if (capture.count != output_count ||
        socket_shutdown(client, OS_SOCKET_SHUT_WRITE) != K_OK ||
        capture.count != output_count) goto cleanup;

    /* An external FIN is acknowledged once; a duplicate remains idempotent. */
    if (socket_inject_tcp_ipv4_reply(
            peer_address, peer_port, local_address, local_port,
            peer_sequence, NET_TCP_FLAG_ACK | NET_TCP_FLAG_FIN, 0, 0U,
            &fin_reply) != K_OK || !fin_reply.valid ||
        fin_reply.flags != NET_TCP_FLAG_ACK ||
        fin_reply.acknowledgement != peer_sequence + 1U ||
        socket_recv(client, &byte, sizeof(byte), &source, 0U, &bytes) != K_OK ||
        bytes != 0U || source.address != peer_address || source.port != peer_port ||
        socket_inject_tcp_ipv4_reply(
            peer_address, peer_port, local_address, local_port,
            peer_sequence, NET_TCP_FLAG_ACK | NET_TCP_FLAG_FIN, 0, 0U,
            &duplicate_reply) != K_OK || !duplicate_reply.valid ||
        duplicate_reply.flags != NET_TCP_FLAG_ACK ||
        duplicate_reply.acknowledgement != fin_reply.acknowledgement ||
        socket_recv(client, &byte, sizeof(byte), &source, 0U, &bytes) != K_OK ||
        bytes != 0U) goto cleanup;
    success = true;

cleanup:
    socket_test_release(client);
    if (output_installed) socket_set_tcp_ipv4_output(saved_output, saved_context);
    return success;
}

static bool socket_address6_self_test(void) {
    uint8_t left[32] = {0}, right[32] = {0};
    for (uint32_t a = 0; a < 16U; ++a) {
        for (uint32_t b = 0; b < 16U; ++b) {
            if (!socket_address6_zero(left + a) ||
                !socket_address6_equal(left + a, right + b)) return false;
            for (uint32_t bit = 0; bit < 128U; ++bit) {
                uint32_t byte = bit / 8U;
                left[a + byte] = (uint8_t)(1U << (bit % 8U));
                if (socket_address6_zero(left + a) ||
                    socket_address6_equal(left + a, right + b)) return false;
                right[b + byte] = left[a + byte];
                if (!socket_address6_equal(left + a, right + b)) return false;
                left[a + byte] = right[b + byte] = 0U;
            }
        }
    }
    return true;
}

/* The capture callback below supplies the synthetic remote handshake. Own an
 * open endpoint for its always-available local route; an unbound ::1 port is
 * correctly refused and cannot stand in for an external TCP peer. The caller
 * owns both output slots, including partial setup on any failure. */
static bool socket_test_open_active6_wire(socket_t **listener_out,
                                          socket_t **client_out,
                                          const uint8_t address[16],
                                          uint16_t port) {
    return socket_create(OS_AF_INET6, OS_SOCK_STREAM, 0, listener_out) == K_OK &&
           socket_bind_ipv6(*listener_out, address, port) == K_OK &&
           socket_listen(*listener_out, 1U) == K_OK &&
           socket_create(OS_AF_INET6, OS_SOCK_STREAM, 0, client_out) == K_OK &&
           socket_connect_ipv6(*client_out, address, port) == K_OK;
}

static bool socket_test_closed_loopback6(void) {
    static const uint8_t loopback[16] = {[15] = 1U};
    socket_t *probe = 0;
    if (socket_create(OS_AF_INET6, OS_SOCK_STREAM, 0, &probe) != K_OK)
        return false;
    kstatus_t status = socket_connect_ipv6(probe, loopback, 17011U);
    socket_test_release(probe);
    return status == K_ECONNREFUSED;
}

bool socket_self_test(void) {
    if (!socket_address6_self_test()) return false;
    if (!socket_test_closed_loopback6()) return false;
    if (!socket_test_closed_loopback()) return false;
    socket_t *sender = 0;
    socket_t *receiver = 0;
    socket_t *sender6 = 0;
    socket_t *receiver6 = 0;
    socket_t *chaos_receiver = 0;
    socket_t *listener = 0;
    socket_t *client = 0;
    socket_t *accepted = 0;
    socket_t *wire_listener = 0;
    socket_t *wire_accepted = 0;
    socket_t *active_client = 0;
    socket_t *active_client6 = 0;
    socket_t *active_listener6 = 0;
    socket_t *connect_timeout_client = 0;
    completion_port_t *completion_port = 0;
    const uint8_t udp_payload[] = {'s', 'o', 'c', 'k', 'e', 't'};
    const uint8_t tcp_payload[] = {'t', 'c', 'p'};
    const uint8_t tcp_reply[] = {'a', 'c', 'k'};
    uint8_t received[sizeof(udp_payload)] = {0};
    uint8_t long_payload[SOCKET_LONG_TEST_CHUNK];
    uint8_t long_received[SOCKET_LONG_TEST_CHUNK];
    static const uint8_t reordered_datagrams[] = {3U, 1U, 1U, 4U};
    static const uint8_t loopback6[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 1};
    socket_ipv4_endpoint_t source = {0};
    socket_ipv6_endpoint_t source6 = {0};
    os_socket_info_t socket_info = {0};
    int32_t socket_option = 0;
    uint64_t bytes = 0;
    bool success = false;
    socket_tcp_reply_t wire_reply = {0};
    socket_tcp_reply_t wire_data_reply = {0};
    socket_tcp_reply_t active_reply = {0};
    socket_tcp_reply_t active_reply6 = {0};
    socket_tcp_ipv4_output_fn saved_tcp_output = 0;
    void *saved_tcp_output_context = 0;
    socket_tcp_ipv6_output_fn saved_tcp6_output = 0;
    void *saved_tcp6_output_context = 0;
    socket_test_tcp_output_t captured_tcp_output;
    socket_test_tcp6_output_t captured_tcp6_output;
    bool tcp_output_installed = false;
    bool tcp6_output_installed = false;
    socket_test_zero(&captured_tcp_output, sizeof(captured_tcp_output));
    socket_test_zero(&captured_tcp6_output, sizeof(captured_tcp6_output));

    if (!socket_test_shutdown_local() || !socket_test_shutdown_wire() ||
        !socket_test_peek_local()) goto cleanup;

    if (socket_create(OS_AF_INET4, OS_SOCK_DGRAM, 0, &sender) != K_OK ||
        socket_create(OS_AF_INET4, OS_SOCK_DGRAM, 0, &receiver) != K_OK ||
        socket_bind(sender, 0x7F000001U, 10000U) != K_OK ||
        socket_bind(receiver, 0x7F000001U, 10001U) != K_OK ||
        socket_connect(sender, 0x7F000001U, 10001U) != K_OK ||
        socket_set_option(sender, OS_SOCKET_OPTION_REUSE_ADDRESS, 1) != K_OK ||
        socket_get_option(sender, OS_SOCKET_OPTION_REUSE_ADDRESS,
                          &socket_option) != K_OK || socket_option != 1 ||
        socket_get_info(sender, &socket_info) != K_OK ||
        socket_info.family != OS_AF_INET4 || socket_info.type != OS_SOCK_DGRAM ||
        socket_info.local_address != 0x7F000001U ||
        socket_info.local_port != 10000U ||
        socket_info.peer_address != 0x7F000001U ||
        socket_info.peer_port != 10001U ||
        (socket_info.flags & (OS_SOCKET_INFO_BOUND | OS_SOCKET_INFO_CONNECTED)) !=
            (OS_SOCKET_INFO_BOUND | OS_SOCKET_INFO_CONNECTED) ||
        socket_send(sender, udp_payload, sizeof(udp_payload), 0, 0, &bytes) != K_OK ||
        bytes != sizeof(udp_payload) ||
        socket_recv(receiver, received, sizeof(received), &source, 1000000000ULL,
                    &bytes) != K_OK || bytes != sizeof(udp_payload) ||
        source.address != 0x7F000001U || source.port != 10000U ||
        !socket_check_payload(received, udp_payload, sizeof(udp_payload))) goto cleanup;

    /* IPv6 UDP loopback：验证 128 位地址不会被截断或按 IPv4 规则匹配。 */
    if (socket_create(OS_AF_INET6, OS_SOCK_DGRAM, 0, &sender6) != K_OK ||
        socket_create(OS_AF_INET6, OS_SOCK_DGRAM, 0, &receiver6) != K_OK ||
        socket_bind_ipv6(sender6, loopback6, 10010U) != K_OK ||
        socket_bind_ipv6(receiver6, loopback6, 10011U) != K_OK ||
        socket_connect_ipv6(sender6, loopback6, 10011U) != K_OK ||
        socket_send_ipv6(sender6, udp_payload, sizeof(udp_payload), 0, 0U, &bytes) != K_OK ||
        bytes != sizeof(udp_payload) ||
        socket_recv_ipv6(receiver6, received, sizeof(received), &source6,
                         1000000000ULL, &bytes) != K_OK || bytes != sizeof(udp_payload) ||
        !socket_test_address6_equal(source6.address, loopback6) || source6.port != 10010U ||
        !socket_check_payload(received, udp_payload, sizeof(udp_payload))) goto cleanup;

    /*
     * 通过注入器构造“丢失 2、乱序 3/1、重复 1”的序列。
     * socket 层必须保持到达顺序和重复包，未注入的序号不能凭空出现。
     */
    if (socket_create(OS_AF_INET4, OS_SOCK_DGRAM, 0, &chaos_receiver) != K_OK ||
        socket_bind(chaos_receiver, 0x7F000001U, 10002U) != K_OK) goto cleanup;
    for (size_t index = 0U; index < sizeof(reordered_datagrams); ++index) {
        uint8_t value = reordered_datagrams[index];
        if (socket_inject_udp(0x7F000001U, 15000U, 0x7F000001U, 10002U,
                              &value, sizeof(value)) != K_OK) goto cleanup;
    }
    for (size_t index = 0U; index < sizeof(reordered_datagrams); ++index) {
        if (socket_recv(chaos_receiver, received, sizeof(uint8_t), &source, 0U,
                        &bytes) != K_OK || bytes != sizeof(uint8_t) ||
            source.address != 0x7F000001U || source.port != 15000U ||
            received[0] != reordered_datagrams[index]) goto cleanup;
    }
    /* 队列已经耗尽；2 的丢失不能被错误地报告为一个额外数据报。 */
    if (socket_recv(chaos_receiver, received, sizeof(uint8_t), &source, 0U,
                    &bytes) != K_ETIMEDOUT) goto cleanup;

    if (socket_create(OS_AF_INET4, OS_SOCK_STREAM, 0, &listener) != K_OK ||
        socket_create(OS_AF_INET4, OS_SOCK_STREAM, 0, &client) != K_OK ||
        socket_bind(listener, 0x7F000001U, 13000U) != K_OK ||
        socket_listen(listener, 4U) != K_OK ||
        socket_bind(client, 0x7F000001U, 13001U) != K_OK ||
        socket_connect(client, 0x7F000001U, 13000U) != K_OK ||
        socket_accept(listener, 1000000000ULL, &accepted) != K_OK || accepted == 0 ||
        socket_send(client, tcp_payload, sizeof(tcp_payload), 0, 0, &bytes) != K_OK ||
        bytes != sizeof(tcp_payload) ||
        socket_recv(accepted, received, sizeof(tcp_payload), &source, 1000000000ULL,
                    &bytes) != K_OK || bytes != sizeof(tcp_payload) ||
        source.address != 0x7F000001U || source.port != 13001U ||
        !socket_check_payload(received, tcp_payload, sizeof(tcp_payload)) ||
        socket_send(accepted, tcp_reply, sizeof(tcp_reply), 0, 0, &bytes) != K_OK ||
        bytes != sizeof(tcp_reply) ||
        socket_recv(client, received, sizeof(tcp_reply), &source, 1000000000ULL,
                    &bytes) != K_OK || bytes != sizeof(tcp_reply) ||
        !socket_check_payload(received, tcp_reply, sizeof(tcp_reply))) goto cleanup;

    /*
     * 每批先填满 8 KiB 对端流缓冲，再按相同分片读回；总量为 64 KiB，
     * 用来覆盖环形缓冲绕回、连续发送和连续接收的长传输路径。
     */
    for (uint32_t round = 0U; round < SOCKET_LONG_TEST_ROUNDS; ++round) {
        for (uint32_t chunk = 0U; chunk < SOCKET_LONG_TEST_BATCH; ++chunk) {
            uint64_t base = ((uint64_t)round * SOCKET_LONG_TEST_BATCH + chunk) *
                            SOCKET_LONG_TEST_CHUNK;
            for (size_t index = 0U; index < sizeof(long_payload); ++index) {
                long_payload[index] = (uint8_t)((base + index) & 0xFFU);
            }
            if (socket_send(client, long_payload, sizeof(long_payload), 0U, 0U,
                            &bytes) != K_OK || bytes != sizeof(long_payload)) {
                goto cleanup;
            }
        }
        for (uint32_t chunk = 0U; chunk < SOCKET_LONG_TEST_BATCH; ++chunk) {
            uint64_t base = ((uint64_t)round * SOCKET_LONG_TEST_BATCH + chunk) *
                            SOCKET_LONG_TEST_CHUNK;
            if (socket_recv(accepted, long_received, sizeof(long_received), &source,
                            0U, &bytes) != K_OK || bytes != sizeof(long_received) ||
                source.address != 0x7F000001U || source.port != 13001U) goto cleanup;
            for (size_t index = 0U; index < sizeof(long_received); ++index) {
                if (long_received[index] != (uint8_t)((base + index) & 0xFFU)) {
                    goto cleanup;
                }
            }
        }
    }

    /* 关闭 socket 后再运行异步任务，完成项必须仍然恰好投递一次。 */
    /* TCP wire RX 边界：连续序号、重复/乱序包和 FIN EOF。 */
    if (socket_inject_tcp_ipv4(0x7F000001U, 13001U, 0x7F000001U, 13000U,
                               700U, NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH,
                               tcp_payload, sizeof(tcp_payload)) != K_OK ||
        socket_recv(accepted, received, sizeof(tcp_payload), &source, 0U, &bytes) != K_OK ||
        bytes != sizeof(tcp_payload) || source.port != 13001U ||
        !socket_check_payload(received, tcp_payload, sizeof(tcp_payload)) ||
        socket_inject_tcp_ipv4(0x7F000001U, 13001U, 0x7F000001U, 13000U,
                               700U, NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH,
                               tcp_payload, sizeof(tcp_payload)) != K_OK ||
        socket_recv(accepted, received, sizeof(tcp_payload), &source, 0U, &bytes) !=
            K_ETIMEDOUT ||
        socket_inject_tcp_ipv4(0x7F000001U, 13001U, 0x7F000001U, 13000U,
                               704U, NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH,
                               tcp_payload, sizeof(tcp_payload)) != K_OK ||
        socket_recv(accepted, received, sizeof(tcp_payload), &source, 0U, &bytes) !=
            K_ETIMEDOUT ||
        socket_inject_tcp_ipv4(0x7F000001U, 13001U, 0x7F000001U, 13000U,
                               703U, NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH,
                               tcp_payload, 1U) != K_OK ||
        socket_recv(accepted, received, sizeof(tcp_payload) + 1U, &source, 0U,
                    &bytes) != K_OK || bytes != sizeof(tcp_payload) + 1U ||
        received[0] != tcp_payload[0] ||
        !socket_check_payload(received + 1U, tcp_payload, sizeof(tcp_payload)) ||
        socket_inject_tcp_ipv4(0x7F000001U, 13001U, 0x7F000001U, 13000U,
                               704U + (uint32_t)sizeof(tcp_payload),
                               NET_TCP_FLAG_ACK | NET_TCP_FLAG_FIN,
                               0, 0U) != K_OK ||
        socket_recv(accepted, received, sizeof(tcp_payload), &source, 0U, &bytes) != K_OK ||
        bytes != 0U) goto cleanup;

    /* 真正的被动握手：SYN 只创建半连接，第三次握手 ACK 后才允许 accept。 */
    if (socket_create(OS_AF_INET4, OS_SOCK_STREAM, 0, &wire_listener) != K_OK ||
        socket_bind(wire_listener, 0x7F000001U, 15000U) != K_OK ||
        socket_listen(wire_listener, 2U) != K_OK ||
        socket_inject_tcp_syn_ipv4(0x7F000001U, 15001U, 0x7F000001U, 15000U,
                                   900U, &wire_reply) != K_OK || !wire_reply.valid ||
        (wire_reply.flags & (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK)) !=
            (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK) ||
        socket_accept(wire_listener, 0U, &wire_accepted) != K_ETIMEDOUT ||
        socket_inject_tcp_ack_ipv4_window(0x7F000001U, 15001U, 0x7F000001U, 15000U,
                                   901U, wire_reply.sequence + 1U,
                                   NET_TCP_FLAG_ACK, SOCKET_STREAM_BUFFER_SIZE) != K_OK ||
        socket_accept(wire_listener, 1000000000ULL, &wire_accepted) != K_OK ||
        socket_inject_tcp_ipv4_reply(0x7F000001U, 15001U, 0x7F000001U, 15000U,
                                     901U, NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH,
                                     tcp_payload, sizeof(tcp_payload),
                                     &wire_data_reply) != K_OK ||
        !wire_data_reply.valid || wire_data_reply.flags != NET_TCP_FLAG_ACK ||
        wire_data_reply.acknowledgement != 904U || wire_data_reply.window !=
            SOCKET_STREAM_BUFFER_SIZE - sizeof(tcp_payload) ||
        socket_recv(wire_accepted, received, sizeof(tcp_payload), &source,
                    1000000000ULL, &bytes) != K_OK || bytes != sizeof(tcp_payload) ||
        !socket_check_payload(received, tcp_payload, sizeof(tcp_payload))) goto cleanup;

    /* 主动 TCP 连接必须走真实的 SYN/SYN-ACK/ACK 状态机，并具备重传能力。 */
    socket_tcp_output_get(&saved_tcp_output, &saved_tcp_output_context);
    socket_set_tcp_ipv4_output(socket_test_tcp_output, &captured_tcp_output);
    tcp_output_installed = true;
    if (socket_create(OS_AF_INET4, OS_SOCK_STREAM, 0, &active_client) != K_OK ||
        socket_connect(active_client, SOCKET_TEST_WIRE_PEER_IPV4, 16000U) != K_OK ||
        socket_connect_status(active_client) != K_EAGAIN ||
        (socket_poll_state(active_client) & SOCKET_POLL_WRITE) != 0U ||
        captured_tcp_output.count != 1U ||
        captured_tcp_output.flags != NET_TCP_FLAG_SYN ||
        captured_tcp_output.payload_length != 0U ||
        socket_inject_tcp_ack_ipv4_reply(
            captured_tcp_output.destination_address,
            captured_tcp_output.destination_port,
            captured_tcp_output.source_address,
            captured_tcp_output.source_port,
            400U, captured_tcp_output.sequence + 1U,
            NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK, 0U, &active_reply) != K_OK ||
        !active_reply.valid || active_reply.flags != NET_TCP_FLAG_ACK ||
        active_reply.acknowledgement != 401U ||
        socket_connect_status(active_client) != K_OK ||
        (socket_poll_state(active_client) & SOCKET_POLL_WRITE) != 0U ||
        socket_inject_tcp_ack_ipv4_window(
            SOCKET_TEST_WIRE_PEER_IPV4, 16000U, captured_tcp_output.source_address,
            captured_tcp_output.source_port, 401U,
            captured_tcp_output.sequence + 1U, NET_TCP_FLAG_ACK,
            4096U) != K_OK ||
        (socket_poll_state(active_client) & SOCKET_POLL_WRITE) == 0U ||
        captured_tcp_output.source_port == 0U) goto cleanup;
    uint32_t first_data_sequence = captured_tcp_output.sequence + 1U;
    socket_test_zero(&captured_tcp_output, sizeof(captured_tcp_output));
    if (socket_send(active_client, tcp_payload, sizeof(tcp_payload), 0U, 0U,
                    &bytes) != K_OK || bytes != sizeof(tcp_payload) ||
        (socket_poll_state(active_client) & SOCKET_POLL_WRITE) == 0U ||
        captured_tcp_output.count != 1U ||
        captured_tcp_output.sequence != first_data_sequence ||
        captured_tcp_output.flags != (NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH) ||
        captured_tcp_output.payload_length != sizeof(tcp_payload) ||
        !socket_check_payload(captured_tcp_output.payload, tcp_payload,
                              sizeof(tcp_payload)) ||
        /* Unacknowledged data must not block a second write while the
         * flight buffer and peer window still have room. */
        socket_send(active_client, tcp_payload, sizeof(tcp_payload), 0U, 0U,
                    &bytes) != K_OK || bytes != sizeof(tcp_payload) ||
        captured_tcp_output.count != 2U ||
        captured_tcp_output.sequence !=
            first_data_sequence + (uint32_t)sizeof(tcp_payload) ||
        captured_tcp_output.payload_length != sizeof(tcp_payload) ||
        !socket_check_payload(captured_tcp_output.payload, tcp_payload,
                              sizeof(tcp_payload)) ||
        socket_inject_tcp_ack_ipv4_window(
            SOCKET_TEST_WIRE_PEER_IPV4, 16000U, captured_tcp_output.source_address,
            captured_tcp_output.source_port, 401U,
            captured_tcp_output.sequence + (uint32_t)sizeof(tcp_payload),
            NET_TCP_FLAG_ACK, 4096U) != K_OK ||
        (socket_poll_state(active_client) & SOCKET_POLL_WRITE) == 0U)
        goto cleanup;
    socket_test_zero(&captured_tcp_output, sizeof(captured_tcp_output));
    if (socket_send(active_client, tcp_payload, sizeof(tcp_payload), 0U, 0U,
                    &bytes) != K_OK || bytes != sizeof(tcp_payload) ||
        captured_tcp_output.count != 1U) goto cleanup;
    uint32_t retransmit_count = captured_tcp_output.count;
    socket_tcp_poll(x86_read_tsc() +
                    x86_timeout_ns_to_tsc(SOCKET_TCP_RETRANSMIT_TIMEOUT_NS) + 1U);
    if (captured_tcp_output.count != retransmit_count + 1U ||
        captured_tcp_output.sequence == 0U) goto cleanup;

    /* IPv6 主动 TCP：验证同一套状态机使用 128 位四元组和 IPv6 输出回调。 */
    socket_tcp6_output_get(&saved_tcp6_output, &saved_tcp6_output_context);
    socket_set_tcp_ipv6_output(socket_test_tcp6_output, &captured_tcp6_output);
    tcp6_output_installed = true;
    static const uint8_t active_peer6[16] = {
        /* A synthetic output callback is not an IPv6 default route. Exercise
         * the active TCP state machine over the always-available local route. */
        [15] = 1U
    };
    if (!socket_test_open_active6_wire(&active_listener6, &active_client6,
                                      active_peer6, 16001U) ||
        socket_connect_status(active_client6) != K_EAGAIN ||
        (socket_poll_state(active_client6) & SOCKET_POLL_WRITE) != 0U ||
        captured_tcp6_output.count != 1U ||
        captured_tcp6_output.flags != NET_TCP_FLAG_SYN ||
        captured_tcp6_output.payload_length != 0U ||
        socket_inject_tcp_ack_ipv6_reply(
            captured_tcp6_output.destination_address,
            captured_tcp6_output.destination_port,
            captured_tcp6_output.source_address,
            captured_tcp6_output.source_port, 500U,
            captured_tcp6_output.sequence + 1U,
            NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK, 4096U, &active_reply6) != K_OK ||
        !active_reply6.valid || active_reply6.flags != NET_TCP_FLAG_ACK ||
        active_reply6.acknowledgement != 501U ||
        socket_connect_status(active_client6) != K_OK ||
        (socket_poll_state(active_client6) & SOCKET_POLL_WRITE) == 0U)
        goto cleanup;
    uint32_t first_data_sequence6 = captured_tcp6_output.sequence + 1U;
    socket_test_zero(&captured_tcp6_output, sizeof(captured_tcp6_output));
    if (socket_send_ipv6(active_client6, tcp_payload, sizeof(tcp_payload), 0, 0U,
                         &bytes) != K_OK || bytes != sizeof(tcp_payload) ||
        (socket_poll_state(active_client6) & SOCKET_POLL_WRITE) == 0U ||
        captured_tcp6_output.count != 1U ||
        captured_tcp6_output.sequence != first_data_sequence6 ||
        captured_tcp6_output.flags != (NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH) ||
        captured_tcp6_output.payload_length != sizeof(tcp_payload) ||
        !socket_check_payload(captured_tcp6_output.payload, tcp_payload,
                               sizeof(tcp_payload)) ||
        socket_send_ipv6(active_client6, tcp_payload, sizeof(tcp_payload), 0, 0U,
                         &bytes) != K_OK || bytes != sizeof(tcp_payload) ||
        captured_tcp6_output.count != 2U ||
        captured_tcp6_output.sequence !=
            first_data_sequence6 + (uint32_t)sizeof(tcp_payload) ||
        captured_tcp6_output.payload_length != sizeof(tcp_payload) ||
        !socket_check_payload(captured_tcp6_output.payload, tcp_payload,
                               sizeof(tcp_payload)) ||
        socket_inject_tcp_ack_ipv6_window(
            active_peer6, 16001U, captured_tcp6_output.source_address,
            captured_tcp6_output.source_port, 501U,
            captured_tcp6_output.sequence + (uint32_t)sizeof(tcp_payload),
            NET_TCP_FLAG_ACK, 4096U) != K_OK ||
        (socket_poll_state(active_client6) & SOCKET_POLL_WRITE) == 0U)
        goto cleanup;
    socket_test_zero(&captured_tcp6_output, sizeof(captured_tcp6_output));
    if (socket_send_ipv6(active_client6, tcp_payload, sizeof(tcp_payload), 0, 0U,
                         &bytes) != K_OK || bytes != sizeof(tcp_payload) ||
        captured_tcp6_output.count != 1U) goto cleanup;
    uint32_t retransmit6_count = captured_tcp6_output.count;
    socket_tcp_poll(x86_read_tsc() +
                    x86_timeout_ns_to_tsc(SOCKET_TCP_RETRANSMIT_TIMEOUT_NS) + 1U);
    if (captured_tcp6_output.count != retransmit6_count + 1U ||
        captured_tcp6_output.sequence == 0U) goto cleanup;

    /* A SYN that never receives a reply must stop being pending and make
     * the error observable to epoll/poll after the retransmit budget. */
    if (socket_create(OS_AF_INET4, OS_SOCK_STREAM, 0,
                      &connect_timeout_client) != K_OK ||
        socket_connect(connect_timeout_client, SOCKET_TEST_TIMEOUT_PEER_IPV4,
                       16002U) != K_OK ||
        socket_connect_status(connect_timeout_client) != K_EAGAIN ||
        (socket_poll_state(connect_timeout_client) &
         (SOCKET_POLL_WRITE | SOCKET_POLL_ERROR)) != 0U) goto cleanup;
    uint64_t timeout_now = x86_read_tsc();
    uint64_t timeout_step =
        x86_timeout_ns_to_tsc(SOCKET_TCP_RETRANSMIT_TIMEOUT_NS) + 1U;
    for (uint32_t retry = 0U; retry <= SOCKET_TCP_MAX_RETRANSMITS;
         ++retry) {
        timeout_now += timeout_step;
        socket_tcp_poll(timeout_now);
    }
    int32_t timeout_error = 0;
    socket_ipv4_endpoint_t timeout_source = {0};
    uint8_t timeout_byte = 0U;
    if (socket_connect_status(connect_timeout_client) != K_ETIMEDOUT ||
        socket_get_option(connect_timeout_client, OS_SOCKET_OPTION_ERROR,
                          &timeout_error) != K_OK || timeout_error != 110 ||
        socket_recv(connect_timeout_client, &timeout_byte, 1U,
                    &timeout_source, 0U, &bytes) != K_ETIMEDOUT ||
        (socket_poll_state(connect_timeout_client) & SOCKET_POLL_ERROR) == 0U)
        goto cleanup;

    uint64_t request_id = 0U;
    os_completion_entry_t completion = {0};
    if (completion_port_create(4U, &completion_port) != K_OK ||
        socket_send_async(sender, udp_payload, sizeof(udp_payload), 0U, 0U,
                          completion_port, 0xCAFEU, &request_id) != K_OK ||
        request_id == 0U || socket_close(sender) != K_OK ||
        deferred_run(64U) == 0U ||
        completion_port_wait(completion_port, 0U, &completion) != K_OK ||
        completion.request_id != request_id || completion.user_key != 0xCAFEU ||
        completion.status != K_EDEVREMOVED || completion.bytes_done != 0U) goto cleanup;

    success = true;
cleanup:
    if (tcp_output_installed) {
        socket_set_tcp_ipv4_output(saved_tcp_output, saved_tcp_output_context);
    }
    if (tcp6_output_installed) {
        socket_set_tcp_ipv6_output(saved_tcp6_output, saved_tcp6_output_context);
    }
    if (completion_port != 0) object_put(completion_port);
    if (sender6 != 0) {
        (void)socket_close(sender6);
        object_put(sender6);
    }
    if (receiver6 != 0) {
        (void)socket_close(receiver6);
        object_put(receiver6);
    }
    if (accepted != 0) {
        (void)socket_close(accepted);
        object_put(accepted);
    }
    if (wire_accepted != 0) {
        (void)socket_close(wire_accepted);
        object_put(wire_accepted);
    }
    if (active_client != 0) {
        (void)socket_close(active_client);
        object_put(active_client);
    }
    if (active_client6 != 0) {
        (void)socket_close(active_client6);
        object_put(active_client6);
    }
    if (active_listener6 != 0) {
        (void)socket_close(active_listener6);
        object_put(active_listener6);
    }
    if (connect_timeout_client != 0) {
        (void)socket_close(connect_timeout_client);
        object_put(connect_timeout_client);
    }
    if (wire_listener != 0) {
        (void)socket_close(wire_listener);
        object_put(wire_listener);
    }
    if (client != 0) {
        (void)socket_close(client);
        object_put(client);
    }
    if (listener != 0) {
        (void)socket_close(listener);
        object_put(listener);
    }
    if (sender != 0) {
        (void)socket_close(sender);
        object_put(sender);
    }
    if (receiver != 0) {
        (void)socket_close(receiver);
        object_put(receiver);
    }
    if (chaos_receiver != 0) {
        (void)socket_close(chaos_receiver);
        object_put(chaos_receiver);
    }
    return success;
}
