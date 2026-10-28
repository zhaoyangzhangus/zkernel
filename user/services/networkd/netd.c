#include <stdint.h>
#include <stdbool.h>

#include <uapi/all.h>
#include <uapi/ipc.h>

#define NETD_DHCP_CLIENT_PORT 68U
#define NETD_DHCP_SERVER_PORT 67U
#define NETD_DHCP_BROADCAST  0xFFFFFFFFU
#define NETD_DHCP_COOKIE     0x63825363U
#define NETD_DHCP_PACKET_SIZE 576U
#define NETD_DHCP_RETRY_NS   2000000000ULL
#define NETD_DHCP_OPTION_END 255U
#define NETD_EVENT_PORT_CAPACITY 8U

enum netd_state {
    NETD_STATE_LINK_DOWN = 0,
    NETD_STATE_WAIT_OFFER,
    NETD_STATE_WAIT_ACK,
    NETD_STATE_BOUND,
};

enum netd_wait_event {
    NETD_WAIT_NONE = 0,
    NETD_WAIT_SOCKET,
    NETD_WAIT_LINK,
    NETD_WAIT_TIMER,
};

typedef struct dhcp_offer {
    uint32_t address;
    uint32_t subnet_mask;
    uint32_t gateway;
    uint32_t server_id;
    uint32_t dns_server;
    uint8_t message_type;
    bool has_subnet_mask;
    bool has_server_id;
    bool has_dns_server;
} dhcp_offer_t;

typedef struct netd_context {
    os_net_status_t status;
    os_handle_t socket;
    os_handle_t link_port;
    os_handle_t timer;
    enum netd_state state;
    uint32_t transaction;
    uint32_t offered_address;
    uint32_t server_id;
    uint32_t offered_dns_server;
    bool debug_link_valid;
    bool debug_link_up;
    uint32_t debug_attempts;
    uint32_t debug_packets;
} netd_context_t;

static int64_t netd_syscall_one(uint64_t number, uint64_t arg0) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t netd_syscall_two(uint64_t number, uint64_t arg0,
                                uint64_t arg1) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static void netd_persist_debug(const char *text, size_t length);

static void netd_debug(const char *text) {
    size_t length = 0U;
    if (text == 0) return;
    while (length < 240U && text[length] != '\0') ++length;
    if (length == 0U) return;
    (void)netd_syscall_two(OS_SYS_DEBUG_WRITE, (uint64_t)(uintptr_t)text,
                           length);
    netd_persist_debug(text, length);
}

static void netd_debug_attempt(netd_context_t *context, const char *text) {
    if (context == 0 || text == 0 || text[0] == '\0' ||
        context->debug_attempts >= 6U) return;
    ++context->debug_attempts;
    netd_debug(text);
}

static void netd_debug_packet(netd_context_t *context, const char *text) {
    if (context == 0 || text == 0 || text[0] == '\0' ||
        context->debug_packets >= 6U) return;
    ++context->debug_packets;
    netd_debug(text);
}

static int64_t netd_syscall_three(uint64_t number, uint64_t arg0,
                                  uint64_t arg1, uint64_t arg2) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx) : : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t netd_syscall_four(uint64_t number, uint64_t arg0,
                                 uint64_t arg1, uint64_t arg2,
                                 uint64_t arg3) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    register uint64_t r10 __asm__("r10") = arg3;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx), "+r"(r10) : : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static void netd_persist_debug(const char *text, size_t length) {
    static os_handle_t file = OS_INVALID_HANDLE;
    static uint64_t total;
    static bool attempted;
    if (text == 0 || length == 0U || length > 240U ||
        total > 16384U || length > 16384U - total) return;
    if (!attempted) {
        attempted = true;
        if (netd_syscall_four(OS_SYS_FILE_OPEN,
                (uint64_t)(uintptr_t)"/log/netd.log",
                OS_FILE_OPEN_WRITE | OS_FILE_OPEN_CREATE | OS_FILE_OPEN_TRUNCATE,
                0644U, (uint64_t)(uintptr_t)&file) < 0)
            file = OS_INVALID_HANDLE;
    }
    if (file == OS_INVALID_HANDLE) return;
    size_t offset = 0U;
    while (offset < length) {
        uint64_t written = 0U;
        if (netd_syscall_four(OS_SYS_FILE_WRITE, file,
                (uint64_t)(uintptr_t)(text + offset), length - offset,
                (uint64_t)(uintptr_t)&written) < 0 ||
            written == 0U || written > length - offset) break;
        offset += (size_t)written;
        total += written;
    }
    (void)netd_syscall_one(OS_SYS_FILE_FSYNC, file);
}

