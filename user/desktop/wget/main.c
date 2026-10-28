#include <stdint.h>
#include <stdbool.h>

#include <uapi/all.h>

#include "tls.h"

#define WGET_URL_CAP                768U
#define WGET_HOST_CAP               256U
#define WGET_PATH_CAP               512U
#define WGET_OUTPUT_CAP             256U
#define WGET_REQUEST_CAP            4096U
#define WGET_HEADER_CAP              8192U
#define WGET_HEADER_LINE_CAP          512U
#define WGET_IO_CAP                  2048U
#define WGET_DNS_PACKET_CAP          512U
#define WGET_DNS_NAME_CAP            256U
#define WGET_REDIRECT_LIMIT          5U
#define WGET_NETWORK_WAIT_NS         10000000000ULL
#define WGET_CONNECT_WAIT_NS         5000000000ULL
#define WGET_RECV_TIMEOUT_NS         10000000000ULL
#define WGET_DNS_TIMEOUT_NS          2000000000ULL
#define WGET_RETRY_SLEEP_NS          10000000ULL
#define WGET_NETWORK_POLL_NS         100000000ULL
#define WGET_EPHEMERAL_FIRST         49152U
#define WGET_EPHEMERAL_COUNT         16384U
#define WGET_BIND_ATTEMPTS           512U
#define WGET_DNS_PORT                53U
#define WGET_HTTP_PORT               80U
#define WGET_HTTPS_PORT              443U
#define WGET_FTP_PORT                21U
#define WGET_EAGAIN                  (-11LL)
#define WGET_MAX_EXTRA_HEADERS         8U
#define WGET_USER_AGENT_CAP          128U
#define WGET_METHOD_CAP               16U
#define WGET_AUTH_CAP                256U
#define WGET_POST_DATA_CAP          1024U

#define WGET_EXIT_OK                 0U
#define WGET_EXIT_USAGE              2U
#define WGET_EXIT_NETWORK            3U
#define WGET_EXIT_HTTP               4U
#define WGET_EXIT_FILE               5U
#define WGET_EXIT_UNSUPPORTED        6U

#define DNS_TYPE_A                   1U
#define DNS_CLASS_IN                 1U
#define DNS_FLAG_RD                  0x0100U
#define DNS_FLAG_QR                  0x8000U
#define DNS_RCODE_MASK               0x000FU

typedef struct wget_url {
    char host[WGET_HOST_CAP];
    char path[WGET_PATH_CAP];
    uint16_t port;
    bool https;
    bool ftp;
} wget_url_t;

typedef struct wget_http_header {
    uint32_t status;
    bool chunked;
    bool have_content_length;
    uint64_t content_length;
    bool have_content_range;
    uint64_t content_range_start;
    uint64_t content_range_end;
    uint64_t content_range_total;
    bool have_content_disposition;
    char content_filename[WGET_OUTPUT_CAP];
    char location[WGET_URL_CAP];
    size_t header_bytes;
} wget_http_header_t;

typedef struct wget_options {
    const char *output;
    const char *directory_prefix;
    const char *referer;
    const char *username;
    const char *password;
    char user_agent[WGET_USER_AGENT_CAP];
    char method[WGET_METHOD_CAP];
    char post_data[WGET_POST_DATA_CAP];
    const char *extra_headers[WGET_MAX_EXTRA_HEADERS];
    uint32_t extra_header_count;
    uint32_t dns_override;
    uint32_t tries;
    uint64_t timeout_ns;
    bool continue_download;
    bool no_clobber;
    bool content_disposition;
    bool post_data_set;
    bool spider;
    bool help;
    bool version;
    bool quiet;
    bool server_response;
    bool no_check_certificate;
} wget_options_t;

typedef struct wget_stream {
    os_handle_t socket;
    wget_tls_transport_t *tls;
    uint64_t timeout_ns;
    const uint8_t *prefix;
    size_t prefix_length;
    size_t prefix_offset;
    uint8_t buffer[WGET_IO_CAP];
    size_t buffer_length;
    size_t buffer_offset;
} wget_stream_t;

static uint32_t g_ephemeral_seed = 0x51A3U;

static int64_t wget_syscall_one(uint64_t number, uint64_t arg0) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = 0U;
    register uint64_t rdx __asm__("rdx") = 0U;
    register uint64_t r10 __asm__("r10") = 0U;
    register uint64_t r8 __asm__("r8") = 0U;
    register uint64_t r9 __asm__("r9") = 0U;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx), "+r"(r10), "+r"(r8), "+r"(r9) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t wget_syscall_three(uint64_t number, uint64_t arg0,
                                  uint64_t arg1, uint64_t arg2) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    register uint64_t r10 __asm__("r10") = 0U;
    register uint64_t r8 __asm__("r8") = 0U;
    register uint64_t r9 __asm__("r9") = 0U;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx), "+r"(r10), "+r"(r8), "+r"(r9) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t wget_syscall_four(uint64_t number, uint64_t arg0,
                                 uint64_t arg1, uint64_t arg2,
                                 uint64_t arg3) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    register uint64_t r10 __asm__("r10") = arg3;
    register uint64_t r8 __asm__("r8") = 0U;
    register uint64_t r9 __asm__("r9") = 0U;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx), "+r"(r10), "+r"(r8), "+r"(r9) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t wget_syscall_six(uint64_t number, uint64_t arg0,
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

static size_t text_length(const char *text) {
    size_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != '\0') ++length;
    return length;
}

static bool text_equal(const char *left, const char *right) {
    if (left == 0 || right == 0) return false;
    while (*left != '\0' && *right != '\0') {
        if (*left++ != *right++) return false;
    }
    return *left == *right;
}

static char ascii_lower(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value + ('a' - 'A')) : value;
}

static bool text_prefix_ignore_case(const char *text, const char *prefix) {
    if (text == 0 || prefix == 0) return false;
    while (*prefix != '\0') {
        if (*text == '\0' || ascii_lower(*text) != ascii_lower(*prefix)) return false;
        ++text;
        ++prefix;
    }
    return true;
}

static void byte_copy(void *destination, const void *source, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    while (length-- != 0U) *out++ = *in++;
}

static void byte_zero(void *destination, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    while (length-- != 0U) *out++ = 0U;
}

