#include <kernel/net_core.h>
#include <uapi/socket.h>

#include "internal.h"

/* REFACTOR_P8_NET_SELF_TEST_OWNER：协议和缓存回归测试夹具。 */

bool net_core_self_test(void) {
    static const uint8_t source_mac[6] = {0x02, 0, 0, 0, 0, 1};
    static const uint8_t destination_mac[6] = {0x02, 0, 0, 0, 0, 2};
    static const uint8_t payload[] = {'h', 'e', 'l', 'l', 'o'};
    uint8_t frame[128];
    uint8_t cached_mac[6];
    net_udp_view_t view;
    net_arp_cache_t cache;
    net_device_t device;
    size_t frame_length = 0;
    net_device_init(&device, "test", source_mac, 1500U, 0, 0);
    if (device.mtu != 1500U || device.max_frame_size != 1518U ||
        device.transmit != 0 || net_device_send(&device, frame, 1U) != K_EINVAL) {
        return false;
    }
    if (net_udp_build_ipv4(frame, sizeof(frame), source_mac, destination_mac,
                           0xC0A80002U, 0xC0A80001U, 1234U, 4321U,
                           payload, sizeof(payload), &frame_length) != K_OK ||
        net_udp_parse_ipv4(frame, frame_length, &view) != K_OK ||
        view.source_address != 0xC0A80002U || view.destination_address != 0xC0A80001U ||
        view.source_port != 1234U || view.destination_port != 4321U ||
        view.payload_length != sizeof(payload) ||
        !net_mac_equal(view.source_mac, source_mac) ||
        !net_mac_equal(view.destination_mac, destination_mac)) return false;
    frame[NET_ETHERNET_HEADER_SIZE + 10U] ^= 1U;
    if (net_udp_parse_ipv4(frame, frame_length, &view) == K_OK) return false;
    net_arp_cache_init(&cache);
    if (net_arp_cache_update(&cache, 0xC0A80001U, destination_mac, 1U) != K_OK ||
        !net_arp_cache_lookup(&cache, 0xC0A80001U, cached_mac) ||
        !net_mac_equal(cached_mac, destination_mac)) return false;
    return !net_arp_cache_lookup(&cache, 0xC0A800FEU, cached_mac);
}

bool net_arp_self_test(void) {
    static const uint8_t source_mac[6] = {0x02, 0, 0, 0, 0, 7};
    static const uint8_t destination_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t empty_mac[6] = {0, 0, 0, 0, 0, 0};
    uint8_t frame[64];
    net_arp_view_t view;
    size_t frame_length = 0U;
    if (net_arp_build_ipv4(frame, sizeof(frame), source_mac, destination_mac,
                           NET_ARP_OPERATION_REQUEST, 0xC0A80002U, source_mac,
                           0xC0A80001U, empty_mac, &frame_length) != K_OK ||
        frame_length != NET_ARP_FRAME_SIZE || net_arp_parse_ipv4(frame, frame_length,
                                                                  &view) != K_OK ||
        view.operation != NET_ARP_OPERATION_REQUEST || view.sender_address != 0xC0A80002U ||
        view.target_address != 0xC0A80001U || !net_mac_equal(view.sender_mac, source_mac) ||
        !net_mac_equal(view.target_mac, empty_mac)) return false;
    frame[21] = 3U;
    return net_arp_parse_ipv4(frame, frame_length, &view) != K_OK;
}

bool net_ipv6_self_test(void) {
    static const uint8_t source_mac[6] = {0x02, 0, 0, 0, 0, 3};
    static const uint8_t destination_mac[6] = {0x02, 0, 0, 0, 0, 4};
    static const uint8_t source_address[16] = {
        0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
    };
    static const uint8_t destination_address[16] = {
        0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2
    };
    static const uint8_t payload[] = {'v', '6'};
    uint8_t frame[128];
    net_ipv6_udp_view_t view;
    size_t frame_length = 0;
    if (net_udp_build_ipv6(frame, sizeof(frame), source_mac, destination_mac,
                           source_address, destination_address, 1234U, 4321U,
                           payload, sizeof(payload), &frame_length) != K_OK ||
        net_udp_parse_ipv6(frame, frame_length, &view) != K_OK ||
        view.source_port != 1234U || view.destination_port != 4321U ||
        view.payload_length != sizeof(payload)) return false;
    for (uint32_t index = 0; index < 16U; ++index) {
        if (view.source_address[index] != source_address[index] ||
            view.destination_address[index] != destination_address[index]) return false;
    }
    if (view.payload[0] != payload[0] || view.payload[1] != payload[1]) return false;
    frame[NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE + NET_UDP_HEADER_SIZE] ^= 1U;
    return net_udp_parse_ipv6(frame, frame_length, &view) != K_OK;
}

