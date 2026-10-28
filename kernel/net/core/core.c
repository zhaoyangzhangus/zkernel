#include <kernel/net_core.h>
#include <kernel/sched.h>
#include <uapi/socket.h>

#include "internal.h"

#define NET_IPV4_VERSION   4U
#define NET_IPV4_UDP       17U
#define NET_IPV4_TCP       6U

/* DF 未设置时，路由器可能对 IPv4 数据包分片。不同数据报必须使用不同的
 * identification 值，包括不同 TCP 段及其重传。relaxed atomic 同时串行化
 * 不同 CPU 分配的 ID；线上字段会自然按 16 位回绕。 */
static atomic_uint g_net_ipv4_identification;

static uint16_t net_next_ipv4_identification(void) {
    return (uint16_t)atomic_fetch_add_explicit(&g_net_ipv4_identification,
                                              1U, memory_order_relaxed);
}

static void net_copy(void *destination, const void *source, size_t length) {
    memmove(destination, source, length);
}

static bool net_address6_zero(const uint8_t address[16]) {
    uint8_t value = 0U;
    if (address == 0) return true;
    for (uint32_t index = 0U; index < 16U; ++index) value |= address[index];
    return value == 0U;
}

static void net_store_be16(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)(value >> 8);
    destination[1] = (uint8_t)value;
}

static void net_store_be32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}

static uint16_t net_load_be16(const uint8_t *source) {
    return (uint16_t)(((uint16_t)source[0] << 8) | source[1]);
}

static uint32_t net_load_be32(const uint8_t *source) {
    return ((uint32_t)source[0] << 24) | ((uint32_t)source[1] << 16) |
           ((uint32_t)source[2] << 8) | source[3];
}

void net_device_init(net_device_t *device, const char *name,
                     const uint8_t mac[6], uint16_t mtu,
                     net_device_transmit_fn transmit, void *context) {
    if (device == 0) return;
    net_zero(device, sizeof(*device));
    if (name != 0) {
        for (size_t index = 0U; index + 1U < sizeof(device->name) &&
                                  name[index] != '\0'; ++index) {
            device->name[index] = name[index];
        }
    }
    if (mac != 0) net_copy(device->mac, mac, sizeof(device->mac));
    device->mtu = mtu;
    device->max_frame_size = (uint16_t)(mtu + NET_ETHERNET_HEADER_SIZE + 4U);
    device->context = context;
    device->transmit = transmit;
}

kstatus_t net_device_send(net_device_t *device, const void *frame, size_t length) {
    if (device == 0 || frame == 0 || length == 0U || device->transmit == 0 ||
        length > device->max_frame_size) return K_EINVAL;
    if (!device->link_up) return K_EAGAIN;
    return device->transmit(device->context, frame, length);
}

