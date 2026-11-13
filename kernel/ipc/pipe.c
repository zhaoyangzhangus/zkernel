#include <kernel/mm.h>
#include <kernel/pipe.h>
#include <kernel/string.h>

struct pipe_state {
    spinlock_t lock;
    wait_queue_t read_waitq;
    wait_queue_t write_waitq;
    uint8_t *buffer;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    uint32_t readers;
    uint32_t writers;
    uint32_t endpoint_refs;
    uint32_t active_destroyers;
    bool released;
    pipe_endpoint_t *read_endpoint;
    pipe_endpoint_t *write_endpoint;
    /* 共享状态的最后一个析构函数执行完毕前，两端点都会保持有效。 */
    pipe_endpoint_t endpoints[2];
};

typedef struct pipe_read_context {
    pipe_state_t *state;
    void *buffer;
    size_t length;
    uint64_t *bytes;
    pipe_read_limit_fn limit;
    void *limit_context;
    pipe_endpoint_t *peer;
    kstatus_t status;
} pipe_read_context_t;

typedef struct pipe_write_context {
    pipe_state_t *state;
    size_t minimum_free;
    const void *buffer;
    size_t length;
    uint64_t *bytes;
    pipe_endpoint_t *peer;
    bool wake_readers;
    kstatus_t status;
} pipe_write_context_t;

static void pipe_lock(pipe_state_t *state) {
    spinlock_lock(&state->lock);
}

static void pipe_unlock(pipe_state_t *state) {
    spinlock_unlock(&state->lock);
}

/* 传输过程已持有环形缓冲区锁时，为对端增加引用。该引用可在解锁后执行唤醒/通知期间，
 * 避免并发关闭最后一个句柄导致对象被释放。 */
static pipe_endpoint_t *pipe_get_peer_locked(pipe_state_t *state, bool read_end) {
    pipe_endpoint_t *endpoint = read_end ? state->read_endpoint : state->write_endpoint;
    if (endpoint != 0 && !object_try_get(endpoint)) endpoint = 0;
    return endpoint;
}

static void pipe_notify_endpoint(pipe_endpoint_t *endpoint) {
    if (endpoint != 0) {
        object_notify_signaled(endpoint);
        object_put(endpoint);
    }
}

static bool pipe_endpoint_is_signaled(const void *object) {
    const pipe_endpoint_t *endpoint = (const pipe_endpoint_t *)object;
    const pipe_state_t *state;
    bool signaled;
    if (!pipe_is_endpoint(endpoint) || endpoint->state == 0) return false;
    state = endpoint->state;
    pipe_lock((pipe_state_t *)state);
    signaled = endpoint->read_end ?
        (state->readers == 0U || state->count != 0U || state->writers == 0U) :
        (state->writers == 0U || state->readers == 0U || state->count < state->capacity);
    pipe_unlock((pipe_state_t *)state);
    return signaled;
}

static int64_t pipe_endpoint_wait_value(const void *object) {
    const pipe_endpoint_t *endpoint = (const pipe_endpoint_t *)object;
    const pipe_state_t *state;
    int64_t value = 0;
    if (!pipe_is_endpoint(endpoint) || endpoint->state == 0) return K_EBADF;
    state = endpoint->state;
    pipe_lock((pipe_state_t *)state);
    if (endpoint->read_end) {
        if (state->count != 0U) value |= PIPE_WAIT_READABLE;
        if (state->readers == 0U || state->writers == 0U) value |= PIPE_WAIT_HUP;
    } else {
        if (state->writers == 0U || state->readers == 0U) value |= PIPE_WAIT_ERROR;
        else if (state->count < state->capacity) value |= PIPE_WAIT_WRITABLE;
    }
    pipe_unlock((pipe_state_t *)state);
    return value;
}

static void pipe_endpoint_destroy(void *object) {
    pipe_endpoint_t *endpoint = (pipe_endpoint_t *)object;
    pipe_state_t *state;
    pipe_endpoint_t *peer = 0;
    bool free_state = false;

    if (endpoint == 0) return;
    state = endpoint->state;
    if (state != 0) {
        pipe_lock(state);
        ++state->active_destroyers;
        if (endpoint->read_end) {
            if (state->read_endpoint == endpoint) state->read_endpoint = 0;
            if (state->readers != 0U) --state->readers;
            peer = state->write_endpoint;
        } else {
            if (state->write_endpoint == endpoint) state->write_endpoint = 0;
            if (state->writers != 0U) --state->writers;
            peer = state->read_endpoint;
        }
        if (peer != 0 && !object_try_get(peer)) peer = 0;
        if (state->endpoint_refs != 0U) --state->endpoint_refs;
        pipe_unlock(state);

        /* 只有对端的谓词发生变化：读端消失会使写入失败，写端消失会使读取返回 EOF。
         * 正在进行的 I/O 持有自己的端点引用，因此关闭中的端点不会仍有活动 I/O。 */
        (void)wake_all(endpoint->read_end ? &state->write_waitq :
                                          &state->read_waitq);
        if (peer != 0) {
            object_notify_signaled(peer);
            object_put(peer);
        }

        pipe_lock(state);
        if (state->active_destroyers != 0U) --state->active_destroyers;
        if (state->endpoint_refs == 0U && state->active_destroyers == 0U &&
            !state->released) {
            state->released = true;
            free_state = true;
        }
        pipe_unlock(state);
        if (free_state) {
            kfree(state->buffer);
            kfree(state);
        }
    }
}

