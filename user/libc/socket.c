#include "libc/libc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <uapi/socket.h>
#include <uapi/linux_abi.h>
#include <netinet/tcp.h>

static int socket_error(int error_number) {
    errno = error_number;
    return -1;
}

static ssize_t socket_count_result(int64_t status) {
    if (status < 0) {
        (void)__libc_status_result(status);
        return -1;
    }
    return (ssize_t)status;
}

static bool libc_linux_personality(void) {
    return __libc_linux_personality();
}

static int socket_handle(int descriptor, os_handle_t *handle,
                         uint32_t *family) {
    if (handle == 0) return socket_error(EINVAL);
    *handle = __libc_handle_for_descriptor(descriptor);
    if (*handle == OS_INVALID_HANDLE) return -1;
    if (!__libc_descriptor_is_socket(descriptor)) return socket_error(ENOTSOCK);
    if (family != 0) *family = __libc_socket_family(descriptor);
    return 0;
}

static bool parse_ipv4(const char *text, uint32_t *address) {
    uint32_t result = 0U;
    const char *cursor = text;
    if (text == 0 || address == 0 || *text == '\0') return false;
    for (unsigned int part = 0U; part < 4U; ++part) {
        uint32_t value = 0U;
        unsigned int digits = 0U;
        while (*cursor >= '0' && *cursor <= '9') {
            unsigned int digit = (unsigned int)(*cursor - '0');
            if (value > (255U - digit) / 10U) return false;
            value = value * 10U + digit;
            ++digits;
            ++cursor;
        }
        if (digits == 0U) return false;
        result = (result << 8U) | value;
        if (part != 3U) {
            if (*cursor != '.') return false;
            ++cursor;
        }
    }
    if (*cursor != '\0') return false;
    *address = result;
    return true;
}

static unsigned int hex_digit(int value) {
    if (value >= '0' && value <= '9') return (unsigned int)(value - '0');
    if (value >= 'a' && value <= 'f') return (unsigned int)(value - 'a' + 10);
    if (value >= 'A' && value <= 'F') return (unsigned int)(value - 'A' + 10);
    return 16U;
}

static bool parse_ipv6(const char *text, uint8_t output[16]) {
    uint16_t words[8] = {0};
    const char *cursor = text;
    int compress = -1;
    unsigned int count = 0U;
    if (text == 0 || output == 0 || *text == '\0') return false;

    while (*cursor != '\0') {
        uint16_t value = 0U;
        unsigned int digits = 0U;
        if (*cursor == ':') {
            if (cursor[1] != ':' || compress >= 0) return false;
            compress = (int)count;
            cursor += 2;
            if (*cursor == '\0') break;
        }
        if (count >= 8U) return false;
        const char *group = cursor;
        while (hex_digit((unsigned char)*cursor) < 16U) {
            if (digits == 4U) return false;
            value = (uint16_t)((value << 4U) |
                               hex_digit((unsigned char)*cursor));
            ++digits;
            ++cursor;
        }
        if (*cursor == '.') {
            uint32_t ipv4;
            if (count > 6U || !parse_ipv4(group, &ipv4)) return false;
            words[count++] = (uint16_t)(ipv4 >> 16U);
            words[count++] = (uint16_t)ipv4;
            cursor += strlen(cursor);
            break;
        }
        if (digits == 0U) return false;
        words[count++] = value;
        if (*cursor == ':') {
            ++cursor;
            if (*cursor == ':') {
                if (compress >= 0) return false;
                compress = (int)count;
                ++cursor;
                if (*cursor == '\0') break;
            } else if (*cursor == '\0') {
                return false;
            }
        } else if (*cursor != '\0') {
            return false;
        }
    }
    if (compress >= 0) {
        unsigned int missing;
        if (count >= 8U) return false;
        missing = 8U - count;
        for (unsigned int index = count; index > (unsigned int)compress; --index) {
            words[index + missing - 1U] = words[index - 1U];
        }
        for (unsigned int index = 0U; index < missing; ++index) {
            words[(unsigned int)compress + index] = 0U;
        }
        count = 8U;
    }
    if (count != 8U) return false;
    for (unsigned int index = 0U; index < 8U; ++index) {
        output[index * 2U] = (uint8_t)(words[index] >> 8U);
        output[index * 2U + 1U] = (uint8_t)words[index];
    }
    return true;
}

const struct in6_addr in6addr_any = {{{0}}};
const struct in6_addr in6addr_loopback =
    {{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}}};

uint16_t htons(uint16_t value) {
    return (uint16_t)((value << 8U) | (value >> 8U));
}

uint16_t ntohs(uint16_t value) {
    return htons(value);
}

uint32_t htonl(uint32_t value) {
    return ((value & 0x000000FFU) << 24U) |
           ((value & 0x0000FF00U) << 8U) |
           ((value & 0x00FF0000U) >> 8U) |
           ((value & 0xFF000000U) >> 24U);
}

uint32_t ntohl(uint32_t value) {
    return htonl(value);
}

int inet_aton(const char *text, struct in_addr *address) {
    uint32_t host_address;
    if (address == 0 || !parse_ipv4(text, &host_address)) return 0;
    address->s_addr = htonl(host_address);
    return 1;
}

in_addr_t inet_addr(const char *text) {
    struct in_addr address;
    return inet_aton(text, &address) ? address.s_addr : INADDR_NONE;
}

int inet_pton(int family, const char *text, void *address) {
    if (address == 0) {
        errno = EINVAL;
        return 0;
    }
    if (family == AF_INET) {
        return inet_aton(text, (struct in_addr *)address) ? 1 : 0;
    }
    if (family == AF_INET6) {
        return parse_ipv6(text, ((struct in6_addr *)address)->s6_addr) ? 1 : 0;
    }
    errno = EAFNOSUPPORT;
    return -1;
}

