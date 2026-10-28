/* REFACTOR_SYSCALL_NETWORK_OWNER：网络和 socket 系统调用处理程序。 */

#include <arch/x86_64/uaccess.h>
#include <arch/x86_64/cpu.h>
#include <kernel/completion_port.h>
#include <kernel/console.h>
#include <kernel/debug_stage.h>
#include <kernel/e1000.h>
#include <kernel/pci.h>
#include <kernel/message_port.h>
#include <kernel/net_manager.h>
#include <kernel/socket.h>
#include <uapi/network.h>
#include <uapi/socket.h>
#include <uapi/syscall.h>

#include "internal.h"

static atomic_uint g_socket_create_progress[MAX_CPUS];

#if OS_DEBUG_SERIAL
static uint32_t g_debug_socket_create_logs;
static uint32_t g_debug_socket_stream_create_logs;
static uint32_t g_debug_socket_connect_logs;
static uint32_t g_debug_socket_io_logs;
static uint32_t g_debug_socket_zero_destination_logs;
static uint32_t g_debug_socket_recv_enter_logs;
static uint32_t g_debug_socket_recv_detail_logs;
#endif

static void socket_create_progress(uint32_t value) {
    uint32_t cpu = x86_current_cpu_index();
    if (cpu < MAX_CPUS) {
        atomic_store_explicit(&g_socket_create_progress[cpu], value,
                              memory_order_release);
    }
}

uint32_t syscall_socket_create_debug_progress(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return atomic_load_explicit(&g_socket_create_progress[cpu_index],
                                memory_order_acquire);
}

static kstatus_t socket_from_handle(os_handle_t handle, uint32_t rights,
                                    socket_t **socket) {
    process_t *process = current_process();
    void *object = 0;
    kstatus_t status;
    if (process == 0 || socket == 0) return K_EINVAL;
    status = handle_lookup(&process->handles, (handle_t)handle, rights, &object);
    if (status != K_OK) return status;
    if (((socket_t *)object)->object.type != KOBJECT_TYPE_SOCKET) {
        object_put(object);
        return K_EINVAL;
    }
    *socket = (socket_t *)object;
    return K_OK;
}

int64_t syscall_socket_get_info(uint64_t arguments_pointer, uint64_t unused1,
                                uint64_t unused2, uint64_t unused3,
                                uint64_t unused4, uint64_t unused5) {
    os_socket_info_t arguments;
    socket_t *socket;
    kstatus_t status;
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    if (arguments_pointer == 0U) return K_EINVAL;
    status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments))) return K_EINVAL;
    status = socket_from_handle(arguments.socket, SOCKET_RIGHT_READ, &socket);
    if (status != K_OK) return status;
    status = socket_get_info(socket, &arguments);
    object_put(socket);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

int64_t syscall_socket_get_option(uint64_t arguments_pointer, uint64_t unused1,
                                  uint64_t unused2, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5) {
    os_socket_option_value_t arguments;
    socket_t *socket;
    kstatus_t status;
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    if (arguments_pointer == 0U) return K_EINVAL;
    status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments))) return K_EINVAL;
    status = socket_from_handle(arguments.socket, SOCKET_RIGHT_READ, &socket);
    if (status != K_OK) return status;
    status = socket_get_option(socket, arguments.option, &arguments.value);
    object_put(socket);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

int64_t syscall_socket_set_option(uint64_t arguments_pointer, uint64_t unused1,
                                  uint64_t unused2, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5) {
    os_socket_option_value_t arguments;
    socket_t *socket;
    kstatus_t status;
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    if (arguments_pointer == 0U) return K_EINVAL;
    status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments))) return K_EINVAL;
    status = socket_from_handle(arguments.socket, SOCKET_RIGHT_WRITE, &socket);
    if (status == K_OK) {
        status = socket_set_option(socket, arguments.option, arguments.value);
        object_put(socket);
    }
    return status;
}

