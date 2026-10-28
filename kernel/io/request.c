#include <kernel/device.h>
#include <kernel/completion_port.h>
#include <kernel/io.h>
#include <kernel/telemetry.h>
#include <kernel/spinlock.h>
#include <kernel/kmem.h>
#include <kernel/process.h>
#include <kernel/string.h>

#define IO_REQUEST_OBJECT_TYPE 0x0106U
#define IO_REGISTRY_BUCKETS 64U

_Static_assert((IO_REGISTRY_BUCKETS & (IO_REGISTRY_BUCKETS - 1U)) == 0,
               "I/O registry bucket count must be a power of two");

static atomic_uint_fast64_t g_next_io_request_id;
static atomic_uint g_io_registry_state;
static spinlock_t g_io_registry_lock;
static list_head_t g_io_registry[IO_REGISTRY_BUCKETS];

static void io_registry_initialize(void) {
    if (atomic_load_explicit(&g_io_registry_state, memory_order_acquire) == 2U) return;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_io_registry_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        spinlock_init(&g_io_registry_lock);
        for (unsigned i = 0; i < IO_REGISTRY_BUCKETS; ++i)
            list_init(&g_io_registry[i]);
        atomic_store_explicit(&g_io_registry_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_io_registry_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static void io_registry_lock(void) {
    spinlock_lock(&g_io_registry_lock);
}

static void io_registry_unlock(void) {
    spinlock_unlock(&g_io_registry_lock);
}

static void io_registry_insert_tail(list_head_t *head, list_head_t *node) {
    node->next = head;
    node->prev = head->prev;
    head->prev->next = node;
    head->prev = node;
}

static list_head_t *io_registry_bucket(uint64_t request_id) {
    return &g_io_registry[request_id & (IO_REGISTRY_BUCKETS - 1U)];
}

static void io_object_destroy(void *object) {
    io_request_t *request = (io_request_t *)object;
    if (request->completion_port != 0) {
        object_put(request->completion_port);
        request->completion_port = 0;
        request->completion_key = 0;
    }
    if (request->private_release != 0) {
        request->private_release(request);
        request->private_release = 0;
    }
    if ((request->internal_flags & IOREQ_INTERNAL_PROCESS_REF) != 0U &&
        request->process != 0) {
        object_put(request->process);
        request->process = 0;
    }
    if ((request->internal_flags & IOREQ_INTERNAL_DYNAMIC) != 0U) {
        kfree(request);
    }
}

static const object_ops_t g_io_object_ops = {
    .destroy = io_object_destroy,
    .type_name = "IoRequest",
    .is_signaled = 0,
    .wait_value = 0,
};

void io_request_init(io_request_t *req, uint32_t opcode, struct device *device,
                     struct process *process, io_vec_t *vecs, uint32_t vec_count) {
    if (req == 0) return;
    memset(req, 0, sizeof(*req));
    object_header_init(&req->object, IO_REQUEST_OBJECT_TYPE, &g_io_object_ops);
    atomic_init(&req->state, IOREQ_NEW);
    req->opcode = opcode;
    req->device = device;
    req->process = process;
    req->vecs = vecs;
    req->vec_count = vec_count;
    req->request_id = atomic_fetch_add_explicit(&g_next_io_request_id, 1U,
                                                memory_order_relaxed) + 1U;
    list_init(&req->queue_node);
    list_init(&req->device_node);
}

static bool io_vectors_valid(const io_request_t *req) {
    if (req->vec_count != 0 && req->vecs == 0) return false;
    uint64_t total = 0;
    for (uint32_t i = 0; i < req->vec_count; ++i) {
        const io_vec_t *vec = &req->vecs[i];
        if (vec->length != 0 && vec->base == 0) return false;
        if (total > UINT64_MAX - vec->length) return false;
        total += vec->length;
    }
    return total != 0 || req->opcode == IO_FLUSH || req->opcode == IO_IOCTL;
}

static bool io_state_is_inflight(unsigned state) {
    /* CANCELLED 由取消者独占完成；晚到的设备完成不能夺取完成权。 */
    return state == IOREQ_QUEUED || state == IOREQ_SUBMITTED;
}

static void io_publish_completion(io_request_t *req) {
    completion_port_t *port = (completion_port_t *)req->completion_port;
    if (port == 0) return;
    os_completion_entry_t entry = {
        .user_key = req->completion_key,
        .status = req->status,
        .bytes_done = req->bytes_done,
        .request_id = req->request_id,
    };
    (void)completion_port_post(port, &entry);
    req->completion_port = 0;
    req->completion_key = 0;
    object_put(port);
}

static bool io_registry_linked(const io_request_t *req) {
    return req != 0 && req->queue_node.next != 0 && req->queue_node.prev != 0 &&
           req->queue_node.next != &req->queue_node;
}

static void io_request_unregister_user(io_request_t *req) {
    bool removed = false;
    if (req == 0 || (req->internal_flags & IOREQ_INTERNAL_USER_REQUEST) == 0U) {
        return;
    }
    io_registry_initialize();
    io_registry_lock();
    if (io_registry_linked(req)) {
        req->queue_node.prev->next = req->queue_node.next;
        req->queue_node.next->prev = req->queue_node.prev;
        list_init(&req->queue_node);
        removed = true;
    }
    io_registry_unlock();
    if (removed) object_put(req); /* 登记表持有的独立引用。 */
}

static void io_finalize_owned(io_request_t *req, kstatus_t status,
                              uint64_t bytes_done, bool execution_ref,
                              unsigned terminal_state) {
    void (*complete)(io_request_t *) = req != 0 ? req->complete : 0;
    void (*notify)(void) = req != 0 ? req->notify : 0;
    bool dynamic = req != 0 &&
                   (req->internal_flags & IOREQ_INTERNAL_DYNAMIC) != 0U;
    if (req == 0) return;
    req->status = status;
    req->bytes_done = bytes_done;
    /* completion_target 只在设备完成前有效，避免 BIO 栈对象变成悬挂指针。 */
    req->completion_target = 0;
    if (req->submit_tsc != 0U) {
        (void)telemetry_record_latency(TELEMETRY_CATEGORY_IO_LATENCY,
                                       req->request_id, req->submit_tsc);
        req->submit_tsc = 0U;
    }
    /* QUEUED 状态已允许取消，而 device_io_begin 仍可能持有设备锁。
     * 结束阶段必须与该注册同步，而不是在拿同一把锁之前读取 device_ref_held。 */
    if (execution_ref && device_io_end(req->device, req)) {
        object_put(req->device);
    }
    /* 在发布前先完成对用户可见的结果。私有 staging 内存跟随请求引用，而不是终态。 */
    if (complete != 0) complete(req);
    io_request_unregister_user(req);
    io_publish_completion(req);
    /*
     * 只有在所有 owner 可见的链接都已移除后，才发布终态。
     * 同步调用方一旦观察到 COMPLETED 就会立即重用栈上请求；过早发布会让它重置
     * device_node，而此时 completion 线程仍持有设备列表。
     */
    if (execution_ref && !dynamic) object_put(req);
    atomic_store_explicit(&req->state, terminal_state, memory_order_release);
    if (execution_ref && dynamic) {
        object_put(req); /* 由 io_submit 建立的异步执行引用。 */
    }
    if (notify != 0) notify();
}

void io_complete(io_request_t *req, kstatus_t status, uint64_t bytes_done) {
    if (req == 0) return;
    unsigned state = atomic_load_explicit(&req->state, memory_order_acquire);
    for (;;) {
        if (state == IOREQ_COMPLETED || state == IOREQ_COMPLETING) return;
        if (!io_state_is_inflight(state)) return;
        if (atomic_compare_exchange_weak_explicit(&req->state, &state,
                                                  IOREQ_COMPLETING,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) break;
    }
    io_finalize_owned(req, status, bytes_done, true, IOREQ_COMPLETED);
}

kstatus_t io_submit(io_request_t *req) {
    if (req == 0 || req->object.type != IO_REQUEST_OBJECT_TYPE ||
        req->device == 0 || !io_vectors_valid(req)) return K_EINVAL;
    /* QUEUED 请求是可取消的，其 finalizer 会释放执行引用。
     * 因此必须在发布 QUEUED 前先建立该引用。 */
    object_get(req);
    unsigned expected = IOREQ_NEW;
    if (!atomic_compare_exchange_strong_explicit(&req->state, &expected,
                                                  IOREQ_PREPARING,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        kstatus_t status = io_request_state_is_terminal(expected) ? req->status : K_EBUSY;
        object_put(req);
        return status;
    }
    req->submit_tsc = telemetry_timestamp();
    /* Finish private setup before exposing a cancellable request. Completion
     * acquires this publication before consuming/clearing submit_tsc. Only
     * the NEW -> PREPARING winner may initialize it. */
    atomic_store_explicit(&req->state, IOREQ_QUEUED, memory_order_release);
    if (!device_io_begin(req->device, req)) {
        unsigned current = atomic_load_explicit(&req->state, memory_order_acquire);
        if (current == IOREQ_QUEUED) {
            io_complete(req, K_EDEVREMOVED, 0);
            return K_EDEVREMOVED;
        }
        return io_request_state_is_terminal(current)
                   ? req->status
                   : K_EBUSY;
    }
    /* 取消可能在设备入队后、正式提交前获胜；只能用 CAS 发布 SUBMITTED。 */
    unsigned queued = IOREQ_QUEUED;
    if (!atomic_compare_exchange_strong_explicit(&req->state, &queued,
                                                  IOREQ_SUBMITTED,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        return io_request_state_is_terminal(queued)
                   ? req->status
                   : K_EBUSY;
    }
    if (req->device->ops == 0 || req->device->ops->submit_io == 0) {
        io_complete(req, K_EINVAL, 0);
        return K_EINVAL;
    }
    kstatus_t status = req->device->ops->submit_io(req->device, req);
    unsigned final_state = atomic_load_explicit(&req->state, memory_order_acquire);
    if (status != K_OK && final_state != IOREQ_COMPLETED) io_complete(req, status, 0);
    return status;
}

static void io_finalize_cancel_owned(io_request_t *req, kstatus_t status,
                                     bool execution_ref) {
    req->status = status;
    if (execution_ref && req->cancel != 0) req->cancel(req);
    /* The winning owner alone runs callbacks and publishes terminal state. */
    io_finalize_owned(req, status, req->bytes_done, execution_ref, IOREQ_COMPLETED);
}

bool io_request_claim_device_cancel(io_request_t *req) {
    if (req == 0) return false;
    unsigned state = atomic_load_explicit(&req->state, memory_order_acquire);
    while (io_state_is_inflight(state)) {
        if (atomic_compare_exchange_weak_explicit(&req->state, &state,
                IOREQ_COMPLETING, memory_order_acq_rel, memory_order_acquire))
            return true;
    }
    return false;
}

void io_request_finish_device_cancel(io_request_t *req, kstatus_t status) {
    /* A successful in-lock claim transfers the existing execution reference.
     * No rival can finalize or reuse the request before this owner finishes. */
    io_finalize_cancel_owned(req, status, true);
}

kstatus_t io_cancel_with_status(io_request_t *req, kstatus_t completion_status) {
    if (req == 0 || req->object.type != IO_REQUEST_OBJECT_TYPE) return K_EINVAL;
    if (completion_status >= 0) return K_EINVAL;
    unsigned state;
    for (;;) {
        state = atomic_load_explicit(&req->state, memory_order_acquire);
        if (io_request_state_is_terminal(state)) return K_OK;
        if (!io_state_is_inflight(state) && state != IOREQ_NEW) return K_EBUSY;
        unsigned expected = state;
        if (atomic_compare_exchange_weak_explicit(&req->state, &expected,
                                                  IOREQ_COMPLETING,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            break;
        }
    }
    io_finalize_cancel_owned(req, completion_status, state != IOREQ_NEW);
    return K_OK;
}

kstatus_t io_cancel(io_request_t *req) {
    return io_cancel_with_status(req, K_ECANCELED);
}

kstatus_t io_request_set_completion_port(io_request_t *req,
                                         struct completion_port *port,
                                         uint64_t user_key) {
    if (req == 0 || req->object.type != IO_REQUEST_OBJECT_TYPE || port == 0) {
        return K_EINVAL;
    }
    if (atomic_load_explicit(&req->state, memory_order_acquire) != IOREQ_NEW) {
        return K_EBUSY;
    }
    if (((completion_port_t *)port)->object.type != KOBJECT_TYPE_COMPLETION_PORT ||
        !object_try_get(port)) {
        return K_EINVAL;
    }
    if (req->completion_port != 0) {
        object_put(port);
        return K_EBUSY;
    }
    req->completion_port = port;
    req->completion_key = user_key;
    return K_OK;
}

kstatus_t io_request_register_user(io_request_t *req) {
    if (req == 0 || req->object.type != IO_REQUEST_OBJECT_TYPE ||
        req->process == 0 || req->request_id == 0U ||
        (req->internal_flags & IOREQ_INTERNAL_USER_REQUEST) != 0U) {
        return K_EINVAL;
    }
    if (process_register_teardown_callback(io_cancel_process) != K_OK) {
        return K_ENOSPC;
    }
    io_registry_initialize();
    io_registry_lock();
    /* The caller keeps its initial reference across publication and submit.
     * Teardown may remove the registry reference before registration returns. */
    object_get(req);
    io_registry_insert_tail(io_registry_bucket(req->request_id), &req->queue_node);
    req->internal_flags |= IOREQ_INTERNAL_USER_REQUEST;
    io_registry_unlock();
    return K_OK;
}

kstatus_t io_request_lookup_user(struct process *process, uint64_t request_id,
                                 io_request_t **out) {
    if (process == 0 || request_id == 0U || out == 0) return K_EINVAL;
    *out = 0;
    io_registry_initialize();
    io_registry_lock();
    list_head_t *bucket = io_registry_bucket(request_id);
    for (list_head_t *node = bucket->next; node != bucket;
         node = node->next) {
        io_request_t *request = list_entry(node, io_request_t, queue_node);
        if (request->process == process && request->request_id == request_id &&
            object_try_get(request)) {
            *out = request;
            io_registry_unlock();
            return K_OK;
        }
    }
    io_registry_unlock();
    return K_ENOENT;
}

void io_cancel_process(struct process *process) {
    if (process == 0) return;
    io_registry_initialize();
    /* Resume at the last selected bucket instead of rescanning drained
     * prefixes. Wrap around under one lock before declaring the owner empty:
     * a completion callback may register another request in an earlier bucket.
     * Cancellation remains outside the lock with an observer reference held. */
    unsigned bucket_index = 0;
    for (;;) {
        io_request_t *request = 0;
        bool busy = false;
        io_registry_lock();
        for (unsigned scanned = 0; scanned < IO_REGISTRY_BUCKETS &&
                                   request == 0; ++scanned) {
            unsigned index = (bucket_index + scanned) & (IO_REGISTRY_BUCKETS - 1U);
            list_head_t *bucket = &g_io_registry[index];
            for (list_head_t *node = bucket->next; node != bucket;
                 node = node->next) {
                io_request_t *candidate = list_entry(node, io_request_t, queue_node);
                if (candidate->process != process) continue;
                unsigned state = atomic_load_explicit(&candidate->state, memory_order_acquire);
                if (state == IOREQ_PREPARING || state == IOREQ_COMPLETING) {
                    busy = true;
                    continue;
                }
                if (object_try_get(candidate)) {
                    request = candidate;
                    bucket_index = index;
                    break;
                }
            }
        }
        io_registry_unlock();
        if (request == 0) {
            if (!busy) return;
            /* Other owners must publish setup/completion before we retry.
             * Do not churn references or starve cancellable requests behind
             * them; the complete locked scan above distinguishes an empty
             * registry from one containing only temporarily busy requests. */
            __asm__ volatile ("pause");
            continue;
        }
        (void)io_cancel(request);
        /* Successful cancellation finalizes synchronously. EBUSY means setup
         * or another finalizer owns the request: do not read bytes_done or
         * attempt completion on that owner's behalf. Retry the registry. */
        object_put(request);
    }
}
