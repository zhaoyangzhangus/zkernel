#pragma once
#pragma once
#include "abi.h"

enum os_address_family {
    OS_AF_INET4 = 2,
    OS_AF_INET6 = 10,
};

enum os_socket_type {
    OS_SOCK_STREAM = 1,
    OS_SOCK_DGRAM = 2,
};

/* 作为 SOCKET_CREATE/ACCEPT 第四个参数传入的可选标志。 */
#define OS_SOCKET_CREATE_FLAG_CLOEXEC (1U << 0)
#define OS_SOCKET_CREATE_FLAG_MASK    OS_SOCKET_CREATE_FLAG_CLOEXEC

/* IPv6 端点使用网络字节序地址，端口字段沿用现有主机端整数 ABI。 */
typedef struct os_socket_ipv6_endpoint {
    uint8_t address[16];
    uint16_t port;
    uint16_t reserved;
} os_socket_ipv6_endpoint_t;

/* 异步发送只保存复制后的负载；用户缓冲在系统调用返回后即可复用。 */
typedef struct os_socket_async_send {
    os_versioned_header_t hdr;
    os_handle_t socket;
    os_handle_t completion_port;
    uint64_t user_key;
    uint64_t buffer;
    uint64_t length;
    uint32_t address;
    uint16_t port;
    uint16_t flags;
} os_socket_async_send_t;

/* IPv6 异步发送单独使用 16 字节地址，避免复用 IPv4 结构造成截断。 */
typedef struct os_socket_ipv6_async_send {
    os_versioned_header_t hdr;
    os_handle_t socket;
    os_handle_t completion_port;
    uint64_t user_key;
    uint64_t buffer;
    uint64_t length;
    uint8_t address[16];
    uint16_t port;
    uint16_t flags;
} os_socket_ipv6_async_send_t;

/* System ABI 1 的 IPv4 socket 参数使用主机端整数，协议层负责 wire 字节序。 */
typedef struct os_socket_ipv4_endpoint {
    uint32_t address;
    uint16_t port;
    uint16_t reserved;
} os_socket_ipv4_endpoint_t;

enum os_socket_info_flags {
    OS_SOCKET_INFO_BOUND = 1U << 0,
    OS_SOCKET_INFO_CONNECTED = 1U << 1,
    OS_SOCKET_INFO_LISTENING = 1U << 2,
};

enum os_socket_option {
    OS_SOCKET_OPTION_REUSE_ADDRESS = 1,
    OS_SOCKET_OPTION_ERROR = 2,
    OS_SOCKET_OPTION_TYPE = 3,
    OS_SOCKET_OPTION_DOMAIN = 4,
    OS_SOCKET_OPTION_PROTOCOL = 5,
    OS_SOCKET_OPTION_ACCEPT_CONNECTION = 6,
    OS_SOCKET_OPTION_RECEIVE_BUFFER = 7,
    OS_SOCKET_OPTION_SEND_BUFFER = 8,
};

enum os_socket_shutdown_how {
    OS_SOCKET_SHUT_READ = 0,
    OS_SOCKET_SHUT_WRITE = 1,
    OS_SOCKET_SHUT_BOTH = 2,
};

typedef struct os_socket_info {
    os_versioned_header_t hdr;
    os_handle_t socket;
    uint16_t family;
    uint16_t type;
    uint16_t protocol;
    uint16_t reserved;
    uint32_t flags;
    uint32_t local_address;
    uint32_t peer_address;
    uint8_t local_address6[16];
    uint8_t peer_address6[16];
    uint16_t local_port;
    uint16_t peer_port;
    uint32_t reserved2;
} os_socket_info_t;

typedef struct os_socket_option_value {
    os_versioned_header_t hdr;
    os_handle_t socket;
    uint32_t option;
    int32_t value;
} os_socket_option_value_t;

typedef struct os_socket_shutdown {
    os_versioned_header_t hdr;
    os_handle_t socket;
    uint32_t how;
    uint32_t reserved;
} os_socket_shutdown_t;

/* Additive options; existing native structure layouts and numbers stay fixed. */
#define OS_SOCKET_OPTION_KEEPALIVE 9U
#define OS_SOCKET_OPTION_KEEPIDLE 10U
#define OS_SOCKET_OPTION_KEEPINTVL 11U
#define OS_SOCKET_OPTION_KEEPCNT 12U
