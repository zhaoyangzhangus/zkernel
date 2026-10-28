#include <kernel/kmem.h>
#include <kernel/mm.h>
#include <kernel/net_manager.h>
#include <kernel/console.h>
#include <arch/x86_64/cpu.h>
#include <kernel/net_core.h>
#include <kernel/socket.h>
#include <kernel/sched.h>
#include <kernel/wait.h>
#include <kernel/string.h>

#include "socket_internal.h"
#include "socket_model.h"
#include "tcp_retry.h"
#include "tcp_output.h"

/* 私有状态类型统一在 socket_model.h 中声明。 */

/* TCP 连接与已接受的 socket 状态在 socket_model.h 中统一定义。 */
/* 连接和等待类型仅在 socket_model.h 中声明一次。 */

/* socket 异步请求状态在 socket_model.h 中统一定义。 */
/* 异步请求状态仅在 socket_model.h 中声明一次。 */

static socket_binding_t g_initial_bindings[SOCKET_BINDING_COUNT];
static socket_tcp_connection_t g_initial_tcp_connections[SOCKET_BINDING_COUNT];
static socket_tcp6_connection_t g_initial_tcp6_connections[SOCKET_BINDING_COUNT];
static socket_binding_t *g_bindings = g_initial_bindings;
static socket_tcp_connection_t *g_tcp_connections = g_initial_tcp_connections;
static socket_tcp6_connection_t *g_tcp6_connections = g_initial_tcp6_connections;
static uint32_t g_registry_capacity = SOCKET_BINDING_COUNT;
/* 由绑定锁保护的索引，不保存借用的 socket 指针。发生变更时使两个提示失效，
 * 保留查找首个匹配槽位的语义。 */
static uint32_t g_tcp_lookup_hint = UINT32_MAX;
static uint32_t g_tcp6_lookup_hint = UINT32_MAX;
/* 仅缓存完全匹配的 UDP 项：通配项仍需完整扫描，以排除后续存在精确匹配项。
 * 绑定变更会使两个索引失效。 */
static uint32_t g_udp_lookup_hint = UINT32_MAX;
static uint32_t g_udp6_lookup_hint = UINT32_MAX;
static spinlock_t g_binding_lock;
static atomic_uint g_socket_init_state;
static atomic_uint g_next_ephemeral_port;
static atomic_uint_fast64_t g_next_async_request_id;
static socket_tcp_ipv4_output_fn g_tcp_ipv4_output;
static void *g_tcp_ipv4_output_context;
static socket_tcp_ipv6_output_fn g_tcp_ipv6_output;
static void *g_tcp_ipv6_output_context;
static socket_udp_ipv4_output_fn g_udp_ipv4_output;
static void *g_udp_ipv4_output_context;
static socket_udp_ipv6_output_fn g_udp_ipv6_output;
static void *g_udp_ipv6_output_context;

#if OS_DEBUG_SERIAL
static uint32_t g_debug_tcp_connect_logs;
#endif

socket_private_t *socket_private(socket_t *socket);

void socket_zero(void *memory, size_t length) {
    memset(memory, 0, length);
}

void socket_copy(void *destination, const void *source, size_t length) {
    memmove(destination, source, length);
}

uint32_t socket_stream_write_locked(socket_private_t *private,
                                    const void *buffer, size_t length) {
    if (length == 0U) return private->stream_write;
    uint32_t capacity = socket_stream_capacity(private);
    uint8_t *data = socket_stream_data(private);
    size_t first = capacity - private->stream_write;
    if (first > length) first = length;
    socket_copy(data + private->stream_write, buffer, first);
    if (length > first) {
        socket_copy(data, (const uint8_t *)buffer + first,
                    length - first);
    }
    uint32_t next = private->stream_write + (uint32_t)first;
    return next == capacity ? (uint32_t)(length - first) : next;
}

uint32_t socket_stream_read_locked(const socket_private_t *private,
                                   void *buffer, size_t length) {
    if (length == 0U) return private->stream_read;
    uint32_t capacity = socket_stream_capacity(private);
    const uint8_t *data = socket_stream_data_const(private);
    size_t first = capacity - private->stream_read;
    if (first > length) first = length;
    socket_copy(buffer, data + private->stream_read, first);
    if (length > first) {
        socket_copy((uint8_t *)buffer + first, data,
                    length - first);
    }
    uint32_t next = private->stream_read + (uint32_t)first;
    return next == capacity ? (uint32_t)(length - first) : next;
}

static uint32_t socket_active_ipv4_address(void) {
    net_manager_status_t network = {0};
    if (net_manager_get_status(&network) && network.link_up &&
        network.ipv4_address != 0U) {
        return network.ipv4_address;
    }
    return 0x7F000001U;
}

bool socket_address6_equal(const uint8_t left[16], const uint8_t right[16]) {
    /* 固定大小复制可处理未对齐的数据包地址，避免违反别名规则或
     * 越过 16 字节地址边界读取。 */
    uint64_t left_low, left_high, right_low, right_high;
    __builtin_memcpy(&left_low, left, sizeof(left_low));
    __builtin_memcpy(&left_high, left + 8U, sizeof(left_high));
    __builtin_memcpy(&right_low, right, sizeof(right_low));
    __builtin_memcpy(&right_high, right + 8U, sizeof(right_high));
    return ((left_low ^ right_low) | (left_high ^ right_high)) == 0U;
}

bool socket_address6_zero(const uint8_t address[16]) {
    uint64_t low, high;
    __builtin_memcpy(&low, address, sizeof(low));
    __builtin_memcpy(&high, address + 8U, sizeof(high));
    return (low | high) == 0U;
}

void socket_lock(spinlock_t *lock) {
    /*
    * System 支持抢占。当某线程持有自旋锁时，若本地另一线程正在等待该锁，
    * 就不能将锁持有者从当前 CPU 上调度出去。
    * 禁止抢占状态使用嵌套计数，因此嵌套获取 socket 锁仍然安全。
    */
    sched_preempt_disable();
    spinlock_lock(lock);
}

void socket_unlock(spinlock_t *lock) {
    spinlock_unlock(lock);
    sched_preempt_enable();
}