static int64_t netd_syscall_six(uint64_t number, uint64_t arg0,
                                uint64_t arg1, uint64_t arg2,
                                uint64_t arg3, uint64_t arg4,
                                uint64_t arg5) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    register uint64_t r10 __asm__("r10") = arg3;
    register uint64_t r8 __asm__("r8") = arg4;
    register uint64_t r9 __asm__("r9") = arg5;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx), "+r"(r10), "+r"(r8), "+r"(r9) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

__attribute__((noreturn)) static void netd_exit(uint64_t status) {
    (void)netd_syscall_one(OS_SYS_NET_SUBSCRIBE, OS_INVALID_HANDLE);
    (void)netd_syscall_one(OS_SYS_THREAD_EXIT, status);
    for (;;) __asm__ volatile ("pause");
}

static void close_handle(os_handle_t *handle) {
    if (handle == 0 || *handle == OS_INVALID_HANDLE) return;
    (void)netd_syscall_one(OS_SYS_HANDLE_CLOSE, *handle);
    *handle = OS_INVALID_HANDLE;
}

static bool resolver_append_decimal(char *buffer, size_t capacity,
                                    size_t *length, uint32_t value) {
    char digits[3];
    uint32_t count = 0U;
    do {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count != 0U) {
        if (*length + 1U >= capacity) return false;
        buffer[(*length)++] = digits[--count];
    }
    buffer[*length] = '\0';
    return true;
}

static void write_resolver_config(uint32_t address) {
    static const char directory[] = "/EFI/OS";
    static const char path[] = "/EFI/OS/resolv.conf";
    os_file_path_op_t operation = {0};
    os_handle_t file = OS_INVALID_HANDLE;
    char line[64] = "nameserver ";
    size_t length = 11U;
    uint64_t written = 0U;

    operation.hdr.size = sizeof(operation);
    operation.hdr.version = OS_SYSCALL_ABI_VERSION;
    operation.path = (uint64_t)(uintptr_t)directory;
    operation.mode = 0755U;
    (void)netd_syscall_one(OS_SYS_FILE_MKDIR,
                           (uint64_t)(uintptr_t)&operation);

    if (address == 0U) {
        operation = (os_file_path_op_t){0};
        operation.hdr.size = sizeof(operation);
        operation.hdr.version = OS_SYSCALL_ABI_VERSION;
        operation.path = (uint64_t)(uintptr_t)path;
        (void)netd_syscall_one(OS_SYS_FILE_REMOVE,
                               (uint64_t)(uintptr_t)&operation);
        return;
    }

    for (uint32_t shift = 24U;; shift -= 8U) {
        if (!resolver_append_decimal(line, sizeof(line), &length,
                                     (address >> shift) & 0xFFU)) return;
        if (shift == 0U) break;
        if (length + 2U >= sizeof(line)) return;
        line[length++] = '.';
        line[length] = '\0';
    }
    if (length + 2U >= sizeof(line)) return;
    line[length++] = '\n';
    line[length] = '\0';

    if (netd_syscall_four(OS_SYS_FILE_OPEN,
                          (uint64_t)(uintptr_t)path,
                          OS_FILE_OPEN_WRITE | OS_FILE_OPEN_CREATE |
                          OS_FILE_OPEN_TRUNCATE,
                          0644U,
                          (uint64_t)(uintptr_t)&file) < 0 ||
        file == OS_INVALID_HANDLE) return;

    if (netd_syscall_four(OS_SYS_FILE_WRITE, file,
                          (uint64_t)(uintptr_t)line, length,
                          (uint64_t)(uintptr_t)&written) >= 0 &&
        written == length) {
        (void)netd_syscall_one(OS_SYS_FILE_FSYNC, file);
    }
    close_handle(&file);
}