int64_t syscall_socket_shutdown(uint64_t arguments_pointer, uint64_t unused1,
                                uint64_t unused2, uint64_t unused3,
                                uint64_t unused4, uint64_t unused5) {
    os_socket_shutdown_t arguments;
    process_t *process = current_process();
    void *object = 0;
    uint32_t rights;
    kstatus_t status;

    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;

    if (process == 0 || arguments_pointer == 0U) return K_EINVAL;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U || arguments.how > OS_SOCKET_SHUT_BOTH) {
        return K_EINVAL;
    }

    rights = arguments.how == OS_SOCKET_SHUT_READ ? SOCKET_RIGHT_READ :
             arguments.how == OS_SOCKET_SHUT_WRITE ? SOCKET_RIGHT_WRITE :
             SOCKET_RIGHT_READ | SOCKET_RIGHT_WRITE;
    status = handle_lookup(&process->handles, (handle_t)arguments.socket,
                           rights, &object);
    if (status != K_OK) return status;
    if (((socket_t *)object)->object.type != KOBJECT_TYPE_SOCKET) {
        status = K_ENOTSOCK;
    } else {
        status = socket_shutdown((socket_t *)object, arguments.how);
    }
    object_put(object);
    return status;
}

int64_t syscall_socket_create(uint64_t family, uint64_t type, uint64_t protocol,
                                 uint64_t output_pointer, uint64_t create_flags,
                                 uint64_t unused5) {
    (void)unused5;
    process_t *process = current_process();
    socket_create_progress(1U);
    debug_trace_stage(OS_DEBUG_PHASE_USER_RUNTIME,
                             OS_DEBUG_STEP_USER_MARK, 0x5000U);
    if (process == 0 || family > UINT16_MAX || type > UINT16_MAX ||
        protocol > UINT16_MAX ||
        (create_flags & ~((uint64_t)OS_SOCKET_CREATE_FLAG_MASK)) != 0U) {
        return K_EINVAL;
    }
    socket_t *socket = 0;
    kstatus_t status = socket_create((uint16_t)family, (uint16_t)type,
                                     (uint16_t)protocol, &socket);
#if OS_DEBUG_SERIAL
    if ((type == OS_SOCK_STREAM &&
         g_debug_socket_stream_create_logs < 64U) ||
        g_debug_socket_create_logs < 128U) {
        kernel_serial_printf_serial_only(
            "OS_SOCKET_CREATE family=%llu type=%llu status=%lld\r\n",
            (unsigned long long)family, (unsigned long long)type,
            (long long)status);
        if (type == OS_SOCK_STREAM) ++g_debug_socket_stream_create_logs;
        else ++g_debug_socket_create_logs;
    }
#endif
    socket_create_progress(2U);
    debug_trace_stage(OS_DEBUG_PHASE_USER_RUNTIME,
                             OS_DEBUG_STEP_USER_MARK, 0x5001U);
    socket_create_progress(3U);
    if (status != K_OK) return status;
    handle_t handle = 0;
    uint32_t handle_flags = (create_flags & OS_SOCKET_CREATE_FLAG_CLOEXEC) != 0U ?
                            HANDLE_FLAG_CLOEXEC : 0U;
    status = handle_create_with_flags(&process->handles, socket,
                                      SOCKET_RIGHT_ALL, handle_flags, &handle);
    socket_create_progress(4U);
    debug_trace_stage(OS_DEBUG_PHASE_USER_RUNTIME,
                             OS_DEBUG_STEP_USER_MARK, 0x5002U);
    socket_create_progress(5U);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                              &handle, sizeof(handle));
        debug_trace_stage(OS_DEBUG_PHASE_USER_RUNTIME,
                                 OS_DEBUG_STEP_USER_MARK, 0x5003U);
    }
    if (status != K_OK && handle != 0) (void)handle_close(&process->handles, handle);
    object_put(socket);
    debug_trace_stage(OS_DEBUG_PHASE_USER_RUNTIME,
                             OS_DEBUG_STEP_USER_MARK, 0x5004U);
    return status;
}

