/* REFACTOR_P8_E1000_PROTOCOL_OWNER */

#include <kernel/console.h>
#include <kernel/net_core.h>
#include <kernel/socket.h>
#include <kernel/string.h>

#include "core_internal.h"

#define E1000_IPV4_TCP_MSS       1460U
#define E1000_IPV6_TCP_MSS       1440U
#define E1000_IPV4_UDP_PAYLOAD   1472U
#define E1000_IPV6_UDP_PAYLOAD   1452U

#if OS_DEBUG_SERIAL
static uint32_t g_debug_tcp_output_logs;
#endif

kstatus_t e1000_device_transmit(void *context, const void *frame,
                                       size_t length) {
    return e1000_transmit_status((e1000_state_t *)context,
                                 (const uint8_t *)frame, length);
}

static uint32_t e1000_ipv4_mask(uint8_t prefix_length) {
    if (prefix_length == 0U) return 0U;
    if (prefix_length >= 32U) return UINT32_MAX;
    return UINT32_MAX << (32U - prefix_length);
}

static uint32_t e1000_ipv4_next_hop(const e1000_state_t *state,
                                    uint32_t destination_address) {
    uint32_t mask;
    if (state == 0 || state->ipv4_gateway == 0U || state->ipv4_address == 0U ||
        state->ipv4_prefix_length > 32U) return destination_address;
    mask = e1000_ipv4_mask(state->ipv4_prefix_length);
    return (destination_address & mask) == (state->ipv4_address & mask) ?
           destination_address : state->ipv4_gateway;
}

static kstatus_t e1000_send_tcp_ipv4_segments(
    e1000_state_t *state, const uint8_t destination_mac[6],
    uint32_t source_address, uint32_t destination_address,
    uint16_t source_port, uint16_t destination_port, uint32_t sequence,
    uint32_t acknowledgement, uint8_t flags, uint16_t window,
    const void *payload, size_t payload_length) {
    const uint8_t *bytes = (const uint8_t *)payload;
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV4_HEADER_SIZE +
                  NET_TCP_HEADER_SIZE + 8U + E1000_IPV4_TCP_MSS];
    size_t offset = 0U;
    int scale = (flags & NET_TCP_FLAG_SYN) != 0U ?
        socket_tcp_syn_scale_ipv4(source_address, source_port, destination_address,
            destination_port, sequence, acknowledgement) : -1;
    do {
        size_t remaining = payload_length - offset;
        uint16_t chunk = (uint16_t)(remaining > E1000_IPV4_TCP_MSS ?
                                    E1000_IPV4_TCP_MSS : remaining);
        uint8_t segment_flags = flags;
        size_t frame_length = 0U;
        if (offset + chunk < payload_length) {
            segment_flags &= (uint8_t)~(NET_TCP_FLAG_FIN | NET_TCP_FLAG_PSH);
        }
        if (net_tcp_build_ipv4_scaled(
                frame, sizeof(frame), state->mac, destination_mac,
                source_address, destination_address, source_port,
                destination_port, sequence + (uint32_t)offset,
                acknowledgement, segment_flags, window, scale,
                bytes == 0 ? 0 : bytes + offset, chunk, &frame_length) != K_OK) {
            return K_EINVAL;
        }
        kstatus_t status = net_device_send(&state->device, frame, frame_length);
        if (status != K_OK) return status;
        offset += chunk;
    } while (offset < payload_length);
    return K_OK;
}