const char *inet_ntop(int family, const void *address, char *buffer,
                      socklen_t capacity) {
    int written;
    if (address == 0 || buffer == 0 || capacity == 0U) {
        errno = EINVAL;
        return 0;
    }
    if (family == AF_INET) {
        uint32_t value = ntohl(((const struct in_addr *)address)->s_addr);
        written = snprintf(buffer, capacity, "%u.%u.%u.%u",
                           (unsigned int)((value >> 24U) & 0xFFU),
                           (unsigned int)((value >> 16U) & 0xFFU),
                           (unsigned int)((value >> 8U) & 0xFFU),
                           (unsigned int)(value & 0xFFU));
    } else if (family == AF_INET6) {
        const uint8_t *bytes = ((const struct in6_addr *)address)->s6_addr;
        uint16_t words[8];
        unsigned int best_start = 8U;
        unsigned int best_length = 0U;
        unsigned int run_start = 0U;
        unsigned int run_length = 0U;
        size_t offset = 0U;

        for (unsigned int index = 0U; index < 8U; ++index) {
            words[index] = (uint16_t)(((uint16_t)bytes[index * 2U] << 8U) |
                                      bytes[index * 2U + 1U]);
            if (words[index] == 0U) {
                if (run_length == 0U) run_start = index;
                ++run_length;
            } else {
                if (run_length > best_length && run_length >= 2U) {
                    best_start = run_start;
                    best_length = run_length;
                }
                run_length = 0U;
            }
        }
        if (run_length > best_length && run_length >= 2U) {
            best_start = run_start;
            best_length = run_length;
        }

        /* 在调用方缓冲区中构造，并为结尾 NUL 留出一个字节。
         * IPv6 地址文本最大长度为 INET6_ADDRSTRLEN，因此结果也不依赖格式化 I/O 实现。 */
        for (unsigned int index = 0U; index < 8U;) {
            if (index == best_start) {
                if (offset + 2U >= (size_t)capacity) {
                    errno = ENOSPC;
                    return 0;
                }
                buffer[offset++] = ':';
                buffer[offset++] = ':';
                index += best_length;
                continue;
            }
            if (offset != 0U && buffer[offset - 1U] != ':') {
                if (offset + 1U >= capacity) {
                    errno = ENOSPC;
                    return 0;
                }
                buffer[offset++] = ':';
            }
            {
                uint16_t value = words[index];
                char digits[4];
                unsigned int digit_count = 0U;
                do {
                    unsigned int digit = value & 0xFU;
                    digits[digit_count++] =
                        (char)(digit < 10U ? '0' + digit : 'a' + digit - 10U);
                    value >>= 4U;
                } while (value != 0U);
                while (digit_count != 0U) {
                    if (offset + 1U >= capacity) {
                        errno = ENOSPC;
                        return 0;
                    }
                    buffer[offset++] = digits[--digit_count];
                }
            }
            ++index;
        }
        if (offset >= capacity) {
            errno = ENOSPC;
            return 0;
        }
        buffer[offset] = '\0';
        return buffer;
    } else {
        errno = EAFNOSUPPORT;
        return 0;
    }
    if (written < 0 || (unsigned int)written >= capacity) {
        errno = ENOSPC;
        return 0;
    }
    return buffer;
}

char *inet_ntoa(struct in_addr address) {
    static char buffer[INET_ADDRSTRLEN];
    return (char *)inet_ntop(AF_INET, &address, buffer, sizeof(buffer));
}

static int decode_ipv4_address(const struct sockaddr *address,
                               socklen_t length, uint32_t *host_address,
                               uint16_t *host_port) {
    const struct sockaddr_in *ipv4;
    if (address == 0 || length < sizeof(struct sockaddr_in) ||
        address->sa_family != AF_INET || host_address == 0 || host_port == 0) {
        return socket_error(EAFNOSUPPORT);
    }
    ipv4 = (const struct sockaddr_in *)address;
    *host_address = ntohl(ipv4->sin_addr.s_addr);
    *host_port = ntohs(ipv4->sin_port);
    return 0;
}

static int decode_ipv6_address(const struct sockaddr *address,
                               socklen_t length, uint8_t output[16],
                               uint16_t *host_port) {
    const struct sockaddr_in6 *ipv6;
    if (address == 0 || length < sizeof(struct sockaddr_in6) ||
        address->sa_family != AF_INET6 || output == 0 || host_port == 0) {
        return socket_error(EAFNOSUPPORT);
    }
    ipv6 = (const struct sockaddr_in6 *)address;
    memcpy(output, ipv6->sin6_addr.s6_addr, 16U);
    *host_port = ntohs(ipv6->sin6_port);
    return 0;
}

static int apply_socket_descriptor_flags(int descriptor, int flags) {
    int current;
    if ((flags & SOCK_NONBLOCK) != 0) {
        current = fcntl(descriptor, F_GETFL);
        if (current < 0 || fcntl(descriptor, F_SETFL,
                                 current | O_NONBLOCK) < 0) return -1;
    }
    if ((flags & SOCK_CLOEXEC) != 0) {
        current = fcntl(descriptor, F_GETFD);
        if (current < 0 || fcntl(descriptor, F_SETFD,
                                 current | FD_CLOEXEC) < 0) return -1;
    }
    return 0;
}

int socket(int domain, int type, int protocol) {
    os_handle_t handle = OS_INVALID_HANDLE;
    int base_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    int descriptor;
    int64_t status;
    if (libc_linux_personality() &&
        (domain == AF_UNIX || domain == AF_INET || domain == AF_INET6)) {
        uint64_t linux_type = (uint64_t)base_type;
        int open_flags = O_RDWR;
        if ((type & SOCK_NONBLOCK) != 0) {
            linux_type |= LINUX_SOCK_NONBLOCK;
            open_flags |= O_NONBLOCK;
        }
        if ((type & SOCK_CLOEXEC) != 0) {
            linux_type |= LINUX_SOCK_CLOEXEC;
            open_flags |= O_CLOEXEC;
        }
        status = __libc_linux_syscall6(
            LINUX_SYS_SOCKET, (uint64_t)(uint32_t)domain, linux_type,
            (uint64_t)(uint32_t)protocol, 0U, 0U, 0U);
        if (status < 0) return __libc_status_result(status);
        descriptor = __libc_adopt_linux_fd((int)status, open_flags);
        if (descriptor < 0) return -1;
        if (__libc_mark_linux_socket_descriptor(
                descriptor, (uint32_t)domain) < 0) {
            int saved_errno = errno;
            (void)close(descriptor);
            errno = saved_errno;
            return -1;
        }
        return descriptor;
    }
    if (domain != AF_INET && domain != AF_INET6) return socket_error(EAFNOSUPPORT);
    if (base_type != SOCK_STREAM && base_type != SOCK_DGRAM) {
        return socket_error(ESOCKTNOSUPPORT);
    }
    status = __libc_native_syscall6(OS_SYS_SOCKET_CREATE, (uint64_t)domain,
                             (uint64_t)base_type, (uint64_t)protocol,
                             (uint64_t)(uintptr_t)&handle,
                             (type & SOCK_CLOEXEC) != 0 ?
                             OS_SOCKET_CREATE_FLAG_CLOEXEC : 0U, 0U);
    if (status < 0) {
        (void)__libc_status_result(status);
        return -1;
    }
    descriptor = __libc_install_handle(handle, O_RDWR, true,
                                       (uint32_t)domain);
    if (descriptor < 0) return -1;
    if (apply_socket_descriptor_flags(descriptor,
                                      type & (SOCK_NONBLOCK | SOCK_CLOEXEC)) < 0) {
        int saved_errno = errno;
        (void)close(descriptor);
        errno = saved_errno;
        return -1;
    }
    return descriptor;
}