/* NET_GET_STATUS(args)：向用户态网络管理器提供稳定的链路和地址快照。 */
int64_t syscall_net_get_status(uint64_t arguments_pointer, uint64_t unused1,
                                  uint64_t unused2, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_net_status_t arguments;
    net_manager_status_t status = {0};
    kstatus_t result;
    if (arguments_pointer == 0U) return K_EINVAL;
    result = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (result != K_OK) return result;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments))) return K_EINVAL;
    if (!net_manager_get_status(&status)) return K_EIO;
    arguments.flags = 0U;
    if (status.hardware_present) arguments.flags |= OS_NET_STATUS_HARDWARE_PRESENT;
    if (status.link_up) arguments.flags |= OS_NET_STATUS_LINK_UP;
    if (status.ipv6_configured) arguments.flags |= OS_NET_STATUS_IPV6_CONFIGURED;
    arguments.ipv4_address = status.ipv4_address;
    arguments.ipv4_gateway = status.ipv4_gateway;
    arguments.ipv4_prefix_length = status.ipv4_prefix_length;
    arguments.reserved[0] = 0U;
    arguments.reserved[1] = 0U;
    arguments.reserved[2] = 0U;
    for (uint32_t index = 0U; index < sizeof(arguments.ipv6_address); ++index) {
        arguments.ipv6_address[index] = status.ipv6_address[index];
    }
    arguments.link_transitions = status.link_transitions;
    arguments.reset_count = status.reset_count;
    for (uint32_t index = 0U; index < sizeof(arguments.mac); ++index) {
        arguments.mac[index] = status.mac[index];
    }
    arguments.mac_reserved[0] = 0U;
    arguments.mac_reserved[1] = 0U;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

/*
 * NET_SUBSCRIBE(message_port): bind the system network daemon's waitable
 * message port to link-state changes. OS_INVALID_HANDLE unsubscribes.
 */
int64_t syscall_net_subscribe(uint64_t port_handle, uint64_t unused1,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    void *object = 0;
    kstatus_t status;

    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;

    if (process == 0) return K_EPERM;
    if (port_handle == OS_INVALID_HANDLE) return net_manager_subscribe(0);

    status = handle_lookup(&process->handles,
                           (handle_t)port_handle,
                           MESSAGE_PORT_RIGHT_WRITE,
                           &object);
    if (status != K_OK) return status;

    if (((message_port_t *)object)->object.type != KOBJECT_TYPE_MESSAGE_PORT) {
        status = K_EINVAL;
    } else {
        status = net_manager_subscribe((message_port_t *)object);
    }

    object_put(object);
    return status;
}

/* NET_SET_IPV4(args)：更新当前网络配置。 */
int64_t syscall_net_set_ipv4(uint64_t arguments_pointer, uint64_t unused1,
                                uint64_t unused2, uint64_t unused3,
                                uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_net_set_ipv4_config_t arguments;
    if (arguments_pointer == 0U) return K_EINVAL;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved[0] != 0U || arguments.reserved[1] != 0U ||
        arguments.reserved[2] != 0U || arguments.prefix_length > 32U ||
        (arguments.gateway != 0U && arguments.address == 0U)) return K_EINVAL;
    return e1000_set_ipv4_config(arguments.address, arguments.prefix_length,
                                  arguments.gateway);
}

int64_t syscall_socket_bind(uint64_t handle, uint64_t address, uint64_t port,
                               uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0 || address > UINT32_MAX || port > UINT16_MAX) return K_EINVAL;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    socket_t *socket = (socket_t *)object;
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_bind(socket, (uint32_t)address, (uint16_t)port) : K_EINVAL;
    object_put(object);
    return status;
}