void socket_initialize_globals(void) {
    if (atomic_load_explicit(&g_socket_init_state, memory_order_acquire) == 2U) return;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_socket_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        socket_zero(g_bindings, sizeof(g_initial_bindings));
        socket_zero(g_tcp_connections, sizeof(g_initial_tcp_connections));
        socket_zero(g_tcp6_connections, sizeof(g_initial_tcp6_connections));
        g_tcp_lookup_hint = g_tcp6_lookup_hint = UINT32_MAX;
        g_udp_lookup_hint = g_udp6_lookup_hint = UINT32_MAX;
        spinlock_init(&g_binding_lock);
        atomic_init(&g_next_ephemeral_port, SOCKET_EPHEMERAL_FIRST);
        atomic_init(&g_next_async_request_id, 0U);
        atomic_store_explicit(&g_socket_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_socket_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

spinlock_t *socket_binding_lock(void) {
    socket_initialize_globals();
    return &g_binding_lock;
}

const socket_binding_t *socket_binding_at(uint32_t index) {
    return index < g_registry_capacity ? &g_bindings[index] : 0;
}

const socket_tcp_connection_t *socket_tcp_connection_at(uint32_t index) {
    return index < g_registry_capacity ? &g_tcp_connections[index] : 0;
}

const socket_tcp6_connection_t *socket_tcp6_connection_at(uint32_t index) {
    return index < g_registry_capacity ? &g_tcp6_connections[index] : 0;
}

uint32_t socket_registry_capacity_locked(void) {
    return g_registry_capacity;
}

uint64_t socket_next_async_request_id(void) {
    socket_initialize_globals();
    return atomic_fetch_add_explicit(&g_next_async_request_id, 1U,
                                     memory_order_relaxed) + 1U;
}

void socket_set_tcp_ipv4_output(socket_tcp_ipv4_output_fn output, void *context) {
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    g_tcp_ipv4_output = output;
    g_tcp_ipv4_output_context = context;
    socket_unlock(&g_binding_lock);
}

void socket_set_tcp_ipv6_output(socket_tcp_ipv6_output_fn output, void *context) {
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    g_tcp_ipv6_output = output;
    g_tcp_ipv6_output_context = context;
    socket_unlock(&g_binding_lock);
}

void socket_set_udp_ipv4_output(socket_udp_ipv4_output_fn output, void *context) {
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    g_udp_ipv4_output = output;
    g_udp_ipv4_output_context = context;
    socket_unlock(&g_binding_lock);
}

void socket_set_udp_ipv6_output(socket_udp_ipv6_output_fn output, void *context) {
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    g_udp_ipv6_output = output;
    g_udp_ipv6_output_context = context;
    socket_unlock(&g_binding_lock);
}

void socket_udp4_output_get(socket_udp_ipv4_output_fn *output,
                            void **context) {
    socket_lock(&g_binding_lock);
    if (output != 0) *output = g_udp_ipv4_output;
    if (context != 0) *context = g_udp_ipv4_output_context;
    socket_unlock(&g_binding_lock);
}

void socket_udp6_output_get(socket_udp_ipv6_output_fn *output,
                            void **context) {
    socket_lock(&g_binding_lock);
    if (output != 0) *output = g_udp_ipv6_output;
    if (context != 0) *context = g_udp_ipv6_output_context;
    socket_unlock(&g_binding_lock);
}

uint64_t socket_tcp_retransmit_deadline(uint64_t now_tsc) {
    uint64_t timeout = x86_timeout_ns_to_tsc(SOCKET_TCP_RETRANSMIT_TIMEOUT_NS);
    return timeout > UINT64_MAX - now_tsc ? UINT64_MAX : now_tsc + timeout;
}

void socket_tcp_output_get(socket_tcp_ipv4_output_fn *output,
                           void **context) {
    socket_lock(&g_binding_lock);
    if (output != 0) *output = g_tcp_ipv4_output;
    if (context != 0) *context = g_tcp_ipv4_output_context;
    socket_unlock(&g_binding_lock);
}

void socket_tcp6_output_get(socket_tcp_ipv6_output_fn *output,
                            void **context) {
    socket_lock(&g_binding_lock);
    if (output != 0) *output = g_tcp_ipv6_output;
    if (context != 0) *context = g_tcp_ipv6_output_context;
    socket_unlock(&g_binding_lock);
}

socket_private_t *socket_private(socket_t *socket) {
    return socket != 0 ? (socket_private_t *)socket->protocol_state : 0;
}

kstatus_t socket_get_info(socket_t *socket, os_socket_info_t *info) {
    socket_private_t *private = socket_private(socket);
    if (socket == 0 || info == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        private == 0) return K_EINVAL;
    socket_lock(&socket->lock);
    info->family = socket->family;
    info->type = socket->type;
    info->protocol = socket->protocol;
    info->reserved = 0U;
    bool stream_connected = private->connected &&
        (socket->type != OS_SOCK_STREAM || private->peer != 0 ||
         private->tcp_state == SOCKET_TCP_ESTABLISHED);
    info->flags = (private->bound ? OS_SOCKET_INFO_BOUND : 0U) |
                  (stream_connected ? OS_SOCKET_INFO_CONNECTED : 0U) |
                  (private->listening ? OS_SOCKET_INFO_LISTENING : 0U);
    info->local_address = private->local_address;
    info->peer_address = private->peer_address;
    socket_copy(info->local_address6, private->local_address6, 16U);
    socket_copy(info->peer_address6, private->peer_address6, 16U);
    info->local_port = private->local_port;
    info->peer_port = private->peer_port;
    info->reserved2 = 0U;
    socket_unlock(&socket->lock);
    return K_OK;
}

static uint64_t socket_tcp_keepalive_deadline(uint64_t now_tsc,
                                             uint32_t seconds) {
    uint64_t ticks = x86_timeout_ns_to_tsc((uint64_t)seconds * 1000000000ULL);
    return ticks > UINT64_MAX - now_tsc ? UINT64_MAX : now_tsc + ticks;
}

void socket_tcp_keepalive_activity_locked(socket_private_t *private,
                                          uint64_t now_tsc) {
    private->tcp_keep_probes = 0U;
    private->tcp_keep_deadline_tsc = private->tcp_keepalive ?
        socket_tcp_keepalive_deadline(now_tsc, private->tcp_keepidle) : 0U;
}

bool socket_tcp_keepalive_poll_locked(socket_private_t *private,
                                      uint64_t now_tsc, bool *failed) {
    if (!private->tcp_keepalive || private->peer != 0 ||
        private->tcp_state != SOCKET_TCP_ESTABLISHED ||
        private->tcp_send_failed || private->tcp_fin_pending ||
        private->tcp_fin_sent ||
        atomic_load_explicit(&private->closed, memory_order_acquire) ||
        atomic_load_explicit(&private->peer_closed, memory_order_acquire)) {
        return false;
    }
    // 尚未确认的数据/FIN 使用现有重传定时器，不消耗保活额度，也不改变序列状态。
    if (private->tcp_send_pending) {
        socket_tcp_keepalive_activity_locked(private, now_tsc);
        return false;
    }
    if (private->tcp_keep_deadline_tsc == 0U) {
        socket_tcp_keepalive_activity_locked(private, now_tsc);
        return false;
    }
    if ((int64_t)(now_tsc - private->tcp_keep_deadline_tsc) < 0) return false;
    if (private->tcp_keep_probes >= private->tcp_keepcnt) {
        socket_tcp_fail_locked(private, K_ETIMEDOUT);
        *failed = true;
        return false;
    }
    ++private->tcp_keep_probes;
    private->tcp_keep_deadline_tsc =
        socket_tcp_keepalive_deadline(now_tsc, private->tcp_keepintvl);
    return true;
}

kstatus_t socket_connect_status(socket_t *socket) {
    socket_private_t *private = socket_private(socket);
    kstatus_t status;
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        private == 0) return K_EINVAL;
    socket_lock(&socket->lock);
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) {
        status = K_EDEVREMOVED;
    } else if (socket->type == OS_SOCK_DGRAM) {
        status = private->connected ? K_OK : K_EINVAL;
    } else if (private->peer != 0 ||
               private->tcp_state == SOCKET_TCP_ESTABLISHED) {
        status = K_OK;
    } else if (private->tcp_send_failed) {
        status = atomic_load_explicit(&private->tcp_error, memory_order_acquire);
        if (status == K_OK) status = K_EIO;
    } else if (private->tcp_state == SOCKET_TCP_SYN_SENT ||
               private->tcp_state == SOCKET_TCP_SYN_RECEIVED) {
        status = K_EAGAIN;
    } else {
        status = K_EINVAL;
    }
    socket_unlock(&socket->lock);
    return status;
}

uint32_t socket_poll_state(socket_t *socket) {
    socket_private_t *private = socket_private(socket);
    uint32_t state = 0U;
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        private == 0) return SOCKET_POLL_ERROR;
    socket_lock(&socket->lock);
    if (atomic_load_explicit(&private->datagram_count, memory_order_acquire) != 0U ||
        atomic_load_explicit(&private->stream_count, memory_order_acquire) != 0U ||
        atomic_load_explicit(&private->accept_count, memory_order_acquire) != 0U ||
        atomic_load_explicit(&private->receive_shutdown, memory_order_acquire)) {
        state |= SOCKET_POLL_READ;
    }
    if (atomic_load_explicit(&private->peer_closed, memory_order_acquire)) {
        state |= SOCKET_POLL_READ | SOCKET_POLL_HUP;
    }
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) {
        state |= SOCKET_POLL_ERROR | SOCKET_POLL_HUP;
    } else if (private->tcp_send_failed) {
        state |= SOCKET_POLL_ERROR;
    } else if (!atomic_load_explicit(&private->send_shutdown,
                                      memory_order_acquire)) {
        if (socket->type == OS_SOCK_DGRAM) {
            state |= SOCKET_POLL_WRITE;
        } else if (!private->listening && private->peer != 0) {
            socket_private_t *peer = socket_private(private->peer);
            if (peer != 0 &&
                atomic_load_explicit(&peer->stream_count,
                                     memory_order_acquire) <
                    socket_stream_capacity(peer)) {
                state |= SOCKET_POLL_WRITE;
            }
        } else if (!private->listening &&
                   private->tcp_state == SOCKET_TCP_ESTABLISHED &&
                   socket_tcp_send_room_locked(socket->family, private) != 0U) {
            state |= SOCKET_POLL_WRITE;
        }
    }
    socket_unlock(&socket->lock);
    return state;
}

kstatus_t socket_get_option(socket_t *socket, uint32_t option, int32_t *value) {
    socket_private_t *private = socket_private(socket);
    if (socket == 0 || value == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        private == 0) return K_EINVAL;
    if ((option == OS_SOCKET_OPTION_KEEPIDLE ||
         option == OS_SOCKET_OPTION_KEEPINTVL || option == OS_SOCKET_OPTION_KEEPCNT) &&
        (socket->type != OS_SOCK_STREAM || socket->protocol != SOCKET_PROTOCOL_TCP))
        return K_EINVAL;
    socket_lock(&socket->lock);
    switch (option) {
    case OS_SOCKET_OPTION_REUSE_ADDRESS:
        *value = atomic_load_explicit(&private->reuse_address,
                                      memory_order_acquire);
        break;
    case OS_SOCKET_OPTION_ERROR:
        *value = -atomic_load_explicit(&private->tcp_error, memory_order_acquire);
        if (*value == 0 && private->tcp_send_failed) *value = 5;
        break;
    case OS_SOCKET_OPTION_KEEPALIVE: *value = private->tcp_keepalive; break;
    case OS_SOCKET_OPTION_KEEPIDLE: *value = (int32_t)private->tcp_keepidle; break;
    case OS_SOCKET_OPTION_KEEPINTVL: *value = (int32_t)private->tcp_keepintvl; break;
    case OS_SOCKET_OPTION_KEEPCNT: *value = (int32_t)private->tcp_keepcnt; break;
    case OS_SOCKET_OPTION_TYPE: *value = socket->type; break;
    case OS_SOCKET_OPTION_DOMAIN: *value = socket->family; break;
    case OS_SOCKET_OPTION_PROTOCOL: *value = socket->protocol; break;
    case OS_SOCKET_OPTION_ACCEPT_CONNECTION: *value = private->listening; break;
    case OS_SOCKET_OPTION_RECEIVE_BUFFER:
        *value = socket->type == OS_SOCK_STREAM ? socket_stream_capacity(private) :
                 SOCKET_RX_QUEUE_DEPTH * SOCKET_MAX_PAYLOAD;
        break;
    case OS_SOCKET_OPTION_SEND_BUFFER: *value = SOCKET_STREAM_BUFFER_SIZE; break;
    default:
        socket_unlock(&socket->lock);
        return K_EINVAL;
    }
    socket_unlock(&socket->lock);
    return K_OK;
}

