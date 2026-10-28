#pragma once

#include <kernel/base.h>
#include <kernel/net.h>
#include <kernel/spinlock.h>

#define NET_ETHERNET_HEADER_SIZE 14U
#define NET_IPV4_HEADER_SIZE     20U
#define NET_IPV6_HEADER_SIZE     40U
#define NET_UDP_HEADER_SIZE      8U
#define NET_TCP_HEADER_SIZE      20U
#define NET_TCP_SYN_HEADER_SIZE  (NET_TCP_HEADER_SIZE + 4U)
#define NET_ARP_FRAME_SIZE       42U
#define NET_ARP_CACHE_SIZE       16U
#define NET_NDP_CACHE_SIZE       16U
#define NET_NDP_FRAME_SIZE       86U
#define NET_FIREWALL_MAX_RULES   32U

#define NET_ETHERTYPE_IPV4 0x0800U
#define NET_ETHERTYPE_ARP  0x0806U
#define NET_ETHERTYPE_IPV6 0x86DDU

#define NET_ARP_OPERATION_REQUEST 1U
#define NET_ARP_OPERATION_REPLY   2U

#define NET_ICMPV6_NEXT_HEADER                  58U
#define NET_ICMPV6_NEIGHBOR_SOLICITATION       135U
#define NET_ICMPV6_NEIGHBOR_ADVERTISEMENT      136U
#define NET_ICMPV6_NEIGHBOR_SOLICITATION_SIZE   24U
#define NET_ICMPV6_NEIGHBOR_ADVERTISEMENT_SIZE  24U
#define NET_NDP_OPTION_SOURCE_LINK_LAYER         1U
#define NET_NDP_OPTION_TARGET_LINK_LAYER         2U
#define NET_NDP_FLAG_ROUTER                 0x80000000U
#define NET_NDP_FLAG_SOLICITED              0x40000000U
#define NET_NDP_FLAG_OVERRIDE               0x20000000U

#define NET_TCP_FLAG_FIN 0x01U
#define NET_TCP_FLAG_SYN 0x02U
#define NET_TCP_FLAG_RST 0x04U
#define NET_TCP_FLAG_PSH 0x08U
#define NET_TCP_FLAG_ACK 0x10U

typedef struct net_udp_view {
    uint8_t source_mac[6];
    uint8_t destination_mac[6];
    uint32_t source_address;
    uint32_t destination_address;
    uint16_t source_port;
    uint16_t destination_port;
    const uint8_t *payload;
    uint16_t payload_length;
} net_udp_view_t;

/* IPv6 地址按网络字节序保存，避免把地址表示和主机端序混在一起。 */
typedef struct net_ipv6_udp_view {
    uint8_t source_mac[6];
    uint8_t destination_mac[6];
    uint8_t source_address[16];
    uint8_t destination_address[16];
    uint16_t source_port;
    uint16_t destination_port;
    const uint8_t *payload;
    uint16_t payload_length;
} net_ipv6_udp_view_t;

/* Parsed TCP view; payload borrows the input frame. SYN options do not by
 * themselves enable scaling: the owning socket must validate the handshake. */
typedef struct net_tcp_view {
    uint8_t source_mac[6];
    uint8_t destination_mac[6];
    uint16_t source_port;
    uint16_t destination_port;
    uint32_t source_address;
    uint32_t destination_address;
    uint8_t source_address6[16];
    uint8_t destination_address6[16];
    uint32_t sequence;
    uint32_t acknowledgement;
    uint16_t window;
    uint8_t flags;
    uint8_t header_length;
    uint16_t peer_mss;
    uint8_t peer_window_scale;
    bool peer_window_scale_present;
    const uint8_t *payload;
    uint16_t payload_length;
} net_tcp_view_t;

/* 仅接受 Ethernet/IPv4 ARP，地址字段按主机整数保存。 */
typedef struct net_arp_view {
    uint8_t source_mac[6];
    uint8_t destination_mac[6];
    uint16_t operation;
    uint8_t sender_mac[6];
    uint32_t sender_address;
    uint8_t target_mac[6];
    uint32_t target_address;
} net_arp_view_t;

typedef struct net_arp_entry {
    bool valid;
    uint32_t address;
    uint8_t mac[6];
    uint64_t updated;
} net_arp_entry_t;