static bool copy_text_cap(char *destination, size_t capacity,
                          const char *source) {
    size_t index = 0U;
    if (destination == 0 || source == 0 || capacity == 0U) return false;
    while (source[index] != '\0') {
        if (index + 1U >= capacity) return false;
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
    return true;
}

static bool append_char(char *buffer, size_t capacity, size_t *length,
                        char value) {
    if (buffer == 0 || length == 0 || *length + 1U >= capacity) return false;
    buffer[(*length)++] = value;
    buffer[*length] = '\0';
    return true;
}

static bool append_text(char *buffer, size_t capacity, size_t *length,
                        const char *text) {
    if (text == 0) return false;
    while (*text != '\0') {
        if (!append_char(buffer, capacity, length, *text++)) return false;
    }
    return true;
}

static bool append_text_until_fragment(char *buffer, size_t capacity, size_t *length,
                                       const char *text) {
    if (text == 0) return false;
    while (*text != '\0' && *text != '#') {
        if (!append_char(buffer, capacity, length, *text++)) return false;
    }
    return true;
}

static bool append_u32_decimal(char *buffer, size_t capacity, size_t *length,
                               uint32_t value) {
    char digits[10];
    uint32_t count = 0U;
    do {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count != 0U) {
        if (!append_char(buffer, capacity, length, digits[--count])) return false;
    }
    return true;
}

static bool append_u64_decimal(char *buffer, size_t capacity, size_t *length,
                               uint64_t value) {
    char digits[20];
    uint32_t count = 0U;
    do {
        digits[count++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    } while (value != 0ULL && count < sizeof(digits));
    while (count != 0U) {
        if (!append_char(buffer, capacity, length, digits[--count])) return false;
    }
    return true;
}

static bool parse_u32_text(const char *text, uint32_t *value) {
    uint64_t result = 0U;
    bool have_digit = false;
    if (text == 0 || value == 0 || *text == '\0') return false;
    while (*text >= '0' && *text <= '9') {
        have_digit = true;
        result = result * 10ULL + (uint32_t)(*text++ - '0');
        if (result > UINT32_MAX) return false;
    }
    if (!have_digit || *text != '\0') return false;
    *value = (uint32_t)result;
    return true;
}

static uint16_t load_be16(const uint8_t *buffer) {
    return (uint16_t)(((uint16_t)buffer[0] << 8U) | buffer[1]);
}

static uint32_t load_be32(const uint8_t *buffer) {
    return ((uint32_t)buffer[0] << 24U) |
           ((uint32_t)buffer[1] << 16U) |
           ((uint32_t)buffer[2] << 8U) |
           buffer[3];
}

static void store_be16(uint8_t *buffer, uint16_t value) {
    buffer[0] = (uint8_t)(value >> 8U);
    buffer[1] = (uint8_t)value;
}

static bool parse_ipv4(const char *text, uint32_t *address) {
    uint32_t result = 0U;
    uint32_t part = 0U;
    uint32_t parts = 0U;
    bool have_digit = false;
    if (text == 0 || address == 0 || *text == '\0') return false;
    for (;;) {
        char value = *text++;
        if (value >= '0' && value <= '9') {
            have_digit = true;
            part = part * 10U + (uint32_t)(value - '0');
            if (part > 255U) return false;
            continue;
        }
        if ((value == '.' || value == '\0') && have_digit) {
            result = (result << 8U) | part;
            ++parts;
            if (value == '\0') break;
            if (parts >= 4U) return false;
            part = 0U;
            have_digit = false;
            continue;
        }
        return false;
    }
    if (parts != 4U) return false;
    *address = result;
    return true;
}

static void close_handle(os_handle_t *handle) {
    if (handle == 0 || *handle == OS_INVALID_HANDLE) return;
    (void)wget_syscall_one(OS_SYS_HANDLE_CLOSE, *handle);
    *handle = OS_INVALID_HANDLE;
}

static bool sleep_ns(uint64_t delay_ns) {
    os_handle_t timer = OS_INVALID_HANDLE;
    os_wait_result_t result = {0};
    int64_t status = wget_syscall_three(OS_SYS_TIMER_CREATE, delay_ns, 0U,
                                        (uint64_t)(uintptr_t)&timer);
    if (status < 0 || timer == OS_INVALID_HANDLE) return false;
    status = wget_syscall_three(OS_SYS_WAIT_ONE, timer, OS_WAIT_INFINITE,
                                (uint64_t)(uintptr_t)&result);
    close_handle(&timer);
    return status >= 0;
}

static bool get_network_status(os_net_status_t *status) {
    if (status == 0) return false;
    *status = (os_net_status_t){0};
    status->hdr.size = sizeof(*status);
    status->hdr.version = OS_SYSCALL_ABI_VERSION;
    return wget_syscall_one(OS_SYS_NET_GET_STATUS,
                            (uint64_t)(uintptr_t)status) >= 0;
}

static bool wait_for_ipv4(os_net_status_t *status) {
    uint64_t waited = 0U;
    while (waited <= WGET_NETWORK_WAIT_NS) {
        if (get_network_status(status) &&
            (status->flags & OS_NET_STATUS_LINK_UP) != 0U &&
            status->ipv4_address != 0U) {
            return true;
        }
        if (!sleep_ns(WGET_NETWORK_POLL_NS)) return false;
        waited += WGET_NETWORK_POLL_NS;
    }
    return false;
}

static int64_t open_file(const char *path, uint32_t flags, os_handle_t *handle) {
    if (path == 0 || handle == 0) return -22;
    *handle = OS_INVALID_HANDLE;
    return wget_syscall_four(OS_SYS_FILE_OPEN,
                             (uint64_t)(uintptr_t)path,
                             flags, 0644U,
                             (uint64_t)(uintptr_t)handle);
}

static bool stat_file(const char *path, os_file_info_t *info) {
    os_file_stat_t request = {0};
    if (path == 0 || info == 0) return false;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)path;
    if (wget_syscall_one(OS_SYS_FILE_STAT, (uint64_t)(uintptr_t)&request) < 0) {
        return false;
    }
    *info = request.info;
    return true;
}

static int64_t read_file(os_handle_t handle, void *buffer, size_t capacity) {
    uint64_t bytes = 0U;
    int64_t status = wget_syscall_four(OS_SYS_FILE_READ, handle,
                                       (uint64_t)(uintptr_t)buffer,
                                       capacity,
                                       (uint64_t)(uintptr_t)&bytes);
    return status < 0 ? status : (int64_t)bytes;
}

static bool write_file_all(os_handle_t handle, const void *buffer, size_t length) {
    const uint8_t *bytes_in = (const uint8_t *)buffer;
    while (length != 0U) {
        uint64_t bytes = 0U;
        int64_t status = wget_syscall_four(OS_SYS_FILE_WRITE, handle,
                                           (uint64_t)(uintptr_t)bytes_in,
                                           length,
                                           (uint64_t)(uintptr_t)&bytes);
        if (status < 0 || bytes == 0U || bytes > length) return false;
        bytes_in += (size_t)bytes;
        length -= (size_t)bytes;
    }
    return true;
}

static void remove_file_best_effort(const char *path) {
    os_file_path_op_t operation = {0};
    if (path == 0) return;
    operation.hdr.size = sizeof(operation);
    operation.hdr.version = OS_SYSCALL_ABI_VERSION;
    operation.path = (uint64_t)(uintptr_t)path;
    (void)wget_syscall_one(OS_SYS_FILE_REMOVE,
                           (uint64_t)(uintptr_t)&operation);
}

static bool parse_resolver_file(uint32_t *dns_server) {
    static const char path[] = "/EFI/OS/resolv.conf";
    uint8_t buffer[256];
    os_handle_t file = OS_INVALID_HANDLE;
    int64_t bytes;
    if (dns_server == 0 ||
        open_file(path, OS_FILE_OPEN_READ, &file) < 0 ||
        file == OS_INVALID_HANDLE) return false;
    bytes = read_file(file, buffer, sizeof(buffer) - 1U);
    close_handle(&file);
    if (bytes <= 0 || (size_t)bytes >= sizeof(buffer)) return false;
    buffer[bytes] = 0U;

    const char *text = (const char *)buffer;
    while (*text != '\0') {
        while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
        if (text_prefix_ignore_case(text, "nameserver")) {
            text += 10U;
            while (*text == ' ' || *text == '\t') ++text;
            char address_text[32];
            size_t length = 0U;
            while (*text != '\0' && *text != ' ' && *text != '\t' &&
                   *text != '\r' && *text != '\n') {
                if (length + 1U >= sizeof(address_text)) return false;
                address_text[length++] = *text++;
            }
            address_text[length] = '\0';
            return parse_ipv4(address_text, dns_server);
        }
        while (*text != '\0' && *text != '\n') ++text;
    }
    return false;
}

static uint32_t text_hash(const char *text) {
    uint32_t hash = 2166136261U;
    while (text != 0 && *text != '\0') {
        hash ^= (uint8_t)*text++;
        hash *= 16777619U;
    }
    return hash;
}

static bool bind_local_ipv4(os_handle_t socket, uint32_t local_address,
                            uint32_t salt) {
    uint32_t start = (salt ^ g_ephemeral_seed ^ (salt >> 16U)) %
                     WGET_EPHEMERAL_COUNT;
    g_ephemeral_seed = g_ephemeral_seed * 1664525U + 1013904223U;
    for (uint32_t attempt = 0U; attempt < WGET_BIND_ATTEMPTS; ++attempt) {
        uint16_t port = (uint16_t)(WGET_EPHEMERAL_FIRST +
            ((start + attempt) % WGET_EPHEMERAL_COUNT));
        if (wget_syscall_three(OS_SYS_SOCKET_BIND, socket,
                               local_address, port) >= 0) return true;
    }
    return false;
}

static int64_t socket_send_raw(os_handle_t socket, const void *buffer,
                               size_t length, uint32_t address,
                               uint16_t port) {
    return wget_syscall_six(OS_SYS_SOCKET_SEND, socket,
                            (uint64_t)(uintptr_t)buffer, length,
                            address, port, 0U);
}

static int64_t socket_recv_raw(os_handle_t socket, void *buffer,
                               size_t capacity, uint64_t timeout_ns) {
    uint32_t source_address = 0U;
    uint16_t source_port = 0U;
    return wget_syscall_six(OS_SYS_SOCKET_RECV, socket,
                            (uint64_t)(uintptr_t)buffer, capacity,
                            (uint64_t)(uintptr_t)&source_address,
                            (uint64_t)(uintptr_t)&source_port,
                            timeout_ns);
}

static bool socket_send_retry(os_handle_t socket, const void *buffer,
                              size_t length, uint32_t address,
                              uint16_t port, uint64_t budget_ns) {
    const uint8_t *cursor = (const uint8_t *)buffer;
    uint64_t waited = 0U;
    while (length != 0U && waited <= budget_ns) {
        size_t chunk = length > WGET_IO_CAP ? WGET_IO_CAP : length;
        int64_t status = socket_send_raw(socket, cursor, chunk, address, port);
        if (status > 0) {
            if ((uint64_t)status > chunk) return false;
            cursor += (size_t)status;
            length -= (size_t)status;
            continue;
        }
        if (status != WGET_EAGAIN && status != 0) return false;
        if (!sleep_ns(WGET_RETRY_SLEEP_NS)) return false;
        waited += WGET_RETRY_SLEEP_NS;
    }
    return length == 0U;
}

static bool dns_encode_name(uint8_t *packet, size_t capacity, size_t *offset,
                            const char *host) {
    const char *label = host;
    if (packet == 0 || offset == 0 || host == 0 || *host == '\0') return false;
    while (*label != '\0') {
        const char *end = label;
        while (*end != '\0' && *end != '.') ++end;
        size_t length = (size_t)(end - label);
        if (length == 0U || length > 63U || *offset + 1U + length >= capacity) return false;
        packet[(*offset)++] = (uint8_t)length;
        byte_copy(packet + *offset, label, length);
        *offset += length;
        if (*end == '\0') break;
        label = end + 1;
    }
    if (*offset >= capacity) return false;
    packet[(*offset)++] = 0U;
    return true;
}

static bool dns_skip_name(const uint8_t *packet, size_t length, size_t *offset) {
    size_t cursor;
    if (packet == 0 || offset == 0 || *offset >= length) return false;
    cursor = *offset;
    for (uint32_t labels = 0U; labels < 128U; ++labels) {
        if (cursor >= length) return false;
        uint8_t size = packet[cursor++];
        if (size == 0U) {
            *offset = cursor;
            return true;
        }
        if ((size & 0xC0U) == 0xC0U) {
            if (cursor >= length) return false;
            *offset = cursor + 1U;
            return true;
        }
        if ((size & 0xC0U) != 0U || size > 63U || size > length - cursor) return false;
        cursor += size;
    }
    return false;
}

static bool dns_parse_a(const uint8_t *packet, size_t length, uint16_t id,
                        uint32_t *address) {
    if (packet == 0 || address == 0 || length < 12U ||
        load_be16(packet) != id) return false;
    uint16_t flags = load_be16(packet + 2U);
    uint16_t questions = load_be16(packet + 4U);
    uint16_t answers = load_be16(packet + 6U);
    if ((flags & DNS_FLAG_QR) == 0U || (flags & DNS_RCODE_MASK) != 0U ||
        questions == 0U) return false;
    size_t offset = 12U;
    for (uint16_t index = 0U; index < questions; ++index) {
        if (!dns_skip_name(packet, length, &offset) || length - offset < 4U) return false;
        offset += 4U;
    }
    for (uint16_t index = 0U; index < answers; ++index) {
        if (!dns_skip_name(packet, length, &offset) || length - offset < 10U) return false;
        uint16_t type = load_be16(packet + offset);
        uint16_t klass = load_be16(packet + offset + 2U);
        uint16_t data_length = load_be16(packet + offset + 8U);
        offset += 10U;
        if (data_length > length - offset) return false;
        if (type == DNS_TYPE_A && klass == DNS_CLASS_IN && data_length == 4U) {
            *address = load_be32(packet + offset);
            return *address != 0U;
        }
        offset += data_length;
    }
    return false;
}

static bool dns_resolve(const char *host, uint32_t local_address,
                        uint32_t dns_server, uint64_t timeout_ns,
                        uint32_t *address) {
    uint8_t packet[WGET_DNS_PACKET_CAP];
    uint8_t response[WGET_DNS_PACKET_CAP];
    os_handle_t socket = OS_INVALID_HANDLE;
    size_t offset = 12U;
    uint16_t id;
    bool success = false;

    if (parse_ipv4(host, address)) return true;
    if (host == 0 || local_address == 0U || dns_server == 0U || address == 0) return false;
    byte_zero(packet, sizeof(packet));
    id = (uint16_t)((text_hash(host) ^ g_ephemeral_seed) & 0xFFFFU);
    if (id == 0U) id = 1U;
    store_be16(packet, id);
    store_be16(packet + 2U, DNS_FLAG_RD);
    store_be16(packet + 4U, 1U);
    if (!dns_encode_name(packet, sizeof(packet), &offset, host) ||
        offset + 4U > sizeof(packet)) return false;
    store_be16(packet + offset, DNS_TYPE_A);
    store_be16(packet + offset + 2U, DNS_CLASS_IN);
    offset += 4U;

    if (wget_syscall_four(OS_SYS_SOCKET_CREATE, OS_AF_INET4, OS_SOCK_DGRAM,
                          0U, (uint64_t)(uintptr_t)&socket) < 0 ||
        socket == OS_INVALID_HANDLE ||
        !bind_local_ipv4(socket, local_address, text_hash(host) ^ 0xD153U)) {
        close_handle(&socket);
        return false;
    }

    for (uint32_t attempt = 0U; attempt < 2U && !success; ++attempt) {
        if (!socket_send_retry(socket, packet, offset, dns_server,
                               WGET_DNS_PORT, timeout_ns)) continue;
        int64_t received = socket_recv_raw(socket, response, sizeof(response),
                                           timeout_ns);
        if (received > 0 && dns_parse_a(response, (size_t)received, id, address)) {
            success = true;
        }
    }
    close_handle(&socket);
    return success;
}

static bool parse_http_url(const char *text, wget_url_t *url) {
    const char *cursor;
    size_t host_length = 0U;
    size_t path_length = 0U;
    uint32_t port;
    bool https;
    bool ftp;
    if (text == 0 || url == 0) return false;
    if (text_prefix_ignore_case(text, "http://")) {
        https = false;
        ftp = false;
        port = WGET_HTTP_PORT;
        cursor = text + 7U;
    } else if (text_prefix_ignore_case(text, "https://")) {
        https = true;
        port = WGET_HTTPS_PORT;
        cursor = text + 8U;
        ftp = false;
    } else if (text_prefix_ignore_case(text, "ftp://")) {
        https = false;
        ftp = true;
        port = WGET_FTP_PORT;
        cursor = text + 6U;
    } else {
        return false;
    }
    byte_zero(url, sizeof(*url));
    url->https = https;
    url->ftp = ftp;

    while (*cursor != '\0' && *cursor != ':' && *cursor != '/' &&
           *cursor != '?' && *cursor != '#') {
        if (*cursor <= ' ' || *cursor == '@' || *cursor == '\\' || *cursor == '[' ||
            *cursor == ']') return false;
        if (host_length + 1U >= sizeof(url->host)) return false;
        url->host[host_length++] = *cursor++;
    }
    url->host[host_length] = '\0';
    if (host_length == 0U) return false;

    if (*cursor == ':') {
        ++cursor;
        port = 0U;
        if (*cursor < '0' || *cursor > '9') return false;
        while (*cursor >= '0' && *cursor <= '9') {
            uint32_t digit = (uint32_t)(*cursor++ - '0');
            if (port > (65535U - digit) / 10U) return false;
            port = port * 10U + digit;
        }
        if (port == 0U) return false;
    }

    if (*cursor == '\0' || *cursor == '#') {
        url->path[0] = '/';
        url->path[1] = '\0';
    } else if (*cursor == '?') {
        url->path[path_length++] = '/';
        while (*cursor != '\0' && *cursor != '#') {
            if (path_length + 1U >= sizeof(url->path)) return false;
            url->path[path_length++] = *cursor++;
        }
        url->path[path_length] = '\0';
    } else if (*cursor == '/') {
        while (*cursor != '\0' && *cursor != '#') {
            if (path_length + 1U >= sizeof(url->path)) return false;
            url->path[path_length++] = *cursor++;
        }
        url->path[path_length] = '\0';
    } else {
        return false;
    }
    url->port = (uint16_t)port;
    return true;
}

static bool build_base_url(const wget_url_t *url, char *buffer,
                           size_t capacity, size_t *length) {
    uint16_t default_port;
    if (url == 0 || buffer == 0 || length == 0 || capacity == 0U) return false;
    default_port = url->ftp ? WGET_FTP_PORT :
                   (url->https ? WGET_HTTPS_PORT : WGET_HTTP_PORT);
    *length = 0U;
    buffer[0] = '\0';
    if (!append_text(buffer, capacity, length,
                     url->ftp ? "ftp://" : (url->https ? "https://" : "http://")) ||
        !append_text(buffer, capacity, length, url->host)) return false;
    if (url->port != default_port) {
        if (!append_char(buffer, capacity, length, ':') ||
            !append_u32_decimal(buffer, capacity, length, url->port)) return false;
    }
    return true;
}

static bool normalize_redirect_path(const char *input, char *output,
                                    size_t capacity) {
    size_t input_length = 0U;
    size_t output_length = 0U;
    if (input == 0 || output == 0 || capacity < 2U || input[0] != '/') return false;
    while (input[input_length] != '\0' && input[input_length] != '#' &&
           input_length + 1U < WGET_PATH_CAP) ++input_length;
    if (input[input_length] != '\0' && input[input_length] != '#' &&
        input_length + 1U >= WGET_PATH_CAP) return false;
    output[output_length++] = '/';
    for (size_t cursor = 1U; cursor <= input_length;) {
        size_t start = cursor;
        while (cursor < input_length && input[cursor] != '/' && input[cursor] != '?') ++cursor;
        size_t segment_length = cursor - start;
        if (segment_length == 0U || (segment_length == 1U && input[start] == '.')) {
            /* no-op */
        } else if (segment_length == 2U && input[start] == '.' && input[start + 1U] == '.') {
            if (output_length > 1U) {
                if (output[output_length - 1U] == '/') --output_length;
                while (output_length > 1U && output[output_length - 1U] != '/') --output_length;
            }
        } else {
            if (output_length > 1U && output[output_length - 1U] != '/' &&
                output_length + 1U >= capacity) return false;
            if (output_length > 1U && output[output_length - 1U] != '/') {
                output[output_length++] = '/';
            }
            if (output_length + segment_length >= capacity) return false;
            for (size_t index = 0U; index < segment_length; ++index) {
                output[output_length++] = input[start + index];
            }
        }
        if (cursor < input_length && input[cursor] == '?') {
            if (output_length + input_length - cursor >= capacity) return false;
            while (cursor < input_length) output[output_length++] = input[cursor++];
            break;
        }
        while (cursor < input_length && input[cursor] == '/') ++cursor;
    }
    if (output_length == 0U || output_length >= capacity) return false;
    output[output_length] = '\0';
    return true;
}

static bool resolve_redirect(const wget_url_t *current, const char *location,
                             char *output, size_t capacity) {
    char joined[WGET_PATH_CAP];
    char normalized[WGET_PATH_CAP];
    size_t length = 0U;
    size_t current_path_length = 0U;
    size_t slash = 0U;
    if (current == 0 || location == 0 || output == 0 || *location == '\0') return false;
    if (text_prefix_ignore_case(location, "http://") ||
        text_prefix_ignore_case(location, "https://") ||
        text_prefix_ignore_case(location, "ftp://")) {
        return append_text_until_fragment(output, capacity, &length, location);
    }
    if (location[0] == '/' && location[1] == '/') {
        length = 0U;
        return append_text(output, capacity, &length,
                           current->ftp ? "ftp:" :
                           (current->https ? "https:" : "http:")) &&
               append_text_until_fragment(output, capacity, &length, location);
    }
    while (current->path[current_path_length] != '\0' &&
           current->path[current_path_length] != '?') {
        if (current->path[current_path_length] == '/') slash = current_path_length;
        ++current_path_length;
    }
    joined[0] = '\0';
    if (location[0] == '/') {
        if (!append_text_until_fragment(joined, sizeof(joined), &length, location)) return false;
    } else if (location[0] == '?') {
        for (size_t index = 0U; index < current_path_length; ++index) {
            if (!append_char(joined, sizeof(joined), &length, current->path[index])) return false;
        }
        if (!append_text_until_fragment(joined, sizeof(joined), &length, location)) return false;
    } else {
        if (current_path_length == 0U || current->path[0] != '/') {
            if (!append_char(joined, sizeof(joined), &length, '/')) return false;
        } else {
            for (size_t index = 0U; index <= slash; ++index) {
                if (!append_char(joined, sizeof(joined), &length, current->path[index])) return false;
            }
        }
        if (!append_text_until_fragment(joined, sizeof(joined), &length, location)) return false;
    }
    if (!normalize_redirect_path(joined, normalized, sizeof(normalized)) ||
        !build_base_url(current, output, capacity, &length) ||
        !append_text(output, capacity, &length, normalized)) return false;
    return true;
}

static bool derive_output_name(const wget_url_t *url, char *output,
                               size_t capacity) {
    size_t path_length = 0U;
    size_t segment = 0U;
    if (url == 0 || output == 0 || capacity == 0U) return false;
    while (url->path[path_length] != '\0' && url->path[path_length] != '?') {
        if (url->path[path_length] == '/') segment = path_length + 1U;
        ++path_length;
    }
    if (segment >= path_length) return copy_text_cap(output, capacity, "index.html");
    size_t length = path_length - segment;
    if (length + 1U > capacity) length = capacity - 1U;
    for (size_t index = 0U; index < length; ++index) output[index] = url->path[segment + index];
    output[length] = '\0';
    return length != 0U;
}

static bool build_output_path(const wget_url_t *url, const wget_options_t *options,
                              const wget_http_header_t *header, char *output,
                              size_t capacity) {
    char filename[WGET_OUTPUT_CAP];
    const char *source = 0;
    size_t length;
    if (url == 0 || options == 0 || output == 0 || capacity == 0U) return false;
    if (options->output != 0) {
        return options->output[0] != '\0' && !text_equal(options->output, "-") &&
               copy_text_cap(output, capacity, options->output);
    }
    if (options->content_disposition && header != 0 && header->have_content_disposition) {
        source = header->content_filename;
    }
    if (source == 0) {
        if (!derive_output_name(url, filename, sizeof(filename))) return false;
        source = filename;
    }
    if (options->directory_prefix == 0 || options->directory_prefix[0] == '\0') {
        return copy_text_cap(output, capacity, source);
    }
    length = text_length(options->directory_prefix);
    if (length + 1U >= capacity || !copy_text_cap(output, capacity,
                                                   options->directory_prefix)) return false;
    if (length != 0U && output[length - 1U] != '/') {
        if (!append_char(output, capacity, &length, '/')) return false;
    }
    return append_text(output, capacity, &length, source);
}

static bool request_append_host(char *request, size_t capacity, size_t *length,
                                const wget_url_t *url) {
    uint16_t default_port;
    if (url == 0) return false;
    default_port = url->ftp ? WGET_FTP_PORT :
                   (url->https ? WGET_HTTPS_PORT : WGET_HTTP_PORT);
    if (!append_text(request, capacity, length, url->host)) return false;
    if (url->port != default_port) {
        if (!append_char(request, capacity, length, ':') ||
            !append_u32_decimal(request, capacity, length, url->port)) return false;
    }
    return true;
}

static bool header_value_safe(const char *value) {
    if (value == 0 || *value == '\0') return false;
    for (const char *cursor = value; *cursor != '\0'; ++cursor) {
        if (*cursor == '\r' || *cursor == '\n') return false;
    }
    return true;
}

static char base64_digit(uint32_t value) {
    if (value < 26U) return (char)('A' + value);
    if (value < 52U) return (char)('a' + value - 26U);
    if (value < 62U) return (char)('0' + value - 52U);
    return value == 62U ? '+' : '/';
}

static bool append_basic_auth(char *request, size_t capacity, size_t *length,
                              const wget_options_t *options) {
    char plain[WGET_AUTH_CAP / 2U];
    char encoded[WGET_AUTH_CAP];
    size_t plain_length = 0U;
    size_t encoded_length = 0U;
    if (options == 0 || options->username == 0) return true;
    if (!header_value_safe(options->username) ||
        (options->password != 0 && !header_value_safe(options->password))) return false;
    if (!copy_text_cap(plain, sizeof(plain), options->username)) return false;
    plain_length = text_length(plain);
    if (
        !append_char(plain, sizeof(plain), &plain_length, ':') ||
        !append_text(plain, sizeof(plain), &plain_length,
                     options->password == 0 ? "" : options->password)) return false;
    for (size_t index = 0U; index < plain_length; index += 3U) {
        uint32_t value = (uint32_t)plain[index] << 16U;
        bool have_second = index + 1U < plain_length;
        bool have_third = index + 2U < plain_length;
        if (have_second) value |= (uint32_t)(uint8_t)plain[index + 1U] << 8U;
        if (have_third) value |= (uint32_t)(uint8_t)plain[index + 2U];
        if (encoded_length + 4U >= sizeof(encoded)) return false;
        encoded[encoded_length++] = base64_digit((value >> 18U) & 0x3FU);
        encoded[encoded_length++] = base64_digit((value >> 12U) & 0x3FU);
        encoded[encoded_length++] = have_second ? base64_digit((value >> 6U) & 0x3FU) : '=';
        encoded[encoded_length++] = have_third ? base64_digit(value & 0x3FU) : '=';
    }
    encoded[encoded_length] = '\0';
    return append_text(request, capacity, length, "Authorization: Basic ") &&
           append_text(request, capacity, length, encoded) &&
           append_text(request, capacity, length, "\r\n");
}

static bool build_http_request(const wget_url_t *url, const wget_options_t *options,
                               uint64_t continue_offset, char *request,
                               size_t capacity, size_t *length) {
    const char *method;
    const char *user_agent;
    bool has_body;
    if (url == 0 || options == 0 || request == 0 || length == 0 || capacity == 0U) return false;
    method = options->method[0] == '\0' ?
             (!options->post_data_set ? "GET" : "POST") : options->method;
    has_body = options->post_data_set;
    user_agent = options->user_agent[0] == '\0' ? "wget/1.0" : options->user_agent;
    request[0] = '\0';
    *length = 0U;
    if (!header_value_safe(method) || !header_value_safe(user_agent) ||
        !header_value_safe(url->host) || !header_value_safe(url->path) ||
        options->extra_header_count > WGET_MAX_EXTRA_HEADERS ||
        !append_text(request, capacity, length, method) ||
        !append_char(request, capacity, length, ' ') ||
        !append_text(request, capacity, length, url->path) ||
        !append_text(request, capacity, length, " HTTP/1.1\r\nHost: ") ||
        !request_append_host(request, capacity, length, url) ||
        !append_text(request, capacity, length,
                     "\r\nUser-Agent: ")) return false;
    if (!append_text(request, capacity, length,
                     user_agent) ||
        !append_text(request, capacity, length,
                     "\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n")) {
        return false;
    }
    if (options->referer != 0) {
        if (!header_value_safe(options->referer) ||
            !append_text(request, capacity, length, "Referer: ") ||
            !append_text(request, capacity, length, options->referer) ||
            !append_text(request, capacity, length, "\r\n")) return false;
    }
    if (!append_basic_auth(request, capacity, length, options)) return false;
    if (continue_offset != 0U &&
        (!append_text(request, capacity, length, "Range: bytes=") ||
         !append_u64_decimal(request, capacity, length, continue_offset) ||
         !append_text(request, capacity, length, "-\r\n"))) return false;
    if (has_body &&
        (!append_text(request, capacity, length,
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: ") ||
         !append_u64_decimal(request, capacity, length, text_length(options->post_data)) ||
         !append_text(request, capacity, length, "\r\n"))) return false;
    for (uint32_t index = 0U; index < options->extra_header_count; ++index) {
        if (!header_value_safe(options->extra_headers[index]) ||
            !append_text(request, capacity, length, options->extra_headers[index]) ||
            !append_text(request, capacity, length, "\r\n")) return false;
    }
    if (!append_text(request, capacity, length, "\r\n")) return false;
    return !has_body || append_text(request, capacity, length, options->post_data);
}

static bool header_name_equal(const uint8_t *line, size_t name_length,
                              const char *name) {
    size_t expected = text_length(name);
    if (name_length != expected) return false;
    for (size_t index = 0U; index < name_length; ++index) {
        if (ascii_lower((char)line[index]) != ascii_lower(name[index])) return false;
    }
    return true;
}

static bool value_contains_ignore_case(const uint8_t *value, size_t length,
                                       const char *needle) {
    size_t needle_length = text_length(needle);
    if (needle_length == 0U || needle_length > length) return false;
    for (size_t start = 0U; start + needle_length <= length; ++start) {
        bool match = true;
        for (size_t index = 0U; index < needle_length; ++index) {
            if (ascii_lower((char)value[start + index]) != ascii_lower(needle[index])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

static bool parse_u64_decimal(const uint8_t *value, size_t length, uint64_t *result) {
    uint64_t output = 0U;
    size_t index = 0U;
    bool have_digit = false;
    if (value == 0 || result == 0) return false;
    while (index < length && (value[index] == ' ' || value[index] == '\t')) ++index;
    for (; index < length; ++index) {
        uint8_t digit = value[index];
        if (digit == ' ' || digit == '\t') break;
        if (digit < '0' || digit > '9') return false;
        have_digit = true;
        if (output > (UINT64_MAX - (uint64_t)(digit - '0')) / 10ULL) return false;
        output = output * 10ULL + (uint64_t)(digit - '0');
    }
    if (!have_digit) return false;
    while (index < length && (value[index] == ' ' || value[index] == '\t')) ++index;
    if (index != length) return false;
    *result = output;
    return true;
}

static bool parse_content_range(const uint8_t *value, size_t length,
                                wget_http_header_t *header) {
    size_t index = 0U;
    uint64_t start = 0U;
    uint64_t end = 0U;
    uint64_t total = 0U;
    bool have_start = false;
    bool have_end = false;
    bool have_total = false;
    if (value == 0 || header == 0) return false;
    while (index < length && (value[index] == ' ' || value[index] == '\t')) ++index;
    if (length - index < 6U || ascii_lower((char)value[index]) != 'b' ||
        ascii_lower((char)value[index + 1U]) != 'y' ||
        ascii_lower((char)value[index + 2U]) != 't' ||
        ascii_lower((char)value[index + 3U]) != 'e' ||
        ascii_lower((char)value[index + 4U]) != 's' || value[index + 5U] != ' ') {
        return false;
    }
    index += 6U;
    while (index < length && value[index] >= '0' && value[index] <= '9') {
        have_start = true;
        if (start > (UINT64_MAX - (uint64_t)(value[index] - '0')) / 10ULL) return false;
        start = start * 10ULL + (uint64_t)(value[index++] - '0');
    }
    if (!have_start || index >= length || value[index++] != '-') return false;
    while (index < length && value[index] >= '0' && value[index] <= '9') {
        have_end = true;
        if (end > (UINT64_MAX - (uint64_t)(value[index] - '0')) / 10ULL) return false;
        end = end * 10ULL + (uint64_t)(value[index++] - '0');
    }
    if (!have_end || index >= length || value[index++] != '/') return false;
    if (index < length && value[index] == '*') {
        ++index;
    } else {
        while (index < length && value[index] >= '0' && value[index] <= '9') {
            have_total = true;
            if (total > (UINT64_MAX - (uint64_t)(value[index] - '0')) / 10ULL) return false;
            total = total * 10ULL + (uint64_t)(value[index++] - '0');
        }
        if (!have_total) return false;
    }
    while (index < length && (value[index] == ' ' || value[index] == '\t')) ++index;
    if (index != length || start > end || (have_total && end >= total)) return false;
    header->have_content_range = true;
    header->content_range_start = start;
    header->content_range_end = end;
    header->content_range_total = have_total ? total : UINT64_MAX;
    return true;
}

static bool copy_content_filename(const uint8_t *value, size_t length,
                                  wget_http_header_t *header) {
    size_t index = 0U;
    size_t start;
    size_t end;
    if (value == 0 || header == 0) return false;
    while (index < length && (value[index] == ' ' || value[index] == '\t')) ++index;
    while (index + 9U < length) {
        if (ascii_lower((char)value[index]) == 'f' &&
            ascii_lower((char)value[index + 1U]) == 'i' &&
            ascii_lower((char)value[index + 2U]) == 'l' &&
            ascii_lower((char)value[index + 3U]) == 'e' &&
            ascii_lower((char)value[index + 4U]) == 'n' &&
            ascii_lower((char)value[index + 5U]) == 'a' &&
            ascii_lower((char)value[index + 6U]) == 'm' &&
            ascii_lower((char)value[index + 7U]) == 'e' && value[index + 8U] == '=') {
            start = index + 9U;
            if (start < length && value[start] == '"') {
                ++start;
                end = start;
                while (end < length && value[end] != '"') ++end;
            } else {
                end = start;
                while (end < length && value[end] != ';' && value[end] != ' ' &&
                       value[end] != '\t') ++end;
            }
            if (end <= start || end - start >= sizeof(header->content_filename)) return false;
            size_t output = 0U;
            for (size_t cursor = start; cursor < end; ++cursor) {
                char item = (char)value[cursor];
                if (item == '/' || item == '\\' || item == ':' || item == '\0') continue;
                if (output + 1U >= sizeof(header->content_filename)) return false;
                header->content_filename[output++] = item;
            }
            if (output == 0U) return false;
            header->content_filename[output] = '\0';
            header->have_content_disposition = true;
            return true;
        }
        ++index;
    }
    return false;
}

static bool parse_http_header(const uint8_t *buffer, size_t length,
                              wget_http_header_t *header) {
    size_t end = 0U;
    size_t line_start;
    if (buffer == 0 || header == 0 || length < 4U) return false;
    for (size_t index = 0U; index + 3U < length; ++index) {
        if (buffer[index] == '\r' && buffer[index + 1U] == '\n' &&
            buffer[index + 2U] == '\r' && buffer[index + 3U] == '\n') {
            end = index + 4U;
            break;
        }
    }
    if (end == 0U) return false;
    *header = (wget_http_header_t){0};
    header->header_bytes = end;

    size_t line_end = 0U;
    while (line_end + 1U < end &&
           !(buffer[line_end] == '\r' && buffer[line_end + 1U] == '\n')) ++line_end;
    size_t space = 0U;
    while (space < line_end && buffer[space] != ' ') ++space;
    while (space < line_end && buffer[space] == ' ') ++space;
    if (space + 3U > line_end || buffer[space] < '0' || buffer[space] > '9' ||
        buffer[space + 1U] < '0' || buffer[space + 1U] > '9' ||
        buffer[space + 2U] < '0' || buffer[space + 2U] > '9') return false;
    header->status = (uint32_t)(buffer[space] - '0') * 100U +
                     (uint32_t)(buffer[space + 1U] - '0') * 10U +
                     (uint32_t)(buffer[space + 2U] - '0');

    line_start = line_end + 2U;
    while (line_start + 1U < end) {
        line_end = line_start;
        while (line_end + 1U < end &&
               !(buffer[line_end] == '\r' && buffer[line_end + 1U] == '\n')) ++line_end;
        if (line_end == line_start) break;
        size_t colon = line_start;
        while (colon < line_end && buffer[colon] != ':') ++colon;
        if (colon < line_end) {
            size_t value_start = colon + 1U;
            while (value_start < line_end &&
                   (buffer[value_start] == ' ' || buffer[value_start] == '\t')) ++value_start;
            size_t value_end = line_end;
            while (value_end > value_start &&
                   (buffer[value_end - 1U] == ' ' || buffer[value_end - 1U] == '\t')) --value_end;
            size_t name_length = colon - line_start;
            size_t value_length = value_end - value_start;
            if (header_name_equal(buffer + line_start, name_length, "Content-Length")) {
                uint64_t value = 0U;
                if (!parse_u64_decimal(buffer + value_start, value_length, &value)) return false;
                header->have_content_length = true;
                header->content_length = value;
            } else if (header_name_equal(buffer + line_start, name_length,
                                         "Transfer-Encoding")) {
                if (value_contains_ignore_case(buffer + value_start, value_length,
                                               "chunked")) header->chunked = true;
            } else if (header_name_equal(buffer + line_start, name_length,
                                         "Content-Range")) {
                (void)parse_content_range(buffer + value_start, value_length, header);
            } else if (header_name_equal(buffer + line_start, name_length,
                                         "Content-Disposition")) {
                (void)copy_content_filename(buffer + value_start, value_length, header);
            } else if (header_name_equal(buffer + line_start, name_length, "Location")) {
                if (value_length == 0U || value_length + 1U > sizeof(header->location)) return false;
                byte_copy(header->location, buffer + value_start, value_length);
                header->location[value_length] = '\0';
            }
        }
        line_start = line_end + 2U;
    }
    if (header->chunked) header->have_content_length = false;
    return true;
}

static int64_t stream_fill(wget_stream_t *stream) {
    int64_t received;
    if (stream == 0) return -22;
    received = stream->tls != 0 && stream->tls->active ?
               wget_tls_recv(stream->tls, stream->buffer,
                             sizeof(stream->buffer)) :
               socket_recv_raw(stream->socket, stream->buffer,
                               sizeof(stream->buffer), stream->timeout_ns);
    if (received <= 0) {
        stream->buffer_length = 0U;
        stream->buffer_offset = 0U;
        return received;
    }
    stream->buffer_length = (size_t)received;
    stream->buffer_offset = 0U;
    return received;
}

static int64_t stream_read(wget_stream_t *stream, void *output, size_t capacity) {
    uint8_t *destination = (uint8_t *)output;
    size_t copied = 0U;
    if (stream == 0 || output == 0 || capacity == 0U) return -22;
    while (copied < capacity) {
        if (stream->prefix_offset < stream->prefix_length) {
            size_t available = stream->prefix_length - stream->prefix_offset;
            size_t chunk = capacity - copied < available ? capacity - copied : available;
            byte_copy(destination + copied, stream->prefix + stream->prefix_offset, chunk);
            stream->prefix_offset += chunk;
            copied += chunk;
            continue;
        }
        if (stream->buffer_offset < stream->buffer_length) {
            size_t available = stream->buffer_length - stream->buffer_offset;
            size_t chunk = capacity - copied < available ? capacity - copied : available;
            byte_copy(destination + copied, stream->buffer + stream->buffer_offset, chunk);
            stream->buffer_offset += chunk;
            copied += chunk;
            continue;
        }
        int64_t filled = stream_fill(stream);
        if (filled <= 0) return copied != 0U ? (int64_t)copied : filled;
    }
    return (int64_t)copied;
}

static int64_t stream_read_byte(wget_stream_t *stream, uint8_t *value) {
    return stream_read(stream, value, 1U);
}

static bool stream_read_line(wget_stream_t *stream, char *line, size_t capacity) {
    size_t length = 0U;
    bool saw_cr = false;
    if (stream == 0 || line == 0 || capacity < 2U) return false;
    for (;;) {
        uint8_t value;
        if (stream_read_byte(stream, &value) != 1) return false;
        if (saw_cr) {
            if (value == '\n') {
                line[length] = '\0';
                return true;
            }
            if (length + 1U >= capacity) return false;
            line[length++] = '\r';
            saw_cr = false;
        }
        if (value == '\r') {
            saw_cr = true;
        } else {
            if (length + 1U >= capacity) return false;
            line[length++] = (char)value;
        }
    }
}

static bool parse_chunk_size(const char *line, uint64_t *size) {
    uint64_t value = 0U;
    bool have_digit = false;
    if (line == 0 || size == 0) return false;
    while (*line == ' ' || *line == '\t') ++line;
    while (*line != '\0' && *line != ';' && *line != ' ' && *line != '\t') {
        uint32_t digit;
        if (*line >= '0' && *line <= '9') digit = (uint32_t)(*line - '0');
        else if (*line >= 'a' && *line <= 'f') digit = (uint32_t)(*line - 'a') + 10U;
        else if (*line >= 'A' && *line <= 'F') digit = (uint32_t)(*line - 'A') + 10U;
        else return false;
        if (value > (UINT64_MAX - digit) / 16ULL) return false;
        value = value * 16ULL + digit;
        have_digit = true;
        ++line;
    }
    if (!have_digit) return false;
    *size = value;
    return true;
}

static bool download_content_length(wget_stream_t *stream, os_handle_t file,
                                    uint64_t remaining) {
    uint8_t buffer[WGET_IO_CAP];
    while (remaining != 0U) {
        size_t request = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        int64_t received = stream_read(stream, buffer, request);
        if (received <= 0 || (uint64_t)received > remaining ||
            !write_file_all(file, buffer, (size_t)received)) return false;
        remaining -= (uint64_t)received;
    }
    return true;
}

static bool download_until_eof(wget_stream_t *stream, os_handle_t file) {
    uint8_t buffer[WGET_IO_CAP];
    for (;;) {
        int64_t received = stream_read(stream, buffer, sizeof(buffer));
        if (received == 0) return true;
        if (received < 0 || !write_file_all(file, buffer, (size_t)received)) return false;
    }
}

static bool download_chunked(wget_stream_t *stream, os_handle_t file) {
    uint8_t buffer[WGET_IO_CAP];
    char line[256];
    for (;;) {
        uint64_t chunk_size = 0U;
        if (!stream_read_line(stream, line, sizeof(line)) ||
            !parse_chunk_size(line, &chunk_size)) return false;
        if (chunk_size == 0U) {
            do {
                if (!stream_read_line(stream, line, sizeof(line))) return false;
            } while (line[0] != '\0');
            return true;
        }
        uint64_t remaining = chunk_size;
        while (remaining != 0U) {
            size_t request = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
            int64_t received = stream_read(stream, buffer, request);
            if (received <= 0 || (uint64_t)received > remaining ||
                !write_file_all(file, buffer, (size_t)received)) return false;
            remaining -= (uint64_t)received;
        }
        uint8_t crlf[2];
        if (stream_read(stream, crlf, sizeof(crlf)) != 2 ||
            crlf[0] != '\r' || crlf[1] != '\n') return false;
    }
}

static bool http_status_redirect(uint32_t status) {
    return status == 301U || status == 302U || status == 303U ||
           status == 307U || status == 308U;
}

static bool open_http_socket(uint32_t local_address, uint32_t peer_address,
                             uint16_t peer_port, os_handle_t *socket) {
    uint64_t waited = 0U;
    if (socket == 0) return false;
    *socket = OS_INVALID_HANDLE;
    if (wget_syscall_four(OS_SYS_SOCKET_CREATE, OS_AF_INET4, OS_SOCK_STREAM,
                          0U, (uint64_t)(uintptr_t)socket) < 0 ||
        *socket == OS_INVALID_HANDLE ||
        !bind_local_ipv4(*socket, local_address,
                         peer_address ^ ((uint32_t)peer_port << 16U))) {
        close_handle(socket);
        return false;
    }
    if (wget_syscall_three(OS_SYS_SOCKET_CONNECT, *socket,
                           peer_address, peer_port) < 0) {
        close_handle(socket);
        return false;
    }
    for (;;) {
        os_socket_info_t info = {0};
        os_socket_option_value_t error = {0};
        int64_t status;
        info.hdr.size = sizeof(info);
        info.hdr.version = OS_SYSCALL_ABI_VERSION;
        info.socket = *socket;
        status = wget_syscall_one(OS_SYS_SOCKET_GET_INFO,
                                  (uint64_t)(uintptr_t)&info);
        if (status < 0) break;
        if ((info.flags & OS_SOCKET_INFO_CONNECTED) != 0U) return true;

        error.hdr.size = sizeof(error);
        error.hdr.version = OS_SYSCALL_ABI_VERSION;
        error.socket = *socket;
        error.option = OS_SOCKET_OPTION_ERROR;
        status = wget_syscall_one(OS_SYS_SOCKET_GET_OPTION,
                                  (uint64_t)(uintptr_t)&error);
        if (status < 0 || error.value != 0) break;
        if (waited >= WGET_CONNECT_WAIT_NS ||
            !sleep_ns(WGET_RETRY_SLEEP_NS)) break;
        waited += WGET_RETRY_SLEEP_NS;
    }
    close_handle(socket);
    return false;
}

static void close_http_transport(os_handle_t *socket,
                                 wget_tls_transport_t *tls) {
    if (tls != 0 && tls->active) wget_tls_stop(tls);
    close_handle(socket);
}

static bool receive_http_header(os_handle_t socket, wget_tls_transport_t *tls,
                                uint8_t *buffer,
                                size_t capacity, uint64_t timeout_ns,
                                size_t *received_total,
                                wget_http_header_t *header) {
    size_t total = 0U;
    if (buffer == 0 || received_total == 0 || header == 0) return false;
    while (total < capacity) {
        size_t request = capacity - total;
        if (request > WGET_IO_CAP) request = WGET_IO_CAP;
        int64_t received = tls != 0 && tls->active ?
                           wget_tls_recv(tls, buffer + total, request) :
                           socket_recv_raw(socket, buffer + total, request,
                                           timeout_ns);
        if (received <= 0) return false;
        total += (size_t)received;
        if (parse_http_header(buffer, total, header)) {
            if (header->status >= 100U && header->status < 200U) {
                size_t remaining = total - header->header_bytes;
                for (size_t index = 0U; index < remaining; ++index) {
                    buffer[index] = buffer[header->header_bytes + index];
                }
                total = remaining;
                continue;
            }
            *received_total = total;
            return true;
        }
    }
    return false;
}

static bool download_one_http(const wget_url_t *url, const wget_options_t *options,
                              uint32_t local_address, uint32_t peer_address,
                              char *redirect, size_t redirect_capacity,
                              uint32_t *exit_code) {
    char request[WGET_REQUEST_CAP];
    uint8_t header_buffer[WGET_HEADER_CAP];
    size_t request_length = 0U;
    size_t received_total = 0U;
    wget_http_header_t header;
    os_handle_t socket = OS_INVALID_HANDLE;
    os_handle_t file = OS_INVALID_HANDLE;
    wget_tls_transport_t tls = {0};
    bool success = false;
    char derived_output[WGET_OUTPUT_CAP];
    const char *target_output = derived_output;
    uint64_t resume_offset = 0U;
    bool append_output = false;
    bool target_known;

    if (url == 0 || options == 0 || exit_code == 0) return false;
    byte_zero(derived_output, sizeof(derived_output));
    target_known = build_output_path(url, options, 0, derived_output,
                                     sizeof(derived_output));
    if (target_known && options->continue_download) {
        os_file_info_t info;
        if (stat_file(derived_output, &info) && info.type == OS_FILE_TYPE_REGULAR) {
            resume_offset = info.size;
        }
    }
    if (!build_http_request(url, options, resume_offset, request, sizeof(request),
                            &request_length) ||
        !open_http_socket(local_address, peer_address, url->port, &socket) ||
        (url->https && !wget_tls_start(&tls, socket, url->host,
                                       options->timeout_ns,
                                       options->no_check_certificate)) ||
        (url->https ? !wget_tls_send_all(&tls, request, request_length) :
                      !socket_send_retry(socket, request, request_length, 0U, 0U,
                                         options->timeout_ns)) ||
        !receive_http_header(socket, url->https ? &tls : 0,
                             header_buffer, sizeof(header_buffer),
                             options->timeout_ns, &received_total, &header)) {
        *exit_code = WGET_EXIT_NETWORK;
        close_http_transport(&socket, &tls);
        return false;
    }

    if (http_status_redirect(header.status)) {
        if (header.location[0] == '\0' ||
            !resolve_redirect(url, header.location, redirect, redirect_capacity)) {
            *exit_code = WGET_EXIT_HTTP;
            close_http_transport(&socket, &tls);
            return false;
        }
        close_http_transport(&socket, &tls);
        *exit_code = WGET_EXIT_OK;
        return false;
    }

    if (resume_offset != 0U && header.status == 416U &&
        header.have_content_range && header.content_range_total == resume_offset) {
        close_http_transport(&socket, &tls);
        *exit_code = WGET_EXIT_OK;
        return true;
    }
    if (header.status < 200U || header.status >= 300U) {
        *exit_code = WGET_EXIT_HTTP;
        close_http_transport(&socket, &tls);
        return false;
    }
    if (options->spider) {
        close_http_transport(&socket, &tls);
        *exit_code = WGET_EXIT_OK;
        return true;
    }

    if (!target_known) {
        target_known = build_output_path(url, options, &header, derived_output,
                                         sizeof(derived_output));
    }
    if (!target_known) {
        *exit_code = WGET_EXIT_FILE;
        close_http_transport(&socket, &tls);
        return false;
    }
    target_output = derived_output;
    if (resume_offset != 0U) {
        if (header.status == 206U && (!header.have_content_range ||
                                      header.content_range_start != resume_offset)) {
            *exit_code = WGET_EXIT_HTTP;
            close_http_transport(&socket, &tls);
            return false;
        }
        if (header.status != 206U) {
            resume_offset = 0U;
        } else {
            append_output = true;
        }
    }
    if (options->no_clobber && !append_output) {
        os_file_info_t info;
        if (stat_file(target_output, &info)) {
            *exit_code = WGET_EXIT_FILE;
            close_http_transport(&socket, &tls);
            return false;
        }
    }

    uint32_t file_flags = OS_FILE_OPEN_WRITE | OS_FILE_OPEN_CREATE;
    if (append_output) file_flags |= OS_FILE_OPEN_APPEND;
    else file_flags |= OS_FILE_OPEN_TRUNCATE;
    if (open_file(target_output, file_flags, &file) < 0 ||
        file == OS_INVALID_HANDLE) {
        *exit_code = WGET_EXIT_FILE;
        close_http_transport(&socket, &tls);
        close_handle(&file);
        return false;
    }

    wget_stream_t stream = {
        .socket = socket,
        .tls = url->https ? &tls : 0,
        .timeout_ns = options->timeout_ns,
        .prefix = header_buffer + header.header_bytes,
        .prefix_length = received_total - header.header_bytes,
        .prefix_offset = 0U,
        .buffer_length = 0U,
        .buffer_offset = 0U,
    };

    if (header.chunked) {
        success = download_chunked(&stream, file);
    } else if (header.have_content_length) {
        success = download_content_length(&stream, file, header.content_length);
    } else {
        success = download_until_eof(&stream, file);
    }

    if (success && wget_syscall_one(OS_SYS_FILE_FSYNC, file) < 0) success = false;
    close_handle(&file);
    close_http_transport(&socket, &tls);
    if (!success) {
        if (!options->continue_download) remove_file_best_effort(target_output);
        *exit_code = WGET_EXIT_NETWORK;
        return false;
    }
    *exit_code = WGET_EXIT_OK;
    return true;
}

static bool ftp_read_line(os_handle_t socket, uint64_t timeout_ns,
                          char *line, size_t capacity) {
    size_t length = 0U;
    bool saw_cr = false;
    if (line == 0 || capacity < 2U) return false;
    for (;;) {
        uint8_t value = 0U;
        int64_t received = socket_recv_raw(socket, &value, 1U, timeout_ns);
        if (received != 1) return false;
        if (saw_cr) {
            if (value == '\n') {
                line[length] = '\0';
                return true;
            }
            if (length + 1U >= capacity) return false;
            line[length++] = '\r';
            saw_cr = false;
        }
        if (value == '\r') {
            saw_cr = true;
        } else {
            if (length + 1U >= capacity) return false;
            line[length++] = (char)value;
        }
    }
}

static bool ftp_read_response(os_handle_t socket, uint64_t timeout_ns,
                              uint32_t *code, char *last_line,
                              size_t last_line_capacity) {
    char line[WGET_HEADER_LINE_CAP];
    uint32_t first_code;
    bool multiline;
    if (code == 0 || !ftp_read_line(socket, timeout_ns, line, sizeof(line)) ||
        line[0] < '1' || line[0] > '5' || line[1] < '0' || line[1] > '9' ||
        line[2] < '0' || line[2] > '9') return false;
    first_code = (uint32_t)(line[0] - '0') * 100U +
                 (uint32_t)(line[1] - '0') * 10U + (uint32_t)(line[2] - '0');
    if (last_line != 0 && last_line_capacity != 0U) {
        (void)copy_text_cap(last_line, last_line_capacity, line);
    }
    multiline = line[3] == '-';
    while (multiline) {
        if (!ftp_read_line(socket, timeout_ns, line, sizeof(line))) return false;
        if (last_line != 0 && last_line_capacity != 0U) {
            (void)copy_text_cap(last_line, last_line_capacity, line);
        }
        if (line[0] == (char)('0' + first_code / 100U) &&
            line[1] == (char)('0' + (first_code / 10U) % 10U) &&
            line[2] == (char)('0' + first_code % 10U) && line[3] == ' ') {
            multiline = false;
        }
    }
    *code = first_code;
    return true;
}

static bool ftp_command(os_handle_t socket, const char *command,
                        uint64_t timeout_ns) {
    char packet[WGET_HEADER_LINE_CAP];
    size_t length = 0U;
    if (!header_value_safe(command) ||
        !append_text(packet, sizeof(packet), &length, command) ||
        !append_text(packet, sizeof(packet), &length, "\r\n")) return false;
    return socket_send_retry(socket, packet, length, 0U, 0U, timeout_ns);
}

static bool ftp_parse_pasv(const char *line, uint32_t *address,
                           uint16_t *port) {
    uint32_t values[6] = {0};
    uint32_t value_index = 0U;
    bool have_digit = false;
    bool in_tuple = false;
    if (line == 0 || address == 0 || port == 0) return false;
    for (const char *cursor = line; *cursor != '\0'; ++cursor) {
        if (!in_tuple) {
            if (*cursor == '(') in_tuple = true;
            continue;
        }
        if (*cursor >= '0' && *cursor <= '9') {
            if (value_index >= 6U) return false;
            have_digit = true;
            values[value_index] = values[value_index] * 10U +
                                   (uint32_t)(*cursor - '0');
            if (values[value_index] > 255U) return false;
        } else if (*cursor == ',' && have_digit) {
            ++value_index;
            have_digit = false;
        } else if (*cursor == ')' && have_digit) {
            break;
        }
    }
    if (!in_tuple || !have_digit || value_index != 5U) return false;
    *address = (values[0] << 24U) | (values[1] << 16U) |
               (values[2] << 8U) | values[3];
    *port = (uint16_t)(values[4] * 256U + values[5]);
    return *port != 0U;
}

static bool ftp_response_class(uint32_t code, uint32_t first_digit) {
    return code / 100U == first_digit;
}

static bool download_one_ftp(const wget_url_t *url, const wget_options_t *options,
                             uint32_t local_address, uint32_t peer_address,
                             uint32_t *exit_code) {
    os_handle_t control = OS_INVALID_HANDLE;
    os_handle_t data = OS_INVALID_HANDLE;
    os_handle_t file = OS_INVALID_HANDLE;
    char response[WGET_HEADER_LINE_CAP];
    char command[WGET_REQUEST_CAP];
    char output[WGET_OUTPUT_CAP];
    uint32_t code = 0U;
    uint32_t data_address = 0U;
    uint16_t data_port = 0U;
    uint64_t resume_offset = 0U;
    bool append_output = false;
    bool success = false;
    if (url == 0 || options == 0 || exit_code == 0) return false;
    if (!build_output_path(url, options, 0, output, sizeof(output))) {
        *exit_code = WGET_EXIT_FILE;
        return false;
    }
    if (options->continue_download) {
        os_file_info_t info;
        if (stat_file(output, &info) && info.type == OS_FILE_TYPE_REGULAR) {
            resume_offset = info.size;
        }
    }
    if (options->no_clobber && resume_offset == 0U) {
        os_file_info_t info;
        if (stat_file(output, &info)) {
            *exit_code = WGET_EXIT_FILE;
            return false;
        }
    }
    if (!open_http_socket(local_address, peer_address, url->port, &control) ||
        !ftp_read_response(control, options->timeout_ns, &code,
                           response, sizeof(response)) ||
        !ftp_response_class(code, 2U)) {
        *exit_code = WGET_EXIT_NETWORK;
        close_handle(&control);
        return false;
    }
    const char *username = options->username == 0 ? "anonymous" : options->username;
    const char *password = options->password == 0 ? "anonymous@" : options->password;
    if (!header_value_safe(username) || !header_value_safe(password) ||
        !header_value_safe(url->path)) {
        close_handle(&control);
        *exit_code = WGET_EXIT_NETWORK;
        return false;
    }
    size_t command_length = 0U;
    if (!append_text(command, sizeof(command), &command_length, "USER ") ||
        !append_text(command, sizeof(command), &command_length, username) ||
        !ftp_command(control, command, options->timeout_ns) ||
        !ftp_read_response(control, options->timeout_ns, &code, response, sizeof(response))) {
        *exit_code = WGET_EXIT_NETWORK;
        close_handle(&control);
        return false;
    }
    if (code == 331U) {
        command_length = 0U;
        if (!append_text(command, sizeof(command), &command_length, "PASS ") ||
            !append_text(command, sizeof(command), &command_length, password) ||
            !ftp_command(control, command, options->timeout_ns) ||
            !ftp_read_response(control, options->timeout_ns, &code, response, sizeof(response))) {
            *exit_code = WGET_EXIT_NETWORK;
            close_handle(&control);
            return false;
        }
    }
    if (!ftp_response_class(code, 2U) ||
        !ftp_command(control, "TYPE I", options->timeout_ns) ||
        !ftp_read_response(control, options->timeout_ns, &code, response, sizeof(response)) ||
        !ftp_response_class(code, 2U)) {
        *exit_code = WGET_EXIT_NETWORK;
        close_handle(&control);
        return false;
    }
    if (resume_offset != 0U) {
        command_length = 0U;
        if (!append_text(command, sizeof(command), &command_length, "REST ") ||
            !append_u64_decimal(command, sizeof(command), &command_length, resume_offset) ||
            !ftp_command(control, command, options->timeout_ns) ||
            !ftp_read_response(control, options->timeout_ns, &code, response, sizeof(response)) ||
            code != 350U) {
            close_handle(&control);
            *exit_code = WGET_EXIT_HTTP;
            return false;
        }
        append_output = true;
    }
    if (!ftp_command(control, "PASV", options->timeout_ns) ||
        !ftp_read_response(control, options->timeout_ns, &code, response, sizeof(response)) ||
        code != 227U || !ftp_parse_pasv(response, &data_address, &data_port) ||
        !open_http_socket(local_address, data_address, data_port, &data)) {
        close_handle(&control);
        close_handle(&data);
        *exit_code = WGET_EXIT_NETWORK;
        return false;
    }
    command_length = 0U;
    if (!append_text(command, sizeof(command), &command_length, "RETR ") ||
        !append_text(command, sizeof(command), &command_length, url->path) ||
        !ftp_command(control, command, options->timeout_ns) ||
        !ftp_read_response(control, options->timeout_ns, &code, response, sizeof(response)) ||
        (code != 125U && code != 150U)) {
        close_handle(&data);
        close_handle(&control);
        *exit_code = WGET_EXIT_NETWORK;
        return false;
    }
    if (options->spider) {
        close_handle(&data);
        close_handle(&control);
        *exit_code = WGET_EXIT_OK;
        return true;
    }
    uint32_t file_flags = OS_FILE_OPEN_WRITE | OS_FILE_OPEN_CREATE;
    if (append_output) file_flags |= OS_FILE_OPEN_APPEND;
    else file_flags |= OS_FILE_OPEN_TRUNCATE;
    if (open_file(output, file_flags, &file) < 0 || file == OS_INVALID_HANDLE) {
        close_handle(&data);
        close_handle(&control);
        *exit_code = WGET_EXIT_FILE;
        return false;
    }
    wget_stream_t stream = {
        .socket = data,
        .tls = 0,
        .timeout_ns = options->timeout_ns,
        .prefix = 0,
        .prefix_length = 0U,
        .prefix_offset = 0U,
        .buffer_length = 0U,
        .buffer_offset = 0U,
    };
    success = download_until_eof(&stream, file);
    if (success && wget_syscall_one(OS_SYS_FILE_FSYNC, file) < 0) success = false;
    close_handle(&file);
    close_handle(&data);
    (void)ftp_read_response(control, options->timeout_ns, &code, response, sizeof(response));
    (void)ftp_command(control, "QUIT", options->timeout_ns);
    close_handle(&control);
    if (!success) {
        if (!options->continue_download) remove_file_best_effort(output);
        *exit_code = WGET_EXIT_NETWORK;
        return false;
    }
    *exit_code = WGET_EXIT_OK;
    return true;
}

static const char *long_option_value(const char *argument, const char *name) {
    size_t length;
    if (argument == 0 || name == 0) return 0;
    length = text_length(name);
    if (!text_prefix_ignore_case(argument, name) || argument[length] != '=') return 0;
    return argument + length + 1U;
}

static bool parse_timeout_seconds(const char *text, uint64_t *timeout_ns) {
    uint32_t seconds;
    if (!parse_u32_text(text, &seconds) || seconds == 0U || timeout_ns == 0) return false;
    *timeout_ns = (uint64_t)seconds * 1000000000ULL;
    return true;
}

static bool parse_cli(uint64_t argc, char **argv, const char **url,
                      wget_options_t *options) {
    if (argv == 0 || url == 0 || options == 0 || argc < 2U) return false;
    *url = 0;
    *options = (wget_options_t){0};
    options->tries = 3U;
    options->timeout_ns = WGET_RECV_TIMEOUT_NS;
    for (uint64_t index = 1U; index < argc; ++index) {
        const char *argument = argv[index];
        const char *value = 0;
        if (text_equal(argument, "--")) {
            if (++index >= argc || *url != 0) return false;
            *url = argv[index];
        } else if (text_equal(argument, "-c") || text_equal(argument, "--continue")) {
            options->continue_download = true;
        } else if (text_equal(argument, "-nc") || text_equal(argument, "--no-clobber")) {
            options->no_clobber = true;
        } else if (text_equal(argument, "--content-disposition")) {
            options->content_disposition = true;
        } else if (text_equal(argument, "-h") || text_equal(argument, "--help")) {
            options->help = true;
        } else if (text_equal(argument, "--version")) {
            options->version = true;
        } else if (text_equal(argument, "--spider")) {
            options->spider = true;
        } else if (text_equal(argument, "-q") || text_equal(argument, "--quiet")) {
            options->quiet = true;
        } else if (text_equal(argument, "-S") || text_equal(argument, "--server-response")) {
            options->server_response = true;
        } else if (text_equal(argument, "-O") || text_equal(argument, "--output-document")) {
            if (++index >= argc || options->output != 0) return false;
            options->output = argv[index];
        } else if (text_equal(argument, "-P") || text_equal(argument, "--directory-prefix")) {
            if (++index >= argc || options->directory_prefix != 0) return false;
            options->directory_prefix = argv[index];
        } else if (text_equal(argument, "-t") || text_equal(argument, "--tries")) {
            if (++index >= argc || !parse_u32_text(argv[index], &options->tries) ||
                options->tries == 0U) return false;
        } else if (text_equal(argument, "--timeout")) {
            if (++index >= argc || !parse_timeout_seconds(argv[index], &options->timeout_ns)) return false;
        } else if (text_equal(argument, "--dns")) {
            if (++index >= argc || options->dns_override != 0U ||
                !parse_ipv4(argv[index], &options->dns_override)) return false;
        } else if (text_equal(argument, "-U") || text_equal(argument, "--user-agent")) {
            if (++index >= argc || !copy_text_cap(options->user_agent,
                                                  sizeof(options->user_agent), argv[index])) return false;
        } else if (text_equal(argument, "-H") || text_equal(argument, "--header")) {
            if (++index >= argc || options->extra_header_count >= WGET_MAX_EXTRA_HEADERS ||
                !header_value_safe(argv[index])) return false;
            options->extra_headers[options->extra_header_count++] = argv[index];
        } else if (text_equal(argument, "--referer")) {
            if (++index >= argc || !header_value_safe(argv[index])) return false;
            options->referer = argv[index];
        } else if (text_equal(argument, "--user")) {
            if (++index >= argc || !header_value_safe(argv[index])) return false;
            options->username = argv[index];
        } else if (text_equal(argument, "--password")) {
            if (++index >= argc || !header_value_safe(argv[index])) return false;
            options->password = argv[index];
        } else if (text_equal(argument, "--post-data")) {
            if (++index >= argc || !copy_text_cap(options->post_data,
                                                  sizeof(options->post_data), argv[index])) return false;
            options->post_data_set = true;
        } else if (text_equal(argument, "--method")) {
            if (++index >= argc || !copy_text_cap(options->method,
                                                  sizeof(options->method), argv[index])) return false;
        } else if (text_equal(argument, "--no-check-certificate")) {
            options->no_check_certificate = true;
        } else if ((value = long_option_value(argument, "--output-document")) != 0) {
            if (*value == '\0' || options->output != 0) return false;
            options->output = value;
        } else if ((value = long_option_value(argument, "--directory-prefix")) != 0) {
            if (*value == '\0' || options->directory_prefix != 0) return false;
            options->directory_prefix = value;
        } else if ((value = long_option_value(argument, "--tries")) != 0) {
            if (!parse_u32_text(value, &options->tries) || options->tries == 0U) return false;
        } else if ((value = long_option_value(argument, "--timeout")) != 0) {
            if (!parse_timeout_seconds(value, &options->timeout_ns)) return false;
        } else if ((value = long_option_value(argument, "--dns")) != 0) {
            if (options->dns_override != 0U || !parse_ipv4(value, &options->dns_override)) return false;
        } else if ((value = long_option_value(argument, "--user-agent")) != 0) {
            if (!copy_text_cap(options->user_agent, sizeof(options->user_agent), value)) return false;
        } else if ((value = long_option_value(argument, "--header")) != 0) {
            if (options->extra_header_count >= WGET_MAX_EXTRA_HEADERS ||
                !header_value_safe(value)) return false;
            options->extra_headers[options->extra_header_count++] = value;
        } else if ((value = long_option_value(argument, "--referer")) != 0) {
            if (!header_value_safe(value)) return false;
            options->referer = value;
        } else if ((value = long_option_value(argument, "--user")) != 0) {
            if (!header_value_safe(value)) return false;
            options->username = value;
        } else if ((value = long_option_value(argument, "--password")) != 0) {
            if (!header_value_safe(value)) return false;
            options->password = value;
        } else if ((value = long_option_value(argument, "--post-data")) != 0) {
            if (!copy_text_cap(options->post_data, sizeof(options->post_data), value)) return false;
            options->post_data_set = true;
        } else if ((value = long_option_value(argument, "--method")) != 0) {
            if (!copy_text_cap(options->method, sizeof(options->method), value)) return false;
        } else if (argument[0] == '-' && argument[1] != '\0') {
            /* Accept common attached short forms: -Ofile, -Pdir, -t3, -Uagent. */
            if (argument[1] == 'O' && argument[2] != '\0' && options->output == 0) {
                options->output = argument + 2U;
            } else if (argument[1] == 'P' && argument[2] != '\0' &&
                       options->directory_prefix == 0) {
                options->directory_prefix = argument + 2U;
            } else if (argument[1] == 't' && argument[2] != '\0' &&
                       parse_u32_text(argument + 2U, &options->tries) && options->tries != 0U) {
                /* parsed */
            } else if (argument[1] == 'U' && argument[2] != '\0' &&
                       copy_text_cap(options->user_agent, sizeof(options->user_agent), argument + 2U)) {
                /* parsed */
            } else {
                return false;
            }
        } else if (*url == 0) {
            *url = argument;
        } else {
            return false;
        }
    }
    return (*url != 0) || options->help || options->version;
}

int64_t wget_main(uint64_t argc, char **argv) {
    const char *input_url = 0;
    wget_options_t options;
    uint32_t dns_server = 0U;
    os_net_status_t network;
    char current_url[WGET_URL_CAP];
    char redirect_url[WGET_URL_CAP];

    if (!parse_cli(argc, argv, &input_url, &options)) {
        return WGET_EXIT_USAGE;
    }
    if (options.help || options.version) return WGET_EXIT_OK;
    if (!copy_text_cap(current_url, sizeof(current_url), input_url)) return WGET_EXIT_USAGE;
    if (!wait_for_ipv4(&network)) return WGET_EXIT_NETWORK;
    if (options.dns_override != 0U) dns_server = options.dns_override;
    else (void)parse_resolver_file(&dns_server);

    for (uint32_t redirect_count = 0U;
         redirect_count <= WGET_REDIRECT_LIMIT;
         ++redirect_count) {
        wget_url_t url;
        uint32_t peer_address = 0U;
        uint32_t exit_code = WGET_EXIT_HTTP;
        redirect_url[0] = '\0';

        if (!parse_http_url(current_url, &url)) {
            return WGET_EXIT_USAGE;
        }
        if (!dns_resolve(url.host, network.ipv4_address,
                         dns_server, options.timeout_ns, &peer_address)) return WGET_EXIT_NETWORK;

        for (uint32_t attempt = 0U; attempt < options.tries; ++attempt) {
            bool downloaded = url.ftp ?
                download_one_ftp(&url, &options, network.ipv4_address,
                                 peer_address, &exit_code) :
                download_one_http(&url, &options, network.ipv4_address,
                                  peer_address, redirect_url,
                                  sizeof(redirect_url), &exit_code);
            if (downloaded) {
                return WGET_EXIT_OK;
            }
            if (redirect_url[0] != '\0' || exit_code != WGET_EXIT_NETWORK ||
                attempt + 1U >= options.tries) break;
            if (!sleep_ns(WGET_RETRY_SLEEP_NS * 10ULL)) break;
        }
        if (redirect_url[0] == '\0') return exit_code;
        if (redirect_count == WGET_REDIRECT_LIMIT) return WGET_EXIT_HTTP;
        if (!copy_text_cap(current_url, sizeof(current_url), redirect_url)) {
            return WGET_EXIT_HTTP;
        }
    }
    return WGET_EXIT_HTTP;
}