kstatus_t socket_set_option(socket_t *socket, uint32_t option, int32_t value) {
    socket_private_t *private = socket_private(socket);
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET || private == 0)
        return K_EINVAL;
    socket_lock(&socket->lock);
    switch (option) {
    case OS_SOCKET_OPTION_REUSE_ADDRESS:
        atomic_store_explicit(&private->reuse_address, value != 0,
                              memory_order_release);
        break;
    case OS_SOCKET_OPTION_KEEPALIVE:
        private->tcp_keepalive = value != 0;
        socket_tcp_keepalive_activity_locked(private, x86_read_tsc());
        break;
    case OS_SOCKET_OPTION_KEEPIDLE:
    case OS_SOCKET_OPTION_KEEPINTVL:
    case OS_SOCKET_OPTION_KEEPCNT:
        if (socket->type != OS_SOCK_STREAM ||
            socket->protocol != SOCKET_PROTOCOL_TCP || value <= 0 ||
            value > (option == OS_SOCKET_OPTION_KEEPCNT ? 127 : 32767)) {
            socket_unlock(&socket->lock);
            return K_EINVAL;
        }
        if (option == OS_SOCKET_OPTION_KEEPIDLE) private->tcp_keepidle = (uint32_t)value;
        if (option == OS_SOCKET_OPTION_KEEPINTVL) private->tcp_keepintvl = (uint32_t)value;
        if (option == OS_SOCKET_OPTION_KEEPCNT) private->tcp_keepcnt = (uint32_t)value;
        socket_tcp_keepalive_activity_locked(private, x86_read_tsc());
        break;
    default:
        socket_unlock(&socket->lock);
        return K_EINVAL;
    }
    socket_unlock(&socket->lock);
    return K_OK;
}

const socket_private_t *socket_private_const(const socket_t *socket) {
    return socket != 0 ? (const socket_private_t *)socket->protocol_state : 0;
}

static bool socket_is_signaled(const void *object) {
    const socket_t *socket = (const socket_t *)object;
    const socket_private_t *private = socket_private_const(socket);
    if (private == 0) return false;
    return atomic_load_explicit(&private->datagram_count, memory_order_acquire) != 0U ||
           atomic_load_explicit(&private->stream_count, memory_order_acquire) != 0U ||
           atomic_load_explicit(&private->accept_count, memory_order_acquire) != 0U ||
           atomic_load_explicit(&private->receive_shutdown, memory_order_acquire) ||
           atomic_load_explicit(&private->closed, memory_order_acquire) ||
           atomic_load_explicit(&private->peer_closed, memory_order_acquire) ||
           atomic_load_explicit(&private->tcp_error, memory_order_acquire) != 0;
}

static int64_t socket_wait_value(const void *object) {
    const socket_t *socket = (const socket_t *)object;
    const socket_private_t *private = socket_private_const(socket);
    if (private == 0) return K_EIO;
    if (socket->type == OS_SOCK_STREAM && private->listening) {
        return (int64_t)atomic_load_explicit(&private->accept_count, memory_order_acquire);
    }
    return socket->type == OS_SOCK_STREAM ?
           (int64_t)atomic_load_explicit(&private->stream_count, memory_order_acquire) :
           (int64_t)atomic_load_explicit(&private->datagram_count, memory_order_acquire);
}

static void socket_unbind_locked(socket_t *socket) {
    g_udp_lookup_hint = g_udp6_lookup_hint = UINT32_MAX;
    for (uint32_t index = 0; index < g_registry_capacity; ++index) {
        if (g_bindings[index].socket == socket) {
            socket_zero(&g_bindings[index], sizeof(g_bindings[index]));
        }
    }
}

bool socket_register_tcp_locked(socket_t *socket, socket_private_t *private) {
    uint32_t slot = g_registry_capacity;
    if (socket == 0 || private == 0 || socket->type != OS_SOCK_STREAM ||
        !private->bound || !private->connected || private->peer_port == 0U) return false;
    g_tcp_lookup_hint = g_tcp6_lookup_hint = UINT32_MAX;
    if (socket->family == OS_AF_INET6) {
        for (uint32_t index = 0; index < g_registry_capacity; ++index) {
            if (g_tcp6_connections[index].socket == socket) return true;
            if (g_tcp6_connections[index].socket == 0 && slot == g_registry_capacity) {
                slot = index;
            }
        }
        if (slot == g_registry_capacity) return false;
        g_tcp6_connections[slot].socket = socket;
        socket_copy(g_tcp6_connections[slot].local_address, private->local_address6, 16U);
        g_tcp6_connections[slot].local_port = private->local_port;
        socket_copy(g_tcp6_connections[slot].peer_address, private->peer_address6, 16U);
        g_tcp6_connections[slot].peer_port = private->peer_port;
        return true;
    }
    if (socket->family != OS_AF_INET4) return false;
    for (uint32_t index = 0; index < g_registry_capacity; ++index) {
        if (g_tcp_connections[index].socket == socket) return true;
        if (g_tcp_connections[index].socket == 0 && slot == g_registry_capacity) {
            slot = index;
        }
    }
    if (slot == g_registry_capacity) return false;
    g_tcp_connections[slot].socket = socket;
    g_tcp_connections[slot].local_address = private->local_address;
    g_tcp_connections[slot].local_port = private->local_port;
    g_tcp_connections[slot].peer_address = private->peer_address;
    g_tcp_connections[slot].peer_port = private->peer_port;
    return true;
}

static uint32_t socket_unregister_tcp_locked(socket_t *socket) {
    uint32_t released = 0U;
    g_tcp_lookup_hint = g_tcp6_lookup_hint = UINT32_MAX;
    for (uint32_t index = 0; index < g_registry_capacity; ++index) {
        if (g_tcp_connections[index].socket != socket) continue;
        socket_zero(&g_tcp_connections[index], sizeof(g_tcp_connections[index]));
        ++released;
    }
    for (uint32_t index = 0; index < g_registry_capacity; ++index) {
        if (g_tcp6_connections[index].socket != socket) continue;
        socket_zero(&g_tcp6_connections[index], sizeof(g_tcp6_connections[index]));
        ++released;
    }
    return released;
}

static bool socket_tcp_matches(const socket_tcp_connection_t *connection,
                                uint32_t source, uint16_t source_port,
                                uint32_t destination, uint16_t destination_port) {
    return connection->socket != 0 && connection->local_port == destination_port &&
           connection->peer_port == source_port &&
           (connection->local_address == 0U || connection->local_address == destination) &&
           connection->peer_address == source;
}

static bool socket_tcp6_matches(const socket_tcp6_connection_t *connection,
                                 const uint8_t source[16], uint16_t source_port,
                                 const uint8_t destination[16], uint16_t destination_port) {
    return connection->socket != 0 && connection->local_port == destination_port &&
           connection->peer_port == source_port &&
           (socket_address6_zero(connection->local_address) ||
            socket_address6_equal(connection->local_address, destination)) &&
           socket_address6_equal(connection->peer_address, source);
}

bool socket_tcp_slots_available_locked(uint32_t required) {
    if (required == 0U) return true;
    if (required > g_registry_capacity) return false;
    uint32_t free_slots = 0U;
    for (uint32_t index = 0; index < g_registry_capacity; ++index) {
        if (g_tcp_connections[index].socket == 0 && ++free_slots == required) return true;
    }
    return false;
}

bool socket_tcp6_slots_available_locked(uint32_t required) {
    if (required == 0U) return true;
    if (required > g_registry_capacity) return false;
    uint32_t free_slots = 0U;
    for (uint32_t index = 0; index < g_registry_capacity; ++index) {
        if (g_tcp6_connections[index].socket == 0 && ++free_slots == required) return true;
    }
    return false;
}

int32_t socket_find_tcp6_locked(const uint8_t source_address[16],
                                       uint16_t source_port,
                                       const uint8_t destination_address[16],
                                       uint16_t destination_port) {
    if (g_tcp6_lookup_hint < g_registry_capacity &&
        socket_tcp6_matches(&g_tcp6_connections[g_tcp6_lookup_hint],
                            source_address, source_port,
                            destination_address, destination_port))
        return (int32_t)g_tcp6_lookup_hint;
    for (uint32_t index = 0; index < g_registry_capacity; ++index) {
        const socket_tcp6_connection_t *connection = &g_tcp6_connections[index];
        if (!socket_tcp6_matches(connection, source_address, source_port,
                                destination_address, destination_port)) continue;
        g_tcp6_lookup_hint = index;
        return (int32_t)index;
    }
    return -1;
}