static int64_t netd_get_status(os_net_status_t *status) {
    if (status == 0) return -22;
    *status = (os_net_status_t){0};
    status->hdr.size = sizeof(*status);
    status->hdr.version = OS_SYSCALL_ABI_VERSION;
    return netd_syscall_one(OS_SYS_NET_GET_STATUS,
                            (uint64_t)(uintptr_t)status);
}

static int64_t netd_set_ipv4(uint32_t address, uint8_t prefix_length,
                             uint32_t gateway) {
    os_net_set_ipv4_config_t config = {0};
    config.hdr.size = sizeof(config);
    config.hdr.version = OS_SYSCALL_ABI_VERSION;
    config.address = address;
    config.gateway = gateway;
    config.prefix_length = prefix_length;
    return netd_syscall_one(OS_SYS_NET_SET_IPV4,
                            (uint64_t)(uintptr_t)&config);
}

static bool create_link_port(netd_context_t *context) {
    os_handle_t port = OS_INVALID_HANDLE;
    if (context == 0) return false;
    if (netd_syscall_three(OS_SYS_PORT_CREATE, OS_PORT_MESSAGE,
                           NETD_EVENT_PORT_CAPACITY,
                           (uint64_t)(uintptr_t)&port) < 0 ||
        port == OS_INVALID_HANDLE) {
        return false;
    }
    if (netd_syscall_one(OS_SYS_NET_SUBSCRIBE, port) < 0) {
        close_handle(&port);
        return false;
    }
    context->link_port = port;
    return true;
}

static bool arm_timer(netd_context_t *context, uint64_t delay_ns) {
    os_handle_t timer = OS_INVALID_HANDLE;
    if (context == 0) return false;
    close_handle(&context->timer);
    if (netd_syscall_three(OS_SYS_TIMER_CREATE, delay_ns, 0U,
                           (uint64_t)(uintptr_t)&timer) < 0 ||
        timer == OS_INVALID_HANDLE) {
        netd_debug("OS_NETD_TIMER_FAIL\r\n");
        return false;
    }
    context->timer = timer;
    return true;
}

static int64_t wait_events(const netd_context_t *context,
                           enum netd_wait_event *event) {
    os_handle_t handles[3];
    enum netd_wait_event kinds[3];
    os_wait_many_t wait = {0};
    uint32_t count = 0U;
    int64_t status;

    if (context == 0 || event == 0 ||
        context->link_port == OS_INVALID_HANDLE) return -22;

    if (context->socket != OS_INVALID_HANDLE) {
        handles[count] = context->socket;
        kinds[count++] = NETD_WAIT_SOCKET;
    }

    handles[count] = context->link_port;
    kinds[count++] = NETD_WAIT_LINK;

    if (context->timer != OS_INVALID_HANDLE) {
        handles[count] = context->timer;
        kinds[count++] = NETD_WAIT_TIMER;
    }

    wait.hdr.size = sizeof(wait);
    wait.hdr.version = OS_SYSCALL_ABI_VERSION;
    wait.count = count;
    wait.handles = (uint64_t)(uintptr_t)handles;
    wait.timeout_ns = OS_WAIT_INFINITE;

    status = netd_syscall_one(OS_SYS_WAIT_MANY,
                              (uint64_t)(uintptr_t)&wait);
    if (status < 0) return status;
    if (wait.result_index >= count) return -5;
    *event = kinds[wait.result_index];
    return 0;
}

static void put_be16(uint8_t *buffer, uint16_t value) {
    buffer[0] = (uint8_t)(value >> 8U);
    buffer[1] = (uint8_t)value;
}

static void put_be32(uint8_t *buffer, uint32_t value) {
    buffer[0] = (uint8_t)(value >> 24U);
    buffer[1] = (uint8_t)(value >> 16U);
    buffer[2] = (uint8_t)(value >> 8U);
    buffer[3] = (uint8_t)value;
}