int64_t syscall_socket_connect(uint64_t handle, uint64_t address, uint64_t port,
                                  uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0 || address > UINT32_MAX || port > UINT16_MAX) return K_EINVAL;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    socket_t *socket = (socket_t *)object;
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_connect(socket, (uint32_t)address, (uint16_t)port) : K_EINVAL;
#if OS_DEBUG_SERIAL
    if (g_debug_socket_connect_logs < 128U) {
        kernel_serial_printf_serial_only(
            "OS_SOCKET_CONNECT handle=%llx addr=%llx port=%llu status=%lld\r\n",
            (unsigned long long)handle, (unsigned long long)address,
            (unsigned long long)port, (long long)status);
        ++g_debug_socket_connect_logs;
    }
#endif
    object_put(object);
    return status;
}

/* SOCKET_BIND6(handle, address[16], port)：为 IPv6 UDP socket 绑定端点。 */
int64_t syscall_socket_bind6(uint64_t handle, uint64_t address_pointer, uint64_t port,
                                uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    uint8_t address[16];
    void *object = 0;
    if (process == 0 || address_pointer == 0U || port == 0U || port > UINT16_MAX ||
        copy_from_user(address, (const void __user *)(uintptr_t)address_pointer,
                       sizeof(address)) != K_OK) return K_EACCES;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    socket_t *socket = (socket_t *)object;
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_bind_ipv6(socket, address, (uint16_t)port) : K_EINVAL;
    object_put(object);
    return status;
}

/* SOCKET_CONNECT6(handle, address[16], port)：连接 IPv6 UDP 对端。 */
int64_t syscall_socket_connect6(uint64_t handle, uint64_t address_pointer, uint64_t port,
                                   uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    uint8_t address[16];
    void *object = 0;
    if (process == 0 || address_pointer == 0U || port == 0U || port > UINT16_MAX ||
        copy_from_user(address, (const void __user *)(uintptr_t)address_pointer,
                       sizeof(address)) != K_OK) return K_EACCES;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    socket_t *socket = (socket_t *)object;
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_connect_ipv6(socket, address, (uint16_t)port) : K_EINVAL;
    object_put(object);
    return status;
}

int64_t syscall_socket_listen(uint64_t handle, uint64_t backlog,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0 || backlog > UINT32_MAX) return K_EINVAL;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    socket_t *socket = (socket_t *)object;
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_listen(socket, (uint32_t)backlog) : K_EINVAL;
    object_put(object);
    return status;
}

int64_t syscall_socket_accept(uint64_t handle, uint64_t timeout_ns,
                                 uint64_t output_handle, uint64_t create_flags,
                                 uint64_t unused4, uint64_t unused5) {
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    if ((create_flags & ~((uint64_t)OS_SOCKET_CREATE_FLAG_MASK)) != 0U) {
        return K_EINVAL;
    }
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_WAIT, &object);
    if (status != K_OK) return status;
    socket_t *listener = (socket_t *)object;
    socket_t *accepted = 0;
    if (listener->object.type != KOBJECT_TYPE_SOCKET) {
        status = K_EINVAL;
    } else {
        status = socket_accept(listener, timeout_ns, &accepted);
    }
    handle_t accepted_handle = 0;
    if (status == K_OK) {
        uint32_t handle_flags =
            (create_flags & OS_SOCKET_CREATE_FLAG_CLOEXEC) != 0U ?
            HANDLE_FLAG_CLOEXEC : 0U;
        status = handle_create_with_flags(&process->handles, accepted,
                                          SOCKET_RIGHT_ALL, handle_flags,
                                          &accepted_handle);
        if (status == K_OK) {
            status = copy_to_user((void __user *)(uintptr_t)output_handle,
                                  &accepted_handle, sizeof(accepted_handle));
        }
        if (status != K_OK && accepted_handle != 0) {
            (void)handle_close(&process->handles, accepted_handle);
        }
        object_put(accepted); /* 丢弃 accept 队列转移给调用方的引用。 */
    }
    object_put(object);
    return status;
}