static kstatus_t e1000_send_tcp_ipv6_segments(
    e1000_state_t *state, const uint8_t destination_mac[6],
    const uint8_t source_address[16], const uint8_t destination_address[16],
    uint16_t source_port, uint16_t destination_port, uint32_t sequence,
    uint32_t acknowledgement, uint8_t flags, uint16_t window,
    const void *payload, size_t payload_length) {
    const uint8_t *bytes = (const uint8_t *)payload;
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE +
                  NET_TCP_HEADER_SIZE + 8U + E1000_IPV6_TCP_MSS];
    size_t offset = 0U;
    int scale = (flags & NET_TCP_FLAG_SYN) != 0U ?
        socket_tcp_syn_scale_ipv6(source_address, source_port, destination_address,
            destination_port, sequence, acknowledgement) : -1;
    do {
        size_t remaining = payload_length - offset;
        uint16_t chunk = (uint16_t)(remaining > E1000_IPV6_TCP_MSS ?
                                    E1000_IPV6_TCP_MSS : remaining);
        uint8_t segment_flags = flags;
        size_t frame_length = 0U;
        if (offset + chunk < payload_length) {
            segment_flags &= (uint8_t)~(NET_TCP_FLAG_FIN | NET_TCP_FLAG_PSH);
        }
        if (net_tcp_build_ipv6_scaled(
                frame, sizeof(frame), state->mac, destination_mac,
                source_address, destination_address, source_port,
                destination_port, sequence + (uint32_t)offset,
                acknowledgement, segment_flags, window, scale,
                bytes == 0 ? 0 : bytes + offset, chunk, &frame_length) != K_OK) {
            return K_EINVAL;
        }
        kstatus_t status = net_device_send(&state->device, frame, frame_length);
        if (status != K_OK) return status;
        offset += chunk;
    } while (offset < payload_length);
    return K_OK;
}

/* A successfully submitted query still leaves the caller waiting for its
 * reply. Ring backpressure is equally retryable; never turn it into EIO. */
static kstatus_t e1000_request_arp(e1000_state_t *state,
                                   uint32_t source, uint32_t neighbor) {
    uint8_t frame[NET_ARP_FRAME_SIZE];
    size_t length = 0U;
    const uint8_t broadcast[6] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    kstatus_t status = net_arp_build_ipv4(
        frame, sizeof(frame), state->mac, broadcast, NET_ARP_OPERATION_REQUEST,
        source, state->mac, neighbor, (const uint8_t[6]){0}, &length);
    if (status != K_OK) return status;
    status = net_device_send(&state->device, frame, length);
    return status == K_OK ? K_EAGAIN : status;
}

kstatus_t e1000_tcp_ipv4_output(
    void *context, uint32_t source_address, uint16_t source_port,
    uint32_t destination_address, uint16_t destination_port,
    uint32_t sequence, uint32_t acknowledgement, uint8_t flags,
    uint16_t window, const void *payload, size_t payload_length) {
    e1000_state_t *state = (e1000_state_t *)context;
    uint8_t destination_mac[6] = {0};
    uint32_t neighbor_address;
    if (state == 0 || !state->initialized || !state->link_up ||
        source_address == 0U || source_port == 0U || destination_address == 0U ||
        destination_port == 0U || payload_length > SOCKET_MAX_PAYLOAD ||
        (payload == 0 && payload_length != 0U)) return K_EINVAL;

    /* 鑷韩鍙戝線鑷韩鐨勬姤鏂囧彲浠ョ洿鎺ヤ娇鐢ㄦ湰鏈?MAC锛屽叾浣欑洰鏍囧繀椤荤粡杩?ARP銆?*/
    neighbor_address = e1000_ipv4_next_hop(state, destination_address);
    if (destination_address == source_address ||
        (state->ipv4_address != 0U && destination_address == state->ipv4_address)) {
        memcpy(destination_mac, state->mac, sizeof(destination_mac));
    } else if (!net_arp_cache_lookup(&state->arp_cache, neighbor_address,
                                     destination_mac)) {
        kstatus_t status = e1000_request_arp(state, source_address, neighbor_address);
        if (status != K_EAGAIN) return status;
#if OS_DEBUG_SERIAL
        if (g_debug_tcp_output_logs < 32U) {
            kernel_serial_printf_serial_only(
                "OS_TCP_OUTPUT arp-miss src=%x:%u dst=%x:%u next=%x\r\n",
                source_address, source_port, destination_address,
                destination_port, neighbor_address);
            ++g_debug_tcp_output_logs;
        }
#endif
        return K_EAGAIN;
    }
    kstatus_t status = e1000_send_tcp_ipv4_segments(
        state, destination_mac, source_address, destination_address,
        source_port, destination_port, sequence, acknowledgement, flags,
        window, payload, payload_length);
#if OS_DEBUG_SERIAL
    if (g_debug_tcp_output_logs < 32U) {
        kernel_serial_printf_serial_only(
            "OS_TCP_OUTPUT arp-hit src=%x:%u dst=%x:%u status=%d\r\n",
            source_address, source_port, destination_address, destination_port,
            status);
        ++g_debug_tcp_output_logs;
    }
#endif
    return status;
}

