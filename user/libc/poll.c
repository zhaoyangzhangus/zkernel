#include "libc/libc.h"

#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <uapi/linux_abi.h>

static bool poll_read_requested(short events) {
    return (events & (POLLIN | POLLPRI | POLLRDNORM | POLLRDBAND)) != 0;
}

static bool poll_write_requested(short events) {
    return (events & (POLLOUT | POLLWRNORM | POLLWRBAND)) != 0;
}

static short poll_socket_read_events(short requested) {
    short result = 0;
    if ((requested & (POLLIN | POLLRDNORM)) != 0) result |= POLLIN;
    if ((requested & (POLLPRI | POLLRDBAND)) != 0) result |= POLLPRI;
    return result;
}

static short poll_pipe_events(short requested, bool read_end, int64_t value) {
    short result = 0;
    if (read_end) {
        if ((value & OS_PIPE_WAIT_READABLE) != 0 &&
            (requested & (POLLIN | POLLRDNORM)) != 0) result |= POLLIN;
        if ((value & OS_PIPE_WAIT_HUP) != 0) result |= POLLHUP;
    } else {
        if ((value & OS_PIPE_WAIT_WRITABLE) != 0 &&
            poll_write_requested(requested)) result |= POLLOUT;
        if ((value & OS_PIPE_WAIT_ERROR) != 0) result |= POLLERR;
    }
    return result;
}

static uint64_t poll_timeout_ns(int timeout) {
    if (timeout < 0) return OS_WAIT_INFINITE;
    return (uint64_t)(unsigned int)timeout * UINT64_C(1000000);
}

