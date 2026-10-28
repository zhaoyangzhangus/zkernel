#pragma once

#include <kernel/completion_port.h>
#include <kernel/socket.h>

/* REFACTOR_P8_SOCKET_MODEL_OWNER：唯一的私有 socket 状态模型。 */

/* 内嵌的初始容量，不是活动连接数上限。注册表读取方持有绑定保护锁时，
 * 使用 socket_registry_capacity_locked()。 */
#define SOCKET_BINDING_COUNT 64U
#define SOCKET_EPHEMERAL_FIRST 49152U
#define SOCKET_EPHEMERAL_LAST  65535U

typedef struct socket_datagram {
    uint16_t family;
    uint16_t reserved;
    uint32_t source_address;
    uint16_t source_port;
    uint32_t destination_address;
    uint16_t destination_port;
    uint16_t length;
    uint8_t source_address6[16];
    uint8_t destination_address6[16];
    uint8_t payload[SOCKET_MAX_PAYLOAD];
} socket_datagram_t;

typedef struct socket_private {
    uint32_t datagram_read;
    uint32_t datagram_write;
    atomic_uint datagram_count;

    uint32_t stream_read;
    uint32_t stream_write;
    atomic_uint stream_count;
    /* 可选的握手前接收环。零表示继续使用内嵌后备缓冲区。
     * 连接发布/建立后容量不可变。 */
    uint8_t *stream_dynamic;
    uint32_t stream_capacity;
    /* 尚未到达的数据保存在流环的未使用槽位中，不另设负载缓冲区。
     * TCP 分配区尾部包含这些槽位的位图。 */
    uint8_t *tcp_rx_present;
    uint32_t tcp_rx_future_count;
    uint32_t tcp_rx_fin_sequence;
    bool tcp_rx_fin_pending;

    atomic_bool closed;
    atomic_bool peer_closed;
    atomic_bool receive_shutdown;
    atomic_bool send_shutdown;
    bool bound;
    bool connected;
    bool listening;
    atomic_bool reuse_address;
    uint32_t local_address;
    uint8_t local_address6[16];
    uint16_t local_port;
    uint32_t peer_address;
    uint8_t peer_address6[16];
    uint16_t peer_port;
    uint32_t tcp_next_sequence;
    bool tcp_sequence_valid;
    uint32_t tcp_initial_sequence;
    uint32_t tcp_peer_initial_sequence;
    uint8_t tcp_state;
    uint32_t tcp_send_next_sequence;
    uint32_t tcp_send_unacknowledged;
    /* SND.WL1/WL2：数据包重排后拒绝过期的通告窗口。 */
    uint32_t tcp_window_sequence;
    uint32_t tcp_window_acknowledgement;
    uint32_t tcp_peer_window;
    uint8_t tcp_receive_scale;
    uint8_t tcp_peer_scale;
    bool tcp_scale_offered;
    bool tcp_scale_negotiated;
    bool tcp_scale_seen;
    uint16_t tcp_peer_mss;
    uint16_t tcp_send_length;
    uint32_t tcp_send_retransmits;
    uint64_t tcp_send_deadline_tsc;
    uint64_t tcp_local_retry_tsc; /* 本地 TX/邻居背压，不是 RTO。 */
    uint64_t tcp_ack_retry_tsc;
    bool tcp_ack_pending; /* 仅表示累积 ACK；不保留数据包或 socket 引用。 */
    uint64_t tcp_persist_deadline_tsc;
    uint32_t tcp_persist_seconds;
    bool tcp_send_pending;
    bool tcp_send_failed;
    bool tcp_fin_pending;
    bool tcp_fin_sent;
    uint8_t tcp_send_flags;
    bool tcp_keepalive;
    uint32_t tcp_keepidle;
    uint32_t tcp_keepintvl;
    uint32_t tcp_keepcnt;
    uint32_t tcp_keep_probes;
    uint64_t tcp_keep_deadline_tsc;
    atomic_int tcp_error; /* 终止状态的负 errno；发布给等待者。 */
    socket_t *peer;
    socket_t *listener;

    socket_t *accept_queue[SOCKET_LISTEN_BACKLOG];
    uint32_t accept_read;
    uint32_t accept_write;
    atomic_uint accept_count;

    /* 上方控制状态会在发布前初始化。缓冲区内容仅在已发布长度/计数
     *（或重组位）范围内有效，并且会先写入内容，再设置这些标记。
     * 无需清零未使用的数据。 */
    uint8_t stream_buffer[SOCKET_STREAM_BUFFER_SIZE];
    uint8_t tcp_send_payload[SOCKET_MAX_PAYLOAD];
    /* 尾部存储对数据报 socket 是 UDP 队列；对 Internet 流 socket 则是接收位图
     *（通过 tcp_rx_present 访问）。 */
    socket_datagram_t datagrams[];
} socket_private_t;