static uint32_t get_be32(const uint8_t *buffer) {
    return ((uint32_t)buffer[0] << 24U) |
           ((uint32_t)buffer[1] << 16U) |
           ((uint32_t)buffer[2] << 8U) |
           buffer[3];
}

static void copy_bytes(uint8_t *destination, const uint8_t *source,
                       size_t length) {
    while (length-- != 0U) *destination++ = *source++;
}

static bool append_option(uint8_t *packet, size_t capacity, size_t *offset,
                          uint8_t type, const void *value, uint8_t length) {
    if (packet == 0 || offset == 0 ||
        (value == 0 && length != 0U) ||
        *offset > capacity ||
        capacity - *offset < (size_t)length + 2U) return false;
    packet[(*offset)++] = type;
    packet[(*offset)++] = length;
    copy_bytes(packet + *offset, (const uint8_t *)value, length);
    *offset += length;
    return true;
}

static bool append_byte_option(uint8_t *packet, size_t capacity,
                               size_t *offset, uint8_t type,
                               uint8_t value) {
    return append_option(packet, capacity, offset, type, &value, 1U);
}

static bool append_u32_option(uint8_t *packet, size_t capacity,
                              size_t *offset, uint8_t type,
                              uint32_t value) {
    uint8_t encoded[4];
    put_be32(encoded, value);
    return append_option(packet, capacity, offset, type,
                         encoded, sizeof(encoded));
}

static bool build_dhcp_packet(uint8_t *packet, size_t capacity,
                              uint32_t xid, const uint8_t mac[6],
                              uint8_t message_type,
                              uint32_t requested_address,
                              uint32_t server_id,
                              size_t *length) {
    static const uint8_t parameter_request_list[] = {
        1U, 3U, 6U, 15U, 51U, 54U, 58U, 59U
    };
    uint8_t client_identifier[7] = {1U, 0U, 0U, 0U, 0U, 0U, 0U};
    size_t offset = 240U;

    if (packet == 0 || mac == 0 || length == 0 ||
        capacity < offset + 1U || message_type == 0U) return false;

    for (size_t index = 0U; index < capacity; ++index) packet[index] = 0U;
    packet[0] = 1U;
    packet[1] = 1U;
    packet[2] = 6U;
    put_be32(packet + 4U, xid);
    put_be16(packet + 10U, 0x8000U);
    copy_bytes(packet + 28U, mac, 6U);
    copy_bytes(client_identifier + 1U, mac, 6U);
    put_be32(packet + 236U, NETD_DHCP_COOKIE);

    if (!append_byte_option(packet, capacity, &offset, 53U, message_type) ||
        !append_option(packet, capacity, &offset, 55U,
                       parameter_request_list,
                       (uint8_t)sizeof(parameter_request_list)) ||
        !append_option(packet, capacity, &offset, 61U,
                       client_identifier,
                       (uint8_t)sizeof(client_identifier))) return false;

    if (requested_address != 0U &&
        !append_u32_option(packet, capacity, &offset, 50U,
                           requested_address)) return false;
    if (server_id != 0U &&
        !append_u32_option(packet, capacity, &offset, 54U,
                           server_id)) return false;

    if (offset >= capacity) return false;
    packet[offset++] = NETD_DHCP_OPTION_END;
    *length = offset;
    return true;
}