int64_t syscall_socket_send(uint64_t handle, uint64_t buffer, uint64_t length,
                               uint64_t address, uint64_t port, uint64_t flags) {
    process_t *process = current_process();
    if (process == 0 || length > SOCKET_MAX_PAYLOAD || address > UINT32_MAX ||
        port > UINT16_MAX || flags != 0U) return K_EINVAL;
    uint8_t payload[SOCKET_MAX_PAYLOAD];
    if (length != 0 && copy_from_user(payload,
                                      (const void __user *)(uintptr_t)buffer,
                                      (size_t)length) != K_OK) return K_EACCES;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    socket_t *socket = (socket_t *)object;
    uint64_t bytes = 0;
#if OS_DEBUG_SERIAL
    if (address == 0U && port == 0U &&
        g_debug_socket_zero_destination_logs < 64U) {
        os_socket_info_t info = {0};
        kstatus_t info_status;
        info.hdr.size = sizeof(info);
        info.hdr.version = OS_SYSCALL_ABI_VERSION;
        info.socket = handle;
        info_status = socket_get_info(socket, &info);
        kernel_serial_printf_serial_only(
            "OS_SOCKET_SEND_STATE handle=%llx status=%d type=%u flags=%x local=%x:%u peer=%x:%u\r\n",
            (unsigned long long)handle, info_status, info.type, info.flags,
            info.local_address, info.local_port, info.peer_address,
            info.peer_port);
        ++g_debug_socket_zero_destination_logs;
    }
#endif
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_send(socket, payload, (size_t)length, (uint32_t)address,
                         (uint16_t)port, &bytes) : K_EINVAL;
    object_put(object);
#if OS_DEBUG_SERIAL
    if (g_debug_socket_io_logs < 256U) {
        kernel_serial_printf_serial_only(
            "OS_SOCKET_SEND handle=%llx len=%llu addr=%llx port=%llu status=%lld bytes=%llu\r\n",
            (unsigned long long)handle, (unsigned long long)length,
            (unsigned long long)address, (unsigned long long)port,
            (long long)status,
            status == K_OK ? (unsigned long long)bytes : 0ULL);
        ++g_debug_socket_io_logs;
    }
#endif
    return status == K_OK ? (int64_t)bytes : status;
}

/* SOCKET_SEND6(handle, buffer, length, address[16], port, flags)。 */
int64_t syscall_socket_send6(uint64_t handle, uint64_t buffer, uint64_t length,
                                uint64_t address_pointer, uint64_t port, uint64_t flags) {
    process_t *process = current_process();
    uint8_t payload[SOCKET_MAX_PAYLOAD];
    uint8_t address[16];
    uint64_t bytes = 0U;
    void *object = 0;
    if (process == 0 || length > SOCKET_MAX_PAYLOAD || address_pointer == 0U ||
        port > UINT16_MAX || flags != 0U ||
        copy_from_user(address, (const void __user *)(uintptr_t)address_pointer,
                       sizeof(address)) != K_OK) return K_EACCES;
    if (length != 0U && copy_from_user(payload,
                                       (const void __user *)(uintptr_t)buffer,
                                       (size_t)length) != K_OK) return K_EACCES;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    socket_t *socket = (socket_t *)object;
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_send_ipv6(socket, payload, (size_t)length, address,
                              (uint16_t)port, &bytes) : K_EINVAL;
    object_put(object);
    return status == K_OK ? (int64_t)bytes : status;
}