int bind(int descriptor, const struct sockaddr *address, socklen_t length) {
    os_handle_t handle;
    uint32_t family;
    uint32_t host_address;
    uint16_t host_port;
    uint8_t address6[16];
    int64_t status;
    if (libc_linux_personality() &&
        __libc_descriptor_is_linux_fd(descriptor)) {
        int linux_descriptor = __libc_linux_fd_for_descriptor(descriptor);
        if (linux_descriptor < 0) return -1;
        status = __libc_linux_syscall6(
            LINUX_SYS_BIND, (uint64_t)(uint32_t)linux_descriptor,
            (uint64_t)(uintptr_t)address, (uint64_t)length, 0U, 0U, 0U);
        return __libc_status_result(status);
    }
    if (socket_handle(descriptor, &handle, &family) < 0) return -1;
    if (family == AF_INET) {
        if (decode_ipv4_address(address, length, &host_address, &host_port) < 0) return -1;
        status = __libc_native_syscall6(OS_SYS_SOCKET_BIND, handle, host_address,
                                 host_port, 0U, 0U, 0U);
    } else {
        if (decode_ipv6_address(address, length, address6, &host_port) < 0) return -1;
        status = __libc_native_syscall6(OS_SYS_SOCKET_BIND6, handle,
                                 (uint64_t)(uintptr_t)address6, host_port,
                                 0U, 0U, 0U);
    }
    return __libc_status_result(status);
}

int connect(int descriptor, const struct sockaddr *address, socklen_t length) {
    os_handle_t handle;
    uint32_t family;
    uint32_t host_address;
    uint16_t host_port;
    uint8_t address6[16];
    int64_t status;
    if (libc_linux_personality() &&
        __libc_descriptor_is_linux_fd(descriptor)) {
        int linux_descriptor = __libc_linux_fd_for_descriptor(descriptor);
        if (linux_descriptor < 0) return -1;
        status = __libc_linux_syscall6(
            LINUX_SYS_CONNECT, (uint64_t)(uint32_t)linux_descriptor,
            (uint64_t)(uintptr_t)address, (uint64_t)length, 0U, 0U, 0U);
        return __libc_status_result(status);
    }
    if (socket_handle(descriptor, &handle, &family) < 0) return -1;
    if (family == AF_INET) {
        if (decode_ipv4_address(address, length, &host_address, &host_port) < 0) return -1;
        status = __libc_native_syscall6(OS_SYS_SOCKET_CONNECT, handle, host_address,
                                 host_port, 0U, 0U, 0U);
    } else {
        if (decode_ipv6_address(address, length, address6, &host_port) < 0) return -1;
        status = __libc_native_syscall6(OS_SYS_SOCKET_CONNECT6, handle,
                                 (uint64_t)(uintptr_t)address6, host_port,
                                 0U, 0U, 0U);
    }
    return __libc_status_result(status);
}

int listen(int descriptor, int backlog) {
    os_handle_t handle;
    if (backlog <= 0) return socket_error(EINVAL);
    if (libc_linux_personality() &&
        __libc_descriptor_is_linux_fd(descriptor)) {
        int linux_descriptor = __libc_linux_fd_for_descriptor(descriptor);
        if (linux_descriptor < 0) return -1;
        return __libc_status_result(__libc_linux_syscall6(
            LINUX_SYS_LISTEN, (uint64_t)(uint32_t)linux_descriptor,
            (uint64_t)(uint32_t)backlog, 0U, 0U, 0U, 0U));
    }
    if (socket_handle(descriptor, &handle, 0) < 0) return -1;
    return __libc_status_result(__libc_native_syscall6(OS_SYS_SOCKET_LISTEN, handle,
                                                 (uint64_t)(unsigned int)backlog,
                                                 0U, 0U, 0U, 0U));
}