#define SOCKET_STREAM_MAX_CAPACITY (4U * 1024U * 1024U)
#define SOCKET_STREAM_TCP_CAPACITY (256U * 1024U)
static inline uint32_t socket_stream_capacity(const socket_private_t *private) {
    return private->stream_dynamic != 0 ? private->stream_capacity :
                                         SOCKET_STREAM_BUFFER_SIZE;
}
static inline uint8_t *socket_stream_data(socket_private_t *private) {
    return private->stream_dynamic != 0 ? private->stream_dynamic :
                                         private->stream_buffer;
}
static inline const uint8_t *socket_stream_data_const(const socket_private_t *private) {
    return private->stream_dynamic != 0 ? private->stream_dynamic :
                                         private->stream_buffer;
}

/* 在握手前为 Internet 流预留空缓冲区。分配和回收都在 socket 锁外进行；
 * 失败时保留原有环形缓冲区。 */
kstatus_t socket_stream_reserve(socket_t *socket, uint32_t capacity);

/* 以下辅助函数均要求持有 socket 保护锁。SYN 窗口永不缩放；即使本地环更大，
 * 也要为旧版对端保留有效的 16 位窗口。 */
static inline uint16_t socket_tcp_window_for_count(const socket_private_t *private,
                                                   uint32_t count, bool syn) {
    uint32_t capacity = socket_stream_capacity(private);
    uint32_t available = count < capacity ? capacity - count : 0U;
    if (!syn && private->tcp_scale_negotiated)
        available >>= private->tcp_receive_scale;
    return (uint16_t)(available > UINT16_MAX ? UINT16_MAX : available);
}
static inline uint16_t socket_tcp_receive_window(const socket_private_t *private,
                                                 bool syn) {
    return socket_tcp_window_for_count(private,
        atomic_load_explicit(&private->stream_count, memory_order_relaxed), syn);
}
static inline uint32_t socket_tcp_decode_window(const socket_private_t *private,
                                                 uint16_t wire, bool syn) {
    return (uint32_t)wire << (!syn && private->tcp_scale_negotiated ?
                             private->tcp_peer_scale : 0U);
}
static inline void socket_tcp_prepare_scale(socket_private_t *private, bool active) {
    uint32_t capacity = socket_stream_capacity(private);
    uint8_t shift = 0U;
    while ((capacity >> shift) > UINT16_MAX && shift < 14U) ++shift;
    private->tcp_receive_scale = shift;
    private->tcp_peer_scale = 0U;
    private->tcp_scale_seen = private->tcp_scale_negotiated = false;
    private->tcp_scale_offered = active && capacity > SOCKET_STREAM_BUFFER_SIZE;
}

typedef struct socket_binding {
    socket_t *socket;
    uint16_t family;
    uint16_t reserved;
    uint32_t address;
    uint8_t address6[16];
    uint16_t port;
    uint16_t type;
} socket_binding_t;

typedef struct socket_tcp_connection {
    socket_t *socket;
    uint32_t local_address;
    uint16_t local_port;
    uint32_t peer_address;
    uint16_t peer_port;
} socket_tcp_connection_t;

typedef struct socket_tcp6_connection {
    socket_t *socket;
    uint8_t local_address[16];
    uint16_t local_port;
    uint8_t peer_address[16];
    uint16_t peer_port;
} socket_tcp6_connection_t;

enum {
    SOCKET_TCP_CLOSED = 0U,
    SOCKET_TCP_SYN_RECEIVED = 1U,
    SOCKET_TCP_ESTABLISHED = 2U,
    SOCKET_TCP_SYN_SENT = 3U,
};

/* Caller holds the socket lock. Keep the first deadline: repeated failures
 * must neither busy-retry nor postpone recovery indefinitely. The pending bit
 * permits a deadline of zero, including a TSC wrap through zero. */
static inline void socket_tcp_ack_retry_locked(socket_private_t *private,
    kstatus_t status, uint64_t now, uint64_t interval) {
    if (status != K_EAGAIN || private->tcp_ack_pending ||
        atomic_load_explicit(&private->closed, memory_order_acquire) ||
        private->tcp_state != SOCKET_TCP_ESTABLISHED ||
        !private->tcp_sequence_valid || private->peer != 0) return;
    private->tcp_ack_pending = true;
    private->tcp_ack_retry_tsc = now + (interval != 0U ? interval : 1U);
}

