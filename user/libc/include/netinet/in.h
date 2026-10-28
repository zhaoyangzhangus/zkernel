#pragma once

#include <stdint.h>
#include <sys/socket.h>

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

/* 匹配 Chromium 使用的 POSIX/glibc 聚合类型布局。特别是，Chromium 的静态初始化器
 * 会使用地址联合体所需的额外花括号层级，部分调用方也会访问 16/32 位视图。 */
struct in6_addr {
    union {
    uint8_t s6_addr[16];
        uint16_t s6_addr16[8];
        uint32_t s6_addr32[4];
};
};

/* 为可移植网络代码提供标准 glibc 风格地址判断宏。这些逐字节实现不依赖宿主字节序，
 * 也不依赖调用方 in6_addr 对象的对齐方式。 */
#ifndef IN6_IS_ADDR_UNSPECIFIED
#define IN6_IS_ADDR_UNSPECIFIED(a) \
    ((a)->s6_addr[0] == 0 && (a)->s6_addr[1] == 0 && \
     (a)->s6_addr[2] == 0 && (a)->s6_addr[3] == 0 && \
     (a)->s6_addr[4] == 0 && (a)->s6_addr[5] == 0 && \
     (a)->s6_addr[6] == 0 && (a)->s6_addr[7] == 0 && \
     (a)->s6_addr[8] == 0 && (a)->s6_addr[9] == 0 && \
     (a)->s6_addr[10] == 0 && (a)->s6_addr[11] == 0 && \
     (a)->s6_addr[12] == 0 && (a)->s6_addr[13] == 0 && \
     (a)->s6_addr[14] == 0 && (a)->s6_addr[15] == 0)
#endif

#ifndef IN6_IS_ADDR_LOOPBACK
#define IN6_IS_ADDR_LOOPBACK(a) \
    ((a)->s6_addr[0] == 0 && (a)->s6_addr[1] == 0 && \
     (a)->s6_addr[2] == 0 && (a)->s6_addr[3] == 0 && \
     (a)->s6_addr[4] == 0 && (a)->s6_addr[5] == 0 && \
     (a)->s6_addr[6] == 0 && (a)->s6_addr[7] == 0 && \
     (a)->s6_addr[8] == 0 && (a)->s6_addr[9] == 0 && \
     (a)->s6_addr[10] == 0 && (a)->s6_addr[11] == 0 && \
     (a)->s6_addr[12] == 0 && (a)->s6_addr[13] == 0 && \
     (a)->s6_addr[14] == 0 && (a)->s6_addr[15] == 1)
#endif

struct sockaddr_in6 {
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

#define INADDR_ANY       ((in_addr_t)0x00000000U)
#define INADDR_LOOPBACK  ((in_addr_t)0x0100007FU)
#define INADDR_BROADCAST ((in_addr_t)0xFFFFFFFFU)
#define INADDR_NONE      ((in_addr_t)0xFFFFFFFFU)

#define IPPROTO_IP   0
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_IPV6 41
#define IPPROTO_RAW  255
#define IPPROTO_HOPOPTS 0
#define IPPROTO_ICMPV6 58
#define IPPROTO_SCTP 132

#define IP_DEFAULT_MULTICAST_TTL 1
#define MCAST_JOIN_GROUP 42
#define MCAST_BLOCK_SOURCE 43
#define MCAST_UNBLOCK_SOURCE 44
#define MCAST_LEAVE_GROUP 45
#define MCAST_JOIN_SOURCE_GROUP 46
#define MCAST_LEAVE_SOURCE_GROUP 47

/* POSIX 网络接口使用的 IPv4 socket 选项名称。 */
#define IP_TOS 1
#define IP_TTL 2
#define IP_MULTICAST_IF 32
#define IP_MULTICAST_TTL 33
#define IP_MULTICAST_LOOP 34
#define IP_ADD_MEMBERSHIP 35
#define IP_DROP_MEMBERSHIP 36
#define IP_RECVTOS 13
#define IP_RECVTTL 12
#define IP_PKTINFO 8
#define IP_MTU_DISCOVER 10
#define IP_PMTUDISC_DONT 0
#define IP_PMTUDISC_DO 2

/* IPv6 socket 选项名称。System socket ABI 当前仅接受 SOL_SOCKET 选项，但公开这些
 * 标准取值可保持 Chromium 网络代码的源代码兼容性；不支持的选项在运行时返回
 * ENOPROTOOPT。 */
#define IPV6_UNICAST_HOPS 16
#define IPV6_MULTICAST_IF 17
#define IPV6_MULTICAST_HOPS 18
#define IPV6_MULTICAST_LOOP 19
#define IPV6_ADD_MEMBERSHIP 20
#define IPV6_DROP_MEMBERSHIP 21
#define IPV6_JOIN_GROUP IPV6_ADD_MEMBERSHIP
#define IPV6_LEAVE_GROUP IPV6_DROP_MEMBERSHIP
#define IPV6_MTU_DISCOVER 23
#define IPV6_V6ONLY 26
#define IPV6_RECVPKTINFO 49
#define IPV6_PKTINFO 50
#define IPV6_RECVHOPLIMIT 51
#define IPV6_HOPLIMIT 52
#define IPV6_DONTFRAG 62
#define IPV6_RECVTCLASS 66
#define IPV6_TCLASS 67
#define IPV6_FLOWINFO_FLOWLABEL 0x000fffffU
#define IPV6_PMTUDISC_DO 2

struct ip_mreqn {
    struct in_addr imr_multiaddr;
    struct in_addr imr_address;
    int imr_ifindex;
};

typedef struct ipv6_mreq {
    struct in6_addr ipv6mr_multiaddr;
    unsigned int ipv6mr_interface;
} ipv6_mreq;

typedef struct in6_pktinfo {
    struct in6_addr ipi6_addr;
    unsigned int ipi6_ifindex;
} in6_pktinfo;

#ifdef __cplusplus
extern "C" {
#endif
extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;
uint32_t htonl(uint32_t value);
uint32_t ntohl(uint32_t value);
uint16_t htons(uint16_t value);
uint16_t ntohs(uint16_t value);
#ifdef __cplusplus
}
#endif

/* RFC 3678 group membership request ABI. */
struct group_req {
    uint32_t gr_interface;
    struct sockaddr_storage gr_group;
};

struct group_source_req {
    uint32_t gsr_interface;
    struct sockaddr_storage gsr_group;
    struct sockaddr_storage gsr_source;
};