bool net_ndp_self_test(void) {
    static const uint8_t source_mac[6] = {0x02, 0, 0, 0, 0, 7};
    static const uint8_t destination_mac[6] = {0x33, 0x33, 0xFF, 0, 0, 2};
    static const uint8_t source_address[16] = {
        0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 1
    };
    static const uint8_t target_address[16] = {
        0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 2
    };
    uint8_t solicited_node[16];
    uint8_t multicast_mac[6];
    uint8_t frame[NET_NDP_FRAME_SIZE];
    uint8_t cached_mac[6];
    net_ndp_view_t view;
    net_ipv6_neighbor_cache_t cache;
    size_t frame_length = 0U;
    net_ndp_solicited_node_address(target_address, solicited_node);
    net_ndp_multicast_mac(solicited_node, multicast_mac);
    if (!net_mac_equal(multicast_mac, destination_mac) ||
        net_ndp_build_neighbor_solicitation(frame, sizeof(frame), source_mac,
                                            multicast_mac, source_address,
                                            solicited_node, target_address,
                                            &frame_length) != K_OK ||
        frame_length != NET_NDP_FRAME_SIZE ||
        net_ndp_parse_neighbor_solicitation(frame, frame_length, &view) != K_OK ||
        !net_address6_equal(view.target_address, target_address) ||
        !net_mac_equal(view.target_mac, source_mac)) return false;
    if (net_ndp_build_neighbor_advertisement(frame, sizeof(frame), source_mac,
                                             source_mac, target_address,
                                             source_address, target_address,
                                             NET_NDP_FLAG_SOLICITED |
                                             NET_NDP_FLAG_OVERRIDE,
                                             source_mac, &frame_length) != K_OK ||
        net_ndp_parse_neighbor_advertisement(frame, frame_length, &view) != K_OK ||
        view.flags != (NET_NDP_FLAG_SOLICITED | NET_NDP_FLAG_OVERRIDE) ||
        !net_address6_equal(view.target_address, target_address) ||
        !net_mac_equal(view.target_mac, source_mac)) return false;
    net_ipv6_neighbor_cache_init(&cache);
    if (net_ipv6_neighbor_cache_update(&cache, target_address, source_mac, 1U) != K_OK ||
        !net_ipv6_neighbor_cache_lookup(&cache, target_address, cached_mac) ||
        !net_mac_equal(cached_mac, source_mac)) return false;
    frame[NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE + 8U] ^= 1U;
    return net_ndp_parse_neighbor_advertisement(frame, frame_length, &view) != K_OK;
}

bool net_tcp_self_test(void) {
    static const uint8_t source_mac[6] = {0x02, 0, 0, 0, 0, 5};
    static const uint8_t destination_mac[6] = {0x02, 0, 0, 0, 0, 6};
    static const uint8_t source_address6[16] = {
        0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5
    };
    static const uint8_t destination_address6[16] = {
        0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6
    };
    static const uint8_t payload[] = {'t', 'c', 'p'};
    uint8_t frame[256];
    net_tcp_view_t view;
    size_t frame_length = 0;
    if (net_tcp_build_ipv4(frame, sizeof(frame), source_mac, destination_mac,
                           0xC0A80002U, 0xC0A80001U, 1234U, 4321U,
                           100U, 200U, NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK,
                           4096U, payload, sizeof(payload), &frame_length) != K_OK ||
        net_tcp_parse_ipv4(frame, frame_length, &view) != K_OK ||
        view.source_address != 0xC0A80002U || view.destination_address != 0xC0A80001U ||
        view.source_port != 1234U || view.destination_port != 4321U ||
        view.sequence != 100U || view.acknowledgement != 200U ||
        view.flags != (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK) ||
        view.window != 4096U || view.payload_length != sizeof(payload) ||
        view.payload[0] != payload[0] || view.payload[1] != payload[1] ||
        view.payload[2] != payload[2]) return false;
    frame[NET_ETHERNET_HEADER_SIZE + NET_IPV4_HEADER_SIZE + NET_TCP_HEADER_SIZE] ^= 1U;
    if (net_tcp_parse_ipv4(frame, frame_length, &view) == K_OK) return false;
    if (net_tcp_build_ipv6(frame, sizeof(frame), source_mac, destination_mac,
                           source_address6, destination_address6, 1234U, 4321U,
                           300U, 400U, NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH,
                           8192U, payload, sizeof(payload), &frame_length) != K_OK ||
        net_tcp_parse_ipv6(frame, frame_length, &view) != K_OK ||
        view.source_port != 1234U || view.destination_port != 4321U ||
        view.sequence != 300U || view.acknowledgement != 400U ||
        view.flags != (NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH) ||
        view.window != 8192U || view.payload_length != sizeof(payload)) return false;
    for (uint32_t index = 0U; index < 16U; ++index) {
        if (view.source_address6[index] != source_address6[index] ||
            view.destination_address6[index] != destination_address6[index]) return false;
    }
    frame[NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE + NET_TCP_HEADER_SIZE] ^= 1U;
    return net_tcp_parse_ipv6(frame, frame_length, &view) != K_OK;
}