/* Caller holds the socket lock. Publish after canceling every transport timer,
 * so lock-free receive/object predicates observe a consistent terminal state.
 * Buffered receive data remains available before the terminal error. */
static inline void socket_tcp_fail_locked(socket_private_t *private,
                                           kstatus_t error) {
    private->tcp_state = SOCKET_TCP_CLOSED;
    private->tcp_send_failed = true;
    private->tcp_send_pending = false;
    private->tcp_send_length = 0U;
    private->tcp_send_flags = 0U;
    private->tcp_send_deadline_tsc = 0U;
    private->tcp_local_retry_tsc = 0U;
    private->tcp_ack_pending = false;
    private->tcp_ack_retry_tsc = 0U;
    private->tcp_keep_deadline_tsc = 0U;
    private->tcp_persist_deadline_tsc = 0U;
    private->tcp_persist_seconds = 0U;
    private->tcp_fin_pending = false;
    private->tcp_rx_fin_pending = false;
    atomic_store_explicit(&private->tcp_error, error, memory_order_release);
}

typedef struct socket_wait_context {
    socket_t *socket;
    socket_private_t *private;
} socket_wait_context_t;

typedef struct socket_async_send {
    socket_t *socket;
    completion_port_t *completion_port;
    size_t length;
    uint32_t address;
    uint8_t address6[16];
    bool ipv6;
    uint16_t port;
    uint64_t user_key;
    uint64_t request_id;
    /* Owned until the deferred worker completes; only length bytes exist. */
    uint8_t payload[];
} socket_async_send_t;

/* socket.c owns the registry storage; protocol and transport use read-only
 * lookup views while holding the binding lock. */
spinlock_t *socket_binding_lock(void);
uint32_t socket_registry_capacity_locked(void);
const socket_binding_t *socket_binding_at(uint32_t index);
const socket_tcp_connection_t *socket_tcp_connection_at(uint32_t index);
const socket_tcp6_connection_t *socket_tcp6_connection_at(uint32_t index);
uint64_t socket_next_async_request_id(void);

socket_private_t *socket_private(socket_t *socket);
const socket_private_t *socket_private_const(const socket_t *socket);
void socket_zero(void *memory, size_t length);
void socket_copy(void *destination, const void *source, size_t length);
/* Caller holds the socket lock, bounds length by available data/space, and
 * provides an external buffer that does not alias private stream storage. */
uint32_t socket_stream_write_locked(socket_private_t *private,
                                    const void *buffer, size_t length);
uint32_t socket_stream_read_locked(const socket_private_t *private,
                                   void *buffer, size_t length);
bool socket_address6_equal(const uint8_t left[16], const uint8_t right[16]);
bool socket_address6_zero(const uint8_t address[16]);
void socket_lock(spinlock_t *lock);
void socket_unlock(spinlock_t *lock);
void socket_initialize_globals(void);
void socket_udp4_output_get(socket_udp_ipv4_output_fn *output, void **context);
void socket_udp6_output_get(socket_udp_ipv6_output_fn *output, void **context);
uint64_t socket_tcp_retransmit_deadline(uint64_t now_tsc);
void socket_tcp_keepalive_activity_locked(socket_private_t *private,
                                          uint64_t now_tsc);
bool socket_tcp_keepalive_poll_locked(socket_private_t *private,
                                      uint64_t now_tsc, bool *failed);
bool socket_register_tcp_locked(socket_t *socket, socket_private_t *private);
bool socket_tcp_slots_available_locked(uint32_t required);
bool socket_tcp6_slots_available_locked(uint32_t required);
int32_t socket_find_tcp_locked(uint32_t source_address, uint16_t source_port,
                               uint32_t destination_address, uint16_t destination_port);
int32_t socket_find_tcp6_locked(const uint8_t source_address[16], uint16_t source_port,
                                const uint8_t destination_address[16], uint16_t destination_port);
int32_t socket_find_binding_locked(uint32_t address, uint16_t port,
                                   uint16_t type, bool listening_only);
int32_t socket_find_binding6_locked(const uint8_t address[16], uint16_t port,
                                    uint16_t type, bool listening_only);
kstatus_t socket_allocate(uint16_t family, uint16_t type, uint16_t protocol,
                          socket_t **out);
void socket_abort_child(socket_t *child);