static int accept_with_flags(int descriptor, struct sockaddr *address,
                             socklen_t *length, int flags) {
    os_handle_t handle;
    os_handle_t accepted = OS_INVALID_HANDLE;
    int64_t status;
    if ((address == 0) != (length == 0)) return socket_error(EINVAL);
    if (libc_linux_personality() &&
        __libc_descriptor_is_linux_fd(descriptor)) {
        int linux_descriptor = __libc_linux_fd_for_descriptor(descriptor);
        uint32_t linux_flags = 0U;
        int accepted_descriptor;
        if (linux_descriptor < 0) return -1;
        if ((flags & SOCK_NONBLOCK) != 0) linux_flags |= LINUX_SOCK_NONBLOCK;
        if ((flags & SOCK_CLOEXEC) != 0) linux_flags |= LINUX_SOCK_CLOEXEC;
        status = __libc_linux_syscall6(
            LINUX_SYS_ACCEPT4, (uint64_t)(uint32_t)linux_descriptor,
            (uint64_t)(uintptr_t)address, (uint64_t)(uintptr_t)length,
            linux_flags, 0U, 0U);
        if (status < 0) return __libc_status_result(status);
        accepted_descriptor = __libc_adopt_linux_fd(
            (int)status, O_RDWR |
            ((flags & SOCK_NONBLOCK) != 0 ? O_NONBLOCK : 0) |
            ((flags & SOCK_CLOEXEC) != 0 ? O_CLOEXEC : 0));
        if (accepted_descriptor < 0) return -1;
        if (__libc_mark_linux_socket_descriptor(
                accepted_descriptor, __libc_socket_family(descriptor)) < 0) {
            int saved_errno = errno;
            (void)close(accepted_descriptor);
            errno = saved_errno;
            return -1;
        }
        return accepted_descriptor;
    }
    if (socket_handle(descriptor, &handle, 0) < 0) return -1;
    status = __libc_native_syscall6(OS_SYS_SOCKET_ACCEPT, handle,
                             (__libc_descriptor_open_flags(descriptor) &
                              O_NONBLOCK) != 0U ? 0U : OS_WAIT_INFINITE,
                             (uint64_t)(uintptr_t)&accepted,
                             (flags & SOCK_CLOEXEC) != 0 ?
                             OS_SOCKET_CREATE_FLAG_CLOEXEC : 0U, 0U, 0U);
    if (status < 0) {
        if ((__libc_descriptor_open_flags(descriptor) & O_NONBLOCK) != 0U &&
            status == -(int64_t)ETIMEDOUT) {
            errno = EAGAIN;
        } else {
            (void)__libc_status_result(status);
        }
        return -1;
    }
    int accepted_descriptor = __libc_install_handle(
        accepted, O_RDWR | ((flags & SOCK_CLOEXEC) != 0 ? O_CLOEXEC : 0),
        true, __libc_socket_family(descriptor));
    if (accepted_descriptor >= 0 && address != 0 &&
        getpeername(accepted_descriptor, address, length) < 0) {
        int saved_errno = errno;
        (void)close(accepted_descriptor);
        errno = saved_errno;
        return -1;
    }
    return accepted_descriptor;
}

int accept(int descriptor, struct sockaddr *address, socklen_t *length) {
    return accept_with_flags(descriptor, address, length, 0);
}

int accept4(int descriptor, struct sockaddr *address, socklen_t *length,
            int flags) {
    int accepted;
    if ((flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) != 0) {
        return socket_error(EINVAL);
    }
    accepted = accept_with_flags(descriptor, address, length, flags);
    if (accepted < 0) return -1;
    if (apply_socket_descriptor_flags(accepted, flags) < 0) {
        int saved_errno = errno;
        (void)close(accepted);
        errno = saved_errno;
        return -1;
    }
    return accepted;
}

ssize_t sendto(int descriptor, const void *buffer, size_t length, int flags,
               const struct sockaddr *address, socklen_t address_length) {
    os_handle_t handle;
    uint32_t family;
    uint32_t host_address = 0U;
    uint16_t host_port = 0U;
    uint8_t address6[16] = {0};
    int64_t status;
    if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0) {
        return (ssize_t)socket_error(EOPNOTSUPP);
    }
    if (buffer == 0 && length != 0U) return (ssize_t)socket_error(EINVAL);
    if (libc_linux_personality() &&
        __libc_descriptor_is_linux_fd(descriptor)) {
        int linux_descriptor = __libc_linux_fd_for_descriptor(descriptor);
        if (linux_descriptor < 0) return -1;
        status = __libc_linux_syscall6(
            LINUX_SYS_SENDTO, (uint64_t)(uint32_t)linux_descriptor,
            (uint64_t)(uintptr_t)buffer, (uint64_t)length,
            (uint64_t)(uint32_t)flags, (uint64_t)(uintptr_t)address,
            (uint64_t)address_length);
        return socket_count_result(status);
    }
    if (socket_handle(descriptor, &handle, &family) < 0) return -1;
    if (address != 0) {
        if (family == AF_INET) {
            if (decode_ipv4_address(address, address_length, &host_address,
                                    &host_port) < 0) return -1;
        } else if (decode_ipv6_address(address, address_length, address6,
                                       &host_port) < 0) {
            return -1;
        }
    }
    if (family == AF_INET) {
        status = __libc_native_syscall6(OS_SYS_SOCKET_SEND, handle,
                                 (uint64_t)(uintptr_t)buffer, length,
                                 host_address, host_port, 0U);
    } else {
        status = __libc_native_syscall6(OS_SYS_SOCKET_SEND6, handle,
                                 (uint64_t)(uintptr_t)buffer, length,
                                 (uint64_t)(uintptr_t)address6, host_port, 0U);
    }
    return socket_count_result(status);
}

ssize_t send(int descriptor, const void *buffer, size_t length, int flags) {
    return sendto(descriptor, buffer, length, flags, 0, 0U);
}

static int validate_receive_address(const struct sockaddr *address,
                                    const socklen_t *length, uint32_t family) {
    socklen_t required = family == AF_INET ? sizeof(struct sockaddr_in) :
                         sizeof(struct sockaddr_in6);
    if (address == 0) return 0;
    if (length == 0 || *length < required) return socket_error(EINVAL);
    /* recvfrom() 会写入此地址。调用方可以传入清零的缓冲区；其中原有的 sa_family
     * 不是需要验证的输入参数。 */
    return 0;
}