static const object_ops_t g_pipe_endpoint_ops = {
    .destroy = pipe_endpoint_destroy,
    .handle_close = 0,
    .type_name = "PipeEndpoint",
    .is_signaled = pipe_endpoint_is_signaled,
    .wait_value = pipe_endpoint_wait_value,
};

static void pipe_copy_in(pipe_state_t *state, const uint8_t *source,
                         size_t length) {
    size_t first = state->capacity - state->tail;
    if (first > length) first = length;
    /* 此处端点持有环形缓冲区锁。复制范围限制在两个连续片段内，并使用内核的
     * 非 SIMD 批量操作原语。 */
    __builtin_memcpy(state->buffer + state->tail, source, first);
    if (length != first)
        __builtin_memcpy(state->buffer, source + first, length - first);
    state->tail += first;
    if (state->tail == state->capacity) state->tail = length - first;
    state->count += length;
}

static void pipe_copy_out(pipe_state_t *state, uint8_t *destination,
                          size_t length) {
    size_t first = state->capacity - state->head;
    if (first > length) first = length;
    __builtin_memcpy(destination, state->buffer + state->head, first);
    if (length != first)
        __builtin_memcpy(destination + first, state->buffer, length - first);
    state->head += first;
    if (state->head == state->capacity) state->head = length - first;
    state->count -= length;
}

/* 在谓词内完成传输，避免就绪检查后被竞争线程抢走数据并重置等待超时。
 * 对端引用随传输一起固定，唤醒和对象通知仍由调用者在锁外完成。 */
static bool pipe_try_read(void *context) {
    pipe_read_context_t *read = context;
    pipe_state_t *state = read->state;
    bool finished = true;
    pipe_lock(state);
    if (state->readers == 0U) {
        read->status = K_OK;
    } else if (state->count != 0U) {
        size_t amount = state->count < read->length ? state->count : read->length;
        if (read->limit != 0) {
            size_t bounded = read->limit(read->limit_context, amount);
            if (bounded == 0U || bounded > amount) {
                read->status = K_EINVAL;
                pipe_unlock(state);
                return true;
            }
            amount = bounded;
        }
        pipe_copy_out(state, read->buffer, amount);
        read->peer = pipe_get_peer_locked(state, false);
        *read->bytes = amount;
        read->status = K_OK;
    } else if (state->writers == 0U) {
        read->status = K_OK;
    } else {
        finished = false;
    }
    pipe_unlock(state);
    return finished;
}

static bool pipe_try_write(void *context) {
    pipe_write_context_t *write = context;
    pipe_state_t *state = write->state;
    bool finished = true;
    pipe_lock(state);
    if (state->writers == 0U || state->readers == 0U) {
        write->status = K_EPIPE;
    } else if (state->capacity - state->count >= write->minimum_free) {
        size_t amount = state->capacity - state->count;
        if (amount > write->length) amount = write->length;
        write->wake_readers = state->count == 0U;
        pipe_copy_in(state, write->buffer, amount);
        write->peer = pipe_get_peer_locked(state, true);
        *write->bytes = amount;
        write->status = K_OK;
    } else {
        finished = false;
    }
    pipe_unlock(state);
    return finished;
}

kstatus_t pipe_create(uint32_t flags, pipe_endpoint_t **read_end,
                      pipe_endpoint_t **write_end) {
    pipe_state_t *state;
    pipe_endpoint_t *reader;
    pipe_endpoint_t *writer;
    if (read_end == 0 || write_end == 0 || (flags & ~OS_PIPE_FLAG_MASK) != 0U) {
        return K_EINVAL;
    }
    state = (pipe_state_t *)kmalloc(sizeof(*state), 0);
    if (state == 0) return K_ENOMEM;
    /* 端点头部由下方的对象构造函数初始化。将 64 KiB 环形缓冲区单独分配，避免
     * 页面分配量翻倍。 */
    memset(state, 0, __builtin_offsetof(pipe_state_t, endpoints));
    state->buffer = (uint8_t *)kmalloc(OS_PIPE_DEFAULT_SIZE, 0);
    if (state->buffer == 0) {
        kfree(state);
        return K_ENOMEM;
    }
    spinlock_init(&state->lock);
    wait_queue_init(&state->read_waitq);
    wait_queue_init(&state->write_waitq);
    state->capacity = OS_PIPE_DEFAULT_SIZE;
    state->readers = 1U;
    state->writers = 1U;
    state->endpoint_refs = 2U;

    reader = &state->endpoints[0];
    writer = &state->endpoints[1];
    object_header_init(&reader->object, KOBJECT_TYPE_PIPE_ENDPOINT,
                       &g_pipe_endpoint_ops);
    reader->state = state;
    reader->read_end = true;
    object_header_init(&writer->object, KOBJECT_TYPE_PIPE_ENDPOINT,
                       &g_pipe_endpoint_ops);
    writer->state = state;
    writer->read_end = false;
    state->read_endpoint = reader;
    state->write_endpoint = writer;
    *read_end = reader;
    *write_end = writer;
    return K_OK;
}