kstatus_t e1000_udp_ipv4_output(
    void *context, uint32_t source_address, uint16_t source_port,
    uint32_t destination_address, uint16_t destination_port,
    const void *payload, size_t payload_length) {
    e1000_state_t *state = (e1000_state_t *)context;
    uint8_t destination_mac[6] = {0};
    uint32_t neighbor_address;
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV4_HEADER_SIZE +
                  NET_UDP_HEADER_SIZE + E1000_IPV4_UDP_PAYLOAD];
    size_t frame_length = 0U;
    bool ipv4_broadcast = destination_address == UINT32_MAX;
    if (state == 0 || !state->initialized || !state->link_up ||
        (source_address == 0U && !ipv4_broadcast) || source_port == 0U ||
        destination_address == 0U ||
        destination_port == 0U || payload_length > SOCKET_MAX_PAYLOAD ||
        payload_length > (size_t)(state->device.mtu - NET_IPV4_HEADER_SIZE -
                                  NET_UDP_HEADER_SIZE) ||
        (payload == 0 && payload_length != 0U)) return K_EINVAL;

    /* 鍚屾満鍦板潃涓嶉渶瑕?ARP锛涘閮?IPv4 鍦板潃蹇呴』鍏堣В鏋愬埌浠ュお缃?MAC銆?*/
    neighbor_address = e1000_ipv4_next_hop(state, destination_address);
    if (ipv4_broadcast) {
        for (uint32_t index = 0U; index < 6U; ++index) destination_mac[index] = 0xFFU;
    } else if (destination_address == source_address ||
        (state->ipv4_address != 0U && destination_address == state->ipv4_address)) {
        memcpy(destination_mac, state->mac, sizeof(destination_mac));
    } else if (!net_arp_cache_lookup(&state->arp_cache, neighbor_address,
                                     destination_mac)) {
        return e1000_request_arp(state, source_address, neighbor_address);
    }
    if (net_udp_build_ipv4(frame, sizeof(frame), state->mac, destination_mac,
                           source_address, destination_address, source_port,
                           destination_port, payload, (uint16_t)payload_length,
                           &frame_length) != K_OK) return K_EINVAL;
    return net_device_send(&state->device, frame, frame_length);
}

bool e1000_address6_equal(const uint8_t left[16], const uint8_t right[16]) {
    uint8_t difference = 0U;
    for (uint32_t index = 0U; index < 16U; ++index) difference |= left[index] ^ right[index];
    return difference == 0U;
}

bool e1000_address6_zero(const uint8_t address[16]) {
    uint8_t value = 0U;
    if (address == 0) return true;
    for (uint32_t index = 0U; index < 16U; ++index) value |= address[index];
    return value == 0U;
}

static kstatus_t e1000_send_neighbor_solicitation(e1000_state_t *state,
                                              const uint8_t source_address[16],
                                              const uint8_t target_address[16]) {
    uint8_t destination_address[16];
    uint8_t destination_mac[6];
    uint8_t frame[NET_NDP_FRAME_SIZE];
    size_t frame_length = 0U;
    if (state == 0 || source_address == 0 || target_address == 0 ||
        e1000_address6_zero(target_address)) return K_EINVAL;
    net_ndp_solicited_node_address(target_address, destination_address);
    net_ndp_multicast_mac(destination_address, destination_mac);
    kstatus_t status = net_ndp_build_neighbor_solicitation(
               frame, sizeof(frame), state->mac, destination_mac,
               source_address, destination_address, target_address,
               &frame_length);
    if (status != K_OK) return status;
    status = net_device_send(&state->device, frame, frame_length);
    return status == K_OK ? K_EAGAIN : status;
}

