/* REFACTOR_P8_E1000_RUNTIME_OWNER */

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/dma.h>
#include <kernel/console.h>
#include <kernel/net_core.h>
#include <kernel/socket.h>
#include <kernel/string.h>
#include <kernel/telemetry.h>

#include "core_internal.h"

#if OS_DEBUG_SERIAL
static uint32_t g_debug_arp_rx_logs;
static uint32_t g_debug_udp_rx_logs;
static uint32_t g_debug_udp_dispatch_logs;
#endif

static bool e1000_build_tcp_reset_reply(const e1000_rx_packet_t *packet,
                                        socket_tcp_reply_t *reply) {
    if (reply == 0) return false;
    memset(reply, 0, sizeof(*reply));
    if (packet == 0 || packet->protocol != SOCKET_PROTOCOL_TCP ||
        packet->payload_length > sizeof(packet->payload) ||
        (packet->flags & NET_TCP_FLAG_RST) != 0U) {
        return false;
    }

    reply->valid = true;
    reply->window = 0U;
    if ((packet->flags & NET_TCP_FLAG_ACK) != 0U) {
        /* RFC 9293: reset an ACK-bearing segment at its acknowledged seq. */
        reply->flags = NET_TCP_FLAG_RST;
        reply->sequence = packet->acknowledgement;
    } else {
        /* A reset to a non-ACK segment acknowledges its sequence space. */
        reply->flags = NET_TCP_FLAG_RST | NET_TCP_FLAG_ACK;
        reply->acknowledgement = packet->sequence + packet->payload_length;
        if ((packet->flags & NET_TCP_FLAG_SYN) != 0U)
            ++reply->acknowledgement;
        if ((packet->flags & NET_TCP_FLAG_FIN) != 0U)
            ++reply->acknowledgement;
    }
    return true;
}

static bool e1000_tcp_self_test_peer_reply(const e1000_state_t *state,
                                          const e1000_rx_packet_t *packet) {
    /* PHY loopback returns our SYN-ACK to the synthetic peer. Its ACK has
     * already been injected by e1000_tcp_loopback_peer_ack[6]; there is no
     * peer socket to receive this SYN-ACK, and resetting it kills the test's
     * real accepted socket. Never suppress resets on the live network. */
    return state->phy_loopback && state->tcp_loopback_peer_enabled &&
           state->tcp_loopback_synack_seen && packet->payload_length == 0U &&
           packet->flags == (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK);
}

bool e1000_unmatched_tcp_reset_self_test(void) {
    e1000_rx_packet_t packet = {0};
    socket_tcp_reply_t reply = {0};

    packet.protocol = SOCKET_PROTOCOL_TCP;
    packet.flags = NET_TCP_FLAG_ACK | NET_TCP_FLAG_FIN;
    packet.acknowledgement = 0x12345678U;
    if (!e1000_build_tcp_reset_reply(&packet, &reply) || !reply.valid ||
        reply.flags != NET_TCP_FLAG_RST ||
        reply.sequence != packet.acknowledgement ||
        reply.acknowledgement != 0U) {
        return false;
    }

    packet.sequence = 0xFFFFFFFDU;
    packet.flags = NET_TCP_FLAG_SYN;
    packet.payload_length = 4U;
    if (!e1000_build_tcp_reset_reply(&packet, &reply) || !reply.valid ||
        reply.flags != (NET_TCP_FLAG_RST | NET_TCP_FLAG_ACK) ||
        reply.sequence != 0U || reply.acknowledgement != 2U) {
        return false;
    }

    packet.flags = NET_TCP_FLAG_RST | NET_TCP_FLAG_ACK;
    return !e1000_build_tcp_reset_reply(&packet, &reply) && !reply.valid;
}

