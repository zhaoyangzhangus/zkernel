#pragma once

/*
 * 可移植 POSIX 代码使用的网络接口定义。
 *
 * System 不使用宿主 glibc 头文件，但 Chromium/WebRTC 针对 System ABI 构建时，
 * 仍需要 Linux net/if.h 中的名称。此头文件保持 ABI 兼容，并独立于 glibc 功能测试宏。
 */

#include <sys/socket.h>
#include <sys/types.h>

#define IF_NAMESIZE 16
#define IFNAMSIZ IF_NAMESIZE
#define IFHWADDRLEN 6

/* Linux 网络接口标志。 */
#define IFF_UP          0x0001
#define IFF_BROADCAST   0x0002
#define IFF_DEBUG       0x0004
#define IFF_LOOPBACK    0x0008
#define IFF_POINTOPOINT 0x0010
#define IFF_NOTRAILERS  0x0020
#define IFF_RUNNING     0x0040
#define IFF_NOARP       0x0080
#define IFF_PROMISC     0x0100
#define IFF_ALLMULTI    0x0200
#define IFF_MASTER      0x0400
#define IFF_SLAVE       0x0800
#define IFF_MULTICAST   0x1000
#define IFF_PORTSEL     0x2000
#define IFF_AUTOMEDIA   0x4000
#define IFF_DYNAMIC     0x8000

/* 可选 QUIC qbone 代码使用的 TUN/TAP 标志。 */
#define IFF_NO_PI       0x1000
#define IFF_ONE_QUEUE   0x2000
#define IFF_TUN         0x0001
#define IFF_TAP         0x0002
#define IFF_MULTI_QUEUE 0x0100
#define IFF_LOWER_UP    0x10000

struct if_nameindex {
    unsigned int if_index;
    char *if_name;
};

struct ifmap {
    unsigned long mem_start;
    unsigned long mem_end;
    unsigned short base_addr;
    unsigned char irq;
    unsigned char dma;
    unsigned char port;
};

struct ifreq {
    union {
        char ifrn_name[IFNAMSIZ];
    } ifr_ifrn;
    union {
        struct sockaddr ifru_addr;
        struct sockaddr ifru_dstaddr;
        struct sockaddr ifru_broadaddr;
        struct sockaddr ifru_netmask;
        struct sockaddr ifru_hwaddr;
        short ifru_flags;
        int ifru_ivalue;
        int ifru_mtu;
        struct ifmap ifru_map;
        char ifru_slave[IFNAMSIZ];
        char ifru_newname[IFNAMSIZ];
        char *ifru_data;
    } ifr_ifru;
};

#define ifr_name      ifr_ifrn.ifrn_name
#define ifr_hwaddr    ifr_ifru.ifru_hwaddr
#define ifr_addr      ifr_ifru.ifru_addr
#define ifr_dstaddr   ifr_ifru.ifru_dstaddr
#define ifr_broadaddr ifr_ifru.ifru_broadaddr
#define ifr_netmask   ifr_ifru.ifru_netmask
#define ifr_flags     ifr_ifru.ifru_flags
#define ifr_metric    ifr_ifru.ifru_ivalue
#define ifr_mtu       ifr_ifru.ifru_mtu
#define ifr_map       ifr_ifru.ifru_map
#define ifr_slave     ifr_ifru.ifru_slave
#define ifr_data      ifr_ifru.ifru_data
#define ifr_ifindex   ifr_ifru.ifru_ivalue
#define ifr_newname   ifr_ifru.ifru_newname

struct ifconf {
    int ifc_len;
    union {
        char *ifcu_buf;
        struct ifreq *ifcu_req;
    } ifc_ifcu;
};

#define ifc_buf ifc_ifcu.ifcu_buf
#define ifc_req ifc_ifcu.ifcu_req

#ifdef __cplusplus
extern "C" {
#endif
unsigned int if_nametoindex(const char *ifname);
char *if_indextoname(unsigned int ifindex, char *ifname);
struct if_nameindex *if_nameindex(void);
void if_freenameindex(struct if_nameindex *ptr);
#ifdef __cplusplus
}
#endif