static bool parse_dhcp_packet(const uint8_t *packet, size_t length,
                              uint32_t xid, dhcp_offer_t *offer) {
    size_t offset = 240U;
    if (packet == 0 || offer == 0 || length < offset ||
        packet[0] != 2U || packet[1] != 1U || packet[2] != 6U ||
        get_be32(packet + 4U) != xid ||
        get_be32(packet + 236U) != NETD_DHCP_COOKIE) return false;

    *offer = (dhcp_offer_t){0};
    offer->address = get_be32(packet + 16U);

    while (offset < length) {
        uint8_t type = packet[offset++];
        uint8_t option_length;

        if (type == 0U) continue;
        if (type == NETD_DHCP_OPTION_END) break;
        if (offset >= length) return false;

        option_length = packet[offset++];
        if ((size_t)option_length > length - offset) return false;

        if (type == 53U && option_length == 1U) {
            offer->message_type = packet[offset];
        } else if (type == 1U && option_length == 4U) {
            offer->subnet_mask = get_be32(packet + offset);
            offer->has_subnet_mask = true;
        } else if (type == 3U && option_length >= 4U) {
            offer->gateway = get_be32(packet + offset);
        } else if (type == 6U && option_length >= 4U) {
            offer->dns_server = get_be32(packet + offset);
            offer->has_dns_server = offer->dns_server != 0U;
        } else if (type == 54U && option_length == 4U) {
            offer->server_id = get_be32(packet + offset);
            offer->has_server_id = true;
        }
        offset += option_length;
    }

    return offer->message_type != 0U;
}

static bool prefix_from_mask(uint32_t mask, uint8_t *prefix) {
    bool saw_zero = false;
    uint8_t count = 0U;
    if (prefix == 0) return false;
    for (int32_t bit = 31; bit >= 0; --bit) {
        if ((mask & (1U << (uint32_t)bit)) != 0U) {
            if (saw_zero) return false;
            ++count;
        } else {
            saw_zero = true;
        }
    }
    *prefix = count;
    return true;
}

static int64_t socket_create_bound(os_handle_t *handle) {
    int64_t status;
    if (handle == 0) return -22;
    *handle = OS_INVALID_HANDLE;
    status = netd_syscall_six(OS_SYS_SOCKET_CREATE,
                              OS_AF_INET4, OS_SOCK_DGRAM,
                              0U, (uint64_t)(uintptr_t)handle, 0U, 0U);
    if (status < 0 || *handle == OS_INVALID_HANDLE)
        return status < 0 ? status : -5;

    status = netd_syscall_three(OS_SYS_SOCKET_BIND, *handle,
                                0U, NETD_DHCP_CLIENT_PORT);
    if (status < 0) {
        close_handle(handle);
        return status;
    }
    return 0;
}

static int64_t socket_send_packet(os_handle_t handle,
                                  const uint8_t *packet,
                                  size_t length) {
    return netd_syscall_six(OS_SYS_SOCKET_SEND, handle,
                            (uint64_t)(uintptr_t)packet,
                            length, NETD_DHCP_BROADCAST,
                            NETD_DHCP_SERVER_PORT, 0U);
}

static int64_t socket_receive_nonblock(os_handle_t handle,
                                       uint8_t *packet,
                                       size_t capacity) {
    uint32_t source_address = 0U;
    uint16_t source_port = 0U;
    return netd_syscall_six(OS_SYS_SOCKET_RECV, handle,
                            (uint64_t)(uintptr_t)packet, capacity,
                            (uint64_t)(uintptr_t)&source_address,
                            (uint64_t)(uintptr_t)&source_port, 0U);
}

static void drain_socket(os_handle_t socket) {
    uint8_t packet[NETD_DHCP_PACKET_SIZE];
    if (socket == OS_INVALID_HANDLE) return;
    for (;;) {
        if (socket_receive_nonblock(socket, packet, sizeof(packet)) <= 0) break;
    }
}

static void drain_link_events(os_handle_t port) {
    os_net_event_t event;
    if (port == OS_INVALID_HANDLE) return;
    for (;;) {
        uint64_t size = 0U;
        int64_t status = netd_syscall_six(
            OS_SYS_PORT_RECEIVE, port,
            (uint64_t)(uintptr_t)&event, sizeof(event),
            (uint64_t)(uintptr_t)&size, 0U, 0U);
        if (status < 0) break;
    }
}

static bool send_discover(netd_context_t *context) {
    uint8_t packet[NETD_DHCP_PACKET_SIZE];
    size_t length = 0U;
    if (context == 0 || context->socket == OS_INVALID_HANDLE) return false;
    return build_dhcp_packet(packet, sizeof(packet), context->transaction,
                             context->status.mac, 1U, 0U, 0U, &length) &&
           socket_send_packet(context->socket, packet, length) >= 0;
}