kstatus_t pipe_shutdown(pipe_endpoint_t *endpoint) {
    if (!pipe_is_endpoint(endpoint)) return K_EBADF;
    pipe_state_t *state = endpoint->state;
    pipe_lock(state);
    uint32_t *open = endpoint->read_end ? &state->readers : &state->writers;
    if (*open == 0U) { pipe_unlock(state); return K_OK; }
    *open = 0U;
    pipe_endpoint_t *peer = pipe_get_peer_locked(state, !endpoint->read_end);
    pipe_unlock(state);
    /* 逻辑关闭必须同时唤醒本端正在进行的操作和对端。引用只保护内存，不代表
     * 通信方向仍处于打开状态。 */
    (void)wake_all(&state->read_waitq);
    (void)wake_all(&state->write_waitq);
    object_notify_signaled(endpoint);
    pipe_notify_endpoint(peer);
    return K_OK;
}

kstatus_t pipe_read(pipe_endpoint_t *endpoint, void *buffer, size_t length,
                    uint64_t timeout_ns, uint64_t *bytes) {
    return pipe_read_limited(endpoint, buffer, length, timeout_ns, bytes, 0, 0);
}

kstatus_t pipe_read_limited(pipe_endpoint_t *endpoint, void *buffer, size_t length,
                            uint64_t timeout_ns, uint64_t *bytes,
                            pipe_read_limit_fn limit, void *context) {
    if (!pipe_is_endpoint(endpoint) || !endpoint->read_end || bytes == 0 ||
        (buffer == 0 && length != 0U)) return K_EINVAL;
    *bytes = 0U;
    if (length == 0U) return K_OK;
    pipe_read_context_t read = {
        .state = endpoint->state, .buffer = buffer, .length = length,
        .bytes = bytes, .limit = limit, .limit_context = context,
        .status = K_ETIMEDOUT,
    };
    if (!pipe_try_read(&read)) {
        if (timeout_ns == 0U) return K_ETIMEDOUT;
        kstatus_t status = wait_on_queue(&read.state->read_waitq,
                                         pipe_try_read, &read, timeout_ns);
        if (status != K_OK) return status;
    }
    if (*bytes != 0U) {
        (void)wake_all(&read.state->write_waitq);
        pipe_notify_endpoint(read.peer);
    }
    return read.status;
}

kstatus_t pipe_write(pipe_endpoint_t *endpoint, const void *buffer,
                     size_t length, uint64_t timeout_ns, uint64_t *bytes) {
    if (!pipe_is_endpoint(endpoint) || endpoint->read_end || bytes == 0 ||
        (buffer == 0 && length != 0U)) return K_EINVAL;
    *bytes = 0U;
    if (length == 0U) return K_OK;
    pipe_write_context_t write = {
        .state = endpoint->state,
        .minimum_free = length <= OS_PIPE_BUF ? length : 1U,
        .buffer = buffer, .length = length, .bytes = bytes,
        .status = K_ETIMEDOUT,
    };
    if (!pipe_try_write(&write)) {
        if (timeout_ns == 0U) return K_ETIMEDOUT;
        kstatus_t status = wait_on_queue(&write.state->write_waitq,
                                         pipe_try_write, &write, timeout_ns);
        if (status != K_OK) return status;
    }
    if (*bytes != 0U) {
        /* 直接读者只等空缓冲区；poll 消费者仍需每次写入的对象通知。 */
        if (write.wake_readers) (void)wake_all(&write.state->read_waitq);
        pipe_notify_endpoint(write.peer);
    }
    return write.status;
}

kstatus_t pipe_pending_bytes(const pipe_endpoint_t *endpoint, uint64_t *bytes) {
    if (!pipe_is_endpoint(endpoint) || bytes == 0) return K_EBADF;
    pipe_state_t *state = endpoint->state;
    pipe_lock(state);
    *bytes = state->count;
    pipe_unlock(state);
    return K_OK;
}

kstatus_t pipe_stat(const pipe_endpoint_t *endpoint, os_file_info_t *info) {
    if (!pipe_is_endpoint(endpoint) || info == 0) return K_EBADF;
    *info = (os_file_info_t){0};
    info->type = OS_FILE_TYPE_FIFO;
    info->mode = 0666U;
    return K_OK;
}

bool pipe_is_endpoint(const void *object) {
    const object_header_t *header = (const object_header_t *)object;
    return header != 0 && header->type == KOBJECT_TYPE_PIPE_ENDPOINT &&
           header->ops == &g_pipe_endpoint_ops;
}

bool pipe_endpoint_is_read(const pipe_endpoint_t *endpoint) {
    return pipe_is_endpoint(endpoint) && endpoint->read_end;
}

int64_t pipe_wait_value(const pipe_endpoint_t *endpoint) {
    return pipe_endpoint_wait_value(endpoint);
}