bool e1000_send_neighbor_advertisement(e1000_state_t *state,
                                              const net_ndp_view_t *request) {
    static const uint8_t all_nodes_address[16] = {
        0xFFU, 0x02U, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
    };
    static const uint8_t all_nodes_mac[6] = {0x33U, 0x33U, 0, 0, 0, 1};
    uint8_t destination_address[16];
    uint8_t destination_mac[6];
    uint8_t frame[NET_NDP_FRAME_SIZE];
    size_t frame_length = 0U;
    uint32_t flags = NET_NDP_FLAG_OVERRIDE;
    if (state == 0 || request == 0 || !state->ipv6_address_configured ||
        !e1000_address6_equal(request->target_address, state->ipv6_address)) return false;
    if (e1000_address6_zero(request->source_address)) {
        memcpy(destination_address, all_nodes_address, sizeof(destination_address));
        memcpy(destination_mac, all_nodes_mac, sizeof(destination_mac));
    } else {
        memcpy(destination_address, request->source_address, sizeof(destination_address));
        memcpy(destination_mac, request->source_mac, sizeof(destination_mac));
        flags |= NET_NDP_FLAG_SOLICITED;
    }
    return net_ndp_build_neighbor_advertisement(
               frame, sizeof(frame), state->mac, destination_mac,
               state->ipv6_address, destination_address, state->ipv6_address,
               flags, state->mac, &frame_length) == K_OK &&
           net_device_send(&state->device, frame, frame_length) == K_OK;
}

kstatus_t e1000_tcp_ipv6_output(
    void *context, const uint8_t source_address[16], uint16_t source_port,
    const uint8_t destination_address[16], uint16_t destination_port,
    uint32_t sequence, uint32_t acknowledgement, uint8_t flags,
    uint16_t window, const void *payload, size_t payload_length) {
    e1000_state_t *state = (e1000_state_t *)context;
    uint8_t destination_mac[6] = {0};
    if (state == 0 || !state->initialized || !state->link_up || source_address == 0 ||
        destination_address == 0 || source_port == 0U || destination_port == 0U ||
        payload_length > SOCKET_MAX_PAYLOAD || (payload == 0 && payload_length != 0U)) {
        return K_EINVAL;
    }
    /* 褰撳墠 e1000 鍚庣鍏堟敮鎸佹湰鏈?IPv6/loopback锛涘閮ㄧ洰鏍囧緟 NDP 閭诲眳缂撳瓨鎺ュ叆銆?*/
    if (e1000_address6_equal(destination_address, source_address) ||
        (state->ipv6_address_configured &&
         e1000_address6_equal(destination_address, state->ipv6_address))) {
        memcpy(destination_mac, state->mac, sizeof(destination_mac));
    } else if (!net_ipv6_neighbor_cache_lookup(&state->ndp_cache, destination_address,
                                               destination_mac)) {
        return e1000_send_neighbor_solicitation(state, source_address, destination_address);
    }
    return e1000_send_tcp_ipv6_segments(
        state, destination_mac, source_address, destination_address,
        source_port, destination_port, sequence, acknowledgement, flags,
        window, payload, payload_length);
}