static uint32_t net_checksum_add(uint32_t sum, const uint8_t *data,
                                 size_t length) {
    while ((sum >> 16) != 0) sum = (sum & 0xFFFFU) + (sum >> 16);
    /* 限制每批数据量，避免 32 位累加器溢出，即使调用方传入的缓冲区
     * 大于 IP 数据包也能保证安全。 */
    while (length >= 8U) {
        size_t batch = length < 65536U ? length : 65536U;
        batch &= ~(size_t)7U;
        length -= batch;
        while (batch != 0U) {
            uint64_t words;
            __builtin_memcpy(&words, data, sizeof(words));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            words = __builtin_bswap64(words);
#endif
            sum += (uint32_t)(words & 0xFFFFU) +
                   (uint32_t)((words >> 16) & 0xFFFFU) +
                   (uint32_t)((words >> 32) & 0xFFFFU) +
                   (uint32_t)(words >> 48);
            data += 8U;
            batch -= 8U;
        }
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    while (length >= 2U) {
        sum += ((uint32_t)data[0] << 8) | data[1];
        data += 2;
        length -= 2U;
    }
    if (length != 0) sum += (uint32_t)data[0] << 8;
    while ((sum >> 16) != 0) sum = (sum & 0xFFFFU) + (sum >> 16);
    return sum;
}

uint16_t net_checksum16(const void *data, size_t length) {
    if (data == 0 && length != 0) return 0;
    uint32_t sum = net_checksum_add(0, (const uint8_t *)data, length);
    return (uint16_t)~sum;
}

static uint16_t net_transport_checksum_ipv4(uint32_t source_address,
                                            uint32_t destination_address,
                                            uint8_t next_header,
                                            const uint8_t *payload, uint16_t length) {
    uint8_t address[8];
    uint8_t protocol[4] = {0, next_header, (uint8_t)(length >> 8), (uint8_t)length};
    net_store_be32(address, source_address);
    net_store_be32(address + 4U, destination_address);
    uint32_t sum = net_checksum_add(0, address, sizeof(address));
    sum = net_checksum_add(sum, protocol, sizeof(protocol));
    sum = net_checksum_add(sum, payload, length);
    return (uint16_t)~sum;
}

static uint16_t net_transport_checksum_ipv6(const uint8_t source_address[16],
                                            const uint8_t destination_address[16],
                                            uint8_t next_header,
                                            const uint8_t *payload, uint16_t length) {
    /* 伪首部的所有组成部分长度均为偶数：直接累加，无需构造并清零
     * 一个 40 字节的临时首部。 */
    uint8_t trailer[8] = {0, 0, (uint8_t)(length >> 8), (uint8_t)length,
                         0, 0, 0, next_header};
    uint32_t sum = net_checksum_add(0, source_address, 16U);
    sum = net_checksum_add(sum, destination_address, 16U);
    sum = net_checksum_add(sum, trailer, sizeof(trailer));
    sum = net_checksum_add(sum, payload, length);
    return (uint16_t)~sum;
}

void net_ndp_solicited_node_address(const uint8_t address[16],
                                    uint8_t solicited_node[16]) {
    if (address == 0 || solicited_node == 0) return;
    net_zero(solicited_node, 16U);
    solicited_node[0] = 0xFFU;
    solicited_node[1] = 0x02U;
    solicited_node[11] = 0x01U;
    solicited_node[12] = 0xFFU;
    solicited_node[13] = address[13];
    solicited_node[14] = address[14];
    solicited_node[15] = address[15];
}

void net_ndp_multicast_mac(const uint8_t multicast_address[16], uint8_t mac[6]) {
    if (multicast_address == 0 || mac == 0) return;
    mac[0] = 0x33U;
    mac[1] = 0x33U;
    mac[2] = multicast_address[12];
    mac[3] = multicast_address[13];
    mac[4] = multicast_address[14];
    mac[5] = multicast_address[15];
}

static kstatus_t net_ndp_build(uint8_t *frame, size_t capacity,
                               const uint8_t source_mac[6],
                               const uint8_t destination_mac[6],
                               const uint8_t source_address[16],
                               const uint8_t destination_address[16],
                               uint8_t type, const uint8_t target_address[16],
                               uint32_t flags, const uint8_t option_mac[6],
                               size_t *frame_length) {
    const uint16_t icmp_length = 32U;
    const uint32_t total_length = NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE +
                                  icmp_length;
    uint8_t *ip;
    uint8_t *icmp;
    if (frame == 0 || capacity < total_length || source_mac == 0 ||
        destination_mac == 0 || source_address == 0 || destination_address == 0 ||
        target_address == 0 || option_mac == 0 || frame_length == 0 ||
        (type != NET_ICMPV6_NEIGHBOR_SOLICITATION &&
         type != NET_ICMPV6_NEIGHBOR_ADVERTISEMENT)) return K_EINVAL;
    net_copy(frame, destination_mac, 6U);
    net_copy(frame + 6U, source_mac, 6U);
    net_store_be16(frame + 12U, NET_ETHERTYPE_IPV6);
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    net_zero(ip, NET_IPV6_HEADER_SIZE);
    ip[0] = 0x60U;
    net_store_be16(ip + 4U, icmp_length);
    ip[6] = NET_ICMPV6_NEXT_HEADER;
    ip[7] = 255U;
    net_copy(ip + 8U, source_address, 16U);
    net_copy(ip + 24U, destination_address, 16U);
    icmp = ip + NET_IPV6_HEADER_SIZE;
    net_zero(icmp, icmp_length);
    icmp[0] = type;
    net_store_be32(icmp + 4U, flags);
    net_copy(icmp + 8U, target_address, 16U);
    icmp[24] = type == NET_ICMPV6_NEIGHBOR_SOLICITATION ?
                   NET_NDP_OPTION_SOURCE_LINK_LAYER : NET_NDP_OPTION_TARGET_LINK_LAYER;
    icmp[25] = 1U;
    net_copy(icmp + 26U, option_mac, 6U);
    net_store_be16(icmp + 2U, net_transport_checksum_ipv6(source_address,
                                                  destination_address,
                                                  NET_ICMPV6_NEXT_HEADER, icmp, icmp_length));
    *frame_length = total_length;
    return K_OK;
}

kstatus_t net_ndp_build_neighbor_solicitation(
    uint8_t *frame, size_t capacity, const uint8_t source_mac[6],
    const uint8_t destination_mac[6], const uint8_t source_address[16],
    const uint8_t destination_address[16], const uint8_t target_address[16],
    size_t *frame_length) {
    return net_ndp_build(frame, capacity, source_mac, destination_mac,
                         source_address, destination_address,
                         NET_ICMPV6_NEIGHBOR_SOLICITATION, target_address, 0U,
                         source_mac, frame_length);
}

kstatus_t net_ndp_build_neighbor_advertisement(
    uint8_t *frame, size_t capacity, const uint8_t source_mac[6],
    const uint8_t destination_mac[6], const uint8_t source_address[16],
    const uint8_t destination_address[16], const uint8_t target_address[16],
    uint32_t flags, const uint8_t target_mac[6], size_t *frame_length) {
    return net_ndp_build(frame, capacity, source_mac, destination_mac,
                         source_address, destination_address,
                         NET_ICMPV6_NEIGHBOR_ADVERTISEMENT, target_address, flags,
                         target_mac, frame_length);
}

static kstatus_t net_ndp_parse(const uint8_t *frame, size_t frame_length,
                               uint8_t type, net_ndp_view_t *view) {
    const uint8_t *ip;
    const uint8_t *icmp;
    uint16_t payload_length;
    if (frame == 0 || view == 0 || frame_length < NET_NDP_FRAME_SIZE ||
        net_load_be16(frame + 12U) != NET_ETHERTYPE_IPV6) return K_EINVAL;
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    payload_length = net_load_be16(ip + 4U);
    if ((ip[0] >> 4) != 6U || ip[6] != NET_ICMPV6_NEXT_HEADER || ip[7] != 255U ||
        payload_length < 32U || NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE +
        payload_length > frame_length) return K_EIO;
    icmp = ip + NET_IPV6_HEADER_SIZE;
    if (icmp[0] != type || icmp[1] != 0U || net_transport_checksum_ipv6(ip + 8U, ip + 24U,
                                      NET_ICMPV6_NEXT_HEADER, icmp, payload_length) != 0U ||
        icmp[24] != (type == NET_ICMPV6_NEIGHBOR_SOLICITATION ?
                     NET_NDP_OPTION_SOURCE_LINK_LAYER : NET_NDP_OPTION_TARGET_LINK_LAYER) ||
        icmp[25] != 1U) return K_EIO;
    net_zero(view, sizeof(*view));
    net_copy(view->destination_mac, frame, 6U);
    net_copy(view->source_mac, frame + 6U, 6U);
    net_copy(view->source_address, ip + 8U, 16U);
    net_copy(view->destination_address, ip + 24U, 16U);
    view->type = type;
    view->flags = type == NET_ICMPV6_NEIGHBOR_ADVERTISEMENT ? net_load_be32(icmp + 4U) : 0U;
    net_copy(view->target_address, icmp + 8U, 16U);
    net_copy(view->target_mac, icmp + 26U, 6U);
    view->has_link_layer_address = true;
    return K_OK;
}

kstatus_t net_ndp_parse_neighbor_solicitation(const uint8_t *frame,
                                              size_t frame_length,
                                              net_ndp_view_t *view) {
    return net_ndp_parse(frame, frame_length, NET_ICMPV6_NEIGHBOR_SOLICITATION, view);
}

kstatus_t net_ndp_parse_neighbor_advertisement(const uint8_t *frame,
                                               size_t frame_length,
                                               net_ndp_view_t *view) {
    return net_ndp_parse(frame, frame_length, NET_ICMPV6_NEIGHBOR_ADVERTISEMENT, view);
}

kstatus_t net_arp_build_ipv4(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             uint16_t operation, uint32_t sender_address,
                             const uint8_t sender_mac[6], uint32_t target_address,
                             const uint8_t target_mac[6], size_t *frame_length) {
    if (frame == 0 || capacity < NET_ARP_FRAME_SIZE || source_mac == 0 ||
        destination_mac == 0 || sender_mac == 0 || target_mac == 0 ||
        frame_length == 0 ||
        (operation != NET_ARP_OPERATION_REQUEST && operation != NET_ARP_OPERATION_REPLY)) {
        return K_EINVAL;
    }
    net_copy(frame, destination_mac, 6U);
    net_copy(frame + 6U, source_mac, 6U);
    net_store_be16(frame + 12U, NET_ETHERTYPE_ARP);
    net_store_be16(frame + 14U, 1U); /* Ethernet 硬件类型。 */
    net_store_be16(frame + 16U, NET_ETHERTYPE_IPV4);
    frame[18] = 6U;
    frame[19] = 4U;
    net_store_be16(frame + 20U, operation);
    net_copy(frame + 22U, sender_mac, 6U);
    net_store_be32(frame + 28U, sender_address);
    net_copy(frame + 32U, target_mac, 6U);
    net_store_be32(frame + 38U, target_address);
    *frame_length = NET_ARP_FRAME_SIZE;
    return K_OK;
}

kstatus_t net_arp_parse_ipv4(const uint8_t *frame, size_t frame_length,
                             net_arp_view_t *view) {
    if (frame == 0 || view == 0 || frame_length < NET_ARP_FRAME_SIZE ||
        net_load_be16(frame + 12U) != NET_ETHERTYPE_ARP ||
        net_load_be16(frame + 14U) != 1U || net_load_be16(frame + 16U) != NET_ETHERTYPE_IPV4 ||
        frame[18] != 6U || frame[19] != 4U ||
        (net_load_be16(frame + 20U) != NET_ARP_OPERATION_REQUEST &&
         net_load_be16(frame + 20U) != NET_ARP_OPERATION_REPLY)) return K_EIO;
    net_copy(view->destination_mac, frame, 6U);
    net_copy(view->source_mac, frame + 6U, 6U);
    view->operation = net_load_be16(frame + 20U);
    net_copy(view->sender_mac, frame + 22U, 6U);
    view->sender_address = net_load_be32(frame + 28U);
    net_copy(view->target_mac, frame + 32U, 6U);
    view->target_address = net_load_be32(frame + 38U);
    return K_OK;
}

kstatus_t net_udp_build_ipv4(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             uint32_t source_address,
                             uint32_t destination_address,
                             uint16_t source_port, uint16_t destination_port,
                             const void *payload, uint16_t payload_length,
                             size_t *frame_length) {
    uint32_t ip_length = NET_IPV4_HEADER_SIZE + NET_UDP_HEADER_SIZE + payload_length;
    uint32_t total_length = NET_ETHERNET_HEADER_SIZE + ip_length;
    uint8_t *ip;
    uint8_t *udp;
    if (frame == 0 || source_mac == 0 || destination_mac == 0 ||
        frame_length == 0 || (payload == 0 && payload_length != 0) ||
        ip_length > UINT16_MAX || total_length > capacity) return K_EINVAL;
    net_copy(frame, destination_mac, 6U);
    net_copy(frame + 6U, source_mac, 6U);
    net_store_be16(frame + 12U, NET_ETHERTYPE_IPV4);
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    net_zero(ip, NET_IPV4_HEADER_SIZE);
    ip[0] = (NET_IPV4_VERSION << 4) | 5U;
    ip[1] = 0;
    net_store_be16(ip + 2U, (uint16_t)ip_length);
    net_store_be16(ip + 4U, net_next_ipv4_identification());
    net_store_be16(ip + 6U, 0U);
    ip[8] = 64U;
    ip[9] = NET_IPV4_UDP;
    net_store_be32(ip + 12U, source_address);
    net_store_be32(ip + 16U, destination_address);
    net_store_be16(ip + 10U, net_checksum16(ip, NET_IPV4_HEADER_SIZE));
    udp = ip + NET_IPV4_HEADER_SIZE;
    net_zero(udp, NET_UDP_HEADER_SIZE);
    net_store_be16(udp, source_port);
    net_store_be16(udp + 2U, destination_port);
    net_store_be16(udp + 4U, (uint16_t)(NET_UDP_HEADER_SIZE + payload_length));
    if (payload_length != 0) net_copy(udp + NET_UDP_HEADER_SIZE, payload, payload_length);
    net_store_be16(udp + 6U, net_transport_checksum_ipv4(source_address, destination_address,
                                               NET_IPV4_UDP, udp,
                                               (uint16_t)(NET_UDP_HEADER_SIZE + payload_length)));
    if (net_load_be16(udp + 6U) == 0U) net_store_be16(udp + 6U, UINT16_MAX);
    *frame_length = total_length;
    return K_OK;
}

kstatus_t net_udp_parse_ipv4(const uint8_t *frame, size_t frame_length,
                             net_udp_view_t *view) {
    const uint8_t *ip;
    const uint8_t *udp;
    uint32_t ip_header_length;
    uint16_t ip_total_length;
    uint16_t udp_length;
    if (frame == 0 || view == 0 || frame_length < NET_ETHERNET_HEADER_SIZE +
        NET_IPV4_HEADER_SIZE + NET_UDP_HEADER_SIZE ||
        net_load_be16(frame + 12U) != NET_ETHERTYPE_IPV4) return K_EINVAL;
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    /* TOS/DSCP 是 IPv4 的合法服务质量字段，不能要求它必须为零。 */
    if ((ip[0] >> 4) != NET_IPV4_VERSION || (ip[0] & 0x0FU) < 5U) return K_EIO;
    ip_header_length = (uint32_t)(ip[0] & 0x0FU) * 4U;
    ip_total_length = net_load_be16(ip + 2U);
    if (ip_total_length < ip_header_length + NET_UDP_HEADER_SIZE ||
        NET_ETHERNET_HEADER_SIZE + ip_total_length > frame_length ||
        ip[9] != NET_IPV4_UDP ||
        net_checksum16(ip, ip_header_length) != 0U) return K_EIO;
    udp = ip + ip_header_length;
    udp_length = net_load_be16(udp + 4U);
    if (udp_length < NET_UDP_HEADER_SIZE || udp_length > ip_total_length - ip_header_length ||
        /* IPv4 允许 UDP 校验和为 0，表示发送端明确关闭校验和；DHCP 常使用此形式。 */
        (net_load_be16(udp + 6U) != 0U &&
         net_transport_checksum_ipv4(net_load_be32(ip + 12U), net_load_be32(ip + 16U),
                                     NET_IPV4_UDP, udp, udp_length) != 0U)) return K_EIO;
    net_copy(view->source_mac, frame + 6U, 6U);
    net_copy(view->destination_mac, frame, 6U);
    view->source_address = net_load_be32(ip + 12U);
    view->destination_address = net_load_be32(ip + 16U);
    view->source_port = net_load_be16(udp);
    view->destination_port = net_load_be16(udp + 2U);
    view->payload = udp + NET_UDP_HEADER_SIZE;
    view->payload_length = (uint16_t)(udp_length - NET_UDP_HEADER_SIZE);
    return K_OK;
}

kstatus_t net_udp_build_ipv6(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             const uint8_t source_address[16],
                             const uint8_t destination_address[16],
                             uint16_t source_port, uint16_t destination_port,
                             const void *payload, uint16_t payload_length,
                             size_t *frame_length) {
    uint32_t udp_length = NET_UDP_HEADER_SIZE + (uint32_t)payload_length;
    uint32_t total_length = NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE + udp_length;
    uint8_t *ip;
    uint8_t *udp;
    if (frame == 0 || source_mac == 0 || destination_mac == 0 ||
        source_address == 0 || destination_address == 0 || frame_length == 0 ||
        (payload == 0 && payload_length != 0) || udp_length > UINT16_MAX ||
        total_length > capacity) return K_EINVAL;
    net_copy(frame, destination_mac, 6U);
    net_copy(frame + 6U, source_mac, 6U);
    net_store_be16(frame + 12U, 0x86DDU);
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    net_zero(ip, NET_IPV6_HEADER_SIZE);
    ip[0] = 0x60U;
    net_store_be16(ip + 4U, (uint16_t)udp_length);
    ip[6] = NET_IPV4_UDP;
    ip[7] = 64U;
    net_copy(ip + 8U, source_address, 16U);
    net_copy(ip + 24U, destination_address, 16U);
    udp = ip + NET_IPV6_HEADER_SIZE;
    net_zero(udp, NET_UDP_HEADER_SIZE);
    net_store_be16(udp, source_port);
    net_store_be16(udp + 2U, destination_port);
    net_store_be16(udp + 4U, (uint16_t)udp_length);
    if (payload_length != 0) net_copy(udp + NET_UDP_HEADER_SIZE,
                                      payload, payload_length);
    net_store_be16(udp + 6U, net_transport_checksum_ipv6(source_address,
                                                    destination_address,
                                                    NET_IPV4_UDP, udp, (uint16_t)udp_length));
    if (net_load_be16(udp + 6U) == 0U) net_store_be16(udp + 6U, UINT16_MAX);
    *frame_length = total_length;
    return K_OK;
}

kstatus_t net_udp_parse_ipv6(const uint8_t *frame, size_t frame_length,
                             net_ipv6_udp_view_t *view) {
    const uint8_t *ip;
    const uint8_t *udp;
    uint16_t ip_payload_length;
    uint16_t udp_length;
    if (frame == 0 || view == 0 || frame_length < NET_ETHERNET_HEADER_SIZE +
        NET_IPV6_HEADER_SIZE + NET_UDP_HEADER_SIZE ||
        net_load_be16(frame + 12U) != 0x86DDU) return K_EINVAL;
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    ip_payload_length = net_load_be16(ip + 4U);
    if ((ip[0] >> 4) != 6U || ip[6] != NET_IPV4_UDP ||
        ip_payload_length < NET_UDP_HEADER_SIZE ||
        NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE + ip_payload_length > frame_length) {
        return K_EIO;
    }
    udp = ip + NET_IPV6_HEADER_SIZE;
    udp_length = net_load_be16(udp + 4U);
    if (udp_length < NET_UDP_HEADER_SIZE || udp_length > ip_payload_length ||
        net_transport_checksum_ipv6(ip + 8U, ip + 24U, NET_IPV4_UDP, udp, udp_length) != 0U) return K_EIO;
    net_copy(view->source_mac, frame + 6U, 6U);
    net_copy(view->destination_mac, frame, 6U);
    net_copy(view->source_address, ip + 8U, 16U);
    net_copy(view->destination_address, ip + 24U, 16U);
    view->source_port = net_load_be16(udp);
    view->destination_port = net_load_be16(udp + 2U);
    view->payload = udp + NET_UDP_HEADER_SIZE;
    view->payload_length = (uint16_t)(udp_length - NET_UDP_HEADER_SIZE);
    return K_OK;
}

static bool net_tcp_parse_options(const uint8_t *tcp, uint32_t header_length,
                                  uint16_t *mss, uint8_t *window_scale,
                                  bool *window_scale_present) {
    *mss = 0U;
    *window_scale = 0U;
    *window_scale_present = false;
    for (uint32_t offset = NET_TCP_HEADER_SIZE; offset < header_length;) {
        uint8_t kind = tcp[offset++];
        if (kind == 0U) break;
        if (kind == 1U) continue;
        if (offset >= header_length) return false;
        uint8_t length = tcp[offset++];
        if (length < 2U || (uint32_t)(length - 2U) > header_length - offset)
            return false;
        if (kind == 2U) {
            if (length != 4U) return false;
            uint16_t value = net_load_be16(tcp + offset);
            if (value == 0U) return false;
            if (*mss == 0U || value < *mss) *mss = value;
        } else if (kind == 3U) {
            if (length != 3U) return false;
            if ((tcp[13] & NET_TCP_FLAG_SYN) != 0U && !*window_scale_present) {
                /* RFC 7323 requires clamping a received shift above 14. */
                *window_scale = tcp[offset] > 14U ? 14U : tcp[offset];
                *window_scale_present = true;
            }
        }
        offset += length - 2U;
    }
    return true;
}

static void net_tcp_store_syn_options(uint8_t *tcp, uint32_t header_length,
                                       uint16_t mss, int receive_scale) {
    if (header_length == NET_TCP_HEADER_SIZE) return;
    tcp[12] = (uint8_t)((header_length / 4U) << 4U);
    tcp[20] = 2U;
    tcp[21] = 4U;
    net_store_be16(tcp + 22U, mss);
    if (receive_scale >= 0) {
        tcp[24] = 1U; /* NOP aligns the three-byte WS option. */
        tcp[25] = 3U;
        tcp[26] = 3U;
        tcp[27] = (uint8_t)receive_scale;
    }
}

static void net_tcp_store_header(uint8_t *tcp, uint16_t source_port,
                                 uint16_t destination_port, uint32_t sequence,
                                 uint32_t acknowledgement, uint8_t flags,
                                 uint16_t window) {
    net_zero(tcp, NET_TCP_HEADER_SIZE);
    net_store_be16(tcp, source_port);
    net_store_be16(tcp + 2U, destination_port);
    net_store_be32(tcp + 4U, sequence);
    net_store_be32(tcp + 8U, acknowledgement);
    tcp[12] = 5U << 4;
    tcp[13] = flags;
    net_store_be16(tcp + 14U, window);
}

kstatus_t net_tcp_build_ipv4_scaled(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             uint32_t source_address,
                             uint32_t destination_address,
                             uint16_t source_port, uint16_t destination_port,
                             uint32_t sequence, uint32_t acknowledgement,
                             uint8_t flags, uint16_t window, int receive_scale,
                             const void *payload, uint16_t payload_length,
                             size_t *frame_length) {
    uint32_t header_length = (flags & NET_TCP_FLAG_SYN) != 0U ?
                             NET_TCP_SYN_HEADER_SIZE + (receive_scale >= 0 ? 4U : 0U) :
                             NET_TCP_HEADER_SIZE;
    uint32_t tcp_length = header_length + (uint32_t)payload_length;
    uint32_t ip_length = NET_IPV4_HEADER_SIZE + tcp_length;
    uint32_t total_length = NET_ETHERNET_HEADER_SIZE + ip_length;
    uint8_t *ip;
    uint8_t *tcp;
    if (frame == 0 || source_mac == 0 || destination_mac == 0 || frame_length == 0 ||
        receive_scale < -1 || receive_scale > 14 ||
        (payload == 0 && payload_length != 0U) || tcp_length > UINT16_MAX ||
        ip_length > UINT16_MAX || total_length > capacity) return K_EINVAL;
    net_copy(frame, destination_mac, 6U);
    net_copy(frame + 6U, source_mac, 6U);
    net_store_be16(frame + 12U, NET_ETHERTYPE_IPV4);
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    net_zero(ip, NET_IPV4_HEADER_SIZE);
    ip[0] = (NET_IPV4_VERSION << 4) | 5U;
    net_store_be16(ip + 2U, (uint16_t)ip_length);
    net_store_be16(ip + 4U, net_next_ipv4_identification());
    ip[8] = 64U;
    ip[9] = NET_IPV4_TCP;
    net_store_be32(ip + 12U, source_address);
    net_store_be32(ip + 16U, destination_address);
    net_store_be16(ip + 10U, net_checksum16(ip, NET_IPV4_HEADER_SIZE));
    tcp = ip + NET_IPV4_HEADER_SIZE;
    net_tcp_store_header(tcp, source_port, destination_port, sequence,
                         acknowledgement, flags, window);
    net_tcp_store_syn_options(tcp, header_length, 1460U, receive_scale);
    if (payload_length != 0U) net_copy(tcp + header_length, payload, payload_length);
    net_store_be16(tcp + 16U, net_transport_checksum_ipv4(source_address,
                                                     destination_address,
                                                     NET_IPV4_TCP, tcp, (uint16_t)tcp_length));
    *frame_length = total_length;
    return K_OK;
}

kstatus_t net_tcp_build_ipv4(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             uint32_t source_address, uint32_t destination_address,
                             uint16_t source_port, uint16_t destination_port,
                             uint32_t sequence, uint32_t acknowledgement,
                             uint8_t flags, uint16_t window,
                             const void *payload, uint16_t payload_length,
                             size_t *frame_length) {
    return net_tcp_build_ipv4_scaled(frame, capacity, source_mac, destination_mac,
        source_address, destination_address, source_port, destination_port,
        sequence, acknowledgement, flags, window, -1, payload, payload_length,
        frame_length);
}

kstatus_t net_tcp_parse_ipv4(const uint8_t *frame, size_t frame_length,
                             net_tcp_view_t *view) {
    const uint8_t *ip;
    const uint8_t *tcp;
    uint32_t ip_header_length;
    uint16_t ip_total_length;
    uint32_t tcp_header_length;
    uint16_t tcp_length;
    if (frame == 0 || view == 0 || frame_length < NET_ETHERNET_HEADER_SIZE +
        NET_IPV4_HEADER_SIZE + NET_TCP_HEADER_SIZE ||
        net_load_be16(frame + 12U) != NET_ETHERTYPE_IPV4) return K_EINVAL;
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    /* TOS/DSCP 由发送端设置，合法非零值不应导致 TCP 包被丢弃。 */
    if ((ip[0] >> 4) != NET_IPV4_VERSION || (ip[0] & 0x0FU) < 5U) {
        return K_EIO;
    }
    ip_header_length = (uint32_t)(ip[0] & 0x0FU) * 4U;
    ip_total_length = net_load_be16(ip + 2U);
    if (ip_total_length < ip_header_length + NET_TCP_HEADER_SIZE ||
        NET_ETHERNET_HEADER_SIZE + ip_total_length > frame_length || ip[9] != NET_IPV4_TCP ||
        net_checksum16(ip, ip_header_length) != 0U) return K_EIO;
    tcp = ip + ip_header_length;
    tcp_header_length = (uint32_t)(tcp[12] >> 4) * 4U;
    tcp_length = (uint16_t)(ip_total_length - ip_header_length);
    if (tcp_header_length < NET_TCP_HEADER_SIZE || tcp_header_length > tcp_length ||
        net_transport_checksum_ipv4(net_load_be32(ip + 12U), net_load_be32(ip + 16U),
                                   NET_IPV4_TCP, tcp, tcp_length) != 0U) return K_EIO;
    net_copy(view->source_mac, frame + 6U, 6U);
    net_copy(view->destination_mac, frame, 6U);
    view->source_address = net_load_be32(ip + 12U);
    view->destination_address = net_load_be32(ip + 16U);
    view->source_port = net_load_be16(tcp);
    view->destination_port = net_load_be16(tcp + 2U);
    view->sequence = net_load_be32(tcp + 4U);
    view->acknowledgement = net_load_be32(tcp + 8U);
    view->header_length = (uint8_t)tcp_header_length;
    if (!net_tcp_parse_options(tcp, tcp_header_length, &view->peer_mss,
        &view->peer_window_scale, &view->peer_window_scale_present)) return K_EIO;
    view->flags = tcp[13];
    view->window = net_load_be16(tcp + 14U);
    view->payload = tcp + tcp_header_length;
    view->payload_length = (uint16_t)(tcp_length - tcp_header_length);
    return K_OK;
}

kstatus_t net_tcp_build_ipv6_scaled(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             const uint8_t source_address[16],
                             const uint8_t destination_address[16],
                             uint16_t source_port, uint16_t destination_port,
                             uint32_t sequence, uint32_t acknowledgement,
                             uint8_t flags, uint16_t window, int receive_scale,
                             const void *payload, uint16_t payload_length,
                             size_t *frame_length) {
    uint32_t header_length = (flags & NET_TCP_FLAG_SYN) != 0U ?
                             NET_TCP_SYN_HEADER_SIZE + (receive_scale >= 0 ? 4U : 0U) :
                             NET_TCP_HEADER_SIZE;
    uint32_t tcp_length = header_length + (uint32_t)payload_length;
    uint32_t total_length = NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE + tcp_length;
    uint8_t *ip;
    uint8_t *tcp;
    if (frame == 0 || source_mac == 0 || destination_mac == 0 || source_address == 0 ||
        destination_address == 0 || frame_length == 0 ||
        receive_scale < -1 || receive_scale > 14 || (payload == 0 && payload_length != 0U) ||
        tcp_length > UINT16_MAX || total_length > capacity) return K_EINVAL;
    net_copy(frame, destination_mac, 6U);
    net_copy(frame + 6U, source_mac, 6U);
    net_store_be16(frame + 12U, NET_ETHERTYPE_IPV6);
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    net_zero(ip, NET_IPV6_HEADER_SIZE);
    ip[0] = 0x60U;
    net_store_be16(ip + 4U, (uint16_t)tcp_length);
    ip[6] = NET_IPV4_TCP;
    ip[7] = 64U;
    net_copy(ip + 8U, source_address, 16U);
    net_copy(ip + 24U, destination_address, 16U);
    tcp = ip + NET_IPV6_HEADER_SIZE;
    net_tcp_store_header(tcp, source_port, destination_port, sequence,
                         acknowledgement, flags, window);
    net_tcp_store_syn_options(tcp, header_length, 1440U, receive_scale);
    if (payload_length != 0U) net_copy(tcp + header_length, payload, payload_length);
    net_store_be16(tcp + 16U, net_transport_checksum_ipv6(source_address, destination_address,
                                                    NET_IPV4_TCP, tcp, (uint16_t)tcp_length));
    *frame_length = total_length;
    return K_OK;
}

kstatus_t net_tcp_build_ipv6(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             const uint8_t source_address[16],
                             const uint8_t destination_address[16],
                             uint16_t source_port, uint16_t destination_port,
                             uint32_t sequence, uint32_t acknowledgement,
                             uint8_t flags, uint16_t window,
                             const void *payload, uint16_t payload_length,
                             size_t *frame_length) {
    return net_tcp_build_ipv6_scaled(frame, capacity, source_mac, destination_mac,
        source_address, destination_address, source_port, destination_port,
        sequence, acknowledgement, flags, window, -1, payload, payload_length,
        frame_length);
}

kstatus_t net_tcp_parse_ipv6(const uint8_t *frame, size_t frame_length,
                             net_tcp_view_t *view) {
    const uint8_t *ip;
    const uint8_t *tcp;
    uint16_t ip_payload_length;
    uint32_t tcp_header_length;
    uint16_t tcp_length;
    if (frame == 0 || view == 0 || frame_length < NET_ETHERNET_HEADER_SIZE +
        NET_IPV6_HEADER_SIZE + NET_TCP_HEADER_SIZE ||
        net_load_be16(frame + 12U) != NET_ETHERTYPE_IPV6) return K_EINVAL;
    ip = frame + NET_ETHERNET_HEADER_SIZE;
    ip_payload_length = net_load_be16(ip + 4U);
    if ((ip[0] >> 4) != 6U || ip[6] != NET_IPV4_TCP ||
        ip_payload_length < NET_TCP_HEADER_SIZE ||
        NET_ETHERNET_HEADER_SIZE + NET_IPV6_HEADER_SIZE + ip_payload_length > frame_length) {
        return K_EIO;
    }
    tcp = ip + NET_IPV6_HEADER_SIZE;
    tcp_length = ip_payload_length;
    tcp_header_length = (uint32_t)(tcp[12] >> 4) * 4U;
    if (tcp_header_length < NET_TCP_HEADER_SIZE || tcp_header_length > tcp_length ||
        net_transport_checksum_ipv6(ip + 8U, ip + 24U, NET_IPV4_TCP, tcp, tcp_length) != 0U) return K_EIO;
    net_copy(view->source_mac, frame + 6U, 6U);
    net_copy(view->destination_mac, frame, 6U);
    net_copy(view->source_address6, ip + 8U, 16U);
    net_copy(view->destination_address6, ip + 24U, 16U);
    view->source_port = net_load_be16(tcp);
    view->destination_port = net_load_be16(tcp + 2U);
    view->sequence = net_load_be32(tcp + 4U);
    view->acknowledgement = net_load_be32(tcp + 8U);
    view->header_length = (uint8_t)tcp_header_length;
    if (!net_tcp_parse_options(tcp, tcp_header_length, &view->peer_mss,
        &view->peer_window_scale, &view->peer_window_scale_present)) return K_EIO;
    view->flags = tcp[13];
    view->window = net_load_be16(tcp + 14U);
    view->payload = tcp + tcp_header_length;
    view->payload_length = (uint16_t)(tcp_length - tcp_header_length);
    return K_OK;
}

static void net_cache_lock(spinlock_t *lock) {
    /* RX and socket output share these short cache guards. Never let a
     * competing local thread displace the owner while it holds the lock. */
    sched_preempt_disable();
    spinlock_lock(lock);
}

static void net_cache_unlock(spinlock_t *lock) {
    spinlock_unlock(lock);
    sched_preempt_enable();
}

void net_arp_cache_init(net_arp_cache_t *cache) {
    if (cache == 0) return;
    net_zero(cache, sizeof(*cache));
    spinlock_init(&cache->lock);
}

kstatus_t net_arp_cache_update(net_arp_cache_t *cache, uint32_t address,
                               const uint8_t mac[6], uint64_t timestamp) {
    if (cache == 0 || mac == 0 || address == 0U) return K_EINVAL;
    net_cache_lock(&cache->lock);
    uint32_t slot = NET_ARP_CACHE_SIZE;
    for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i) {
        if (cache->entries[i].valid && cache->entries[i].address == address) {
            slot = i;
            break;
        }
    }
    if (slot == NET_ARP_CACHE_SIZE) {
        for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i) {
            if (!cache->entries[i].valid) {
                slot = i;
                break;
            }
        }
    }
    if (slot == NET_ARP_CACHE_SIZE) {
        slot = cache->next_replacement++ % NET_ARP_CACHE_SIZE;
    }
    cache->entries[slot].valid = true;
    cache->entries[slot].address = address;
    net_copy(cache->entries[slot].mac, mac, 6U);
    cache->entries[slot].updated = timestamp;
    net_cache_unlock(&cache->lock);
    return K_OK;
}