int32_t socket_find_tcp_locked(uint32_t source_address, uint16_t source_port,
                                      uint32_t destination_address,
                                      uint16_t destination_port) {
    if (g_tcp_lookup_hint < g_registry_capacity &&
        socket_tcp_matches(&g_tcp_connections[g_tcp_lookup_hint],
                           source_address, source_port,
                           destination_address, destination_port))
        return (int32_t)g_tcp_lookup_hint;
    for (uint32_t index = 0; index < g_registry_capacity; ++index) {
        const socket_tcp_connection_t *connection = &g_tcp_connections[index];
        if (!socket_tcp_matches(connection, source_address, source_port,
                               destination_address, destination_port)) continue;
        g_tcp_lookup_hint = index;
        return (int32_t)index;
    }
    return -1;
}

static void socket_lock_pair(socket_t *left, socket_t *right) {
    if (left == right) {
        socket_lock(&left->lock);
        return;
    }
    if ((uintptr_t)left < (uintptr_t)right) {
        socket_lock(&left->lock);
        socket_lock(&right->lock);
    } else {
        socket_lock(&right->lock);
        socket_lock(&left->lock);
    }
}

static void socket_unlock_pair(socket_t *left, socket_t *right) {
    if (left == right) {
        socket_unlock(&left->lock);
        return;
    }
    if ((uintptr_t)left < (uintptr_t)right) {
        socket_unlock(&right->lock);
        socket_unlock(&left->lock);
    } else {
        socket_unlock(&left->lock);
        socket_unlock(&right->lock);
    }
}

static void socket_detach_peer(socket_t *socket, socket_private_t *private) {
    socket_t *peer;
    if (socket == 0 || private == 0) return;
    socket_lock(&socket->lock);
    peer = private->peer;
    private->peer = 0;
    socket_unlock(&socket->lock);
    if (peer == 0) return;

    /* 娑撱倗顏幐澶婃勾閸р偓閹烘帒绨崝鐘绘敚閿涘矂浼╅崗宥呮倱閺冭泛鍙ч梻顓＄箾閹恒儲妞傝ぐ銏″灇 ABBA 濮濆鏀ｉ妴?*/
    socket_lock_pair(socket, peer);
    socket_private_t *peer_private = socket_private(peer);
    if (peer_private != 0 && peer_private->peer == socket) {
        peer_private->peer = 0;
        atomic_store_explicit(&peer_private->peer_closed, true, memory_order_release);
    }
    socket_unlock_pair(socket, peer);
    (void)wake_all(&peer->waitq);
    object_notify_signaled(peer);
    object_put(peer);
}

/* 发布每个 socket 前，先在每个注册表中预留一个槽位。一个 socket 最多占用一个绑定项
 * 和一个 TCP 地址族项，因此可按活动对象预留空间，后续 bind/connect 时无需在自旋锁下
 * 分配内存。读取方仅在持有 g_binding_lock 时访问这些视图。 */
static uint32_t g_registry_reservations;
static void *g_registry_storage;

static kstatus_t socket_registry_reserve(void) {
    void *replacement = 0;
    uint32_t capacity = 0U;
    socket_initialize_globals();
    for (;;) {
        socket_lock(&g_binding_lock);
        if (g_registry_reservations < g_registry_capacity) {
            ++g_registry_reservations;
            socket_unlock(&g_binding_lock);
            kfree(replacement);
            return K_OK;
        }
        if (replacement != 0 && capacity > g_registry_capacity) {
            socket_binding_t *bindings = replacement;
            socket_tcp_connection_t *tcp = (void *)(bindings + capacity);
            socket_tcp6_connection_t *tcp6 = (void *)(tcp + capacity);
            socket_copy(bindings, g_bindings, g_registry_capacity * sizeof(*bindings));
            socket_copy(tcp, g_tcp_connections, g_registry_capacity * sizeof(*tcp));
            socket_copy(tcp6, g_tcp6_connections, g_registry_capacity * sizeof(*tcp6));
            void *old = g_registry_storage;
            g_registry_storage = replacement;
            g_bindings = bindings;
            g_tcp_connections = tcp;
            g_tcp6_connections = tcp6;
            g_registry_capacity = capacity;
            ++g_registry_reservations;
            socket_unlock(&g_binding_lock);
            kfree(old);
            return K_OK;
        }
        /* 查找 API 使用有符号注册表索引。扩容前同时检查该上限和分配大小计算。 */
        size_t stride = sizeof(socket_binding_t) +
                        sizeof(socket_tcp_connection_t) + sizeof(socket_tcp6_connection_t);
        if (g_registry_capacity > INT32_MAX / 2U ||
            (size_t)g_registry_capacity > SIZE_MAX / stride / 2U) {
            socket_unlock(&g_binding_lock);
            kfree(replacement);
            return K_ENOMEM;
        }
        capacity = g_registry_capacity * 2U;
        socket_unlock(&g_binding_lock);
        kfree(replacement);
        replacement = kzalloc((size_t)capacity * stride, 0U);
        if (replacement == 0) return K_ENOMEM;
    }
}

static void socket_registry_release(void) {
    void *old = 0;
    socket_lock(&g_binding_lock);
    --g_registry_reservations;
    if (g_registry_reservations == 0U && g_registry_storage != 0) {
        old = g_registry_storage;
        g_registry_storage = 0;
        g_bindings = g_initial_bindings;
        g_tcp_connections = g_initial_tcp_connections;
        g_tcp6_connections = g_initial_tcp6_connections;
        g_registry_capacity = SOCKET_BINDING_COUNT;
        g_udp_lookup_hint = g_udp6_lookup_hint = UINT32_MAX;
        socket_zero(g_bindings, sizeof(g_initial_bindings));
        socket_zero(g_tcp_connections, sizeof(g_initial_tcp_connections));
        socket_zero(g_tcp6_connections, sizeof(g_initial_tcp6_connections));
    }
    socket_unlock(&g_binding_lock);
    kfree(old);
}

static void socket_destroy(void *object) {
    socket_t *socket = (socket_t *)object;
    if (socket == 0) return;
    socket_private_t *private = socket_private(socket);
    (void)socket_close(socket);
    socket_registry_release();
    if (private != 0 && private->stream_dynamic != 0)
        vfree(private->stream_dynamic);
    kfree(private);
    kfree(socket);
}

static void socket_handle_close(void *object) {
    (void)socket_close((socket_t *)object);
}

static const object_ops_t g_socket_object_ops = {
    .destroy = socket_destroy,
    .handle_close = socket_handle_close,
    .type_name = "Socket",
    .is_signaled = socket_is_signaled,
    .wait_value = socket_wait_value,
};

int32_t socket_find_binding_locked(uint32_t address, uint16_t port,
                                          uint16_t type, bool listening_only) {
    int32_t wildcard = -1;
    bool cacheable = type == OS_SOCK_DGRAM && !listening_only;
    if (cacheable && g_udp_lookup_hint < g_registry_capacity) {
        const socket_binding_t *binding = &g_bindings[g_udp_lookup_hint];
        if (binding->socket != 0 && binding->family == OS_AF_INET4 &&
            binding->type == type && binding->port == port &&
            binding->address == address) return (int32_t)g_udp_lookup_hint;
    }
    for (uint32_t index = 0; index < g_registry_capacity; ++index) {
        const socket_binding_t *binding = &g_bindings[index];
        const socket_private_t *private;
        if (binding->socket == 0 || binding->family != OS_AF_INET4 ||
            binding->port != port || binding->type != type) continue;
        private = socket_private_const(binding->socket);
        if (listening_only && (private == 0 || !private->listening)) continue;
        if (binding->address == address) {
            if (cacheable) g_udp_lookup_hint = index;
            return (int32_t)index;
        }
        if (binding->address == 0U) wildcard = (int32_t)index;
    }
    return wildcard;
}

int32_t socket_find_binding6_locked(const uint8_t address[16], uint16_t port,
                                           uint16_t type, bool listening_only) {
    int32_t wildcard = -1;
    bool cacheable = type == OS_SOCK_DGRAM && !listening_only;
    if (cacheable && g_udp6_lookup_hint < g_registry_capacity) {
        const socket_binding_t *binding = &g_bindings[g_udp6_lookup_hint];
        if (binding->socket != 0 && binding->family == OS_AF_INET6 &&
            binding->type == type && binding->port == port &&
            socket_address6_equal(binding->address6, address)) return (int32_t)g_udp6_lookup_hint;
    }
    for (uint32_t index = 0; index < g_registry_capacity; ++index) {
        const socket_binding_t *binding = &g_bindings[index];
        const socket_private_t *private;
        if (binding->socket == 0 || binding->family != OS_AF_INET6 ||
            binding->port != port || binding->type != type) continue;
        private = socket_private_const(binding->socket);
        if (listening_only && (private == 0 || !private->listening)) continue;
        if (socket_address6_equal(binding->address6, address)) {
            if (cacheable) g_udp6_lookup_hint = index;
            return (int32_t)index;
        }
        if (socket_address6_zero(binding->address6)) wildcard = (int32_t)index;
    }
    return wildcard;
}

static bool socket_port_available_locked(uint32_t address, uint16_t port,
                                         uint16_t type, const socket_t *self) {
    for (uint32_t index = 0; index < g_registry_capacity; ++index) {
        const socket_binding_t *binding = &g_bindings[index];
        if (binding->socket == 0 || binding->socket == self ||
            binding->family != OS_AF_INET4 || binding->port != port ||
            binding->type != type) continue;
        if (binding->address == 0U || address == 0U || binding->address == address) {
            return false;
        }
    }
    return true;
}