/* SOCKET_SEND_ASYNC(args)：提交时复制负载，完成后通过 completion port 通知。 */
int64_t syscall_socket_send_async(uint64_t arguments_pointer,
                                     uint64_t unused1, uint64_t unused2,
                                     uint64_t unused3, uint64_t unused4,
                                     uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    os_socket_async_send_t arguments;
    uint8_t payload[SOCKET_MAX_PAYLOAD];
    void *socket_object = 0;
    void *port_object = 0;
    uint64_t request_id = 0U;
    kstatus_t status;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK || !versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.flags != 0U || arguments.length > SOCKET_MAX_PAYLOAD) return K_EINVAL;
    if (arguments.length != 0U &&
        copy_from_user(payload, (const void __user *)(uintptr_t)arguments.buffer,
                       (size_t)arguments.length) != K_OK) return K_EACCES;
    status = handle_lookup(&process->handles, (handle_t)arguments.socket,
                           SOCKET_RIGHT_WRITE, &socket_object);
    if (status != K_OK) return status;
    status = handle_lookup(&process->handles, (handle_t)arguments.completion_port,
                           COMPLETION_PORT_RIGHT_WRITE, &port_object);
    if (status == K_OK &&
        ((socket_t *)socket_object)->object.type == KOBJECT_TYPE_SOCKET &&
        ((completion_port_t *)port_object)->object.type ==
            KOBJECT_TYPE_COMPLETION_PORT) {
        status = socket_send_async((socket_t *)socket_object, payload,
                                   (size_t)arguments.length,
                                   (uint32_t)arguments.address,
                                   (uint16_t)arguments.port,
                                   (completion_port_t *)port_object,
                                   arguments.user_key, &request_id);
    } else if (status == K_OK) {
        status = K_EINVAL;
    }
    if (port_object != 0) object_put(port_object);
    object_put(socket_object);
    if (status == K_OK) return (int64_t)request_id;
    return status;
}

/* SOCKET_SEND_ASYNC6(args)：复制 IPv6 地址和负载后投递 deferred 发送任务。 */
int64_t syscall_socket_send_async6(uint64_t arguments_pointer,
                                      uint64_t unused1, uint64_t unused2,
                                      uint64_t unused3, uint64_t unused4,
                                      uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    os_socket_ipv6_async_send_t arguments;
    uint8_t payload[SOCKET_MAX_PAYLOAD];
    void *socket_object = 0;
    void *port_object = 0;
    uint64_t request_id = 0U;
    kstatus_t status;
    if (process == 0 || arguments_pointer == 0U) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK || !versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.flags != 0U || arguments.port == 0U ||
        arguments.length > SOCKET_MAX_PAYLOAD) return K_EINVAL;
    if (arguments.length != 0U &&
        copy_from_user(payload, (const void __user *)(uintptr_t)arguments.buffer,
                       (size_t)arguments.length) != K_OK) return K_EACCES;
    status = handle_lookup(&process->handles, (handle_t)arguments.socket,
                           SOCKET_RIGHT_WRITE, &socket_object);
    if (status != K_OK) return status;
    status = handle_lookup(&process->handles, (handle_t)arguments.completion_port,
                           COMPLETION_PORT_RIGHT_WRITE, &port_object);
    if (status == K_OK &&
        ((socket_t *)socket_object)->object.type == KOBJECT_TYPE_SOCKET &&
        ((completion_port_t *)port_object)->object.type == KOBJECT_TYPE_COMPLETION_PORT) {
        status = socket_send_async_ipv6((socket_t *)socket_object, payload,
                                        (size_t)arguments.length, arguments.address,
                                        arguments.port, (completion_port_t *)port_object,
                                        arguments.user_key, &request_id);
    } else if (status == K_OK) {
        status = K_EINVAL;
    }
    if (port_object != 0) object_put(port_object);
    object_put(socket_object);
    return status == K_OK ? (int64_t)request_id : status;
}

