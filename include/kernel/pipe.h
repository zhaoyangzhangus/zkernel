#pragma once

#include <kernel/base.h>
#include <kernel/object.h>
#include <kernel/spinlock.h>
#include <kernel/wait.h>
#include <uapi/file.h>
#include <uapi/pipe.h>

#define KOBJECT_TYPE_PIPE_ENDPOINT 0x0118U

#define PIPE_RIGHT_READ  (1U << 0)
#define PIPE_RIGHT_WRITE (1U << 1)
#define PIPE_RIGHT_WAIT  (1U << 31)
#define PIPE_RIGHT_ALL   (PIPE_RIGHT_READ | PIPE_RIGHT_WRITE | PIPE_RIGHT_WAIT)

#define PIPE_WAIT_READABLE OS_PIPE_WAIT_READABLE
#define PIPE_WAIT_HUP      OS_PIPE_WAIT_HUP
#define PIPE_WAIT_WRITABLE OS_PIPE_WAIT_WRITABLE
#define PIPE_WAIT_ERROR    OS_PIPE_WAIT_ERROR

typedef struct pipe_state pipe_state_t;

typedef struct pipe_endpoint {
    object_header_t object;
    pipe_state_t *state;
    bool read_end;
} pipe_endpoint_t;

kstatus_t pipe_create(uint32_t flags, pipe_endpoint_t **read_end,
                      pipe_endpoint_t **write_end);
kstatus_t pipe_read(pipe_endpoint_t *endpoint, void *buffer, size_t length,
                    uint64_t timeout_ns, uint64_t *bytes);
/* Recheck a protocol boundary immediately before consuming available bytes.
 * Called under the ring spinlock: the limiter must not sleep, allocate, or
 * re-enter pipe operations, and must return a value in [1, available]. */
typedef size_t (*pipe_read_limit_fn)(void *context, size_t available);
kstatus_t pipe_read_limited(pipe_endpoint_t *endpoint, void *buffer, size_t length,
                            uint64_t timeout_ns, uint64_t *bytes,
                            pipe_read_limit_fn limit, void *context);
kstatus_t pipe_write(pipe_endpoint_t *endpoint, const void *buffer,
                     size_t length, uint64_t timeout_ns, uint64_t *bytes);
kstatus_t pipe_stat(const pipe_endpoint_t *endpoint, os_file_info_t *info);
bool pipe_is_endpoint(const void *object);
bool pipe_endpoint_is_read(const pipe_endpoint_t *endpoint);
int64_t pipe_wait_value(const pipe_endpoint_t *endpoint);
/* Byte count snapshot; the caller retains an endpoint reference. */
kstatus_t pipe_pending_bytes(const pipe_endpoint_t *endpoint, uint64_t *bytes);
kstatus_t pipe_shutdown(pipe_endpoint_t *endpoint);