static bool socket_port6_available_locked(const uint8_t address[16], uint16_t port,
                                          uint16_t type, const socket_t *self) {
    bool wildcard = socket_address6_zero(address);
    for (uint32_t index = 0; index < g_registry_capacity; ++index) {
        const socket_binding_t *binding = &g_bindings[index];
        if (binding->socket == 0 || binding->socket == self ||
            binding->family != OS_AF_INET6 || binding->port != port ||
            binding->type != type) continue;
        if (wildcard || socket_address6_zero(binding->address6) ||
            socket_address6_equal(binding->address6, address)) return false;
    }
    return true;
}

static kstatus_t socket_bind_locked(socket_t *socket, socket_private_t *private,
                                    uint32_t address, uint16_t port) {
    uint32_t slot = g_registry_capacity;
    if (private->bound || !socket_port_available_locked(address, port, socket->type, socket)) {
        return K_EBUSY;
    }
    for (uint32_t index = 0; index < g_registry_capacity; ++index) {
        if (g_bindings[index].socket == 0) {
            slot = index;
            break;
        }
    }
    if (slot == g_registry_capacity) return K_ENOMEM;
    g_udp_lookup_hint = g_udp6_lookup_hint = UINT32_MAX;
    g_bindings[slot].socket = socket;
    g_bindings[slot].family = OS_AF_INET4;
    g_bindings[slot].address = address;
    g_bindings[slot].port = port;
    g_bindings[slot].type = socket->type;
    private->bound = true;
    private->local_address = address;
    private->local_port = port;
    return K_OK;
}

static kstatus_t socket_bind6_locked(socket_t *socket, socket_private_t *private,
                                     const uint8_t address[16], uint16_t port) {
    uint32_t slot = g_registry_capacity;
    if (private->bound || !socket_port6_available_locked(address, port, socket->type, socket)) {
        return K_EBUSY;
    }
    for (uint32_t index = 0; index < g_registry_capacity; ++index) {
        if (g_bindings[index].socket == 0) {
            slot = index;
            break;
        }
    }
    if (slot == g_registry_capacity) return K_ENOMEM;
    g_udp_lookup_hint = g_udp6_lookup_hint = UINT32_MAX;
    g_bindings[slot].socket = socket;
    g_bindings[slot].family = OS_AF_INET6;
    socket_copy(g_bindings[slot].address6, address, 16U);
    g_bindings[slot].port = port;
    g_bindings[slot].type = socket->type;
    private->bound = true;
    socket_copy(private->local_address6, address, 16U);
    private->local_port = port;
    return K_OK;
}

/* 调用方持有 g_binding_lock。保留显式通配符/回环绑定；只有 connect 隐式绑定时才选择活动网卡的源地址。 */
static kstatus_t socket_bind_port_locked(socket_t *socket,
                                         socket_private_t *private,
                                         uint32_t address4,
                                         const uint8_t address6[16],
                                         uint16_t requested_port) {
    if (atomic_load_explicit(&private->closed, memory_order_acquire))
        return K_EDEVREMOVED;
    if (private->bound) return K_EBUSY;
    if (requested_port != 0U) {
        return socket->family == OS_AF_INET4 ?
            socket_bind_locked(socket, private, address4, requested_port) :
            socket_bind6_locked(socket, private, address6, requested_port);
    }
    uint32_t first = atomic_fetch_add_explicit(&g_next_ephemeral_port, 1U,
                                               memory_order_relaxed);
    uint32_t span = SOCKET_EPHEMERAL_LAST - SOCKET_EPHEMERAL_FIRST + 1U;
    kstatus_t status = K_EAGAIN;
    for (uint32_t attempt = 0; attempt < span; ++attempt) {
        uint16_t port = (uint16_t)(SOCKET_EPHEMERAL_FIRST +
            ((first - SOCKET_EPHEMERAL_FIRST + attempt) % span));
        status = socket->family == OS_AF_INET4 ?
            socket_bind_locked(socket, private, address4, port) :
            socket_bind6_locked(socket, private, address6, port);
        if (status != K_EBUSY) break;
    }
    return status;
}

static kstatus_t socket_bind_ephemeral(socket_t *socket, socket_private_t *private) {
    uint32_t local_address = socket_active_ipv4_address();
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    kstatus_t status = socket_bind_port_locked(socket, private, local_address, 0, 0U);
    socket_unlock(&g_binding_lock);
    return status;
}

/* 当前 IPv6 路由仅支持回环、接口自身和链路本地邻居。尚未实现 RA/默认路由，因此不能仅因 UDP connect
 * 不发送数据包，就宣称存在互联网路由或选择 ::1 作为源地址。这样也能保证可达性探测结果真实。 */
static kstatus_t socket_ipv6_connect_source(const uint8_t destination[16],
                                            uint8_t source[16]) {
    static const uint8_t loopback[16] = {[15] = 1U};
    if (socket_address6_equal(destination, loopback)) {
        socket_copy(source, loopback, 16U);
        return K_OK;
    }
    net_manager_status_t network = {0};
    if (!net_manager_get_status(&network) || !network.link_up ||
        !network.ipv6_configured || socket_address6_zero(network.ipv6_address))
        return K_ENETUNREACH;
    bool local = socket_address6_equal(destination, network.ipv6_address);
    bool neighbour = destination[0] == 0xfeU && (destination[1] & 0xc0U) == 0x80U &&
                     network.ipv6_address[0] == 0xfeU &&
                     (network.ipv6_address[1] & 0xc0U) == 0x80U;
    if (!local && !neighbour) return K_ENETUNREACH;
    socket_copy(source, network.ipv6_address, 16U);
    return K_OK;
}

static kstatus_t socket_bind6_ephemeral(socket_t *socket, socket_private_t *private,
                                        const uint8_t source[16]) {
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    kstatus_t status = socket_bind_port_locked(socket, private, 0U, source, 0U);
    socket_unlock(&g_binding_lock);
    return status;
}

static kstatus_t socket_stream_resize_status(const socket_private_t *private) {
    if (atomic_load_explicit(&private->closed, memory_order_acquire))
        return K_EDEVREMOVED;
    if (private->connected || private->listening || private->peer != 0 ||
        private->tcp_state != SOCKET_TCP_CLOSED ||
        atomic_load_explicit(&private->stream_count, memory_order_relaxed) != 0U ||
        private->tcp_rx_future_count != 0U || private->tcp_rx_fin_pending)
        return K_EBUSY;
    return K_OK;
}

kstatus_t socket_stream_reserve(socket_t *socket, uint32_t capacity) {
    socket_private_t *private = socket_private(socket);
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET || private == 0 ||
        socket->type != OS_SOCK_STREAM ||
        (socket->family != OS_AF_INET4 && socket->family != OS_AF_INET6) ||
        capacity < SOCKET_STREAM_BUFFER_SIZE || capacity > SOCKET_STREAM_MAX_CAPACITY ||
        (capacity & (capacity - 1U)) != 0U) return K_EINVAL;
    socket_lock(&socket->lock);
    kstatus_t status = socket_stream_resize_status(private);
    bool sufficient = capacity <= socket_stream_capacity(private);
    socket_unlock(&socket->lock);
    if (status != K_OK || sufficient) return status;

    /* 使用非连续物理页可避免高阶 Buddy 分配压力。只需清除所有权位；未读取的负载绝不会被发布。 */
    uint8_t *storage = vmalloc((size_t)capacity + (capacity + 7U) / 8U);
    if (storage == 0) return K_ENOMEM;
    memset(storage + capacity, 0, (capacity + 7U) / 8U);

    uint8_t *retired = storage;
    socket_lock(&socket->lock);
    status = socket_stream_resize_status(private);
    if (status == K_OK && capacity > socket_stream_capacity(private)) {
        retired = private->stream_dynamic;
        private->stream_dynamic = storage;
        private->stream_capacity = capacity;
        private->tcp_rx_present = storage + capacity;
        private->stream_read = private->stream_write = 0U;
    }
    socket_unlock(&socket->lock);
    if (retired != 0) vfree(retired);
    return status;
}