static bool send_request(netd_context_t *context) {
    uint8_t packet[NETD_DHCP_PACKET_SIZE];
    size_t length = 0U;
    if (context == 0 || context->socket == OS_INVALID_HANDLE ||
        context->offered_address == 0U || context->server_id == 0U) return false;
    return build_dhcp_packet(packet, sizeof(packet), context->transaction,
                             context->status.mac, 3U,
                             context->offered_address, context->server_id,
                             &length) &&
           socket_send_packet(context->socket, packet, length) >= 0;
}

static bool start_dhcp(netd_context_t *context) {
    bool sent;
    if (context == 0) return false;
    if (context->socket == OS_INVALID_HANDLE &&
        socket_create_bound(&context->socket) < 0) {
        netd_debug("OS_NETD_SOCKET_FAIL\r\n");
        context->state = NETD_STATE_WAIT_OFFER;
        return arm_timer(context, NETD_DHCP_RETRY_NS);
    }
    if (context->debug_attempts == 0U)
        netd_debug("OS_NETD_SOCKET_OK\r\n");

    drain_socket(context->socket);
    ++context->transaction;
    context->offered_address = 0U;
    context->server_id = 0U;
    context->offered_dns_server = 0U;
    context->state = NETD_STATE_WAIT_OFFER;
    sent = send_discover(context);
    netd_debug_attempt(context, sent ?
        "OS_NETD_DISCOVER_SEND_OK\r\n" :
        "OS_NETD_DISCOVER_SEND_FAIL\r\n");
    return arm_timer(context, NETD_DHCP_RETRY_NS);
}

static bool enter_bound(netd_context_t *context) {
    if (context == 0) return false;
    close_handle(&context->socket);
    close_handle(&context->timer);
    context->state = NETD_STATE_BOUND;
    return true;
}

static bool enter_link_down(netd_context_t *context) {
    if (context == 0) return false;
    close_handle(&context->socket);
    close_handle(&context->timer);
    if (context->status.ipv4_address != 0U) {
        (void)netd_set_ipv4(0U, 0U, 0U);
        context->status.ipv4_address = 0U;
        context->status.ipv4_gateway = 0U;
        context->status.ipv4_prefix_length = 0U;
    }
    context->state = NETD_STATE_LINK_DOWN;
    /* Cold PHY negotiation can finish after the initial status snapshot.
     * Keep a bounded fallback even if a link notification is lost. */
    return arm_timer(context, NETD_DHCP_RETRY_NS);
}

static bool refresh_and_transition(netd_context_t *context) {
    if (context == 0 || netd_get_status(&context->status) < 0) return false;
    if (!context->debug_link_valid ||
        context->debug_link_up !=
            ((context->status.flags & OS_NET_STATUS_LINK_UP) != 0U)) {
        context->debug_link_valid = true;
        context->debug_link_up =
            (context->status.flags & OS_NET_STATUS_LINK_UP) != 0U;
        netd_debug(context->debug_link_up ?
            "OS_NETD_LINK_UP\r\n" : "OS_NETD_LINK_DOWN\r\n");
    }
    if ((context->status.flags & OS_NET_STATUS_LINK_UP) == 0U)
        return enter_link_down(context);
    if (context->status.ipv4_address != 0U &&
        context->status.ipv4_prefix_length != 0U)
        return enter_bound(context);
    return start_dhcp(context);
}