ssize_t recvfrom(int descriptor, void *buffer, size_t length, int flags,
                 struct sockaddr *address, socklen_t *address_length) {
    os_handle_t handle;
    uint32_t family;
    uint32_t source_address = 0U;
    uint16_t source_port = 0U;
    uint8_t source_address6[16] = {0};
    int64_t status;
    if (buffer == 0 && length != 0U) return (ssize_t)socket_error(EINVAL);
    if (libc_linux_personality() &&
        __libc_descriptor_is_linux_fd(descriptor)) {
        int linux_descriptor = __libc_linux_fd_for_descriptor(descriptor);
        if (linux_descriptor < 0) return -1;
        if ((flags & ~(MSG_DONTWAIT | MSG_PEEK)) != 0)
            return (ssize_t)socket_error(EOPNOTSUPP);
        status = __libc_linux_syscall6(
            LINUX_SYS_RECVFROM, (uint64_t)(uint32_t)linux_descriptor,
            (uint64_t)(uintptr_t)buffer, (uint64_t)length,
            (uint64_t)(uint32_t)flags, (uint64_t)(uintptr_t)address,
            (uint64_t)(uintptr_t)address_length);
        return socket_count_result(status);
    }
    if ((flags & ~MSG_DONTWAIT) != 0)
        return (ssize_t)socket_error(EOPNOTSUPP);
    if (socket_handle(descriptor, &handle, &family) < 0) return -1;
    if (validate_receive_address(address, address_length, family) < 0) return -1;
    uint64_t timeout = ((flags & MSG_DONTWAIT) != 0 ||
                        (__libc_descriptor_open_flags(descriptor) &
                         O_NONBLOCK) != 0U) ? 0U : OS_WAIT_INFINITE;
    if (family == AF_INET) {
        status = __libc_native_syscall6(OS_SYS_SOCKET_RECV, handle,
                                 (uint64_t)(uintptr_t)buffer, length,
                                 (uint64_t)(uintptr_t)&source_address,
                                 (uint64_t)(uintptr_t)&source_port,
                                 timeout);
    } else {
        status = __libc_native_syscall6(OS_SYS_SOCKET_RECV6, handle,
                                 (uint64_t)(uintptr_t)buffer, length,
                                 (uint64_t)(uintptr_t)source_address6,
                                 (uint64_t)(uintptr_t)&source_port,
                                 timeout);
    }
    if (status < 0) {
        if (timeout == 0U && status == -(int64_t)ETIMEDOUT) {
            errno = EAGAIN;
            return -1;
        }
        return socket_count_result(status);
    }
    if (address != 0) {
        if (family == AF_INET) {
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)address;
            memset(ipv4, 0, sizeof(*ipv4));
            ipv4->sin_family = AF_INET;
            ipv4->sin_port = htons(source_port);
            ipv4->sin_addr.s_addr = htonl(source_address);
            *address_length = sizeof(*ipv4);
        } else {
            struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)address;
            memset(ipv6, 0, sizeof(*ipv6));
            ipv6->sin6_family = AF_INET6;
            ipv6->sin6_port = htons(source_port);
            memcpy(ipv6->sin6_addr.s6_addr, source_address6, 16U);
            *address_length = sizeof(*ipv6);
        }
    } else if (address_length != 0) {
        *address_length = 0U;
    }
    return (ssize_t)status;
}

ssize_t recv(int descriptor, void *buffer, size_t length, int flags) {
    return recvfrom(descriptor, buffer, length, flags, 0, 0);
}