kstatus_t socket_allocate(uint16_t family, uint16_t type, uint16_t protocol,
                                  socket_t **out) {
    socket_t *socket = (socket_t *)kzalloc(sizeof(*socket), 0);
    /* 小对象分配已经失败后，不要再尝试大型分配。尤其是在无法发布 socket 时，OOM 路径不得进入私有
     * 接收缓冲区的分配/回收流程。 */
    if (socket == 0) return K_ENOMEM;
    size_t private_size = sizeof(socket_private_t);
    if (type == OS_SOCK_DGRAM)
        private_size += sizeof(socket_datagram_t) * SOCKET_RX_QUEUE_DEPTH;
    bool reassembly = type == OS_SOCK_STREAM &&
        (family == OS_AF_INET4 || family == OS_AF_INET6);
    if (reassembly)
        private_size += (SOCKET_STREAM_BUFFER_SIZE + 7U) / 8U;
    socket_private_t *private = (socket_private_t *)kmalloc(private_size, 0);
    if (private == 0) {
        kfree(socket);
        return K_ENOMEM;
    }
    socket_zero(private, offsetof(socket_private_t, stream_buffer));
    kstatus_t reservation = socket_registry_reserve();
    if (reservation != K_OK) {
        kfree(private);
        kfree(socket);
        return reservation;
    }
    /* 重用分配区域的尾部，而非另行分配整页。位图与 TCP 缓冲区合计仍可放入同一 Buddy 阶。 */
    if (reassembly) {
        private->tcp_rx_present = (uint8_t *)(private + 1);
        memset(private->tcp_rx_present, 0,
               (SOCKET_STREAM_BUFFER_SIZE + 7U) / 8U);
    }
    object_header_init(&socket->object, KOBJECT_TYPE_SOCKET,
                       &g_socket_object_ops);
    socket->family = family;
    socket->type = type;
    socket->protocol = protocol;
    wait_queue_init(&socket->waitq);
    spinlock_init(&socket->lock);
    atomic_init(&private->datagram_count, 0U);
    atomic_init(&private->stream_count, 0U);
    atomic_init(&private->accept_count, 0U);
    atomic_init(&private->closed, false);
    atomic_init(&private->peer_closed, false);
    atomic_init(&private->receive_shutdown, false);
    atomic_init(&private->send_shutdown, false);
    atomic_init(&private->reuse_address, false);
    atomic_init(&private->tcp_error, 0);
    private->tcp_keepidle = 7200U;
    private->tcp_keepintvl = 75U;
    private->tcp_keepcnt = 9U;
    socket->rx_queue = private;
    socket->tx_queue = 0;
    socket->protocol_state = private;
    *out = socket;
    return K_OK;
}

kstatus_t socket_shutdown(socket_t *socket, uint32_t how) {
    socket_private_t *private;
    socket_t *peer = 0;
    bool notify_self = false;
    bool notify_peer = false;
    bool kick_transport = false;

    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        how > OS_SOCKET_SHUT_BOTH) return K_EINVAL;
    private = socket_private(socket);
    if (private == 0) return K_EIO;

    socket_lock(&socket->lock);
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) {
        socket_unlock(&socket->lock);
        return K_EDEVREMOVED;
    }

    if (how == OS_SOCKET_SHUT_READ || how == OS_SOCKET_SHUT_BOTH) {
        if (private->tcp_rx_present != 0)
            memset(private->tcp_rx_present, 0, (socket_stream_capacity(private) + 7U) / 8U);
        private->tcp_rx_fin_pending = false;
        private->tcp_rx_future_count = 0U;
        atomic_store_explicit(&private->receive_shutdown, true,
                              memory_order_release);
        private->datagram_read = private->datagram_write = 0U;
        private->stream_read = private->stream_write = 0U;
        atomic_store_explicit(&private->datagram_count, 0U,
                              memory_order_release);
        atomic_store_explicit(&private->stream_count, 0U,
                              memory_order_release);
        notify_self = true;
    }

    if (how == OS_SOCKET_SHUT_WRITE || how == OS_SOCKET_SHUT_BOTH) {
        atomic_store_explicit(&private->send_shutdown, true,
                              memory_order_release);
        notify_self = true;
        if (socket->type == OS_SOCK_STREAM && private->peer == 0 &&
            private->connected && private->tcp_state != SOCKET_TCP_CLOSED &&
            !private->tcp_fin_sent) {
            private->tcp_fin_pending = true;
            kick_transport = true;
        }
    }

    /* 向本地对端发布半关闭状态时保持描述符有效。只有释放当前锁后，才获取对端自身的锁。 */
    if (socket->type == OS_SOCK_STREAM && private->peer != 0 &&
        object_try_get(private->peer)) {
        peer = private->peer;
        notify_peer = true;
    }
    socket_unlock(&socket->lock);

    if (notify_self) {
        (void)wake_all(&socket->waitq);
        object_notify_signaled(socket);
    }
    if (peer != 0) {
        socket_private_t *peer_private = socket_private(peer);
        if (peer_private != 0) {
            socket_lock(&peer->lock);
            if (peer_private->peer == socket) {
                if (how == OS_SOCKET_SHUT_WRITE || how == OS_SOCKET_SHUT_BOTH) {
                    atomic_store_explicit(&peer_private->peer_closed, true,
                                          memory_order_release);
                }
            }
            socket_unlock(&peer->lock);
            if (notify_peer) {
                (void)wake_all(&peer->waitq);
                object_notify_signaled(peer);
            }
        }
        object_put(peer);
    }
    if (kick_transport) socket_tcp_poll(x86_read_tsc());
    return K_OK;
}

kstatus_t socket_create(uint16_t family, uint16_t type, uint16_t protocol,
                        socket_t **out) {
    if (out == 0 || (family != OS_AF_INET4 && family != OS_AF_INET6) ||
        (type != OS_SOCK_DGRAM && type != OS_SOCK_STREAM) ||
        (protocol != 0U && protocol != SOCKET_PROTOCOL_UDP &&
         protocol != SOCKET_PROTOCOL_TCP) ||
        (type == OS_SOCK_DGRAM && protocol == SOCKET_PROTOCOL_TCP) ||
        (type == OS_SOCK_STREAM && protocol == SOCKET_PROTOCOL_UDP)) return K_EINVAL;
    socket_initialize_globals();
    return socket_allocate(family, type,
                           protocol == 0U ? (type == OS_SOCK_STREAM ?
                                             SOCKET_PROTOCOL_TCP : SOCKET_PROTOCOL_UDP) : protocol,
                           out);
}

kstatus_t socket_bind(socket_t *socket, uint32_t address, uint16_t port) {
    socket_private_t *private;
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET4) {
        return K_EINVAL;
    }
    private = socket_private(socket);
    if (private == 0 || atomic_load_explicit(&private->closed, memory_order_acquire)) {
        return K_EDEVREMOVED;
    }
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    kstatus_t status = socket_bind_port_locked(socket, private, address, 0, port);
    socket_unlock(&g_binding_lock);
    return status;
}

kstatus_t socket_bind_ipv6(socket_t *socket, const uint8_t address[16], uint16_t port) {
    socket_private_t *private;
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET6 || address == 0) return K_EINVAL;
    private = socket_private(socket);
    if (private == 0 || atomic_load_explicit(&private->closed, memory_order_acquire)) {
        return K_EDEVREMOVED;
    }
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    kstatus_t status = socket_bind_port_locked(socket, private, 0U, address, port);
    socket_unlock(&g_binding_lock);
    return status;
}

void socket_abort_child(socket_t *child) {
    (void)socket_close(child);
    object_put(child);
}

static void socket_remove_from_accept_queue_locked(socket_t *listener,
                                                   socket_t *child) {
    socket_private_t *private = socket_private(listener);
    uint32_t count;
    uint32_t offset;
    if (private == 0 || child == 0) return;
    count = atomic_load_explicit(&private->accept_count, memory_order_relaxed);
    for (offset = 0U; offset < count; ++offset) {
        uint32_t index = (private->accept_read + offset) % SOCKET_LISTEN_BACKLOG;
        if (private->accept_queue[index] != child) continue;
        for (uint32_t shift = offset; shift + 1U < count; ++shift) {
            uint32_t from = (private->accept_read + shift + 1U) % SOCKET_LISTEN_BACKLOG;
            uint32_t to = (private->accept_read + shift) % SOCKET_LISTEN_BACKLOG;
            private->accept_queue[to] = private->accept_queue[from];
        }
        uint32_t last = (private->accept_read + count - 1U) % SOCKET_LISTEN_BACKLOG;
        private->accept_queue[last] = 0;
        private->accept_write = last;
        atomic_store_explicit(&private->accept_count, count - 1U, memory_order_release);
        return;
    }
}

kstatus_t socket_close(socket_t *socket) {
    socket_private_t *private;
    socket_t *accepted[SOCKET_LISTEN_BACKLOG];
    socket_t *listener_ref = 0;
    uint32_t accepted_count = 0;
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET) return K_EINVAL;
    private = socket_private(socket);
    if (private == 0) return K_EIO;
    socket_initialize_globals();

    socket_lock(&socket->lock);
    bool was_closed = atomic_exchange_explicit(&private->closed, true,
                                               memory_order_acq_rel);
    while (!was_closed &&
           atomic_load_explicit(&private->accept_count, memory_order_relaxed) != 0U &&
           accepted_count < SOCKET_LISTEN_BACKLOG) {
        accepted[accepted_count++] = private->accept_queue[private->accept_read];
        private->accept_queue[private->accept_read] = 0;
        private->accept_read = (private->accept_read + 1U) % SOCKET_LISTEN_BACKLOG;
        atomic_fetch_sub_explicit(&private->accept_count, 1U, memory_order_relaxed);
    }
    listener_ref = private->listener;
    private->listener = 0;
    socket_unlock(&socket->lock);
    if (was_closed) return K_OK;

    if (listener_ref != 0) {
        socket_lock(&listener_ref->lock);
        socket_remove_from_accept_queue_locked(listener_ref, socket);
        socket_unlock(&listener_ref->lock);
        object_put(listener_ref);
    }

    socket_lock(&g_binding_lock);
    socket_unbind_locked(socket);
    (void)socket_unregister_tcp_locked(socket);
    socket_unlock(&g_binding_lock);
    (void)wake_all(&socket->waitq);
    object_notify_signaled(socket);
    socket_detach_peer(socket, private);
    for (uint32_t index = 0; index < accepted_count; ++index) {
        (void)socket_close(accepted[index]);
        object_put(accepted[index]);
    }
    return K_OK;
}