static int poll_wait_many(const os_handle_t *handles, size_t count,
                          uint64_t timeout_ns, uint32_t *index,
                          int64_t *value) {
    os_wait_many_t request = {0};
    int64_t status;
    if (handles == 0 || count == 0U || count > OS_WAIT_MAX_HANDLES ||
        index == 0 || value == 0) {
        errno = EINVAL;
        return -1;
    }
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.count = (uint32_t)count;
    request.handles = (uint64_t)(uintptr_t)handles;
    request.timeout_ns = timeout_ns;
    status = __libc_native_syscall6(OS_SYS_WAIT_MANY,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    if (status == -(int64_t)ETIMEDOUT) return 0;
    if (status < 0) {
        (void)__libc_status_result(status);
        return -1;
    }
    if (request.result_index >= count) {
        errno = EIO;
        return -1;
    }
    *index = request.result_index;
    *value = request.result_value;
    return 1;
}

static int poll_sleep(int timeout) {
    struct timespec request;
    if (timeout == 0) return 0;
    if (timeout < 0) {
        request.tv_sec = 1;
        request.tv_nsec = 0;
        for (;;) {
            if (nanosleep(&request, 0) < 0) return -1;
        }
    }
    request.tv_sec = timeout / 1000;
    request.tv_nsec = (long)(timeout % 1000) * 1000000L;
    return nanosleep(&request, 0);
}

int poll(struct pollfd *fds, nfds_t count, int timeout) {
    os_handle_t handles[OS_WAIT_MAX_HANDLES];
    size_t handle_count = 0U;
    bool handle_ready[OS_WAIT_MAX_HANDLES] = {0};
    int64_t handle_values[OS_WAIT_MAX_HANDLES] = {0};
    int ready_count = 0;
    int saved_errno = errno;
    os_handle_t handle;
    size_t index;

    if (count > OS_WAIT_MAX_HANDLES || (fds == 0 && count != 0U)) {
        errno = fds == 0 && count != 0U ? EFAULT : EINVAL;
        return -1;
    }
    /* Linux 兼容模式下的 socket 位于 Linux fd 表中。等待操作应使用
     * their public fd numbers as native System handles cannot observe data. */
    if (count != 0U) {
        bool all_linux = true;
        bool any_linux = false;
        for (index = 0U; index < count; ++index) {
            if (fds[index].fd < 0) continue;
            if (__libc_descriptor_is_linux_fd(fds[index].fd)) {
                any_linux = true;
            } else {
                all_linux = false;
            }
        }
        if (all_linux && any_linux) {
            struct pollfd linux_fds[OS_WAIT_MAX_HANDLES];
            int64_t status;
            for (index = 0U; index < count; ++index) {
                linux_fds[index] = fds[index];
                if (fds[index].fd >= 0) {
                    linux_fds[index].fd =
                        __libc_linux_fd_for_descriptor(fds[index].fd);
                    if (linux_fds[index].fd < 0) return -1;
                }
            }
            status = __libc_linux_syscall6(
                LINUX_SYS_POLL, (uint64_t)(uintptr_t)linux_fds,
                (uint64_t)count, (uint64_t)(int64_t)timeout,
                0U, 0U, 0U);
            if (status < 0) return __libc_status_result(status);
            for (index = 0U; index < count; ++index) {
                fds[index].revents = linux_fds[index].revents;
            }
            return (int)status;
        }
    }
    for (index = 0U; index < count; ++index) {
        struct pollfd *entry = &fds[index];
        uint32_t open_flags;
        short immediate = 0;
        int descriptor_errno = errno;

        entry->revents = 0;
        if (entry->fd < 0) continue;
        handle = __libc_handle_for_descriptor(entry->fd);
        if (handle == OS_INVALID_HANDLE) {
            entry->revents = POLLNVAL;
            ++ready_count;
            errno = descriptor_errno;
            continue;
        }
        errno = descriptor_errno;
        if (__libc_descriptor_is_socket(entry->fd)) {
            if (poll_write_requested(entry->events)) immediate |= POLLOUT;
            if (immediate != 0) {
                entry->revents |= immediate;
                ++ready_count;
            }
            if (poll_read_requested(entry->events)) {
                handles[handle_count] = handle;
                ++handle_count;
            }
            continue;
        }
        if (__libc_descriptor_is_pipe(entry->fd)) {
            bool read_end = __libc_pipe_descriptor_is_read(entry->fd);
            if ((read_end && poll_read_requested(entry->events)) ||
                (!read_end && poll_write_requested(entry->events))) {
                handles[handle_count] = handle;
                ++handle_count;
            }
            continue;
        }
        open_flags = __libc_descriptor_open_flags(entry->fd);
        if ((entry->events & (POLLIN | POLLRDNORM)) != 0 &&
            (open_flags & O_ACCMODE) != O_WRONLY) {
            immediate |= POLLIN;
        }
        if ((entry->events & (POLLOUT | POLLWRNORM | POLLWRBAND)) != 0 &&
            (open_flags & O_ACCMODE) != O_RDONLY) {
            immediate |= POLLOUT;
        }
        entry->revents = immediate;
        if (immediate != 0) ++ready_count;
    }

    if (handle_count != 0U) {
        uint32_t signaled = 0U;
        int64_t wait_value = 0;
        int wait_result = poll_wait_many(handles, handle_count,
                                         ready_count == 0 ?
                                             poll_timeout_ns(timeout) : 0U,
                                         &signaled, &wait_value);
        if (wait_result < 0) {
            return -1;
        }
        if (wait_result > 0) {
            handle_ready[signaled] = true;
            handle_values[signaled] = wait_value;
        }
        for (index = 0U; index < handle_count; ++index) {
            uint32_t ignored = 0U;
            int64_t ignored_value = 0;
            if (handle_ready[index]) continue;
            wait_result = poll_wait_many(&handles[index], 1U, 0U, &ignored,
                                         &ignored_value);
            if (wait_result < 0) {
                return -1;
            }
            if (wait_result > 0) {
                handle_ready[index] = true;
                handle_values[index] = ignored_value;
            }
        }
        for (index = 0U; index < count; ++index) {
            struct pollfd *entry = &fds[index];
            size_t handle_index;
            if (entry->fd < 0 || (!__libc_descriptor_is_socket(entry->fd) &&
                                  !__libc_descriptor_is_pipe(entry->fd))) continue;
            handle = __libc_handle_for_descriptor(entry->fd);
            for (handle_index = 0U; handle_index < handle_count;
                 ++handle_index) {
                if (handles[handle_index] == handle && handle_ready[handle_index]) {
                    short events;
                    if (__libc_descriptor_is_socket(entry->fd)) {
                        if (!poll_read_requested(entry->events)) break;
                        events = poll_socket_read_events(entry->events);
                    } else {
                        events = poll_pipe_events(
                            entry->events,
                            __libc_pipe_descriptor_is_read(entry->fd),
                            handle_values[handle_index]);
                    }
                    if (events != 0) {
                        if (entry->revents == 0) ++ready_count;
                        entry->revents |= events;
                    }
                    break;
                }
            }
        }
    } else if (ready_count == 0) {
        if (poll_sleep(timeout) < 0) return -1;
    }
    errno = saved_errno;
    return ready_count;
}