static int message_iov_size(const struct msghdr *message, size_t *total) {
    size_t length = 0U;
    size_t index;
    if (message == 0 || total == 0 ||
        (message->msg_iovlen != 0U && message->msg_iov == 0)) {
        errno = EINVAL;
        return -1;
    }
    if (message->msg_iovlen > IOV_MAX) {
        errno = EINVAL;
        return -1;
    }
    for (index = 0U; index < message->msg_iovlen; ++index) {
        const struct iovec *vector = &message->msg_iov[index];
        if (vector->iov_base == 0 && vector->iov_len != 0U) {
            errno = EFAULT;
            return -1;
        }
        if (vector->iov_len > SIZE_MAX - length) {
            errno = EOVERFLOW;
            return -1;
        }
        length += vector->iov_len;
    }
    if (length > (size_t)SSIZE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    *total = length;
    return 0;
}

static void message_copy_to_buffer(unsigned char *buffer,
                                    const struct msghdr *message) {
    size_t offset = 0U;
    size_t index;
    if (buffer == 0) return;
    for (index = 0U; index < message->msg_iovlen; ++index) {
        const struct iovec *vector = &message->msg_iov[index];
        if (vector->iov_len != 0U) {
            memcpy(buffer + offset, vector->iov_base, vector->iov_len);
            offset += vector->iov_len;
        }
    }
}

static void message_copy_from_buffer(const unsigned char *buffer,
                                      size_t length,
                                      const struct msghdr *message) {
    size_t offset = 0U;
    size_t index;
    for (index = 0U; index < message->msg_iovlen && offset < length; ++index) {
        const struct iovec *vector = &message->msg_iov[index];
        size_t amount = vector->iov_len;
        if (amount > length - offset) amount = length - offset;
        if (amount != 0U) {
            memcpy(vector->iov_base, buffer + offset, amount);
            offset += amount;
        }
    }
}

/* Linux x86-64 的 msghdr 布局独立于公开 libc 封装。特别是 msg_namelen 为 32 位字段，
 * 后接显式填充；面向 System 的 struct msghdr 则允许继续演进。 */
typedef struct libc_linux_msghdr {
    uint64_t name;
    uint32_t name_length;
    uint32_t name_padding;
    uint64_t iov_pointer;
    uint64_t iov_length;
    uint64_t control;
    uint64_t control_length;
    uint32_t flags;
    uint32_t flags_padding;
} libc_linux_msghdr_t;

static int linux_translate_send_control(const struct msghdr *message,
                                        unsigned char **copy_output) {
    struct msghdr copy_message;
    size_t offset = 0U;
    if (copy_output == 0 || message == 0) return socket_error(EINVAL);
    *copy_output = 0;
    if (message->msg_controllen == 0U) return 0;
    if (message->msg_control == 0) return socket_error(EINVAL);
    *copy_output = (unsigned char *)malloc(message->msg_controllen);
    if (*copy_output == 0) return -1;
    memcpy(*copy_output, message->msg_control, message->msg_controllen);
    copy_message = *message;
    copy_message.msg_control = *copy_output;
    while (offset < copy_message.msg_controllen) {
        struct cmsghdr *header = (struct cmsghdr *)(uintptr_t)(
            (uintptr_t)copy_message.msg_control + offset);
        size_t remaining = copy_message.msg_controllen - offset;
        size_t aligned_length;
        size_t data_length;
        if (remaining < sizeof(*header) || header->cmsg_len < CMSG_LEN(0) ||
            header->cmsg_len > remaining) {
            free(*copy_output);
            *copy_output = 0;
            return socket_error(EINVAL);
        }
        data_length = header->cmsg_len - CMSG_LEN(0);
        if (header->cmsg_level == SOL_SOCKET &&
            header->cmsg_type == SCM_RIGHTS) {
            if ((data_length % sizeof(int)) != 0U) {
                free(*copy_output);
                *copy_output = 0;
                return socket_error(EINVAL);
            }
            int *descriptors = (int *)CMSG_DATA(header);
            for (size_t index = 0U; index < data_length / sizeof(int);
                 ++index) {
                int linux_descriptor =
                    __libc_linux_fd_for_descriptor(descriptors[index]);
                if (linux_descriptor < 0) {
                    free(*copy_output);
                    *copy_output = 0;
                    return -1;
                }
                descriptors[index] = linux_descriptor;
            }
        }
        aligned_length = CMSG_ALIGN(header->cmsg_len);
        if (aligned_length > remaining) {
            if (header->cmsg_len != remaining) {
                free(*copy_output);
                *copy_output = 0;
                return socket_error(EINVAL);
            }
            break;
        }
        offset += aligned_length;
    }
    return 0;
}

static int linux_adopt_received_control(struct msghdr *message, int flags) {
    size_t offset = 0U;
    if (message == 0 || message->msg_controllen == 0U) return 0;
    if (message->msg_control == 0) return socket_error(EINVAL);
    while (offset < message->msg_controllen) {
        struct cmsghdr *header = (struct cmsghdr *)(uintptr_t)(
            (uintptr_t)message->msg_control + offset);
        size_t remaining = message->msg_controllen - offset;
        size_t aligned_length;
        size_t data_length;
        if (remaining < sizeof(*header) || header->cmsg_len < CMSG_LEN(0) ||
            header->cmsg_len > remaining) return socket_error(EINVAL);
        data_length = header->cmsg_len - CMSG_LEN(0);
        if (header->cmsg_level == SOL_SOCKET &&
            header->cmsg_type == SCM_RIGHTS) {
            if ((data_length % sizeof(int)) != 0U) return socket_error(EINVAL);
            int *descriptors = (int *)CMSG_DATA(header);
            for (size_t index = 0U; index < data_length / sizeof(int);
                 ++index) {
                int open_flags = O_RDWR;
                int descriptor;
                if ((flags & MSG_CMSG_CLOEXEC) != 0) open_flags |= O_CLOEXEC;
                descriptor = __libc_adopt_linux_fd(
                    descriptors[index], open_flags);
                if (descriptor < 0) return -1;
                descriptors[index] = descriptor;
            }
        }
        aligned_length = CMSG_ALIGN(header->cmsg_len);
        if (aligned_length > remaining) {
            if (header->cmsg_len != remaining) return socket_error(EINVAL);
            break;
        }
        offset += aligned_length;
    }
    return 0;
}

static ssize_t linux_sendmsg(int descriptor, const struct msghdr *message,
                             int flags) {
    unsigned char *control_copy = 0;
    int linux_descriptor;
    int control_status;
    void *control;
    libc_linux_msghdr_t linux_message = {
        .name = (uint64_t)(uintptr_t)message->msg_name,
        .name_length = message->msg_namelen,
        .iov_pointer = (uint64_t)(uintptr_t)message->msg_iov,
        .iov_length = message->msg_iovlen,
        .control = 0U,
        .control_length = message->msg_controllen,
    };
    linux_descriptor = __libc_linux_fd_for_descriptor(descriptor);
    if (linux_descriptor < 0) return -1;
    control_status = linux_translate_send_control(message, &control_copy);
    if (control_status < 0) return -1;
    control = control_copy != 0 ? (void *)control_copy : message->msg_control;
    linux_message.control = (uint64_t)(uintptr_t)control;
    int64_t result = __libc_linux_syscall6(
        LINUX_SYS_SENDMSG, (uint64_t)(uint32_t)linux_descriptor,
        (uint64_t)(uintptr_t)&linux_message, (uint64_t)(uint32_t)flags,
        0U, 0U, 0U);
    free(control_copy);
    if (result < 0) {
        (void)__libc_status_result(result);
        return -1;
    }
    return result > (int64_t)SSIZE_MAX ? (errno = EOVERFLOW, -1) :
           (ssize_t)result;
}

static ssize_t linux_recvmsg(int descriptor, struct msghdr *message,
                             int flags) {
    int linux_descriptor;
    libc_linux_msghdr_t linux_message = {
        .name = (uint64_t)(uintptr_t)message->msg_name,
        .name_length = message->msg_namelen,
        .iov_pointer = (uint64_t)(uintptr_t)message->msg_iov,
        .iov_length = message->msg_iovlen,
        .control = (uint64_t)(uintptr_t)message->msg_control,
        .control_length = message->msg_controllen,
    };
    linux_descriptor = __libc_linux_fd_for_descriptor(descriptor);
    if (linux_descriptor < 0) return -1;
    int64_t result = __libc_linux_syscall6(
        LINUX_SYS_RECVMSG, (uint64_t)(uint32_t)linux_descriptor,
        (uint64_t)(uintptr_t)&linux_message, (uint64_t)(uint32_t)flags,
        0U, 0U, 0U);
    if (result < 0) {
        (void)__libc_status_result(result);
        return -1;
    }
    message->msg_namelen = (socklen_t)linux_message.name_length;
    message->msg_controllen = (size_t)linux_message.control_length;
    message->msg_flags = (int)linux_message.flags;
    if (linux_adopt_received_control(message, flags) < 0) return -1;
    return result > (int64_t)SSIZE_MAX ? (errno = EOVERFLOW, -1) :
           (ssize_t)result;
}

ssize_t sendmsg(int descriptor, const struct msghdr *message, int flags) {
    unsigned char *buffer = 0;
    size_t length;
    ssize_t result;
    if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0) {
        return (ssize_t)socket_error(EOPNOTSUPP);
    }
    if (message == 0 ||
        (message->msg_name == 0 && message->msg_namelen != 0U)) {
        return (ssize_t)socket_error(EINVAL);
    }
    if (message_iov_size(message, &length) < 0) return -1;
    if (libc_linux_personality() &&
        __libc_descriptor_is_linux_fd(descriptor)) {
        return linux_sendmsg(descriptor, message, flags);
    }
    if (length != 0U) {
        buffer = (unsigned char *)malloc(length);
        if (buffer == 0) return -1;
        message_copy_to_buffer(buffer, message);
    }
    result = sendto(descriptor, buffer, length, flags,
                    (const struct sockaddr *)message->msg_name,
                    message->msg_namelen);
    free(buffer);
    return result;
}