static bool e1000_enqueue_packet(e1000_state_t *state,
                                 const e1000_rx_packet_t *packet,
                                 const uint8_t *payload,
                                 uint32_t flow_hash) {
    uint32_t queue_index;
    e1000_software_queue_t *queue;
    if (state == 0 || packet == 0 || packet->payload_length > SOCKET_MAX_PAYLOAD) {
        return false;
    }
    queue_index = e1000_select_software_queue(state->software_queue_count,
                                              flow_hash);
    queue = &state->software_queues[queue_index];
    return e1000_queue_push_payload(queue, packet, payload);
}

static bool e1000_send_tcp_reply(e1000_state_t *state,
                                 const e1000_rx_packet_t *packet,
                                 const socket_tcp_reply_t *reply) {
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE +
                  NET_TCP_SYN_HEADER_SIZE];
    size_t frame_length = 0U;
    if (state == 0 || packet == 0 || reply == 0 || !reply->valid) return false;
    bool ipv6 = packet->family == OS_AF_INET6;
    if (!ipv6 && packet->family != OS_AF_INET4) return false;
    kstatus_t status = ipv6 ? net_tcp_build_ipv6(
               frame, sizeof(frame), packet->destination_mac, packet->source_mac,
               packet->destination_address6, packet->source_address6,
               packet->destination_port, packet->source_port,
               reply->sequence, reply->acknowledgement, reply->flags,
               reply->window, 0, 0U, &frame_length) : net_tcp_build_ipv4(
               frame, sizeof(frame), packet->destination_mac, packet->source_mac,
               packet->destination_address, packet->source_address,
               packet->destination_port, packet->source_port,
               reply->sequence, reply->acknowledgement, reply->flags,
               reply->window, 0, 0U, &frame_length);
    if (status == K_OK) status = net_device_send(&state->device, frame, frame_length);
    /* Failed ACK output must reach the transport's retry owner for either
     * family; a local TX queue being full is not successful delivery. */
    if (ipv6) {
        socket_tcp_ack_output_ipv6(packet->destination_address6, packet->destination_port,
            packet->source_address6, packet->source_port, reply->flags, status);
    } else {
        socket_tcp_ack_output_ipv4(packet->destination_address, packet->destination_port,
            packet->source_address, packet->source_port, reply->flags, status);
    }
    bool sent = status == K_OK;
    if (sent && !state->tcp_loopback_peer_enabled && packet->payload_length != 0U) {
        state->tcp_loopback_data_ack_seen = true;
    }
    return sent;
}