typedef struct net_arp_cache {
    net_arp_entry_t entries[NET_ARP_CACHE_SIZE];
    spinlock_t lock;
    uint32_t next_replacement;
} net_arp_cache_t;

/* NDP 邻居缓存保存 IPv6 地址到链路层地址的解析结果。 */
typedef struct net_ipv6_neighbor_entry {
    bool valid;
    uint8_t address[16];
    uint8_t mac[6];
    uint64_t updated;
} net_ipv6_neighbor_entry_t;

typedef struct net_ipv6_neighbor_cache {
    net_ipv6_neighbor_entry_t entries[NET_NDP_CACHE_SIZE];
    spinlock_t lock;
    uint32_t next_replacement;
} net_ipv6_neighbor_cache_t;

typedef struct net_ndp_view {
    uint8_t source_mac[6];
    uint8_t destination_mac[6];
    uint8_t source_address[16];
    uint8_t destination_address[16];
    uint8_t target_address[16];
    uint8_t target_mac[6];
    uint32_t flags;
    uint8_t type;
    bool has_link_layer_address;
} net_ndp_view_t;

enum net_firewall_direction {
    NET_FIREWALL_INGRESS = 1,
    NET_FIREWALL_EGRESS = 2,
};

enum net_firewall_action {
    NET_FIREWALL_DENY = 0,
    NET_FIREWALL_ALLOW = 1,
};

typedef struct net_firewall_packet {
    uint16_t family;
    uint8_t direction;
    uint8_t protocol;
    uint8_t source_address[16];
    uint8_t destination_address[16];
    uint16_t source_port;
    uint16_t destination_port;
} net_firewall_packet_t;

typedef struct net_firewall_rule {
    bool enabled;
    uint8_t direction;
    uint8_t protocol;
    uint8_t source_prefix_length;
    uint8_t destination_prefix_length;
    uint16_t family;
    uint16_t source_port;
    uint16_t destination_port;
    uint8_t source_address[16];
    uint8_t destination_address[16];
    uint8_t action;
    uint8_t reserved[3];
} net_firewall_rule_t;

typedef struct net_firewall {
    net_firewall_rule_t rules[NET_FIREWALL_MAX_RULES];
    spinlock_t lock;
    uint8_t default_action;
    uint8_t reserved[3];
} net_firewall_t;

/* 地址和端口使用主机字节序；wire frame 始终按网络字节序编码。 */
uint16_t net_checksum16(const void *data, size_t length);
kstatus_t net_udp_build_ipv4(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             uint32_t source_address,
                             uint32_t destination_address,
                             uint16_t source_port, uint16_t destination_port,
                             const void *payload, uint16_t payload_length,
                             size_t *frame_length);
kstatus_t net_udp_parse_ipv4(const uint8_t *frame, size_t frame_length,
                             net_udp_view_t *view);

kstatus_t net_udp_build_ipv6(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             const uint8_t source_address[16],
                             const uint8_t destination_address[16],
                             uint16_t source_port, uint16_t destination_port,
                             const void *payload, uint16_t payload_length,
                             size_t *frame_length);
kstatus_t net_udp_parse_ipv6(const uint8_t *frame, size_t frame_length,
                             net_ipv6_udp_view_t *view);

kstatus_t net_tcp_build_ipv4(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             uint32_t source_address,
                             uint32_t destination_address,
                             uint16_t source_port, uint16_t destination_port,
                             uint32_t sequence, uint32_t acknowledgement,
                             uint8_t flags, uint16_t window,
                             const void *payload, uint16_t payload_length,
                             size_t *frame_length);
kstatus_t net_tcp_parse_ipv4(const uint8_t *frame, size_t frame_length,
                             net_tcp_view_t *view);

/* receive_scale=-1 omits WS; 0..14 offers it on SYN only. The supplied window
 * is already the wire value; SYN windows are never shifted by the builder. */
kstatus_t net_tcp_build_ipv4_scaled(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             uint32_t source_address, uint32_t destination_address,
                             uint16_t source_port, uint16_t destination_port,
                             uint32_t sequence, uint32_t acknowledgement,
                             uint8_t flags, uint16_t window, int receive_scale,
                             const void *payload, uint16_t payload_length,
                             size_t *frame_length);

