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
    /* Both endpoints live until the shared state's last destructor exits. */
    pipe_endpoint_t endpoints[2];
};

typedef struct pipe_write_context {
    pipe_state_t *state;
    size_t minimum_free;
} pipe_write_context_t;

static void pipe_lock(pipe_state_t *state) {
    spinlock_lock(&state->lock);
}

static void pipe_unlock(pipe_state_t *state) {
    spinlock_unlock(&state->lock);
}

static bool pipe_read_ready(void *context) {
    pipe_state_t *state = (pipe_state_t *)context;
    bool ready;
    pipe_lock(state);
    ready = state->readers == 0U || state->count != 0U || state->writers == 0U;
    pipe_unlock(state);
    return ready;
}

static bool pipe_write_ready(void *context) {
    pipe_write_context_t *write = (pipe_write_context_t *)context;
    pipe_state_t *state = write->state;
    bool ready;
    pipe_lock(state);
    ready = state->writers == 0U || state->readers == 0U ||
            state->capacity - state->count >= write->minimum_free;
    pipe_unlock(state);
    return ready;
}

/* Pin the peer while the transfer already holds the ring lock. The reference
 * survives a concurrent last handle close during the unlocked wake/notify. */
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

        /* Only the peer's predicate changed: reader loss breaks writes,
         * writer loss produces EOF for reads. An in-flight I/O owns its
         * endpoint reference, so the closing end cannot have live I/O. */
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
    /* The endpoint holds the ring lock here. Keep copying bounded to its
     * two contiguous spans and use the non-SIMD kernel bulk primitive. */
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
    /* Endpoint headers are initialized by their object constructor below.
     * Keep the 64 KiB ring separate to avoid doubling its page allocation. */
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
    /* Logical shutdown must wake both the local in-flight operation and
     * its peer. Reference pins protect memory, not the open direction. */
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
    pipe_state_t *state;
    size_t amount;
    if (!pipe_is_endpoint(endpoint) || !endpoint->read_end || bytes == 0 ||
        (buffer == 0 && length != 0U)) return K_EINVAL;
    *bytes = 0U;
    if (length == 0U) return K_OK;
    state = endpoint->state;
    for (;;) {
        /* The wait path rechecks readiness before subscribing. Try the
         * protected data/EOF state first, without its extra predicate lock. */
        pipe_lock(state);
        if (state->readers == 0U) {
            pipe_unlock(state);
            return K_OK;
        }
        if (state->count != 0U) {
            amount = state->count < length ? state->count : length;
            if (limit != 0) {
                size_t bounded = limit(context, amount);
                if (bounded == 0U || bounded > amount) {
                    pipe_unlock(state);
                    return K_EINVAL;
                }
                amount = bounded;
            }
            pipe_copy_out(state, (uint8_t *)buffer, amount);
            pipe_endpoint_t *peer = pipe_get_peer_locked(state, false);
            pipe_unlock(state);
            *bytes = amount;
            (void)wake_all(&state->write_waitq);
            pipe_notify_endpoint(peer);
            return K_OK;
        }
        if (state->writers == 0U) {
            pipe_unlock(state);
            return K_OK;
        }
        pipe_unlock(state);
        if (timeout_ns == 0U) return K_ETIMEDOUT;
        kstatus_t status = wait_on_queue(&state->read_waitq, pipe_read_ready,
                                         state, timeout_ns);
        if (status != K_OK) return status;
    }
}

kstatus_t pipe_write(pipe_endpoint_t *endpoint, const void *buffer,
                     size_t length, uint64_t timeout_ns, uint64_t *bytes) {
    pipe_state_t *state;
    pipe_write_context_t write;
    size_t amount;
    if (!pipe_is_endpoint(endpoint) || endpoint->read_end || bytes == 0 ||
        (buffer == 0 && length != 0U)) return K_EINVAL;
    *bytes = 0U;
    if (length == 0U) return K_OK;
    state = endpoint->state;
    write.state = state;
    write.minimum_free = length <= OS_PIPE_BUF ? length : 1U;
    for (;;) {
        /* Keep the atomic-write threshold under the same lock as copying. */
        pipe_lock(state);
        if (state->writers == 0U || state->readers == 0U) {
            pipe_unlock(state);
            return K_EPIPE;
        }
        if (state->capacity - state->count >= write.minimum_free) {
            amount = state->capacity - state->count;
            if (amount > length) amount = length;
            bool wake_readers = state->count == 0U;
            pipe_copy_in(state, (const uint8_t *)buffer, amount);
            pipe_endpoint_t *peer = pipe_get_peer_locked(state, true);
            pipe_unlock(state);
            *bytes = amount;
            /* Direct readers only block on an empty ring. Their predicate
             * and subscription are serialized by the wait queue lock.
             * Keep object notifications on every write for poll consumers. */
            if (wake_readers) (void)wake_all(&state->read_waitq);
            pipe_notify_endpoint(peer);
            return K_OK;
        }
        pipe_unlock(state);
        if (timeout_ns == 0U) return K_ETIMEDOUT;
        kstatus_t status = wait_on_queue(&state->write_waitq, pipe_write_ready,
                                         &write, timeout_ns);
        if (status != K_OK) return status;
    }
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