bool e1000_dispatch_software_queues(e1000_state_t *state) {
    bool received = false;
    if (state == 0) return false;
    for (uint32_t queue_index = 0; queue_index < state->software_queue_count;
         ++queue_index) {
        e1000_software_queue_t *queue = &state->software_queues[queue_index];
        const e1000_rx_packet_t *packet;
        while ((packet = e1000_queue_borrow(queue)) != 0) {
            kstatus_t status;
            if (packet->protocol == SOCKET_PROTOCOL_TCP && packet->family == OS_AF_INET4) {
                if ((packet->flags & NET_TCP_FLAG_RST) != 0U) {
                    socket_tcp_reply_t reply = {0};
                    status = socket_inject_tcp_ack_ipv4_reply(
                        packet->source_address, packet->source_port,
                        packet->destination_address, packet->destination_port,
                        packet->sequence, packet->acknowledgement, packet->flags,
                        packet->window, &reply);
                    if (reply.valid && !e1000_send_tcp_reply(state, packet, &reply)) status = K_EIO;
                } else if ((packet->flags & NET_TCP_FLAG_SYN) != 0U &&
                    (packet->flags & NET_TCP_FLAG_ACK) == 0U) {
                    socket_tcp_reply_t reply = {0};
                    status = socket_inject_tcp_syn_ipv4_scaled(
                        packet->source_address, packet->source_port,
                        packet->destination_address, packet->destination_port,
                        packet->sequence, packet->peer_window_scale_present ?
                            packet->peer_window_scale : -1, &reply);
                    socket_tcp_mss_ipv4(packet->source_address, packet->source_port,
                        packet->destination_address, packet->destination_port,
                        packet->sequence, packet->acknowledgement, packet->flags, packet->peer_mss);
                    if (status == K_OK && reply.valid) {
                        uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV4_HEADER_SIZE +
                                      NET_TCP_SYN_HEADER_SIZE + 4U];
                        size_t frame_length = 0U;
                        int scale = socket_tcp_syn_scale_ipv4(packet->destination_address,
                            packet->destination_port, packet->source_address, packet->source_port,
                            reply.sequence, reply.acknowledgement);
                        if (net_tcp_build_ipv4_scaled(
                                frame, sizeof(frame), packet->destination_mac,
                                packet->source_mac, packet->destination_address,
                                packet->source_address, packet->destination_port,
                                packet->source_port, reply.sequence,
                                reply.acknowledgement, reply.flags, reply.window, scale,
                                0, 0U, &frame_length) != K_OK ||
                            net_device_send(&state->device, frame, frame_length) != K_OK) {
                            status = K_EIO;
                        } else if (state->tcp_loopback_peer_enabled &&
                                   !e1000_tcp_loopback_peer_ack(state, packet, &reply)) {
                            status = K_EIO;
                        }
                    }
                } else if ((packet->flags & NET_TCP_FLAG_SYN) != 0U ||
                           ((packet->flags & NET_TCP_FLAG_ACK) != 0U &&
                            packet->payload_length == 0U &&
                            (packet->flags & (NET_TCP_FLAG_FIN | NET_TCP_FLAG_PSH)) == 0U)) {
                    socket_tcp_reply_t reply = {0};
                    if ((packet->flags & NET_TCP_FLAG_SYN) != 0U) {
                        socket_tcp_scale_ipv4(packet->source_address, packet->source_port,
                            packet->destination_address, packet->destination_port,
                            packet->sequence, packet->acknowledgement, packet->flags,
                            packet->peer_window_scale_present ? packet->peer_window_scale : -1);
                        socket_tcp_mss_ipv4(packet->source_address, packet->source_port,
                            packet->destination_address, packet->destination_port,
                            packet->sequence, packet->acknowledgement, packet->flags, packet->peer_mss);
                    }
                    status = socket_inject_tcp_ack_ipv4_reply(
                        packet->source_address, packet->source_port,
                        packet->destination_address, packet->destination_port,
                        packet->sequence, packet->acknowledgement, packet->flags,
                        packet->window, &reply);
                    if (status == K_OK && reply.valid &&
                        !e1000_send_tcp_reply(state, packet, &reply)) {
                        status = K_EIO;
                    }
                } else {
                    socket_tcp_reply_t reply = {0};
                    /* ACK is independent of payload/PSH/FIN. Complete the
                     * local send before applying receive-window policy. */
                    if ((packet->flags & NET_TCP_FLAG_ACK) != 0U) {
                        (void)socket_inject_tcp_ack_ipv4_window(
                            packet->source_address, packet->source_port,
                            packet->destination_address, packet->destination_port,
                            packet->sequence, packet->acknowledgement, packet->flags,
                            packet->window);
                    }
                    status = socket_inject_tcp_ipv4_reply(
                        packet->source_address, packet->source_port,
                        packet->destination_address, packet->destination_port,
                        packet->sequence, packet->flags, packet->payload,
                        packet->payload_length, &reply);
                    /* 杩炵画鏁版嵁銆侀噸澶嶆暟鎹€佷贡搴忔暟鎹拰 FIN 閮借杩斿洖褰撳墠 ACK/绐楀彛銆?*/
                    if (reply.valid && !e1000_send_tcp_reply(state, packet, &reply)) {
                        status = K_EIO;
                    }
                }
                if (status == K_ENOENT &&
                    !e1000_tcp_self_test_peer_reply(state, packet)) {
                    socket_tcp_reply_t reset = {0};
                    if (e1000_build_tcp_reset_reply(packet, &reset) &&
                        !e1000_send_tcp_reply(state, packet, &reset)) {
                        status = K_EIO;
                    }
                }
            } else if (packet->protocol == SOCKET_PROTOCOL_TCP && packet->family == OS_AF_INET6) {
                if ((packet->flags & NET_TCP_FLAG_RST) != 0U) {
                    socket_tcp_reply_t reply = {0};
                    status = socket_inject_tcp_ack_ipv6_reply(
                        packet->source_address6, packet->source_port,
                        packet->destination_address6, packet->destination_port,
                        packet->sequence, packet->acknowledgement, packet->flags,
                        packet->window, &reply);
                    if (reply.valid && !e1000_send_tcp_reply(state, packet, &reply)) status = K_EIO;
                } else if ((packet->flags & NET_TCP_FLAG_SYN) != 0U &&
                    (packet->flags & NET_TCP_FLAG_ACK) == 0U) {
                    socket_tcp_reply_t reply = {0};
                    status = socket_inject_tcp_syn_ipv6_scaled(
                        packet->source_address6, packet->source_port,
                        packet->destination_address6, packet->destination_port,
                        packet->sequence, packet->peer_window_scale_present ?
                            packet->peer_window_scale : -1, &reply);
                    socket_tcp_mss_ipv6(packet->source_address6, packet->source_port,
                        packet->destination_address6, packet->destination_port,
                        packet->sequence, packet->acknowledgement, packet->flags, packet->peer_mss);
                    if (status == K_OK && reply.valid) {
                        uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE +
                                      NET_TCP_SYN_HEADER_SIZE + 4U];
                        size_t frame_length = 0U;
                        int scale = socket_tcp_syn_scale_ipv6(packet->destination_address6,
                            packet->destination_port, packet->source_address6, packet->source_port,
                            reply.sequence, reply.acknowledgement);
                        if (net_tcp_build_ipv6_scaled(
                                frame, sizeof(frame), packet->destination_mac,
                                packet->source_mac, packet->destination_address6,
                                packet->source_address6, packet->destination_port,
                                packet->source_port, reply.sequence,
                                reply.acknowledgement, reply.flags, reply.window, scale,
                                0, 0U, &frame_length) != K_OK ||
                            net_device_send(&state->device, frame, frame_length) != K_OK) {
                            status = K_EIO;
                        } else if (state->tcp_loopback_peer_enabled &&
                                   !e1000_tcp_loopback_peer_ack6(state, packet, &reply)) {
                            status = K_EIO;
                        }
                    }
                } else if ((packet->flags & NET_TCP_FLAG_SYN) != 0U ||
                           ((packet->flags & NET_TCP_FLAG_ACK) != 0U &&
                            packet->payload_length == 0U &&
                            (packet->flags & (NET_TCP_FLAG_FIN | NET_TCP_FLAG_PSH)) == 0U)) {
                    socket_tcp_reply_t reply = {0};
                    if ((packet->flags & NET_TCP_FLAG_SYN) != 0U) {
                        socket_tcp_scale_ipv6(packet->source_address6, packet->source_port,
                            packet->destination_address6, packet->destination_port,
                            packet->sequence, packet->acknowledgement, packet->flags,
                            packet->peer_window_scale_present ? packet->peer_window_scale : -1);
                        socket_tcp_mss_ipv6(packet->source_address6, packet->source_port,
                            packet->destination_address6, packet->destination_port,
                            packet->sequence, packet->acknowledgement, packet->flags, packet->peer_mss);
                    }
                    status = socket_inject_tcp_ack_ipv6_reply(
                        packet->source_address6, packet->source_port,
                        packet->destination_address6, packet->destination_port,
                        packet->sequence, packet->acknowledgement, packet->flags,
                        packet->window, &reply);
                    if (status == K_OK && reply.valid &&
                        !e1000_send_tcp_reply(state, packet, &reply)) {
                        status = K_EIO;
                    }
                } else {
                    socket_tcp_reply_t reply = {0};
                    if ((packet->flags & NET_TCP_FLAG_ACK) != 0U) {
                        (void)socket_inject_tcp_ack_ipv6_window(
                            packet->source_address6, packet->source_port,
                            packet->destination_address6, packet->destination_port,
                            packet->sequence, packet->acknowledgement, packet->flags,
                            packet->window);
                    }
                    status = socket_inject_tcp_ipv6_reply(
                        packet->source_address6, packet->source_port,
                        packet->destination_address6, packet->destination_port,
                        packet->sequence, packet->flags, packet->payload,
                        packet->payload_length, &reply);
                    if (reply.valid && !e1000_send_tcp_reply(state, packet, &reply)) {
                        status = K_EIO;
                    }
                }
                if (status == K_ENOENT &&
                    !e1000_tcp_self_test_peer_reply(state, packet)) {
                    socket_tcp_reply_t reset = {0};
                    if (e1000_build_tcp_reset_reply(packet, &reset) &&
                        !e1000_send_tcp_reply(state, packet, &reset)) {
                        status = K_EIO;
                    }
                }
            } else if (packet->protocol == SOCKET_PROTOCOL_UDP && packet->family == OS_AF_INET6) {
                status = socket_inject_udp_ipv6(packet->source_address6,
                                                packet->source_port,
                                                packet->destination_address6,
                                                packet->destination_port,
                                                packet->payload,
                                                packet->payload_length);
            } else if (packet->protocol == SOCKET_PROTOCOL_UDP) {
                status = socket_inject_udp(packet->source_address,
                                           packet->source_port,
                                           packet->destination_address,
                                           packet->destination_port,
                                           packet->payload,
                                           packet->payload_length);
#if OS_DEBUG_SERIAL
                if (g_debug_udp_dispatch_logs < 128U) {
                    kernel_serial_printf_serial_only(
                        "OS_UDP_DISPATCH src=%x:%u dst=%x:%u status=%d\r\n",
                        packet->source_address, packet->source_port,
                        packet->destination_address, packet->destination_port,
                        status);
                    ++g_debug_udp_dispatch_logs;
                }
#endif
            } else status = K_EINVAL;
            if (status == K_OK) received = true;
            e1000_queue_release(queue, packet);
        }
    }
    return received;
}