ssize_t recvmsg(int descriptor, struct msghdr *message, int flags) {
    unsigned char *buffer = 0;
    struct sockaddr_storage source;
    size_t length;
    socklen_t source_length = sizeof(source);
    ssize_t result;
    bool capture_source = false;
    if (message == 0) return (ssize_t)socket_error(EINVAL);
    if (message_iov_size(message, &length) < 0) return -1;
    if (libc_linux_personality() &&
        __libc_descriptor_is_linux_fd(descriptor)) {
        if ((flags & ~(MSG_DONTWAIT | MSG_CMSG_CLOEXEC | MSG_PEEK)) != 0)
            return (ssize_t)socket_error(EOPNOTSUPP);
        return linux_recvmsg(descriptor, message, flags);
    }
    if ((flags & ~(MSG_DONTWAIT | MSG_CMSG_CLOEXEC)) != 0)
        return (ssize_t)socket_error(EOPNOTSUPP);
    if (message->msg_name != 0) {
        uint32_t family = __libc_socket_family(descriptor);
        socklen_t required = family == AF_INET ? sizeof(struct sockaddr_in) :
                             family == AF_INET6 ? sizeof(struct sockaddr_in6) : 0U;
        if (required == 0U) return -1;
        if (message->msg_namelen < required) {
            errno = EINVAL;
            return -1;
        }
        memset(&source, 0, sizeof(source));
        source.ss_family = (sa_family_t)family;
        capture_source = true;
    }
    if (length != 0U) {
        buffer = (unsigned char *)malloc(length);
        if (buffer == 0) return -1;
    }
    result = recvfrom(descriptor, buffer, length, flags,
                      capture_source ? (struct sockaddr *)&source : 0,
                      capture_source ? &source_length : 0);
    if (result >= 0) {
        message_copy_from_buffer(buffer, (size_t)result, message);
        if (capture_source) {
            memcpy(message->msg_name, &source, source_length);
            message->msg_namelen = source_length;
        } else {
            message->msg_namelen = 0U;
        }
        message->msg_flags = 0;
    }
    free(buffer);
    return result;
}

int shutdown(int descriptor, int how) {
    os_socket_shutdown_t arguments = {
        .hdr = {sizeof(os_socket_shutdown_t), OS_SYSCALL_ABI_VERSION, 0U}
    };
    if (how < OS_SOCKET_SHUT_READ || how > OS_SOCKET_SHUT_BOTH) {
        return socket_error(EINVAL);
    }
    /* socketpair() 由 Linux ABI 兼容层提供，返回 Linux 描述符，而非原生 System 对象句柄。
     * 关闭操作也必须走同一 ABI 路径；若将该描述符传给原生 socket 系统调用，它会在原生
     * 句柄表中查找并返回 ENOTSOCK。 */
    if (libc_linux_personality() &&
        __libc_descriptor_is_linux_fd(descriptor)) {
        int linux_descriptor = __libc_linux_fd_for_descriptor(descriptor);
        if (linux_descriptor < 0) return -1;
        return __libc_status_result(__libc_linux_syscall6(
            LINUX_SYS_SHUTDOWN, (uint64_t)(uint32_t)linux_descriptor,
            (uint64_t)(uint32_t)how, 0U, 0U, 0U, 0U));
    }
    if (socket_handle(descriptor, &arguments.socket, 0) < 0) return -1;
    arguments.how = (uint32_t)how;
    return __libc_status_result(__libc_native_syscall6(OS_SYS_SOCKET_SHUTDOWN,
        (uint64_t)(uintptr_t)&arguments, 0U, 0U, 0U, 0U, 0U));
}

int getsockname(int descriptor, struct sockaddr *address, socklen_t *length) {
    os_socket_info_t info = {
        .hdr = {sizeof(os_socket_info_t), OS_SYSCALL_ABI_VERSION, 0U}
    };
    os_handle_t handle;
    if (address == 0 || length == 0) return socket_error(EINVAL);
    if (libc_linux_personality() &&
        __libc_descriptor_is_linux_fd(descriptor)) {
        int linux_descriptor = __libc_linux_fd_for_descriptor(descriptor);
        if (linux_descriptor < 0) return -1;
        return __libc_status_result(__libc_linux_syscall6(
            LINUX_SYS_GETSOCKNAME, (uint64_t)(uint32_t)linux_descriptor,
            (uint64_t)(uintptr_t)address, (uint64_t)(uintptr_t)length,
            0U, 0U, 0U));
    }
    if (address == 0 || length == 0 || socket_handle(descriptor, &handle, 0) < 0) {
        if (address == 0 || length == 0) errno = EINVAL;
        return -1;
    }
    info.socket = handle;
    if (__libc_status_result(__libc_native_syscall6(OS_SYS_SOCKET_GET_INFO,
            (uint64_t)(uintptr_t)&info, 0U, 0U, 0U, 0U, 0U)) < 0) return -1;
    if (info.family == AF_INET) {
        struct sockaddr_in result = {0};
        if (*length < sizeof(result)) return socket_error(EINVAL);
        result.sin_family = AF_INET;
        result.sin_port = htons(info.local_port);
        result.sin_addr.s_addr = htonl(info.local_address);
        memcpy(address, &result, sizeof(result));
        *length = sizeof(result);
    } else {
        struct sockaddr_in6 result = {0};
        if (*length < sizeof(result)) return socket_error(EINVAL);
        result.sin6_family = AF_INET6;
        result.sin6_port = htons(info.local_port);
        memcpy(result.sin6_addr.s6_addr, info.local_address6, 16U);
        memcpy(address, &result, sizeof(result));
        *length = sizeof(result);
    }
    return 0;
}