kstatus_t e1000_udp_ipv6_output(
    void *context, const uint8_t source_address[16], uint16_t source_port,
    const uint8_t destination_address[16], uint16_t destination_port,
    const void *payload, size_t payload_length) {
    e1000_state_t *state = (e1000_state_t *)context;
    uint8_t destination_mac[6] = {0};
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE +
                  NET_UDP_HEADER_SIZE + E1000_IPV6_UDP_PAYLOAD];
    size_t frame_length = 0U;
    if (state == 0 || !state->initialized || !state->link_up ||
        source_address == 0 || destination_address == 0 || source_port == 0U ||
        destination_port == 0U || payload_length > SOCKET_MAX_PAYLOAD ||
        payload_length > (size_t)(state->device.mtu - NET_IPV6_HEADER_SIZE -
                                  NET_UDP_HEADER_SIZE) ||
        (payload == 0 && payload_length != 0U)) return K_EINVAL;

    /* 鍚屾満 IPv6 浠ュ強鏈満閰嶇疆鍦板潃鍙互鐩存帴浜ょ粰 PHY loopback 鎴栫綉鍗″彂閫併€?*/
    if (e1000_address6_equal(destination_address, source_address) ||
        (state->ipv6_address_configured &&
         e1000_address6_equal(destination_address, state->ipv6_address))) {
        memcpy(destination_mac, state->mac, sizeof(destination_mac));
    } else if (!net_ipv6_neighbor_cache_lookup(&state->ndp_cache, destination_address,
                                               destination_mac)) {
        return e1000_send_neighbor_solicitation(state, source_address, destination_address);
    }
    if (net_udp_build_ipv6(frame, sizeof(frame), state->mac, destination_mac,
                           source_address, destination_address, source_port,
                           destination_port, payload, (uint16_t)payload_length,
                           &frame_length) != K_OK) return K_EINVAL;
    return net_device_send(&state->device, frame, frame_length);
}

bool e1000_arp_reply(e1000_state_t *state, const net_arp_view_t *request) {
    uint8_t frame[NET_ARP_FRAME_SIZE];
    size_t frame_length = 0;
    if (state == 0 || request == 0 || state->ipv4_address == 0U ||
        request->operation != NET_ARP_OPERATION_REQUEST ||
        request->target_address != state->ipv4_address) return true;
    if (net_arp_build_ipv4(frame, sizeof(frame), state->mac, request->sender_mac,
                           NET_ARP_OPERATION_REPLY, state->ipv4_address, state->mac,
                           request->sender_address, request->sender_mac,
                           &frame_length) != K_OK ||
        net_device_send(&state->device, frame, frame_length) != K_OK) {
        e1000_record_error(15U);
        return false;
    }
    return true;
}

bool e1000_tcp_loopback_peer_ack(e1000_state_t *state,
                                        const e1000_rx_packet_t *packet,
                                        const socket_tcp_reply_t *reply) {
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV4_HEADER_SIZE +
                  NET_TCP_HEADER_SIZE];
    size_t frame_length = 0U;
    if (state == 0 || packet == 0 || reply == 0 || !state->tcp_loopback_peer_enabled ||
        !reply->valid || reply->flags != (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK)) return false;
    if (net_tcp_build_ipv4(
            frame, sizeof(frame), packet->source_mac, packet->destination_mac,
            packet->source_address, packet->destination_address,
            packet->source_port, packet->destination_port,
            packet->sequence + 1U, reply->sequence + 1U, NET_TCP_FLAG_ACK,
            packet->window, 0, 0U, &frame_length) != K_OK ||
        !e1000_transmit(state, frame, frame_length)) return false;
    state->tcp_loopback_synack_seen = true;
    return true;
}

bool e1000_tcp_loopback_peer_ack6(e1000_state_t *state,
                                         const e1000_rx_packet_t *packet,
                                         const socket_tcp_reply_t *reply) {
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE +
                  NET_TCP_HEADER_SIZE];
    size_t frame_length = 0U;
    if (state == 0 || packet == 0 || reply == 0 || !state->tcp_loopback_peer_enabled ||
        !reply->valid || reply->flags != (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK)) return false;
    if (net_tcp_build_ipv6(
            frame, sizeof(frame), packet->source_mac, packet->destination_mac,
            packet->source_address6, packet->destination_address6,
            packet->source_port, packet->destination_port,
            packet->sequence + 1U, reply->sequence + 1U, NET_TCP_FLAG_ACK,
            packet->window, 0, 0U, &frame_length) != K_OK ||
        !e1000_transmit(state, frame, frame_length)) return false;
    state->tcp_loopback_synack_seen = true;
    return true;
}