bool e1000_process_rx_frame(e1000_state_t *state, const uint8_t *buffer,
                            uint16_t length) {
    bool received = false;
    net_udp_view_t view;
    net_ipv6_udp_view_t view6;
    net_tcp_view_t tcp_view;
    net_tcp_view_t tcp_view6;
    net_arp_view_t arp_view;
    net_ndp_view_t ndp_view;
    e1000_rx_packet_t packet;
    if (state == 0 || buffer == 0 || length == 0U) return false;
    if (length != 0U &&
            net_ndp_parse_neighbor_solicitation(buffer, length,
                                                 &ndp_view) == K_OK) {
            if (!e1000_address6_zero(ndp_view.source_address)) {
                received = net_ipv6_neighbor_cache_update(
                    &state->ndp_cache, ndp_view.source_address, ndp_view.source_mac,
                    telemetry_timestamp()) == K_OK || received;
            }
            if (state->ipv6_address_configured &&
                e1000_address6_equal(ndp_view.target_address, state->ipv6_address) &&
                !e1000_send_neighbor_advertisement(state, &ndp_view)) {
                e1000_record_error(26U);
                received = true;
            }
    } else if (length != 0U &&
            net_ndp_parse_neighbor_advertisement(buffer, length,
                                                  &ndp_view) == K_OK) {
            received = net_ipv6_neighbor_cache_update(
                &state->ndp_cache, ndp_view.target_address, ndp_view.target_mac,
                telemetry_timestamp()) == K_OK || received;
    } else if (length != 0U &&
                   net_arp_parse_ipv4(buffer, length, &arp_view) == K_OK) {
            /* ARP 鍙洿鏂扮紦瀛橈紱鍥炲绛栫暐鐢变笂灞傜綉缁滅鐞嗗櫒鍐冲畾銆?*/
            bool arp_updated = net_arp_cache_update(
                &state->arp_cache, arp_view.sender_address, arp_view.sender_mac,
                telemetry_timestamp()) == K_OK;
            received = arp_updated || received;
#if OS_DEBUG_SERIAL
            if (g_debug_arp_rx_logs < 16U) {
                kernel_serial_printf_serial_only(
                    "OS_ARP_RX sender=%x updated=%u target=%x\r\n",
                    arp_view.sender_address, arp_updated ? 1U : 0U,
                    arp_view.target_address);
                ++g_debug_arp_rx_logs;
            }
#endif
            /* 鍙湁閰嶇疆浜嗘湰鏈?IPv4 涓旂洰鏍囧湴鍧€鍛戒腑鏃舵墠鍥炲 ARP銆?*/
            if (!e1000_arp_reply(state, &arp_view)) received = true;
            /* Neighbor resolution uses the bounded local retry clock. Never
             * advance the transport clock to flush it: that consumes network
             * retry budget and moves unrelated flights' deadlines forward. */
            socket_tcp_poll(x86_read_tsc());
    } else if (length != 0U &&
            net_udp_parse_ipv4(buffer, length, &view) == K_OK) {
            /* Only metadata is staged here. Queue admission copies the DMA
             * payload before the caller can recycle its RX descriptor. */
            memset(&packet, 0, offsetof(e1000_rx_packet_t, payload));
            packet.family = OS_AF_INET4;
            packet.protocol = SOCKET_PROTOCOL_UDP;
            packet.source_address = view.source_address;
            packet.destination_address = view.destination_address;
            packet.source_port = view.source_port;
            packet.destination_port = view.destination_port;
            packet.payload_length = view.payload_length;
#if OS_DEBUG_SERIAL
            if (g_debug_udp_rx_logs < 128U) {
                kernel_serial_printf_serial_only(
                    "OS_UDP_RX src=%x:%u dst=%x:%u len=%u\r\n",
                    view.source_address, view.source_port,
                    view.destination_address, view.destination_port,
                    view.payload_length);
                ++g_debug_udp_rx_logs;
            }
#endif
            if (view.payload_length <= sizeof(packet.payload)) {
                received = e1000_enqueue_packet(
                    state, &packet, view.payload,
                    e1000_flow_hash_ipv4(view.source_address, view.destination_address,
                                         view.source_port, view.destination_port,
                                         SOCKET_PROTOCOL_UDP)) || received;
            }
    } else if (length != 0U &&
                   net_udp_parse_ipv6(buffer, length, &view6) == K_OK) {
            memset(&packet, 0, offsetof(e1000_rx_packet_t, payload));
            packet.family = OS_AF_INET6;
            packet.protocol = SOCKET_PROTOCOL_UDP;
            memcpy(packet.source_address6, view6.source_address, 16U);
            memcpy(packet.destination_address6, view6.destination_address, 16U);
            packet.source_port = view6.source_port;
            packet.destination_port = view6.destination_port;
            packet.payload_length = view6.payload_length;
            if (view6.payload_length <= sizeof(packet.payload)) {
                received = e1000_enqueue_packet(
                    state, &packet, view6.payload,
                    e1000_flow_hash_ipv6(view6.source_address, view6.destination_address,
                                         view6.source_port, view6.destination_port)) || received;
            }
    } else if (length != 0U &&
                   net_tcp_parse_ipv4(buffer, length, &tcp_view) == K_OK) {
            memset(&packet, 0, offsetof(e1000_rx_packet_t, payload));
            packet.family = OS_AF_INET4;
            packet.protocol = SOCKET_PROTOCOL_TCP;
            packet.flags = tcp_view.flags;
            packet.source_address = tcp_view.source_address;
            packet.destination_address = tcp_view.destination_address;
            packet.source_port = tcp_view.source_port;
            packet.destination_port = tcp_view.destination_port;
            packet.sequence = tcp_view.sequence;
            packet.acknowledgement = tcp_view.acknowledgement;
            packet.window = tcp_view.window;
            packet.peer_mss = tcp_view.peer_mss;
            packet.peer_window_scale = tcp_view.peer_window_scale;
            packet.peer_window_scale_present = tcp_view.peer_window_scale_present;
            memcpy(packet.source_mac, tcp_view.source_mac, 6U);
            memcpy(packet.destination_mac, tcp_view.destination_mac, 6U);
            packet.payload_length = tcp_view.payload_length;
            if (tcp_view.payload_length <= sizeof(packet.payload)) {
                received = e1000_enqueue_packet(
                    state, &packet, tcp_view.payload,
                    e1000_flow_hash_ipv4(tcp_view.source_address,
                                         tcp_view.destination_address,
                                         tcp_view.source_port,
                                         tcp_view.destination_port,
                                         SOCKET_PROTOCOL_TCP)) || received;
            }
    } else if (length != 0U &&
                   net_tcp_parse_ipv6(buffer, length, &tcp_view6) == K_OK) {
            memset(&packet, 0, offsetof(e1000_rx_packet_t, payload));
            packet.family = OS_AF_INET6;
            packet.protocol = SOCKET_PROTOCOL_TCP;
            packet.flags = tcp_view6.flags;
            memcpy(packet.source_address6, tcp_view6.source_address6, 16U);
            memcpy(packet.destination_address6, tcp_view6.destination_address6, 16U);
            packet.source_port = tcp_view6.source_port;
            packet.destination_port = tcp_view6.destination_port;
            packet.sequence = tcp_view6.sequence;
            packet.acknowledgement = tcp_view6.acknowledgement;
            packet.window = tcp_view6.window;
            packet.peer_mss = tcp_view6.peer_mss;
            packet.peer_window_scale = tcp_view6.peer_window_scale;
            packet.peer_window_scale_present = tcp_view6.peer_window_scale_present;
            memcpy(packet.source_mac, tcp_view6.source_mac, 6U);
            memcpy(packet.destination_mac, tcp_view6.destination_mac, 6U);
            packet.payload_length = tcp_view6.payload_length;
            if (tcp_view6.payload_length <= sizeof(packet.payload)) {
                received = e1000_enqueue_packet(
                    state, &packet, tcp_view6.payload,
                    e1000_flow_hash_ipv6(tcp_view6.source_address6,
                                         tcp_view6.destination_address6,
                                         tcp_view6.source_port,
                                         tcp_view6.destination_port)) || received;
            }
        }
    return received;
}

bool e1000_poll_receive(e1000_state_t *state) {
    bool received = false;
    uint32_t processed = 0U;
    if (state == 0) return false;
    while (processed < E1000_RX_BUDGET &&
           (state->rx_ring[state->rx_clean].status & E1000_RX_STATUS_DD) != 0U) {
        e1000_descriptor_t *descriptor = &state->rx_ring[state->rx_clean];
        uint8_t *buffer = (uint8_t *)phys_to_direct(
            page_to_phys(state->rx_pages[state->rx_clean]));
        dma_sync_for_cpu(&state->rx_dma[state->rx_clean]);
        received = e1000_process_rx_frame(state, buffer, descriptor->length) ||
                   received;
        descriptor->status = 0U;
        state->rx_clean = (state->rx_clean + 1U) % E1000_RING_COUNT;
        e1000_write(state, E1000_REG_RDT,
                    (state->rx_clean + E1000_RING_COUNT - 1U) % E1000_RING_COUNT);
        ++processed;
    }
    bool dispatched = e1000_dispatch_software_queues(state);
    socket_tcp_poll(x86_read_tsc());
    return dispatched || received;
}