int getpeername(int descriptor, struct sockaddr *address, socklen_t *length) {
    os_socket_info_t info = {
        .hdr = {sizeof(os_socket_info_t), OS_SYSCALL_ABI_VERSION, 0U}
    };
    os_handle_t handle;
    if (address == 0 || length == 0) return socket_error(EINVAL);
    if (libc_linux_personality() &&
        __libc_descriptor_is_linux_fd(descriptor)) {
        int linux_descriptor = __libc_linux_fd_for_descriptor(descriptor);
        if (linux_descriptor < 0) return -1;
        return __libc_status_result(__libc_linux_syscall6(
            LINUX_SYS_GETPEERNAME, (uint64_t)(uint32_t)linux_descriptor,
            (uint64_t)(uintptr_t)address, (uint64_t)(uintptr_t)length,
            0U, 0U, 0U));
    }
    if (address == 0 || length == 0 || socket_handle(descriptor, &handle, 0) < 0) {
        if (address == 0 || length == 0) errno = EINVAL;
        return -1;
    }
    info.socket = handle;
    if (__libc_status_result(__libc_native_syscall6(OS_SYS_SOCKET_GET_INFO,
            (uint64_t)(uintptr_t)&info, 0U, 0U, 0U, 0U, 0U)) < 0) return -1;
    if ((info.flags & OS_SOCKET_INFO_CONNECTED) == 0U) return socket_error(ENOTCONN);
    if (info.family == AF_INET) {
        struct sockaddr_in result = {0};
        if (*length < sizeof(result)) return socket_error(EINVAL);
        result.sin_family = AF_INET;
        result.sin_port = htons(info.peer_port);
        result.sin_addr.s_addr = htonl(info.peer_address);
        memcpy(address, &result, sizeof(result));
        *length = sizeof(result);
    } else {
        struct sockaddr_in6 result = {0};
        if (*length < sizeof(result)) return socket_error(EINVAL);
        result.sin6_family = AF_INET6;
        result.sin6_port = htons(info.peer_port);
        memcpy(result.sin6_addr.s6_addr, info.peer_address6, 16U);
        memcpy(address, &result, sizeof(result));
        *length = sizeof(result);
    }
    return 0;
}

static uint32_t socket_option_number(int level, int option) {
    if (level == IPPROTO_TCP) {
        switch (option) {
        case TCP_KEEPIDLE: return OS_SOCKET_OPTION_KEEPIDLE;
        case TCP_KEEPINTVL: return OS_SOCKET_OPTION_KEEPINTVL;
        case TCP_KEEPCNT: return OS_SOCKET_OPTION_KEEPCNT;
        default: return 0U;
        }
    }
    if (level != SOL_SOCKET) return 0U;
    switch (option) {
    case SO_REUSEADDR: return OS_SOCKET_OPTION_REUSE_ADDRESS;
    case SO_ERROR: return OS_SOCKET_OPTION_ERROR;
    case SO_TYPE: return OS_SOCKET_OPTION_TYPE;
    case SO_DOMAIN: return OS_SOCKET_OPTION_DOMAIN;
    case SO_PROTOCOL: return OS_SOCKET_OPTION_PROTOCOL;
    case SO_ACCEPTCONN: return OS_SOCKET_OPTION_ACCEPT_CONNECTION;
    case SO_RCVBUF: return OS_SOCKET_OPTION_RECEIVE_BUFFER;
    case SO_SNDBUF: return OS_SOCKET_OPTION_SEND_BUFFER;
    case SO_KEEPALIVE: return OS_SOCKET_OPTION_KEEPALIVE;
    default: return 0U;
    }
}

int setsockopt(int descriptor, int level, int option, const void *value,
               socklen_t length) {
    os_socket_option_value_t arguments = {
        .hdr = {sizeof(os_socket_option_value_t), OS_SYSCALL_ABI_VERSION, 0U}
    };
    uint32_t kernel_option = socket_option_number(level, option);
    if (kernel_option == 0U) return socket_error(ENOPROTOOPT);
    if (value == 0 || length < sizeof(int)) return socket_error(EINVAL);
    if (libc_linux_personality() &&
        __libc_descriptor_is_linux_fd(descriptor)) {
        int linux_descriptor = __libc_linux_fd_for_descriptor(descriptor);
        if (linux_descriptor < 0) return -1;
        return __libc_status_result(__libc_linux_syscall6(
            LINUX_SYS_SETSOCKOPT, (uint64_t)(uint32_t)linux_descriptor,
            (uint64_t)(uint32_t)level, (uint64_t)(uint32_t)option,
            (uint64_t)(uintptr_t)value, (uint64_t)length, 0U));
    }
    if (socket_handle(descriptor, &arguments.socket, 0) < 0) return -1;
    arguments.option = kernel_option;
    arguments.value = *(const int *)value;
    return __libc_status_result(__libc_native_syscall6(OS_SYS_SOCKET_SET_OPTION,
        (uint64_t)(uintptr_t)&arguments, 0U, 0U, 0U, 0U, 0U));
}

int getsockopt(int descriptor, int level, int option, void *value,
               socklen_t *length) {
    os_socket_option_value_t arguments = {
        .hdr = {sizeof(os_socket_option_value_t), OS_SYSCALL_ABI_VERSION, 0U}
    };
    uint32_t kernel_option = socket_option_number(level, option);
    if (kernel_option == 0U) return socket_error(ENOPROTOOPT);
    if (value == 0 || length == 0 || *length < sizeof(int)) return socket_error(EINVAL);
    if (libc_linux_personality() &&
        __libc_descriptor_is_linux_fd(descriptor)) {
        int linux_descriptor = __libc_linux_fd_for_descriptor(descriptor);
        if (linux_descriptor < 0) return -1;
        return __libc_status_result(__libc_linux_syscall6(
            LINUX_SYS_GETSOCKOPT, (uint64_t)(uint32_t)linux_descriptor,
            (uint64_t)(uint32_t)level, (uint64_t)(uint32_t)option,
            (uint64_t)(uintptr_t)value, (uint64_t)(uintptr_t)length,
            0U));
    }
    if (socket_handle(descriptor, &arguments.socket, 0) < 0) return -1;
    arguments.option = kernel_option;
    if (__libc_status_result(__libc_native_syscall6(OS_SYS_SOCKET_GET_OPTION,
            (uint64_t)(uintptr_t)&arguments, 0U, 0U, 0U, 0U, 0U)) < 0) return -1;
    *(int *)value = arguments.value;
    *length = sizeof(int);
    return 0;
}