kstatus_t socket_connect(socket_t *socket, uint32_t address, uint16_t port) {
    socket_private_t *private = socket_private(socket);
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET4 || private == 0 ||
        address == 0U || port == 0U) return K_EINVAL;
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) return K_EDEVREMOVED;

    if (socket->type == OS_SOCK_DGRAM) {
        /* POSIX connect() 会为尚未绑定的数据报 socket 隐式选择本地端点。若不执行此绑定，后续
         * send()/sendto(NULL, 0) 将没有源端口并返回 EINVAL；Chromium 的 DNS 探测正使用这种已连接 UDP 模式。 */
        if (!private->bound) {
            kstatus_t bind_status = socket_bind_ephemeral(socket, private);
            if (bind_status != K_OK) return bind_status;
        } else if (private->local_address == 0U) {
            /* Chromium 的 RANDOM_BIND 路径可能在 connect() 前使用 INADDR_ANY 预留端口。保留通配符绑定
             * 以匹配接收数据，但将活动接口选作已连接 socket 的源地址。 */
            uint32_t local_address = socket_active_ipv4_address();
            socket_lock(&socket->lock);
            if (private->local_address == 0U) {
                private->local_address = local_address;
            }
            socket_unlock(&socket->lock);
        }
        socket_lock(&socket->lock);
        private->connected = true;
        private->peer_address = address;
        private->peer_port = port;
        socket_unlock(&socket->lock);
        return K_OK;
    }

    if (!private->bound) {
        kstatus_t bind_status = socket_bind_ephemeral(socket, private);
        if (bind_status != K_OK) return bind_status;
    }
    socket_initialize_globals();
    socket_lock(&g_binding_lock);
    int32_t binding = socket_find_binding_locked(address, port, OS_SOCK_STREAM, true);
    socket_t *listener = binding >= 0 ? g_bindings[binding].socket : 0;
    if (listener != 0 && !object_try_get(listener)) listener = 0;
    socket_unlock(&g_binding_lock);
    if (listener == 0) {
        /* 已关闭的回环端口应在本地拒绝。绝不通过物理网卡发送 127/8，也不应为浏览器的本地应用探测消耗
         * 外部 SYN 重试预算。已有监听端使用下方的内核内连接路径。 */
        if ((address >> 24U) == 127U) {
            socket_lock(&socket->lock);
            bool busy = private->connected ||
                atomic_load_explicit(&private->closed, memory_order_acquire);
            socket_unlock(&socket->lock);
            return busy ? K_EBUSY : K_ECONNREFUSED;
        }
        socket_tcp_ipv4_output_fn output = 0;
        void *output_context = 0;
        uint32_t initial_sequence;
        uint16_t syn_window;
        kstatus_t output_status;
        socket_tcp_output_get(&output, &output_context);
        if (output == 0) return K_ENOENT;

        /* 尽力执行；在发布前完成，并且不持有任何传输层保护。OOM 时保留内嵌环形缓冲区和常规旧式握手。 */
        kstatus_t reserve = socket_stream_reserve(socket, SOCKET_STREAM_TCP_CAPACITY);
        if (reserve != K_OK && reserve != K_ENOMEM) return reserve;

        socket_lock(&socket->lock);
        if (private->connected ||
            atomic_load_explicit(&private->closed, memory_order_acquire)) {
            socket_unlock(&socket->lock);
            return K_EBUSY;
        }
        initial_sequence = 0x20000000U ^ ((uint32_t)private->local_port << 8U) ^ port;
        private->connected = true;
        private->peer_address = address;
        private->peer_port = port;
        private->tcp_initial_sequence = initial_sequence;
        private->tcp_send_next_sequence = initial_sequence + 1U;
        private->tcp_send_unacknowledged = initial_sequence;
        private->tcp_send_length = 0U;
        private->tcp_send_flags = NET_TCP_FLAG_SYN;
        private->tcp_send_retransmits = 0U;
        private->tcp_send_pending = true;
        private->tcp_local_retry_tsc = 0U;
        private->tcp_send_failed = false;
        atomic_store_explicit(&private->tcp_error, 0, memory_order_release);
        private->tcp_send_deadline_tsc = socket_tcp_retransmit_deadline(x86_read_tsc());
        private->tcp_peer_window = 0U;
        private->tcp_sequence_valid = false;
        socket_tcp_prepare_scale(private, true);
        private->tcp_state = SOCKET_TCP_SYN_SENT;
        syn_window = socket_tcp_receive_window(private, true);
        socket_unlock(&socket->lock);

        socket_lock(&g_binding_lock);
        bool registered = socket_register_tcp_locked(socket, private);
        socket_unlock(&g_binding_lock);
        if (!registered) {
            socket_lock(&socket->lock);
            private->connected = false;
            private->tcp_state = SOCKET_TCP_CLOSED;
            private->tcp_send_pending = false;
            socket_unlock(&socket->lock);
            return K_ENOMEM;
        }

        output_status = output(output_context, private->local_address,
                               private->local_port, address, port,
                               initial_sequence, 0U, NET_TCP_FLAG_SYN,
                               syn_window, 0, 0U);
#if OS_DEBUG_SERIAL
        if (g_debug_tcp_connect_logs < 16U) {
            kernel_serial_printf_serial_only(
                "OS_TCP_CONNECT src=%x:%u dst=%x:%u status=%lld\r\n",
                private->local_address, private->local_port, address, port,
                (long long)output_status);
            ++g_debug_tcp_connect_logs;
        }
#endif
        socket_lock(&socket->lock);
        bool failed = socket_tcp_local_output_locked(private, initial_sequence, initial_sequence + 1U,
                                                      output_status, x86_read_tsc());
        socket_unlock(&socket->lock);
        if (failed) {
            (void)wake_all(&socket->waitq);
            object_notify_changed(socket);
        }
        return output_status == K_EAGAIN ? K_OK : output_status;
    }

    socket_lock(&g_binding_lock);
    bool tcp_slots_available = socket_tcp_slots_available_locked(2U);
    socket_unlock(&g_binding_lock);
    if (!tcp_slots_available) {
        object_put(listener);
        return K_ENOMEM;
    }

    socket_private_t *listener_private = socket_private(listener);
    socket_t *child = 0;
    if (listener_private == 0 ||
        socket_allocate(OS_AF_INET4, OS_SOCK_STREAM, SOCKET_PROTOCOL_TCP, &child) != K_OK) {
        object_put(listener);
        return K_ENOMEM;
    }
    socket_private_t *child_private = socket_private(child);
    socket_lock(&socket->lock);
    bool already_connected = private->connected ||
        atomic_load_explicit(&private->closed, memory_order_acquire);
    socket_unlock(&socket->lock);
    if (already_connected) {
        object_put(listener);
        socket_abort_child(child);
        return K_EBUSY;
    }

    socket_lock(&listener->lock);
    bool can_accept = listener_private->listening &&
        !atomic_load_explicit(&listener_private->closed, memory_order_acquire) &&
        atomic_load_explicit(&listener_private->accept_count, memory_order_relaxed) <
            SOCKET_LISTEN_BACKLOG;
    if (!can_accept) {
        socket_unlock(&listener->lock);
        object_put(listener);
        socket_abort_child(child);
        return K_EAGAIN;
    }

    child_private->bound = true;
    child_private->local_address = listener_private->local_address;
    child_private->local_port = listener_private->local_port;
    child_private->connected = true;
    child_private->peer_address = private->local_address;
    child_private->peer_port = private->local_port;
    child_private->peer = socket;
    child_private->tcp_state = SOCKET_TCP_ESTABLISHED;
    child_private->tcp_sequence_valid = false;
    object_get(socket);

    socket_lock(&socket->lock);
    private->connected = true;
    private->peer_address = child_private->local_address;
    private->peer_port = child_private->local_port;
    private->peer = child;
    object_get(child);
    socket_unlock(&socket->lock);

    listener_private->accept_queue[listener_private->accept_write] = child;
    listener_private->accept_write =
        (listener_private->accept_write + 1U) % SOCKET_LISTEN_BACKLOG;
    atomic_fetch_add_explicit(&listener_private->accept_count, 1U, memory_order_release);
    socket_unlock(&listener->lock);
    (void)wake_all(&listener->waitq);
    object_notify_signaled(listener);
    object_put(listener);

    socket_lock(&g_binding_lock);
    bool registered = socket_register_tcp_locked(socket, private) &&
                      socket_register_tcp_locked(child, child_private);
    socket_unlock(&g_binding_lock);
    if (!registered) {
        /* 鏉╃偞甯寸悰銊﹀姬鏉炶棄褰цぐ鍗炴惙缂冩垵宕?RX閿涘奔绗夐惍鏉戞綎瀹歌尙绮″铏圭彌閻ㄥ嫬鍞撮弽鍛婃拱閸︽媽绻涢幒銉ｂ偓?*/
    }
    return K_OK;
}