static bool process_socket(netd_context_t *context) {
    uint8_t packet[NETD_DHCP_PACKET_SIZE];

    if (context == 0 || context->socket == OS_INVALID_HANDLE) return true;

    for (;;) {
        dhcp_offer_t offer;
        int64_t received = socket_receive_nonblock(context->socket,
                                                   packet, sizeof(packet));
        if (received <= 0) return true;
        netd_debug_packet(context, "OS_NETD_PACKET_RX\r\n");

        if (!parse_dhcp_packet(packet, (size_t)received,
                               context->transaction, &offer)) {
            netd_debug_packet(context, "OS_NETD_PACKET_INVALID\r\n");
            continue;
        }

        if (offer.message_type == 6U) {
            netd_debug("OS_NETD_DHCP_NAK\r\n");
            return start_dhcp(context);
        }

        if (context->state == NETD_STATE_WAIT_OFFER &&
            offer.message_type == 2U &&
            offer.address != 0U &&
            offer.has_subnet_mask &&
            offer.has_server_id) {
            uint8_t prefix;
            if (!prefix_from_mask(offer.subnet_mask, &prefix)) continue;

            context->offered_address = offer.address;
            context->server_id = offer.server_id;
            context->offered_dns_server =
                offer.has_dns_server ? offer.dns_server : 0U;
            netd_debug("OS_NETD_DHCP_OFFER\r\n");
            if (send_request(context)) {
                netd_debug("OS_NETD_REQUEST_SEND_OK\r\n");
                context->state = NETD_STATE_WAIT_ACK;
                if (!arm_timer(context, NETD_DHCP_RETRY_NS)) return false;
            } else {
                netd_debug("OS_NETD_REQUEST_SEND_FAIL\r\n");
            }
            continue;
        }

        if (context->state == NETD_STATE_WAIT_ACK &&
            offer.message_type == 5U &&
            offer.address != 0U &&
            offer.has_subnet_mask) {
            uint8_t prefix;
            if (!prefix_from_mask(offer.subnet_mask, &prefix)) continue;

            if (netd_set_ipv4(offer.address, prefix, offer.gateway) == 0) {
                netd_debug("OS_NETD_DHCP_ACK\r\n");
                context->status.ipv4_address = offer.address;
                context->status.ipv4_gateway = offer.gateway;
                context->status.ipv4_prefix_length = prefix;
                write_resolver_config(offer.has_dns_server ?
                    offer.dns_server : context->offered_dns_server);
                return enter_bound(context);
            }
            netd_debug("OS_NETD_IPV4_SET_FAIL\r\n");
            return start_dhcp(context);
        }
    }
}

static bool process_link(netd_context_t *context) {
    if (context == 0) return false;
    drain_link_events(context->link_port);
    return refresh_and_transition(context);
}

static bool process_timer(netd_context_t *context) {
    if (context == 0) return false;
    close_handle(&context->timer);
    if (netd_get_status(&context->status) < 0) return false;
    if ((context->status.flags & OS_NET_STATUS_LINK_UP) == 0U)
        return enter_link_down(context);
    if (context->status.ipv4_address != 0U &&
        context->status.ipv4_prefix_length != 0U)
        return enter_bound(context);
    return start_dhcp(context);
}

__attribute__((noreturn)) void netd_entry(void) {
    netd_context_t context = {
        .socket = OS_INVALID_HANDLE,
        .link_port = OS_INVALID_HANDLE,
        .timer = OS_INVALID_HANDLE,
        .state = NETD_STATE_LINK_DOWN,
        .transaction = 0x4C544F53U,
    };

    netd_debug("OS_NETD_START\r\n");
    if (!create_link_port(&context)) {
        netd_debug("OS_NETD_LINK_PORT_FAIL\r\n");
        netd_exit(1U);
    }
    if (!refresh_and_transition(&context)) {
        netd_debug("OS_NETD_INITIAL_STATUS_FAIL\r\n");
        netd_exit(1U);
    }

    for (;;) {
        enum netd_wait_event event = NETD_WAIT_NONE;
        if (wait_events(&context, &event) < 0) {
            netd_debug("OS_NETD_WAIT_FAIL\r\n");
            netd_exit(1U);
        }

        if (event == NETD_WAIT_SOCKET) {
            if (!process_socket(&context)) netd_exit(1U);
        } else if (event == NETD_WAIT_LINK) {
            if (!process_link(&context)) netd_exit(1U);
        } else if (event == NETD_WAIT_TIMER) {
            if (!process_timer(&context)) netd_exit(1U);
        }
    }
}