kstatus_t net_tcp_build_ipv6(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             const uint8_t source_address[16],
                             const uint8_t destination_address[16],
                             uint16_t source_port, uint16_t destination_port,
                             uint32_t sequence, uint32_t acknowledgement,
                             uint8_t flags, uint16_t window,
                             const void *payload, uint16_t payload_length,
                             size_t *frame_length);
kstatus_t net_tcp_parse_ipv6(const uint8_t *frame, size_t frame_length,
                             net_tcp_view_t *view);
kstatus_t net_tcp_build_ipv6_scaled(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             const uint8_t source_address[16],
                             const uint8_t destination_address[16],
                             uint16_t source_port, uint16_t destination_port,
                             uint32_t sequence, uint32_t acknowledgement,
                             uint8_t flags, uint16_t window, int receive_scale,
                             const void *payload, uint16_t payload_length,
                             size_t *frame_length);

kstatus_t net_arp_build_ipv4(uint8_t *frame, size_t capacity,
                             const uint8_t source_mac[6],
                             const uint8_t destination_mac[6],
                             uint16_t operation, uint32_t sender_address,
                             const uint8_t sender_mac[6], uint32_t target_address,
                             const uint8_t target_mac[6], size_t *frame_length);
kstatus_t net_arp_parse_ipv4(const uint8_t *frame, size_t frame_length,
                             net_arp_view_t *view);

/* 构造和解析 RFC 4861 邻居请求/通告，地址字段均按网络字节序保存。 */
kstatus_t net_ndp_build_neighbor_solicitation(
    uint8_t *frame, size_t capacity, const uint8_t source_mac[6],
    const uint8_t destination_mac[6], const uint8_t source_address[16],
    const uint8_t destination_address[16], const uint8_t target_address[16],
    size_t *frame_length);
kstatus_t net_ndp_parse_neighbor_solicitation(const uint8_t *frame,
                                              size_t frame_length,
                                              net_ndp_view_t *view);
kstatus_t net_ndp_build_neighbor_advertisement(
    uint8_t *frame, size_t capacity, const uint8_t source_mac[6],
    const uint8_t destination_mac[6], const uint8_t source_address[16],
    const uint8_t destination_address[16], const uint8_t target_address[16],
    uint32_t flags, const uint8_t target_mac[6], size_t *frame_length);
kstatus_t net_ndp_parse_neighbor_advertisement(const uint8_t *frame,
                                               size_t frame_length,
                                               net_ndp_view_t *view);
void net_ndp_solicited_node_address(const uint8_t address[16],
                                    uint8_t solicited_node[16]);
void net_ndp_multicast_mac(const uint8_t multicast_address[16], uint8_t mac[6]);

void net_firewall_init(net_firewall_t *firewall, uint8_t default_action);
kstatus_t net_firewall_add(net_firewall_t *firewall,
                           const net_firewall_rule_t *rule,
                           uint32_t *rule_id);
kstatus_t net_firewall_remove(net_firewall_t *firewall, uint32_t rule_id);
bool net_firewall_check(net_firewall_t *firewall,
                        const net_firewall_packet_t *packet);

void net_arp_cache_init(net_arp_cache_t *cache);
kstatus_t net_arp_cache_update(net_arp_cache_t *cache, uint32_t address,
                               const uint8_t mac[6], uint64_t timestamp);
bool net_arp_cache_lookup(net_arp_cache_t *cache, uint32_t address,
                          uint8_t mac[6]);
void net_ipv6_neighbor_cache_init(net_ipv6_neighbor_cache_t *cache);
kstatus_t net_ipv6_neighbor_cache_update(net_ipv6_neighbor_cache_t *cache,
                                          const uint8_t address[16],
                                          const uint8_t mac[6],
                                          uint64_t timestamp);
bool net_ipv6_neighbor_cache_lookup(net_ipv6_neighbor_cache_t *cache,
                                    const uint8_t address[16], uint8_t mac[6]);

bool net_core_self_test(void);
bool net_arp_self_test(void);
bool net_ipv6_self_test(void);
bool net_ndp_self_test(void);
bool net_tcp_self_test(void);
bool net_firewall_self_test(void);
