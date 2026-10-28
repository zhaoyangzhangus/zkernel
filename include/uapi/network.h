#pragma once
#pragma once

#include "abi.h"

#define OS_NET_STATUS_HARDWARE_PRESENT (1U << 0)
#define OS_NET_STATUS_LINK_UP          (1U << 1)
#define OS_NET_STATUS_IPV6_CONFIGURED  (1U << 2)

typedef struct os_net_status {
    os_versioned_header_t hdr;
    uint32_t flags;
    uint32_t ipv4_address;
    uint32_t ipv4_gateway;
    uint8_t ipv4_prefix_length;
    uint8_t reserved[3];
    uint8_t ipv6_address[16];
    uint64_t link_transitions;
    uint64_t reset_count;
    /* DHCP 客户端构造 chaddr/client-id 所需的硬件地址。 */
    uint8_t mac[6];
    uint8_t mac_reserved[2];
} os_net_status_t;

typedef struct os_net_set_ipv4_config {
    os_versioned_header_t hdr;
    uint32_t address;
    uint32_t gateway;
    uint8_t prefix_length;
    uint8_t reserved[3];
} os_net_set_ipv4_config_t;

/* PCI 网卡逐项枚举；活动网卡标记用于关联当前网络配置。 */
#define OS_NET_ADAPTER_ACTIVE 1U
typedef struct os_net_adapter {
    os_versioned_header_t hdr;
    uint32_t index;
    uint32_t count;
    uint32_t flags;
    uint16_t segment;
    uint16_t vendor;
    uint16_t device;
    uint8_t bus,slot,function,subclass;
    uint16_t reserved;
} os_net_adapter_t;

#define OS_NET_EVENT_LINK_CHANGED 1U

typedef struct os_net_event {
    os_versioned_header_t hdr;
    uint32_t type;
    uint32_t status_flags;
    uint64_t link_transitions;
} os_net_event_t;