int64_t syscall_socket_recv(uint64_t handle, uint64_t buffer, uint64_t length,
                               uint64_t source_address, uint64_t source_port,
                               uint64_t timeout_ns) {
    process_t *process = current_process();
    if (process == 0 || length > SOCKET_MAX_PAYLOAD) return K_EINVAL;
    uint8_t payload[SOCKET_MAX_PAYLOAD];
    socket_ipv4_endpoint_t source = {0};
    uint64_t bytes = 0;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_READ, &object);
    if (status != K_OK) {
#if OS_DEBUG_SERIAL
        if (g_debug_socket_recv_enter_logs < 128U) {
            kernel_serial_printf_serial_only(
                "OS_SOCKET_RECV_LOOKUP_FAIL handle=%llu status=%lld\r\n",
                (unsigned long long)handle, (long long)status);
            ++g_debug_socket_recv_enter_logs;
        }
#endif
        return status;
    }
    socket_t *socket = (socket_t *)object;
#if OS_DEBUG_SERIAL
    if (g_debug_socket_recv_enter_logs < 128U) {
        kernel_serial_printf_serial_only(
            "OS_SOCKET_RECV_ENTER handle=%llu len=%llu timeout=%llu\r\n",
            (unsigned long long)handle, (unsigned long long)length,
            (unsigned long long)timeout_ns);
        ++g_debug_socket_recv_enter_logs;
    }
#endif
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_recv(socket, payload, (size_t)length, &source, timeout_ns,
                         &bytes) : K_EINVAL;
    if (status == K_OK) {
        if (bytes != 0 && copy_to_user((void __user *)(uintptr_t)buffer,
                                       payload, (size_t)bytes) != K_OK) status = K_EACCES;
        if (status == K_OK && copy_to_user(
                (void __user *)(uintptr_t)source_address, &source.address,
                sizeof(source.address)) != K_OK) status = K_EACCES;
        if (status == K_OK && copy_to_user(
                (void __user *)(uintptr_t)source_port, &source.port,
                sizeof(source.port)) != K_OK) status = K_EACCES;
    }
    object_put(object);
#if OS_DEBUG_SERIAL
    if (g_debug_socket_io_logs < 256U) {
        kernel_serial_printf_serial_only(
            "OS_SOCKET_RECV handle=%llx len=%llu timeout=%llu status=%lld bytes=%llu\r\n",
            (unsigned long long)handle, (unsigned long long)length,
            (unsigned long long)timeout_ns, (long long)status,
            status == K_OK ? (unsigned long long)bytes : 0ULL);
        ++g_debug_socket_io_logs;
    }
    if (g_debug_socket_recv_detail_logs < 512U) {
        kernel_serial_printf_serial_only(
            "OS_SOCKET_RECV_DETAIL family=4 handle=%llx len=%llu timeout=%llu status=%lld bytes=%llu\r\n",
            (unsigned long long)handle, (unsigned long long)length,
            (unsigned long long)timeout_ns, (long long)status,
            status == K_OK ? (unsigned long long)bytes : 0ULL);
        ++g_debug_socket_recv_detail_logs;
    }
#endif
    return status == K_OK ? (int64_t)bytes : status;
}

/* SOCKET_RECV6(handle, buffer, length, source_address[16], source_port, timeout)。 */
int64_t syscall_socket_recv6(uint64_t handle, uint64_t buffer, uint64_t length,
                                uint64_t source_address, uint64_t source_port,
                                uint64_t timeout_ns) {
    process_t *process = current_process();
    uint8_t payload[SOCKET_MAX_PAYLOAD];
    socket_ipv6_endpoint_t source = {0};
    uint64_t bytes = 0U;
    void *object = 0;
    if (process == 0 || length > SOCKET_MAX_PAYLOAD || source_address == 0U ||
        source_port == 0U) return K_EINVAL;
    if (length != 0U && buffer == 0U) return K_EINVAL;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     SOCKET_RIGHT_READ, &object);
    if (status != K_OK) {
#if OS_DEBUG_SERIAL
        if (g_debug_socket_recv_enter_logs < 128U) {
            kernel_serial_printf_serial_only(
                "OS_SOCKET_RECV6_LOOKUP_FAIL handle=%llu status=%lld\r\n",
                (unsigned long long)handle, (long long)status);
            ++g_debug_socket_recv_enter_logs;
        }
#endif
        return status;
    }
    socket_t *socket = (socket_t *)object;