kstatus_t socket_connect_ipv6(socket_t *socket, const uint8_t address[16], uint16_t port) {
    socket_private_t *private = socket_private(socket);
    socket_tcp_ipv6_output_fn output = 0;
    void *output_context = 0;
    uint32_t initial_sequence;
    uint16_t syn_window;
    kstatus_t output_status;
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->family != OS_AF_INET6 || private == 0 || address == 0 || port == 0U ||
        socket_address6_zero(address)) return K_EINVAL;
    if (atomic_load_explicit(&private->closed, memory_order_acquire)) return K_EDEVREMOVED;
    uint8_t selected_source[16];
    kstatus_t route_status = socket_ipv6_connect_source(address, selected_source);
    if (route_status != K_OK) return route_status;
    socket_lock(&socket->lock);
    bool incompatible_source = private->bound &&
        !socket_address6_zero(private->local_address6) &&
        !socket_address6_equal(private->local_address6, selected_source);
    if (private->bound && !incompatible_source)
        socket_copy(private->local_address6, selected_source, 16U);
    socket_unlock(&socket->lock);
    if (incompatible_source) return K_ENETUNREACH;
    if (socket->type == OS_SOCK_DGRAM) {
        if (!private->bound) {
            kstatus_t bind_status = socket_bind6_ephemeral(socket, private, selected_source);
            if (bind_status != K_OK) return bind_status;
        }
        socket_lock(&socket->lock);
        private->connected = true;
        socket_copy(private->peer_address6, address, 16U);
        private->peer_port = port;
        socket_unlock(&socket->lock);
        return K_OK;
    }
    if (!private->bound) {
        kstatus_t bind_status = socket_bind6_ephemeral(socket, private, selected_source);
        if (bind_status != K_OK) return bind_status;
    }
    /* 在分配接收环、发布 SYN 状态或查找物理输出前，先处理 IPv4 关闭回环端口的拒绝。浏览器的本地应用
     * 探测不得等待外部重传超时。现有监听端处理和非回环路由保持不变。 */
    static const uint8_t loopback[16] = {[15] = 1U};
    if (socket_address6_equal(address, loopback)) {
        socket_initialize_globals();
        socket_lock(&g_binding_lock);
        int32_t listener = socket_find_binding6_locked(address, port, OS_SOCK_STREAM, true);
        socket_unlock(&g_binding_lock);
        if (listener < 0) {
            socket_lock(&socket->lock);
            bool busy = private->connected ||
                atomic_load_explicit(&private->closed, memory_order_acquire);
            socket_unlock(&socket->lock);
            return busy ? K_EBUSY : K_ECONNREFUSED;
        }
    }
    socket_tcp6_output_get(&output, &output_context);
    if (output == 0) return K_ENOENT;
    kstatus_t reserve = socket_stream_reserve(socket, SOCKET_STREAM_TCP_CAPACITY);
    if (reserve != K_OK && reserve != K_ENOMEM) return reserve;
    socket_lock(&socket->lock);
    if (private->connected || atomic_load_explicit(&private->closed, memory_order_acquire)) {
        socket_unlock(&socket->lock);
        return K_EBUSY;
    }
    initial_sequence = 0x30000000U ^ ((uint32_t)private->local_port << 8U) ^ port;
    private->connected = true;
    socket_copy(private->peer_address6, address, 16U);
    private->peer_port = port;
    private->tcp_initial_sequence = initial_sequence;
    private->tcp_send_next_sequence = initial_sequence + 1U;
    private->tcp_send_unacknowledged = initial_sequence;
    private->tcp_send_length = 0U;
    private->tcp_send_flags = NET_TCP_FLAG_SYN;
    private->tcp_send_retransmits = 0U;
    private->tcp_send_pending = true;
    private->tcp_local_retry_tsc = 0U;
    private->tcp_send_failed = false;
    atomic_store_explicit(&private->tcp_error, 0, memory_order_release);
    private->tcp_send_deadline_tsc = socket_tcp_retransmit_deadline(x86_read_tsc());
    private->tcp_peer_window = 0U;
    private->tcp_sequence_valid = false;
    socket_tcp_prepare_scale(private, true);
    private->tcp_state = SOCKET_TCP_SYN_SENT;
    syn_window = socket_tcp_receive_window(private, true);
    uint8_t source_address[16];
    socket_copy(source_address, private->local_address6, 16U);
    socket_unlock(&socket->lock);

    socket_lock(&g_binding_lock);
    bool registered = socket_register_tcp_locked(socket, private);
    socket_unlock(&g_binding_lock);
    if (!registered) {
        socket_lock(&socket->lock);
        private->connected = false;
        private->tcp_state = SOCKET_TCP_CLOSED;
        private->tcp_send_pending = false;
        socket_unlock(&socket->lock);
        return K_ENOMEM;
    }
    output_status = output(output_context, source_address, private->local_port,
                           address, port, initial_sequence, 0U, NET_TCP_FLAG_SYN,
                           syn_window, 0, 0U);
    socket_lock(&socket->lock);
    bool failed = socket_tcp_local_output_locked(private, initial_sequence, initial_sequence + 1U,
                                                  output_status, x86_read_tsc());
    socket_unlock(&socket->lock);
    if (failed) {
        (void)wake_all(&socket->waitq);
        object_notify_changed(socket);
    }
    return output_status == K_EAGAIN ? K_OK : output_status;
}

kstatus_t socket_listen(socket_t *socket, uint32_t backlog) {
    socket_private_t *private = socket_private(socket);
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->type != OS_SOCK_STREAM || private == 0 || backlog == 0U) return K_EINVAL;
    if (backlog > SOCKET_LISTEN_BACKLOG) backlog = SOCKET_LISTEN_BACKLOG;
    socket_lock(&socket->lock);
    if (!private->bound || private->connected || private->listening ||
        atomic_load_explicit(&private->closed, memory_order_acquire)) {
        socket_unlock(&socket->lock);
        return K_EBUSY;
    }
    private->listening = true;
    socket_unlock(&socket->lock);
    (void)backlog;
    return K_OK;
}

static bool socket_accept_ready(void *context) {
    socket_wait_context_t *wait = (socket_wait_context_t *)context;
    bool ready = false;
    if (wait == 0 || wait->private == 0) return false;
    socket_t *socket = wait->socket;
    socket_lock(&socket->lock);
    uint32_t count = atomic_load_explicit(&wait->private->accept_count,
                                          memory_order_acquire);
    for (uint32_t offset = 0U; offset < count; ++offset) {
        uint32_t index = (wait->private->accept_read + offset) % SOCKET_LISTEN_BACKLOG;
        socket_t *candidate = wait->private->accept_queue[index];
        socket_private_t *candidate_private = socket_private(candidate);
        if (candidate_private != 0 &&
            candidate_private->tcp_state == SOCKET_TCP_ESTABLISHED) {
            ready = true;
            break;
        }
    }
    ready = ready || atomic_load_explicit(&wait->private->closed, memory_order_acquire);
    socket_unlock(&socket->lock);
    return ready;
}

kstatus_t socket_accept(socket_t *socket, uint64_t timeout_ns, socket_t **out) {
    socket_private_t *private = socket_private(socket);
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET ||
        socket->type != OS_SOCK_STREAM || private == 0 || out == 0) return K_EINVAL;
    socket_wait_context_t wait = {.socket = socket, .private = private};
    for (;;) {
        kstatus_t status = wait_on_queue(&socket->waitq, socket_accept_ready,
                                         &wait, timeout_ns);
        if (status != K_OK) return status;
        socket_lock(&socket->lock);
        uint32_t count = atomic_load_explicit(&private->accept_count, memory_order_relaxed);
        socket_t *accepted = 0;
        uint32_t accepted_offset = 0U;
        for (; accepted_offset < count; ++accepted_offset) {
            uint32_t index = (private->accept_read + accepted_offset) % SOCKET_LISTEN_BACKLOG;
            socket_private_t *candidate_private = socket_private(private->accept_queue[index]);
            if (candidate_private != 0 &&
                candidate_private->tcp_state == SOCKET_TCP_ESTABLISHED) {
                accepted = private->accept_queue[index];
                break;
            }
        }
        if (accepted != 0) {
            for (uint32_t shift = accepted_offset; shift + 1U < count; ++shift) {
                uint32_t from = (private->accept_read + shift + 1U) % SOCKET_LISTEN_BACKLOG;
                uint32_t to = (private->accept_read + shift) % SOCKET_LISTEN_BACKLOG;
                private->accept_queue[to] = private->accept_queue[from];
            }
            uint32_t last = (private->accept_read + count - 1U) % SOCKET_LISTEN_BACKLOG;
            private->accept_queue[last] = 0;
            private->accept_write = last;
            atomic_store_explicit(&private->accept_count, count - 1U, memory_order_release);
            socket_unlock(&socket->lock);
            *out = accepted; /* 鏉烆剛些閻╂垵鎯夐梼鐔峰灙閹镐焦婀侀惃鍕嚠鐠炩€崇穿閻劎绮扮拫鍐暏閼板懌鈧?*/
            return K_OK;
        }
        bool closed = atomic_load_explicit(&private->closed, memory_order_acquire);
        socket_unlock(&socket->lock);
        if (closed) return K_EDEVREMOVED;
    }
}