bool net_arp_cache_lookup(net_arp_cache_t *cache, uint32_t address,
                          uint8_t mac[6]) {
    bool found = false;
    if (cache == 0 || mac == 0 || address == 0U) return false;
    net_cache_lock(&cache->lock);
    for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i) {
        if (!cache->entries[i].valid || cache->entries[i].address != address) continue;
        net_copy(mac, cache->entries[i].mac, 6U);
        found = true;
        break;
    }
    net_cache_unlock(&cache->lock);
    return found;
}

void net_ipv6_neighbor_cache_init(net_ipv6_neighbor_cache_t *cache) {
    if (cache == 0) return;
    net_zero(cache, sizeof(*cache));
    spinlock_init(&cache->lock);
}

kstatus_t net_ipv6_neighbor_cache_update(net_ipv6_neighbor_cache_t *cache,
                                          const uint8_t address[16],
                                          const uint8_t mac[6],
                                          uint64_t timestamp) {
    uint32_t slot = NET_NDP_CACHE_SIZE;
    if (cache == 0 || address == 0 || mac == 0 || net_address6_zero(address)) {
        return K_EINVAL;
    }
    net_cache_lock(&cache->lock);
    for (uint32_t index = 0U; index < NET_NDP_CACHE_SIZE; ++index) {
        if (cache->entries[index].valid &&
            net_address6_equal(cache->entries[index].address, address)) {
            slot = index;
            break;
        }
    }
    if (slot == NET_NDP_CACHE_SIZE) {
        for (uint32_t index = 0U; index < NET_NDP_CACHE_SIZE; ++index) {
            if (!cache->entries[index].valid) {
                slot = index;
                break;
            }
        }
    }
    if (slot == NET_NDP_CACHE_SIZE) {
        slot = cache->next_replacement++ % NET_NDP_CACHE_SIZE;
    }
    cache->entries[slot].valid = true;
    net_copy(cache->entries[slot].address, address, 16U);
    net_copy(cache->entries[slot].mac, mac, 6U);
    cache->entries[slot].updated = timestamp;
    net_cache_unlock(&cache->lock);
    return K_OK;
}

bool net_ipv6_neighbor_cache_lookup(net_ipv6_neighbor_cache_t *cache,
                                    const uint8_t address[16], uint8_t mac[6]) {
    bool found = false;
    if (cache == 0 || address == 0 || mac == 0 || net_address6_zero(address)) return false;
    net_cache_lock(&cache->lock);
    for (uint32_t index = 0U; index < NET_NDP_CACHE_SIZE; ++index) {
        if (!cache->entries[index].valid ||
            !net_address6_equal(cache->entries[index].address, address)) continue;
        net_copy(mac, cache->entries[index].mac, 6U);
        found = true;
        break;
    }
    net_cache_unlock(&cache->lock);
    return found;
}