#if OS_DEBUG_SERIAL
    if (g_debug_socket_recv_enter_logs < 128U) {
        kernel_serial_printf_serial_only(
            "OS_SOCKET_RECV6_ENTER handle=%llu len=%llu timeout=%llu\r\n",
            (unsigned long long)handle, (unsigned long long)length,
            (unsigned long long)timeout_ns);
        ++g_debug_socket_recv_enter_logs;
    }
#endif
    status = socket->object.type == KOBJECT_TYPE_SOCKET ?
             socket_recv_ipv6(socket, payload, (size_t)length, &source, timeout_ns,
                              &bytes) : K_EINVAL;
    if (status == K_OK) {
        if (bytes != 0U && copy_to_user((void __user *)(uintptr_t)buffer,
                                       payload, (size_t)bytes) != K_OK) status = K_EACCES;
        if (status == K_OK && copy_to_user(
                (void __user *)(uintptr_t)source_address, source.address,
                sizeof(source.address)) != K_OK) status = K_EACCES;
        if (status == K_OK && copy_to_user(
                (void __user *)(uintptr_t)source_port, &source.port,
                sizeof(source.port)) != K_OK) status = K_EACCES;
    }
    object_put(object);
#if OS_DEBUG_SERIAL
    if (g_debug_socket_io_logs < 256U) {
        kernel_serial_printf_serial_only(
            "OS_SOCKET_RECV6 handle=%llx len=%llu timeout=%llu status=%lld bytes=%llu\r\n",
            (unsigned long long)handle, (unsigned long long)length,
            (unsigned long long)timeout_ns, (long long)status,
            status == K_OK ? (unsigned long long)bytes : 0ULL);
        ++g_debug_socket_io_logs;
    }
    if (g_debug_socket_recv_detail_logs < 512U) {
        kernel_serial_printf_serial_only(
            "OS_SOCKET_RECV_DETAIL family=6 handle=%llx len=%llu timeout=%llu status=%lld bytes=%llu\r\n",
            (unsigned long long)handle, (unsigned long long)length,
            (unsigned long long)timeout_ns, (long long)status,
            status == K_OK ? (unsigned long long)bytes : 0ULL);
        ++g_debug_socket_recv_detail_logs;
    }
#endif
    return status == K_OK ? (int64_t)bytes : status;
}

int64_t syscall_net_enum_adapter(uint64_t pointer,uint64_t a,uint64_t b,
    uint64_t c,uint64_t d,uint64_t e) {
    (void)a;(void)b;(void)c;(void)d;(void)e;
    os_net_adapter_t input;
    kstatus_t result=copy_from_user(&input,(const void __user *)(uintptr_t)pointer,sizeof(input));
    if (result!=K_OK) return result;
    if (!versioned_header_valid(&input.hdr,sizeof(input))) return K_EINVAL;
    os_net_adapter_t output={.hdr=input.hdr,.index=input.index};
    const pci_host_t *host=pci_current_host();
    const pci_device_t *selected=0,*active=0;
    result=e1000_active_adapter(&active);
    if (result!=K_OK) return result;
    if (host && host->initialized) {
        for (uint32_t i=0;i<host->device_count && i<PCI_MAX_DEVICES;++i) {
            const pci_device_t *device=&host->devices[i];
            if (device->class_code!=2U) continue;
            if (output.count++==input.index) selected=device;
        }
    }
    if (selected) {
        output.vendor=selected->vendor_id;output.device=selected->device_id;
        output.segment=selected->segment;output.bus=selected->bus;
        output.slot=selected->slot;output.function=selected->function;
        output.subclass=selected->subclass;
        if (selected==active) output.flags=OS_NET_ADAPTER_ACTIVE;
    }
    return copy_to_user((void __user *)(uintptr_t)pointer,&output,sizeof(output));
}
