/*
 * 面向 Chromium 兼容层的小型 Linux/glibc 运行时探测程序。它有意使用真实的 libc 系统调用
 * 封装，使探测过程与分阶段启动的浏览器走相同的 Linux personality 入口路径。
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <pthread.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <libc/libc.h>
#include <uapi/socket.h>
#include <uapi/syscall.h>
#include <uapi/time.h>

extern bool __libc_descriptor_is_socket(int descriptor);
extern bool __libc_descriptor_is_linux_fd(int descriptor);

#define SYS_epoll_wait 232
#define SYS_epoll_ctl 233
#define SYS_read 0
#define SYS_write 1
#define SYS_close 3
#define SYS_dup 32
#define SYS_fstat 5
#define SYS_ftruncate 77
#define SYS_lseek 8
#define SYS_poll 7
#define SYS_dup2 33
#define SYS_socketpair 53
#define SYS_sendto 44
#define SYS_recvfrom 45
#define SYS_sendmsg 46
#define SYS_recvmsg 47
#define SYS_eventfd2 290
#define SYS_epoll_create1 291
#define SYS_pipe2 293
#define SYS_openat 257
#define SYS_newfstatat 262
#define SYS_statx 332
#define SYS_execve 59
#define SYS_fcntl 72
#define SYS_clock_gettime 228
#define SYS_futex 202

static __thread volatile uint64_t g_linux_tls_probe
    __attribute__((tls_model("local-exec"), aligned(32)));

#define LINUX_MAIN_INITIALIZER_MAGIC UINT64_C(0x4c4954454f53494e)
#define LINUX_MAIN_TLS_INITIALIZER_MAGIC UINT64_C(0x544c53494e495421)

static volatile uint64_t g_linux_main_bss_probe;
static volatile uint64_t g_linux_main_initializer_probe;
static volatile uint64_t g_linux_main_initializer_count;
static volatile uintptr_t g_linux_main_argc_probe;
static volatile uintptr_t g_linux_main_argv_probe;
static volatile uintptr_t g_linux_main_envp_probe;

typedef void (*linux_main_initializer_t)(int argc, char **argv, char **envp);
static void linux_main_initializer(int argc, char **argv, char **envp);
__attribute__((used, section(".init_array")))
static linux_main_initializer_t const g_linux_main_initializer_entry =
    linux_main_initializer;

static void linux_main_initializer(int argc, char **argv, char **envp) {
    ++g_linux_main_initializer_count;
    g_linux_main_initializer_probe =
        (g_linux_main_bss_probe == 0U && g_linux_tls_probe == 0U &&
         argc > 0 && argv != 0 && argv[0] != 0 && envp != 0)
            ? LINUX_MAIN_INITIALIZER_MAGIC : 0U;
    g_linux_main_argc_probe = (uintptr_t)argc;
    g_linux_main_argv_probe = (uintptr_t)argv;
    g_linux_main_envp_probe = (uintptr_t)envp;
    g_linux_main_bss_probe = LINUX_MAIN_INITIALIZER_MAGIC;
    g_linux_tls_probe = LINUX_MAIN_TLS_INITIALIZER_MAGIC;
}

#define EFD_NONBLOCK O_NONBLOCK
#define EFD_CLOEXEC O_CLOEXEC
#define EPOLL_CLOEXEC O_CLOEXEC
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLLIN 0x00000001U
#define EPOLLOUT 0x00000004U
#define EPOLLONESHOT (1U << 30)
#define POLLOUT 0x0004
#define AF_UNIX 1
#define SOCK_STREAM 1
#define POLLIN 0x0001

struct __attribute__((packed)) epoll_event {
    uint32_t events;
    uint64_t data;
};

struct linux_iovec {
    uint64_t base;
    uint64_t length;
};

struct linux_msghdr {
    uint64_t name;
    uint32_t name_length;
    uint32_t name_padding;
    uint64_t iov_pointer;
    uint64_t iov_length;
    uint64_t control;
    uint64_t control_length;
    uint32_t flags;
    uint32_t flags_padding;
};

struct linux_cmsghdr {
    uint64_t length;
    int32_t level;
    int32_t type;
};

struct linux_timespec_abi {
    int64_t seconds;
    int64_t nanoseconds;
};

struct linux_stat_abi {
    uint64_t device;
    uint64_t inode;
    uint64_t links;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode_padding;
    uint64_t rdevice;
    int64_t size;
    int64_t block_size;
    int64_t blocks;
    struct linux_timespec_abi access_time;
    struct linux_timespec_abi modify_time;
    struct linux_timespec_abi change_time;
    int64_t reserved[3];
};

struct linux_statx_timestamp_abi {
    int64_t seconds;
    uint32_t nanoseconds;
    int32_t reserved;
};

struct linux_statx_abi {
    uint32_t mask;
    uint32_t block_size;
    uint64_t attributes;
    uint32_t link_count;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
    uint16_t reserved0;
    uint64_t inode;
    uint64_t size;
    uint64_t blocks;
    uint64_t attributes_mask;
    struct linux_statx_timestamp_abi access_time;
    struct linux_statx_timestamp_abi birth_time;
    struct linux_statx_timestamp_abi change_time;
    struct linux_statx_timestamp_abi modify_time;
    uint32_t rdevice_major;
    uint32_t rdevice_minor;
    uint32_t device_major;
    uint32_t device_minor;
    uint64_t reserved[14];
};

_Static_assert(sizeof(struct linux_stat_abi) == 144U,
               "Linux x86-64 stat ABI changed");
_Static_assert(sizeof(struct linux_statx_abi) == 256U,
               "Linux statx ABI changed");

static int g_failure_code = 1;

/* 这些描述符由原始 Linux 系统调用创建，因此 I/O 也使用相同的系统调用路径，而不是依赖
 * libc 私有描述符表来判断各 fd 的打开方式。 */
static ssize_t linux_raw_read(int descriptor, void *buffer, size_t length) {
    return (ssize_t)syscall(SYS_read, descriptor, buffer, length);
}

static ssize_t linux_raw_write(int descriptor, const void *buffer,
                               size_t length) {
    return (ssize_t)syscall(SYS_write, descriptor, buffer, length);
}

static int linux_raw_close(int descriptor) {
    return (int)syscall(SYS_close, descriptor);
}

/* ABI 探测中使用的原始 Linux 系统调用别名位于此处；libc 创建的描述符仍走下方 libc
 * close 路径。 */
static int libc_close_descriptor(int descriptor) {
    return close(descriptor);
}

static ssize_t libc_read_descriptor(int descriptor, void *buffer,
                                    size_t length) {
    return read(descriptor, buffer, length);
}

static ssize_t libc_write_descriptor(int descriptor, const void *buffer,
                                     size_t length) {
    return write(descriptor, buffer, length);
}

static int libc_poll_descriptor(struct pollfd *descriptors,
                                unsigned long count, int timeout) {
    return poll(descriptors, count, timeout);
}

static int linux_raw_poll(struct pollfd *descriptors, unsigned long count,
                          int timeout) {
    return (int)syscall(SYS_poll, descriptors, count, timeout);
}

#define read linux_raw_read
#define write linux_raw_write
#define close linux_raw_close
#define poll linux_raw_poll

enum epoll_worker_action {
    EPOLL_WORKER_MOD_READY,
    EPOLL_WORKER_DEL_AND_SIGNAL,
    EPOLL_WORKER_ADD_AND_SIGNAL,
    EPOLL_WORKER_MOD_QUIET_REPEATEDLY,
};

struct epoll_worker_context {
    enum epoll_worker_action action;
    int epoll_descriptor;
    int first_descriptor;
    int second_descriptor;
    struct epoll_event event;
    int64_t control_status[3];
    int64_t write_status;
};

static void delay_milliseconds(uint64_t milliseconds) {
    struct timespec delay = {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)((milliseconds % 1000U) * 1000000U),
    };
    while (nanosleep(&delay, &delay) != 0) {}
}

static void *epoll_control_worker(void *opaque) {
    struct epoll_worker_context *context =
        (struct epoll_worker_context *)opaque;
    uint64_t one = 1U;
    delay_milliseconds(25U);
    if (context->action == EPOLL_WORKER_MOD_READY) {
        context->control_status[0] = syscall(
            SYS_epoll_ctl, context->epoll_descriptor, EPOLL_CTL_MOD,
            context->first_descriptor, &context->event);
    } else if (context->action == EPOLL_WORKER_DEL_AND_SIGNAL) {
        context->control_status[0] = syscall(
            SYS_epoll_ctl, context->epoll_descriptor, EPOLL_CTL_DEL,
            context->first_descriptor, 0U);
        (void)close(context->first_descriptor);
        context->write_status = write(context->second_descriptor, &one,
                                      sizeof(one));
    } else if (context->action == EPOLL_WORKER_ADD_AND_SIGNAL) {
        context->control_status[0] = syscall(
            SYS_epoll_ctl, context->epoll_descriptor, EPOLL_CTL_ADD,
            context->first_descriptor, &context->event);
        context->write_status = write(context->first_descriptor, &one,
                                      sizeof(one));
    } else {
        for (unsigned index = 0U; index < 3U; ++index) {
            context->event.data = UINT64_C(0x515549455400) + index;
            context->control_status[index] = syscall(
                SYS_epoll_ctl, context->epoll_descriptor, EPOLL_CTL_MOD,
                context->first_descriptor, &context->event);
            if (context->control_status[index] != 0) break;
            if (index != 2U) delay_milliseconds(25U);
        }
    }
    return 0;
}

static int test_epoll_control_change_wakes_waiter(void) {
    struct epoll_worker_context context = {0};
    struct epoll_event result = {0};
    pthread_t worker;
    int epoll_descriptor = (int)syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
    int event_descriptor = (int)syscall(SYS_eventfd2, 0U,
                                         EFD_NONBLOCK | EFD_CLOEXEC);
    int result_count = -1;
    int success = 0;
    if (epoll_descriptor < 0 || event_descriptor < 0) goto cleanup;
    context.action = EPOLL_WORKER_MOD_READY;
    context.epoll_descriptor = epoll_descriptor;
    context.first_descriptor = event_descriptor;
    struct epoll_event initial = {
        .events = EPOLLIN,
        .data = UINT64_C(0x4D4F4449),
    };
    context.event = (struct epoll_event){
        .events = EPOLLOUT,
        .data = initial.data,
    };
    if (syscall(SYS_epoll_ctl, epoll_descriptor, EPOLL_CTL_ADD,
                event_descriptor, &initial) != 0 ||
        pthread_create(&worker, 0, epoll_control_worker, &context) != 0) {
        goto cleanup;
    }
    result_count = (int)syscall(SYS_epoll_wait, epoll_descriptor, &result,
                                1, 1000);
    (void)pthread_join(worker, 0);
    success = context.control_status[0] == 0 && result_count == 1 &&
              result.events == EPOLLOUT &&
              result.data == UINT64_C(0x4D4F4449);
cleanup:
    if (event_descriptor >= 0) (void)close(event_descriptor);
    if (epoll_descriptor >= 0) (void)close(epoll_descriptor);
    return success;
}

static int test_epoll_source_wake_survives_del_and_close(void) {
    struct epoll_worker_context context = {0};
    struct epoll_event removed_event = {
        .events = EPOLLIN,
        .data = UINT64_C(0x44454C45),
    };
    struct epoll_event remaining_event = {
        .events = EPOLLIN,
        .data = UINT64_C(0x52454D41),
    };
    struct epoll_event result = {0};
    pthread_t worker;
    int epoll_descriptor = (int)syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
    int removed_descriptor = (int)syscall(SYS_eventfd2, 0U,
                                           EFD_NONBLOCK | EFD_CLOEXEC);
    int remaining_descriptor = (int)syscall(SYS_eventfd2, 0U,
                                             EFD_NONBLOCK | EFD_CLOEXEC);
    int result_count = -1;
    int worker_started = 0;
    int success = 0;
    if (epoll_descriptor < 0 || removed_descriptor < 0 ||
        remaining_descriptor < 0) goto cleanup;
    if (syscall(SYS_epoll_ctl, epoll_descriptor, EPOLL_CTL_ADD,
                removed_descriptor, &removed_event) != 0 ||
        syscall(SYS_epoll_ctl, epoll_descriptor, EPOLL_CTL_ADD,
                remaining_descriptor, &remaining_event) != 0) goto cleanup;
    context.action = EPOLL_WORKER_DEL_AND_SIGNAL;
    context.epoll_descriptor = epoll_descriptor;
    context.first_descriptor = removed_descriptor;
    context.second_descriptor = remaining_descriptor;
    if (pthread_create(&worker, 0, epoll_control_worker, &context) != 0) {
        goto cleanup;
    }
    worker_started = 1;
    result_count = (int)syscall(SYS_epoll_wait, epoll_descriptor, &result,
                                1, 1000);
    (void)pthread_join(worker, 0);
    worker_started = 0;
    success = context.control_status[0] == 0 &&
              context.write_status == (int64_t)sizeof(uint64_t) &&
              result_count == 1 && result.events == EPOLLIN &&
              result.data == remaining_event.data;
cleanup:
    if (worker_started) (void)pthread_join(worker, 0);
    if (removed_descriptor >= 0) (void)close(removed_descriptor);
    if (remaining_descriptor >= 0) (void)close(remaining_descriptor);
    if (epoll_descriptor >= 0) (void)close(epoll_descriptor);
    return success;
}

static int test_epoll_add_then_signal_resnapshots(void) {
    struct epoll_worker_context context = {0};
    struct epoll_event result = {0};
    pthread_t worker;
    int epoll_descriptor = (int)syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
    int event_descriptor = (int)syscall(SYS_eventfd2, 0U,
                                         EFD_NONBLOCK | EFD_CLOEXEC);
    int result_count = -1;
    int worker_started = 0;
    int success = 0;
    if (epoll_descriptor < 0 || event_descriptor < 0) goto cleanup;
    context.action = EPOLL_WORKER_ADD_AND_SIGNAL;
    context.epoll_descriptor = epoll_descriptor;
    context.first_descriptor = event_descriptor;
    context.event = (struct epoll_event){
        .events = EPOLLIN,
        .data = UINT64_C(0x41444445),
    };
    if (pthread_create(&worker, 0, epoll_control_worker, &context) != 0) {
        goto cleanup;
    }
    worker_started = 1;
    result_count = (int)syscall(SYS_epoll_wait, epoll_descriptor, &result,
                                1, 1000);
    (void)pthread_join(worker, 0);
    worker_started = 0;
    success = context.control_status[0] == 0 &&
              context.write_status == (int64_t)sizeof(uint64_t) &&
              result_count == 1 && result.events == EPOLLIN &&
              result.data == context.event.data;
cleanup:
    if (worker_started) (void)pthread_join(worker, 0);
    if (event_descriptor >= 0) (void)close(event_descriptor);
    if (epoll_descriptor >= 0) (void)close(epoll_descriptor);
    return success;
}

static uint64_t monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0U;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static int test_epoll_false_control_wakes_keep_original_deadline(void) {
    struct epoll_worker_context context = {0};
    struct epoll_event result = {0};
    struct epoll_event initial = {
        .events = EPOLLIN,
        .data = UINT64_C(0x5155494554),
    };
    pthread_t worker;
    int epoll_descriptor = (int)syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
    int event_descriptor = (int)syscall(SYS_eventfd2, 0U,
                                         EFD_NONBLOCK | EFD_CLOEXEC);
    uint64_t start_ms;
    uint64_t elapsed_ms;
    int result_count = -1;
    int worker_started = 0;
    int success = 0;
    if (epoll_descriptor < 0 || event_descriptor < 0) goto cleanup;
    if (syscall(SYS_epoll_ctl, epoll_descriptor, EPOLL_CTL_ADD,
                event_descriptor, &initial) != 0) goto cleanup;
    context.action = EPOLL_WORKER_MOD_QUIET_REPEATEDLY;
    context.epoll_descriptor = epoll_descriptor;
    context.first_descriptor = event_descriptor;
    context.event = initial;
    if (pthread_create(&worker, 0, epoll_control_worker, &context) != 0) {
        goto cleanup;
    }
    worker_started = 1;
    start_ms = monotonic_milliseconds();
    result_count = (int)syscall(SYS_epoll_wait, epoll_descriptor, &result,
                                1, 160);
    elapsed_ms = monotonic_milliseconds() - start_ms;
    (void)pthread_join(worker, 0);
    worker_started = 0;
    success = context.control_status[0] == 0 &&
              context.control_status[1] == 0 &&
              context.control_status[2] == 0 && result_count == 0 &&
              elapsed_ms >= 120U && elapsed_ms < 230U;
cleanup:
    if (worker_started) (void)pthread_join(worker, 0);
    if (event_descriptor >= 0) (void)close(event_descriptor);
    if (epoll_descriptor >= 0) (void)close(epoll_descriptor);
    return success;
}

struct unix_epoll_worker_context {
    int writer;
    ssize_t write_result;
};

static void *unix_epoll_writer(void *opaque) {
    struct unix_epoll_worker_context *context =
        (struct unix_epoll_worker_context *)opaque;
    const char byte = 'M';
    delay_milliseconds(25U);
    context->write_result = write(context->writer, &byte, 1U);
    return 0;
}

static int test_epoll_unix_socket_wakes_on_delayed_write(void) {
    int sockets[2] = {-1, -1};
    int epoll_descriptor = -1;
    pthread_t worker;
    int worker_started = 0;
    int success = 0;
    char received = 0;
    struct epoll_event result = {0};
    struct epoll_event watch = {
        .events = EPOLLIN,
        .data = UINT64_C(0x554E4958),
    };
    struct unix_epoll_worker_context context = {
        .writer = -1,
        .write_result = -1,
    };
    if (syscall(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        goto cleanup;
    epoll_descriptor = (int)syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
    if (epoll_descriptor < 0 ||
        syscall(SYS_epoll_ctl, epoll_descriptor, EPOLL_CTL_ADD,
                sockets[1], &watch) != 0) goto cleanup;
    context.writer = sockets[0];
    if (pthread_create(&worker, 0, unix_epoll_writer, &context) != 0)
        goto cleanup;
    worker_started = 1;
    int ready = (int)syscall(SYS_epoll_wait, epoll_descriptor, &result,
                             1, 1000);
    (void)pthread_join(worker, 0);
    worker_started = 0;
    success = ready == 1 && result.events == EPOLLIN &&
              result.data == watch.data && context.write_result == 1 &&
              read(sockets[1], &received, 1U) == 1 && received == 'M';
cleanup:
    if (worker_started) (void)pthread_join(worker, 0);
    if (epoll_descriptor >= 0) (void)close(epoll_descriptor);
    if (sockets[0] >= 0) (void)close(sockets[0]);
    if (sockets[1] >= 0) (void)close(sockets[1]);
    return success;
}

static int test_poll_unix_socket_wakes_on_delayed_write(void) {
    int sockets[2] = {-1, -1};
    pthread_t worker;
    int worker_started = 0;
    int success = 0;
    char received = 0;
    struct pollfd descriptor = {.fd = -1, .events = POLLIN};
    struct unix_epoll_worker_context context = {
        .writer = -1,
        .write_result = -1,
    };
    if (syscall(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        goto cleanup;
    descriptor.fd = sockets[1];
    context.writer = sockets[0];
    if (pthread_create(&worker, 0, unix_epoll_writer, &context) != 0)
        goto cleanup;
    worker_started = 1;
    int ready = poll(&descriptor, 1U, 1000);
    (void)pthread_join(worker, 0);
    worker_started = 0;
    success = ready == 1 && (descriptor.revents & POLLIN) != 0 &&
              context.write_result == 1 &&
              read(sockets[1], &received, 1U) == 1 && received == 'M';
cleanup:
    if (worker_started) (void)pthread_join(worker, 0);
    if (sockets[0] >= 0) (void)close(sockets[0]);
    if (sockets[1] >= 0) (void)close(sockets[1]);
    return success;
}

static int test_epoll_libc_inet_socket(void) {
    struct epoll_event watch = {
        .events = EPOLLIN,
        .data = UINT64_C(0x494E4554),
    };
    struct epoll_event result = {0};
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(18083U),
        .sin_addr.s_addr = htonl(0x7F000001U),
    };
    struct sockaddr_in source = {0};
    socklen_t source_length = sizeof(source);
    struct pollfd poll_event;
    char received = 0;
    const char payload = 'e';
    int epoll_descriptor = (int)syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
    int receiver = -1;
    int sender = -1;
    int success = 0;
    if (epoll_descriptor < 0) return 0;
    receiver = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sender = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (receiver < 0 || sender < 0 ||
        !__libc_descriptor_is_linux_fd(receiver) ||
        !__libc_descriptor_is_linux_fd(sender) ||
        bind(receiver, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        connect(sender, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        syscall(SYS_epoll_ctl, epoll_descriptor, EPOLL_CTL_ADD,
                receiver, &watch) != 0 ||
        send(sender, &payload, sizeof(payload), 0) !=
            (ssize_t)sizeof(payload)) goto cleanup;
    if (syscall(SYS_epoll_wait, epoll_descriptor, &result, 1, 1000) != 1 ||
        (result.events & EPOLLIN) == 0U || result.data != watch.data)
        goto cleanup;
    poll_event = (struct pollfd){receiver, POLLIN, 0};
    if (libc_poll_descriptor(&poll_event, 1U, 0) != 1 ||
        (poll_event.revents & POLLIN) == 0 ||
        recvfrom(receiver, &received, sizeof(received), MSG_DONTWAIT,
                 (struct sockaddr *)&source, &source_length) !=
            (ssize_t)sizeof(received) || received != payload ||
        source_length != sizeof(source) || source.sin_family != AF_INET)
        goto cleanup;
    if (syscall(SYS_epoll_ctl, epoll_descriptor, EPOLL_CTL_DEL,
                receiver, 0U) != 0) goto cleanup;
    success = 1;
cleanup:
    if (sender >= 0) (void)libc_close_descriptor(sender);
    if (receiver >= 0) (void)libc_close_descriptor(receiver);
    (void)close(epoll_descriptor);
    return success;
}

static int test_epoll_watch_holes_and_tail_reuse(void) {
    int epoll_descriptor = (int)syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
    int descriptors[3] = {-1, -1, -1};
    struct epoll_event watches[3] = {
        {.events = EPOLLIN, .data = UINT64_C(0x484F4C4530)},
        {.events = EPOLLIN, .data = UINT64_C(0x484F4C4531)},
        {.events = EPOLLIN, .data = UINT64_C(0x484F4C4532)},
    };
    struct epoll_event result = {0};
    uint64_t counter = 1U;
    uint64_t received = 0U;
    int success = 0;
    if (epoll_descriptor < 0) goto cleanup;
    for (unsigned index = 0U; index < 3U; ++index) {
        descriptors[index] = (int)syscall(SYS_eventfd2, 0U,
                                           EFD_NONBLOCK | EFD_CLOEXEC);
        if (descriptors[index] < 0 ||
            syscall(SYS_epoll_ctl, epoll_descriptor, EPOLL_CTL_ADD,
                    descriptors[index], &watches[index]) != 0) goto cleanup;
    }
    /* 删除中间的监视项，并在后续监视项仍有效时重用其槽位。 */
    if (syscall(SYS_epoll_ctl, epoll_descriptor, EPOLL_CTL_DEL,
                descriptors[1], 0U) != 0 ||
        write(descriptors[2], &counter, sizeof(counter)) !=
            (ssize_t)sizeof(counter) ||
        syscall(SYS_epoll_wait, epoll_descriptor, &result, 1, 0) != 1 ||
        result.data != watches[2].data ||
        read(descriptors[2], &received, sizeof(received)) !=
            (ssize_t)sizeof(received) || received != counter ||
        syscall(SYS_epoll_ctl, epoll_descriptor, EPOLL_CTL_ADD,
                descriptors[1], &watches[1]) != 0 ||
        write(descriptors[1], &counter, sizeof(counter)) !=
            (ssize_t)sizeof(counter) ||
        syscall(SYS_epoll_wait, epoll_descriptor, &result, 1, 0) != 1 ||
        result.data != watches[1].data ||
        read(descriptors[1], &received, sizeof(received)) !=
            (ssize_t)sizeof(received) || received != counter) goto cleanup;
    /* 移除尾部及其余所有监视项后，必须允许新监视项使用首个槽位，且不能暴露过期事件。 */
    for (int index = 2; index >= 0; --index) {
        if (syscall(SYS_epoll_ctl, epoll_descriptor, EPOLL_CTL_DEL,
                    descriptors[index], 0U) != 0) goto cleanup;
    }
    if (syscall(SYS_epoll_wait, epoll_descriptor, &result, 1, 0) != 0 ||
        syscall(SYS_epoll_ctl, epoll_descriptor, EPOLL_CTL_ADD,
                descriptors[2], &watches[2]) != 0 ||
        write(descriptors[2], &counter, sizeof(counter)) !=
            (ssize_t)sizeof(counter) ||
        syscall(SYS_epoll_wait, epoll_descriptor, &result, 1, 0) != 1 ||
        result.data != watches[2].data) goto cleanup;
    success = 1;
cleanup:
    for (unsigned index = 0U; index < 3U; ++index) {
        if (descriptors[index] >= 0) (void)close(descriptors[index]);
    }
    if (epoll_descriptor >= 0) (void)close(epoll_descriptor);
    return success;
}

static int test_epoll_close_and_fd_reuse(void) {
    int epoll_descriptor = -1;
    int original = -1;
    int closed_descriptor = -1;
    int alias = -1;
    int replacement = -1;
    int success = 0;
    uint64_t one = 1U;
    struct epoll_event events[2] = {{0}, {0}};
    struct epoll_event old_watch = {
        .events = EPOLLIN,
        .data = UINT64_C(0x45504F4C4C4F4C44),
    };
    struct epoll_event new_watch = {
        .events = EPOLLIN,
        .data = UINT64_C(0x45504F4C4C4E4557),
    };

    epoll_descriptor = (int)syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
    original = (int)syscall(SYS_eventfd2, 0U,
                            EFD_NONBLOCK | EFD_CLOEXEC);
    if (epoll_descriptor < 0 || original < 0 ||
        syscall(SYS_epoll_ctl, epoll_descriptor, EPOLL_CTL_ADD,
                original, &old_watch) != 0) goto cleanup;

    alias = (int)syscall(SYS_dup, original);
    closed_descriptor = original;
    if (alias < 0 || close(original) != 0) goto cleanup;
    original = -1;
    replacement = (int)syscall(SYS_eventfd2, 0U,
                                EFD_NONBLOCK | EFD_CLOEXEC);
    if (replacement != closed_descriptor || replacement == alias ||
        syscall(SYS_epoll_ctl, epoll_descriptor, EPOLL_CTL_ADD,
                replacement, &new_watch) != 0 ||
        write(alias, &one, sizeof(one)) != (ssize_t)sizeof(one) ||
        write(replacement, &one, sizeof(one)) != (ssize_t)sizeof(one) ||
        syscall(SYS_epoll_wait, epoll_descriptor, events, 2, 1000) != 2 ||
        !((events[0].data == old_watch.data &&
           events[1].data == new_watch.data) ||
          (events[0].data == new_watch.data &&
           events[1].data == old_watch.data))) goto cleanup;

    if (close(alias) != 0) goto cleanup;
    alias = -1;
    if (syscall(SYS_epoll_wait, epoll_descriptor, events, 2, 0) != 1 ||
        events[0].data != new_watch.data ||
        close(replacement) != 0) goto cleanup;
    replacement = -1;
    if (syscall(SYS_epoll_wait, epoll_descriptor, events, 2, 0) != 0)
        goto cleanup;
    success = 1;
cleanup:
    if (original >= 0) (void)close(original);
    if (alias >= 0) (void)close(alias);
    if (replacement >= 0) (void)close(replacement);
    if (epoll_descriptor >= 0) (void)close(epoll_descriptor);
    return success;
}

static int test_unix_sendto_recvfrom(void) {
    static const char request[] = "mojo-ping";
    static const char reply[] = "mojo-pong";
    char received[sizeof(request)] = {0};
    int sockets[2] = {-1, -1};
    int success = 0;
    if (syscall(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        goto cleanup;
    if (syscall(SYS_sendto, sockets[0], request, sizeof(request),
                MSG_NOSIGNAL, 0, 0) != (long)sizeof(request) ||
        syscall(SYS_recvfrom, sockets[1], received, sizeof(received),
                MSG_DONTWAIT, 0, 0) != (long)sizeof(request) ||
        memcmp(received, request, sizeof(request)) != 0 ||
        syscall(SYS_sendto, sockets[1], reply, sizeof(reply),
                MSG_NOSIGNAL, 0, 0) != (long)sizeof(reply) ||
        syscall(SYS_recvfrom, sockets[0], received, sizeof(reply),
                MSG_DONTWAIT, 0, 0) != (long)sizeof(reply) ||
        memcmp(received, reply, sizeof(reply)) != 0) goto cleanup;
    success = 1;
cleanup:
    if (sockets[0] >= 0) (void)close(sockets[0]);
    if (sockets[1] >= 0) (void)close(sockets[1]);
    return success;
}

static int test_unix_libc_send_recv(void) {
    static const char request[] = "mojo-libc-ping";
    static const char reply[] = "mojo-libc-pong";
    char received[sizeof(request)] = {0};
    int sockets[2] = {-1, -1};
    int success = 0;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        goto cleanup;
    if (send(sockets[0], request, sizeof(request), MSG_NOSIGNAL) !=
            (ssize_t)sizeof(request) ||
        recv(sockets[1], received, sizeof(request), MSG_DONTWAIT) !=
            (ssize_t)sizeof(request) ||
        memcmp(received, request, sizeof(request)) != 0 ||
        sendto(sockets[1], reply, sizeof(reply), MSG_NOSIGNAL, 0, 0) !=
            (ssize_t)sizeof(reply) ||
        recvfrom(sockets[0], received, sizeof(reply), MSG_DONTWAIT, 0, 0) !=
            (ssize_t)sizeof(reply) ||
        memcmp(received, reply, sizeof(reply)) != 0)
        goto cleanup;
    success = 1;
cleanup:
    if (sockets[0] >= 0) (void)close(sockets[0]);
    if (sockets[1] >= 0) (void)close(sockets[1]);
    return success;
}

static int test_unix_ancillary_stream_offsets(void) {
    static const char plain[] = "plain";
    static const char raw[] = "raw";
    char received[sizeof(plain)] = {0};
    char tag = 'F';
    char received_tag = 0;
    char send_control[CMSG_SPACE(sizeof(int))] = {0};
    char receive_control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec send_vector = {.iov_base = &tag, .iov_len = 1U};
    struct iovec receive_vector = {.iov_base = received,
                                   .iov_len = sizeof(plain)};
    struct msghdr send_message = {
        .msg_iov = &send_vector,
        .msg_iovlen = 1U,
        .msg_control = send_control,
        .msg_controllen = sizeof(send_control),
    };
    struct msghdr receive_message = {
        .msg_iov = &receive_vector,
        .msg_iovlen = 1U,
        .msg_control = receive_control,
        .msg_controllen = sizeof(receive_control),
    };
    int sockets[2] = {-1, -1};
    int sent_fd = -1;
    int received_fd = -1;
    int success = 0;
    struct cmsghdr *control;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        goto cleanup;
    sent_fd = open("/dev/null", O_RDONLY);
    if (sent_fd < 0) goto cleanup;
    control = CMSG_FIRSTHDR(&send_message);
    if (control == 0) goto cleanup;
    control->cmsg_level = SOL_SOCKET;
    control->cmsg_type = SCM_RIGHTS;
    control->cmsg_len = CMSG_LEN(sizeof(sent_fd));
    memcpy(CMSG_DATA(control), &sent_fd, sizeof(sent_fd));

    if (send(sockets[0], plain, sizeof(plain), MSG_NOSIGNAL) !=
            (ssize_t)sizeof(plain) ||
        sendmsg(sockets[0], &send_message, MSG_NOSIGNAL) != 1 ||
        recvmsg(sockets[1], &receive_message, MSG_DONTWAIT) !=
            (ssize_t)sizeof(plain) ||
        memcmp(received, plain, sizeof(plain)) != 0 ||
        receive_message.msg_controllen != 0U)
        goto cleanup;
    receive_vector.iov_base = &received_tag;
    receive_vector.iov_len = 1U;
    receive_message.msg_controllen = sizeof(receive_control);
    if (recvmsg(sockets[1], &receive_message, MSG_DONTWAIT) != 1 ||
        received_tag != tag)
        goto cleanup;
    control = CMSG_FIRSTHDR(&receive_message);
    if (control == 0 || control->cmsg_level != SOL_SOCKET ||
        control->cmsg_type != SCM_RIGHTS ||
        control->cmsg_len < CMSG_LEN(sizeof(received_fd)))
        goto cleanup;
    memcpy(&received_fd, CMSG_DATA(control), sizeof(received_fd));
    if (fcntl(received_fd, F_GETFD) < 0) goto cleanup;
    (void)close(received_fd);
    received_fd = -1;

    tag = 'G';
    received_tag = 0;
    receive_message.msg_controllen = sizeof(receive_control);
    if (write(sockets[0], raw, sizeof(raw)) != (ssize_t)sizeof(raw) ||
        sendmsg(sockets[0], &send_message, MSG_NOSIGNAL) != 1 ||
        read(sockets[1], received, sizeof(raw)) != (ssize_t)sizeof(raw) ||
        memcmp(received, raw, sizeof(raw)) != 0 ||
        recvmsg(sockets[1], &receive_message, MSG_DONTWAIT) != 1 ||
        received_tag != tag)
        goto cleanup;
    control = CMSG_FIRSTHDR(&receive_message);
    if (control == 0 || control->cmsg_level != SOL_SOCKET ||
        control->cmsg_type != SCM_RIGHTS ||
        control->cmsg_len < CMSG_LEN(sizeof(received_fd)))
        goto cleanup;
    memcpy(&received_fd, CMSG_DATA(control), sizeof(received_fd));
    success = fcntl(received_fd, F_GETFD) >= 0;
cleanup:
    if (received_fd >= 0) (void)close(received_fd);
    if (sent_fd >= 0) (void)close(sent_fd);
    if (sockets[0] >= 0) (void)close(sockets[0]);
    if (sockets[1] >= 0) (void)close(sockets[1]);
    return success;
}

static void emit(const char *text) {
    (void)write(STDOUT_FILENO, text, strlen(text));
}

static int fail(const char *text) {
    emit(text);
    return g_failure_code;
}

/* 可选的 QEMU/主机代理集成探测。保持常规回归测试独立可复现：此路径要求主机上的 7890
 * 端口有代理正在监听。 */
static int test_tcp_proxy_connect(bool late_registration) {
    static const char request[] =
        "CONNECT example.com:443 HTTP/1.1\r\n"
        "Host: example.com:443\r\n\r\n";
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(7890U),
        .sin_addr.s_addr = htonl(0x0A000202U),
    };
    struct epoll_event watch = {
        .events = EPOLLOUT | EPOLLIN |
                  (late_registration ? EPOLLONESHOT : 0U),
        .data = UINT64_C(0x544350),
    };
    struct epoll_event result = {0};
    struct pollfd descriptor = {0};
    char response[64] = {0};
    char peeked[5] = {0};
    socklen_t error_length = sizeof(int);
    int socket_error = -1;
    int fd = -1;
    int epoll_fd = -1;
    int success = 0;

    emit("OS_CHROMIUM_TCP_PROXY_BEGIN\n");
    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    epoll_fd = (int)syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
    if (fd < 0 || epoll_fd < 0 ||
        (!late_registration &&
         syscall(SYS_epoll_ctl, epoll_fd, EPOLL_CTL_ADD, fd, &watch) != 0)) {
        emit("OS_CHROMIUM_TCP_PROXY_SETUP_FAIL\n");
        goto cleanup;
    }
    errno = 0;
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) != -1 ||
        errno != EINPROGRESS) {
        emit("OS_CHROMIUM_TCP_PROXY_CONNECT_FAIL\n");
        goto cleanup;
    }
    emit("OS_CHROMIUM_TCP_PROXY_PENDING_PASS\n");
    /* connect() 返回 EINPROGRESS 后，Chromium 会注册一次性写监视，并在进入消息泵前检查
     * SO_ERROR。 */
    if (late_registration &&
        (syscall(SYS_epoll_ctl, epoll_fd, EPOLL_CTL_ADD, fd, &watch) != 0 ||
         getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                    &error_length) != 0 ||
         error_length != sizeof(int) || socket_error != 0)) {
        emit("OS_CHROMIUM_TCP_PROXY_LATE_SETUP_FAIL\n");
        goto cleanup;
    }
    if (syscall(SYS_epoll_wait, epoll_fd, &result, 1, 5000) != 1 ||
        (result.events & EPOLLOUT) == 0U || result.data != watch.data) {
        emit("OS_CHROMIUM_TCP_PROXY_EPOLL_FAIL\n");
        goto cleanup;
    }
    emit("OS_CHROMIUM_TCP_PROXY_EPOLL_PASS\n");
    if (late_registration)
        emit("OS_CHROMIUM_TCP_PROXY_LATE_EPOLL_PASS\n");
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) != 0 ||
        error_length != sizeof(int) || socket_error != 0) {
        emit("OS_CHROMIUM_TCP_PROXY_SO_ERROR_FAIL\n");
        goto cleanup;
    }
    descriptor = (struct pollfd){fd, POLLOUT, 0};
    if (libc_poll_descriptor(&descriptor, 1U, 0) != 1 ||
        (descriptor.revents & POLLOUT) == 0) {
        emit("OS_CHROMIUM_TCP_PROXY_POLL_FAIL\n");
        goto cleanup;
    }
    emit("OS_CHROMIUM_TCP_PROXY_POLL_PASS\n");
    if (send(fd, request, sizeof(request) - 1U, MSG_DONTWAIT) !=
        (ssize_t)(sizeof(request) - 1U)) {
        emit("OS_CHROMIUM_TCP_PROXY_SEND_FAIL\n");
        goto cleanup;
    }
    emit("OS_CHROMIUM_TCP_PROXY_SEND_PASS\n");
    descriptor = (struct pollfd){fd, POLLIN, 0};
    if (libc_poll_descriptor(&descriptor, 1U, 5000) != 1 ||
        (descriptor.revents & POLLIN) == 0) {
        emit("OS_CHROMIUM_TCP_PROXY_RESPONSE_FAIL\n");
        goto cleanup;
    }
    if (recv(fd, peeked, sizeof(peeked), MSG_PEEK | MSG_DONTWAIT) !=
            (ssize_t)sizeof(peeked) ||
        memcmp(peeked, "HTTP/", sizeof(peeked)) != 0 ||
        recv(fd, peeked, sizeof(peeked), MSG_PEEK | MSG_DONTWAIT) !=
            (ssize_t)sizeof(peeked) ||
        memcmp(peeked, "HTTP/", sizeof(peeked)) != 0) {
        emit("OS_CHROMIUM_TCP_PROXY_PEEK_FAIL\n");
        goto cleanup;
    }
    emit("OS_CHROMIUM_TCP_PROXY_PEEK_PASS\n");
    if (
        recv(fd, response, sizeof(response), MSG_DONTWAIT) < 5 ||
        memcmp(response, "HTTP/", 5U) != 0) {
        emit("OS_CHROMIUM_TCP_PROXY_RESPONSE_FAIL\n");
        goto cleanup;
    }
    emit("OS_CHROMIUM_TCP_PROXY_RESPONSE_PASS\n");
    success = 1;
cleanup:
    if (fd >= 0) (void)close(fd);
    if (epoll_fd >= 0) (void)close(epoll_fd);
    return success;
}

/* 当 maxevents 小于就绪 fd 数量时，繁忙事件源不得饿死后续注册项。Chromium 的 I/O 泵会
 * 分批处理事件，此时许多 eventfd/pipe/socket 监视项可能持续就绪。 */
static int test_epoll_ready_fairness(void) {
    enum { WATCH_COUNT = 24, BATCH_SIZE = 4 };
    int fds[WATCH_COUNT];
    int epoll_fd = -1;
    uint32_t seen = 0U;
    int success = 0;
    for (int i = 0; i < WATCH_COUNT; ++i) fds[i] = -1;
    epoll_fd = (int)syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
    if (epoll_fd < 0) goto cleanup;
    for (int i = 0; i < WATCH_COUNT; ++i) {
        struct epoll_event watch = {
            .events = EPOLLIN,
            .data = (uint64_t)(i + 1),
        };
        fds[i] = (int)syscall(SYS_eventfd2, 1U,
                              EFD_NONBLOCK | EFD_CLOEXEC);
        if (fds[i] < 0 ||
            syscall(SYS_epoll_ctl, epoll_fd, EPOLL_CTL_ADD,
                    fds[i], &watch) != 0) goto cleanup;
    }
    for (int attempt = 0; attempt < WATCH_COUNT; ++attempt) {
        struct epoll_event events[BATCH_SIZE] = {{0}};
        long count = syscall(SYS_epoll_wait, epoll_fd, events,
                             BATCH_SIZE, 0);
        if (count <= 0 || count > BATCH_SIZE) goto cleanup;
        for (long index = 0; index < count; ++index) {
            if ((events[index].events & EPOLLIN) == 0U ||
                events[index].data == 0U ||
                events[index].data > WATCH_COUNT) goto cleanup;
            seen |= 1U << (uint32_t)(events[index].data - 1U);
        }
    }
    success = seen == ((1U << WATCH_COUNT) - 1U);
cleanup:
    for (int i = 0; i < WATCH_COUNT; ++i) {
        if (fds[i] >= 0) (void)close(fds[i]);
    }
    if (epoll_fd >= 0) (void)close(epoll_fd);
    if (success) emit("OS_CHROMIUM_EPOLL_FAIRNESS_PASS\n");
    else emit("OS_CHROMIUM_EPOLL_FAIRNESS_FAIL\n");
    return success;
}

static int test_epoll_oneshot_rearm(void) {
    int epoll_fd = (int)syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
    int event_fd = (int)syscall(SYS_eventfd2, 1U,
                                EFD_NONBLOCK | EFD_CLOEXEC);
    struct epoll_event watch = {
        .events = EPOLLIN | EPOLLONESHOT,
        .data = UINT64_C(0x4F4E45),
    };
    struct epoll_event result = {0};
    int success = epoll_fd >= 0 && event_fd >= 0 &&
                  syscall(SYS_epoll_ctl, epoll_fd, EPOLL_CTL_ADD,
                          event_fd, &watch) == 0 &&
                  syscall(SYS_epoll_wait, epoll_fd, &result, 1, 0) == 1 &&
                  result.data == watch.data &&
                  (result.events & EPOLLIN) != 0U &&
                  syscall(SYS_epoll_wait, epoll_fd, &result, 1, 0) == 0 &&
                  syscall(SYS_epoll_ctl, epoll_fd, EPOLL_CTL_MOD,
                          event_fd, &watch) == 0 &&
                  syscall(SYS_epoll_wait, epoll_fd, &result, 1, 0) == 1 &&
                  result.data == watch.data;
    if (event_fd >= 0) (void)close(event_fd);
    if (epoll_fd >= 0) (void)close(epoll_fd);
    if (success) emit("OS_CHROMIUM_EPOLL_ONESHOT_PASS\n");
    else emit("OS_CHROMIUM_EPOLL_ONESHOT_FAIL\n");
    return success;
}

static int test_realtime_clock_matches_native(void) {
    os_timespec_t native_realtime = {0};
    os_timespec_t native_monotonic = {0};
    struct linux_timespec_abi linux_realtime = {0};
    struct linux_timespec_abi linux_monotonic = {0};
    if (__libc_native_syscall6(OS_SYS_CLOCK_GET, OS_CLOCK_REALTIME,
                        (uint64_t)(uintptr_t)&native_realtime,
                        0U, 0U, 0U, 0U) != 0 ||
        __libc_native_syscall6(OS_SYS_CLOCK_GET, OS_CLOCK_MONOTONIC,
                        (uint64_t)(uintptr_t)&native_monotonic,
                        0U, 0U, 0U, 0U) != 0 ||
        syscall(SYS_clock_gettime, CLOCK_REALTIME, &linux_realtime) != 0 ||
        syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &linux_monotonic) != 0)
        return 0;
    return native_realtime.seconds >= 1577836800LL &&
           linux_realtime.seconds >= native_realtime.seconds &&
           linux_realtime.seconds <= native_realtime.seconds + 2LL &&
           linux_monotonic.seconds >= native_monotonic.seconds &&
           linux_monotonic.seconds <= native_monotonic.seconds + 2LL;
}

static void *wake_realtime_futex(void *opaque) {
    uint32_t *word = (uint32_t *)opaque;
    delay_milliseconds(100U);
    (void)syscall(SYS_futex, word, 1 | 128, 1U, 0U, 0U, 0U);
    return 0;
}

static int test_futex_wake_without_waiter(void) {
    uint32_t word = 0U;
    pthread_t worker;
    struct linux_timespec_abi timeout = {.seconds = 2, .nanoseconds = 0};
    for (unsigned int i = 0; i < 128U; ++i) {
        if (syscall(SYS_futex, &word, 1 | 128, 1U, 0U, 0U, 0U) != 0)
            return 0;
    }
    errno = 0;
    if (syscall(SYS_futex, &word, 0 | 128, 1U, &timeout, 0U, 0U) != -1 ||
        errno != EAGAIN ||
        pthread_create(&worker, 0, wake_realtime_futex, &word) != 0)
        return 0;
    long result = syscall(SYS_futex, &word, 0 | 128, 0U,
                          &timeout, 0U, 0U);
    (void)pthread_join(worker, 0);
    return result == 0;
}

static int test_realtime_futex_past_deadline(void) {
    uint32_t word = 0U;
    struct linux_timespec_abi now = {0};
    pthread_t worker;
    long result;
    int saved_errno;
    if (syscall(SYS_clock_gettime, CLOCK_REALTIME, &now) != 0 ||
        now.seconds < 1 || pthread_create(&worker, 0,
                                          wake_realtime_futex, &word) != 0)
        return 0;
    now.seconds -= 1;
    errno = 0;
    result = syscall(SYS_futex, &word, 9 | 128 | 256, 0U,
                     &now, 0U, 0xffffffffU);
    saved_errno = errno;
    (void)pthread_join(worker, 0);
    return result == -1 && saved_errno == ETIMEDOUT;
}

static int test_dev_null(void) {
    static const char discarded[] = "discarded output";
    struct linux_stat_abi descriptor_stat = {0};
    struct linux_stat_abi path_stat = {0};
    struct linux_statx_abi extended_stat = {0};
    struct pollfd descriptor_poll = {0};
    char byte = 0;
    int descriptor = -1;
    int raw_descriptor = -1;
    int duplicate = -1;
    int success = 0;

    descriptor = open("/dev/null", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (descriptor < 0) goto cleanup;
    raw_descriptor = (int)syscall(SYS_openat, -100, "/dev/null",
                                  O_RDWR | O_NOCTTY | O_CLOEXEC, 0);
    if (raw_descriptor < 0) goto cleanup;
    if (syscall(SYS_fstat, descriptor, &descriptor_stat) != 0 ||
        (descriptor_stat.mode & 0170000U) != 0020000U ||
        (descriptor_stat.mode & 0777U) != 0666U ||
        descriptor_stat.rdevice != 0x103U || descriptor_stat.size != 0) {
        goto cleanup;
    }
    if (syscall(SYS_newfstatat, -100, "/dev/null", &path_stat, 0) != 0 ||
        (path_stat.mode & 0170000U) != 0020000U ||
        path_stat.rdevice != 0x103U) {
        goto cleanup;
    }
    if (syscall(SYS_statx, -100, "/dev/null", 0, 0x000007FFU,
                &extended_stat) != 0 ||
        (extended_stat.mode & 0170000U) != 0020000U ||
        extended_stat.rdevice_major != 1U ||
        extended_stat.rdevice_minor != 3U) {
        goto cleanup;
    }
    descriptor_poll = (struct pollfd){
        .fd = descriptor,
        .events = POLLIN | POLLOUT,
        .revents = 0,
    };
    if (poll(&descriptor_poll, 1, 0) != 1 ||
        (descriptor_poll.revents & (POLLIN | POLLOUT)) !=
            (POLLIN | POLLOUT) ||
        write(descriptor, discarded, sizeof(discarded) - 1U) !=
            (ssize_t)(sizeof(discarded) - 1U) ||
        read(descriptor, &byte, sizeof(byte)) != 0) {
        goto cleanup;
    }
    if (syscall(SYS_lseek, descriptor, 5, 0) != 0)
        goto cleanup;
    errno = 0;
    if (syscall(SYS_lseek, descriptor, 0, 42) != -1 || errno != EINVAL)
        goto cleanup;
    errno = 0;
    if (syscall(SYS_openat, -100, "/dev/null", O_RDWR | O_DIRECTORY, 0) !=
            -1 || errno != ENOTDIR) {
        goto cleanup;
    }
    errno = 0;
    if (syscall(SYS_openat, -100, "/dev/null",
                O_RDWR | O_CREAT | O_EXCL, 0600) != -1 || errno != EEXIST) {
        goto cleanup;
    }
    duplicate = (int)syscall(SYS_dup2, descriptor, 200);
    if (duplicate != 200 || read(duplicate, &byte, sizeof(byte)) != 0)
        goto cleanup;
    success = 1;

cleanup:
    if (duplicate >= 0) (void)linux_raw_close(duplicate);
    if (descriptor >= 0) (void)libc_close_descriptor(descriptor);
    if (raw_descriptor >= 0) (void)syscall(SYS_close, raw_descriptor);
    return success;
}

static int test_tmp_file_scm_rights(void) {
    char path[] = "/tmp/system-fdpass-XXXXXX";
    uint64_t send_control[3] = {0};
    uint64_t receive_control[3] = {0};
    char payload = 'F';
    char received_payload = 0;
    int socket_descriptors[2] = {-1, -1};
    int file_descriptor = -1;
    int received_descriptor = -1;
    struct linux_iovec send_vector = {
        .base = (uint64_t)(uintptr_t)&payload,
        .length = sizeof(payload),
    };
    struct linux_iovec receive_vector = {
        .base = (uint64_t)(uintptr_t)&received_payload,
        .length = sizeof(received_payload),
    };
    struct linux_msghdr send_message = {
        .iov_pointer = (uint64_t)(uintptr_t)&send_vector,
        .iov_length = 1U,
        .control = (uint64_t)(uintptr_t)send_control,
        .control_length = sizeof(struct linux_cmsghdr) + sizeof(int32_t),
    };
    struct linux_msghdr receive_message = {
        .iov_pointer = (uint64_t)(uintptr_t)&receive_vector,
        .iov_length = 1U,
        .control = (uint64_t)(uintptr_t)receive_control,
        .control_length = sizeof(struct linux_cmsghdr) + sizeof(int32_t),
    };
    struct linux_cmsghdr control_header = {
        .length = sizeof(struct linux_cmsghdr) + sizeof(int32_t),
        .level = 1,
        .type = 1,
    };
    struct linux_stat_abi received_status = {0};
    int success = 0;

    file_descriptor = mkstemp(path);
    if (file_descriptor < 0) goto cleanup;
    if (ftruncate(file_descriptor, 4096) != 0 ||
        syscall(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0,
                socket_descriptors) != 0) {
        goto cleanup;
    }
    memcpy(send_control, &control_header, sizeof(control_header));
    memcpy((char *)send_control + sizeof(control_header), &file_descriptor,
           sizeof(file_descriptor));
    if (syscall(SYS_sendmsg, socket_descriptors[0], &send_message, 0) !=
            (long)sizeof(payload) ||
        syscall(SYS_recvmsg, socket_descriptors[1], &receive_message, 0) !=
            (long)sizeof(received_payload) ||
        received_payload != payload ||
        receive_message.control_length <
            sizeof(struct linux_cmsghdr) + sizeof(int32_t)) {
        goto cleanup;
    }
    memcpy(&control_header, receive_control, sizeof(control_header));
    memcpy(&received_descriptor,
           (char *)receive_control + sizeof(control_header),
           sizeof(received_descriptor));
    if (control_header.length !=
            sizeof(struct linux_cmsghdr) + sizeof(int32_t) ||
        control_header.level != 1 || control_header.type != 1 ||
        received_descriptor < 0 ||
        (syscall(SYS_fcntl, received_descriptor, 3, 0) & O_ACCMODE) != O_RDWR ||
        syscall(SYS_fstat, received_descriptor, &received_status) != 0 ||
        received_status.size != 4096) {
        goto cleanup;
    }
    success = 1;

cleanup:
    if (received_descriptor >= 0) (void)linux_raw_close(received_descriptor);
    if (socket_descriptors[0] >= 0)
        (void)linux_raw_close(socket_descriptors[0]);
    if (socket_descriptors[1] >= 0)
        (void)linux_raw_close(socket_descriptors[1]);
    if (file_descriptor >= 0) (void)(close)(file_descriptor);
    if (path[0] != '\0') (void)unlink(path);
    return success;
}

/* 不仅测试原始系统调用 ABI，也要覆盖 Chromium 的 libc 路径：Mojo 通过 recvmsg() 接收
 * 共享内存描述符，再用 fcntl(F_GETFL/F_GETFD) 验证。 */
static int test_libc_scm_rights_fcntl(void) {
    char path[] = "/tmp/system-libc-fdpass-XXXXXX";
    char sent_payload = 'L';
    char received_payload = 0;
    int socket_descriptors[2] = {-1, -1};
    int file_descriptor = -1;
    int readonly_descriptor = -1;
    int passed_descriptors[2] = {-1, -1};
    int received_descriptors[2] = {-1, -1};
    int failure_stage = 0;
    int success = 0;
    int descriptor_flags;
    ssize_t transfer_result;
    struct iovec send_vector = {
        .iov_base = &sent_payload,
        .iov_len = sizeof(sent_payload),
    };
    struct iovec receive_vector = {
        .iov_base = &received_payload,
        .iov_len = sizeof(received_payload),
    };
    uint64_t send_control[(CMSG_SPACE(2U * sizeof(int)) + sizeof(uint64_t) -
                           1U) / sizeof(uint64_t)] = {0};
    uint64_t receive_control[(CMSG_SPACE(2U * sizeof(int)) +
                              sizeof(uint64_t) - 1U) / sizeof(uint64_t)] = {0};
    struct msghdr send_message = {
        .msg_iov = &send_vector,
        .msg_iovlen = 1U,
        .msg_control = send_control,
        .msg_controllen = sizeof(send_control),
    };
    struct msghdr receive_message = {
        .msg_iov = &receive_vector,
        .msg_iovlen = 1U,
        .msg_control = receive_control,
        .msg_controllen = sizeof(receive_control),
    };

    file_descriptor = mkstemp(path);
    if (file_descriptor < 0) {
        failure_stage = 1;
        goto cleanup;
    }
    readonly_descriptor = open(path, O_RDONLY);
    if (readonly_descriptor < 0) {
        failure_stage = 2;
        goto cleanup;
    }
    passed_descriptors[0] = file_descriptor;
    passed_descriptors[1] = readonly_descriptor;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, socket_descriptors) != 0) {
        failure_stage = 3;
        goto cleanup;
    }
    struct cmsghdr *send_header = CMSG_FIRSTHDR(&send_message);
    if (send_header == 0) {
        failure_stage = 4;
        goto cleanup;
    }
    send_header->cmsg_len = CMSG_LEN(sizeof(passed_descriptors));
    send_header->cmsg_level = SOL_SOCKET;
    send_header->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(send_header), passed_descriptors,
           sizeof(passed_descriptors));

    transfer_result = sendmsg(socket_descriptors[0], &send_message, 0);
    if (transfer_result != (ssize_t)sizeof(sent_payload)) {
        failure_stage = 5;
        goto cleanup;
    }
    transfer_result = recvmsg(socket_descriptors[1], &receive_message,
                              MSG_CMSG_CLOEXEC);
    if (transfer_result != (ssize_t)sizeof(received_payload) ||
        received_payload != sent_payload ||
        (receive_message.msg_flags & MSG_CTRUNC) != 0) {
        failure_stage = 6;
        goto cleanup;
    }
    struct cmsghdr *receive_header = CMSG_FIRSTHDR(&receive_message);
    if (receive_header == 0 ||
        receive_header->cmsg_len < CMSG_LEN(sizeof(received_descriptors)) ||
        receive_header->cmsg_level != SOL_SOCKET ||
        receive_header->cmsg_type != SCM_RIGHTS) {
        failure_stage = 7;
        goto cleanup;
    }
    memcpy(received_descriptors, CMSG_DATA(receive_header),
           sizeof(received_descriptors));
    descriptor_flags = fcntl(received_descriptors[0], F_GETFL);
    if (descriptor_flags < 0) {
        failure_stage = 8;
        goto cleanup;
    }
    if ((descriptor_flags & O_ACCMODE) != O_RDWR) {
        failure_stage = 9;
        goto cleanup;
    }
    descriptor_flags = fcntl(received_descriptors[1], F_GETFL);
    if (descriptor_flags < 0) {
        failure_stage = 10;
        goto cleanup;
    }
    if ((descriptor_flags & O_ACCMODE) != O_RDONLY) {
        failure_stage = 11;
        goto cleanup;
    }
    descriptor_flags = fcntl(received_descriptors[0], F_GETFD);
    if (descriptor_flags < 0) {
        failure_stage = 12;
        goto cleanup;
    }
    if ((descriptor_flags & FD_CLOEXEC) == 0) {
        failure_stage = 13;
        goto cleanup;
    }
    descriptor_flags = fcntl(received_descriptors[1], F_GETFD);
    if (descriptor_flags < 0) {
        failure_stage = 14;
        goto cleanup;
    }
    if ((descriptor_flags & FD_CLOEXEC) == 0) {
        failure_stage = 15;
        goto cleanup;
    }
    success = 1;

cleanup:
    for (unsigned int index = 0U; index < 2U; ++index) {
        if (received_descriptors[index] >= 0)
            (void)libc_close_descriptor(received_descriptors[index]);
    }
    if (socket_descriptors[0] >= 0)
        (void)libc_close_descriptor(socket_descriptors[0]);
    if (socket_descriptors[1] >= 0)
        (void)libc_close_descriptor(socket_descriptors[1]);
    if (readonly_descriptor >= 0)
        (void)libc_close_descriptor(readonly_descriptor);
    if (file_descriptor >= 0)
        (void)libc_close_descriptor(file_descriptor);
    if (path[0] != '\0') (void)unlink(path);
    static const char *const failure_markers[] = {
        "",
        "OS_CHROMIUM_SCM_LIBC_FAIL_MKSTEMP\n",
        "OS_CHROMIUM_SCM_LIBC_FAIL_OPEN_READONLY\n",
        "OS_CHROMIUM_SCM_LIBC_FAIL_SOCKETPAIR\n",
        "OS_CHROMIUM_SCM_LIBC_FAIL_CMSG\n",
        "OS_CHROMIUM_SCM_LIBC_FAIL_SENDMSG\n",
        "OS_CHROMIUM_SCM_LIBC_FAIL_RECVMSG\n",
        "OS_CHROMIUM_SCM_LIBC_FAIL_RECEIVED_CMSG\n",
        "OS_CHROMIUM_SCM_LIBC_FAIL_F_GETFL_WRITABLE\n",
        "OS_CHROMIUM_SCM_LIBC_FAIL_MODE_WRITABLE\n",
        "OS_CHROMIUM_SCM_LIBC_FAIL_F_GETFL_READONLY\n",
        "OS_CHROMIUM_SCM_LIBC_FAIL_MODE_READONLY\n",
        "OS_CHROMIUM_SCM_LIBC_FAIL_F_GETFD_WRITABLE\n",
        "OS_CHROMIUM_SCM_LIBC_FAIL_CLOEXEC_WRITABLE\n",
        "OS_CHROMIUM_SCM_LIBC_FAIL_F_GETFD_READONLY\n",
        "OS_CHROMIUM_SCM_LIBC_FAIL_CLOEXEC_READONLY\n",
    };
    if (failure_stage != 0) emit(failure_markers[failure_stage]);
    return success;
}

typedef struct linux_fd_race_context {
    const char *path;
    volatile uint32_t ready;
    volatile uint32_t start;
    volatile uint32_t failure;
} linux_fd_race_context_t;

typedef struct linux_fd_race_worker {
    linux_fd_race_context_t *context;
    int use_native_socket;
} linux_fd_race_worker_t;

static int create_native_udp_socket(void) {
    os_handle_t handle = OS_INVALID_HANDLE;
    int64_t status = __libc_native_syscall6(
        OS_SYS_SOCKET_CREATE, AF_INET, SOCK_DGRAM, 0U,
        (uint64_t)(uintptr_t)&handle, OS_SOCKET_CREATE_FLAG_CLOEXEC, 0U);
    if (status < 0) return __libc_status_result(status);
    return __libc_install_handle(handle, O_RDWR | O_CLOEXEC, true, AF_INET);
}

static void linux_fd_race_record_failure(linux_fd_race_context_t *context,
                                         uint32_t failure) {
    uint32_t expected = 0U;
    (void)__atomic_compare_exchange_n(&context->failure, &expected, failure,
                                      false, __ATOMIC_RELAXED,
                                      __ATOMIC_RELAXED);
}

static void *linux_fd_race_worker_main(void *argument) {
    linux_fd_race_worker_t *worker = (linux_fd_race_worker_t *)argument;
    linux_fd_race_context_t *context = worker->context;
    __atomic_add_fetch(&context->ready, 1U, __ATOMIC_RELEASE);
    while (__atomic_load_n(&context->start, __ATOMIC_ACQUIRE) == 0U) {
        __asm__ volatile("pause");
    }

    for (uint32_t iteration = 0U; iteration < 64U; ++iteration) {
        int descriptor = worker->use_native_socket ?
            create_native_udp_socket() :
            open(context->path, O_RDWR | O_CLOEXEC);
        int access_flags;
        int descriptor_flags;
        if (descriptor < 0) {
            linux_fd_race_record_failure(context,
                worker->use_native_socket ? 5U : 4U);
            break;
        }
        access_flags = fcntl(descriptor, F_GETFL);
        if (access_flags < 0) {
            linux_fd_race_record_failure(context, 6U);
            (void)libc_close_descriptor(descriptor);
            break;
        }
        if ((access_flags & O_ACCMODE) != O_RDWR) {
            linux_fd_race_record_failure(context, 7U);
            (void)libc_close_descriptor(descriptor);
            break;
        }
        descriptor_flags = fcntl(descriptor, F_GETFD);
        if (descriptor_flags < 0) {
            linux_fd_race_record_failure(context, 8U);
            (void)libc_close_descriptor(descriptor);
            break;
        }
        if ((descriptor_flags & FD_CLOEXEC) == 0) {
            linux_fd_race_record_failure(context, 9U);
            (void)libc_close_descriptor(descriptor);
            break;
        }
        if (worker->use_native_socket &&
            (!__libc_descriptor_is_socket(descriptor) ||
             __libc_descriptor_is_linux_fd(descriptor))) {
            linux_fd_race_record_failure(context, 13U);
            (void)libc_close_descriptor(descriptor);
            break;
        }
        errno = 0;
        if (libc_close_descriptor(descriptor) != 0) {
            uint32_t failure = worker->use_native_socket ?
                (errno == EBADF ? 11U : 12U) : 10U;
            linux_fd_race_record_failure(context, failure);
            break;
        }
    }
    return 0;
}

/* Linux 文件和原生 socket 的并发安装共享 libc fd 资源池。Chromium 会在不同线程上测试
 * 这两条路径。 */
static int test_concurrent_fd_resource_install(void) {
    enum { WORKER_COUNT = 4 };
    char path[] = "/tmp/system-fd-race-XXXXXX";
    linux_fd_race_context_t context = {.path = path};
    linux_fd_race_worker_t workers[WORKER_COUNT] = {0};
    pthread_t threads[WORKER_COUNT] = {0};
    uint32_t thread_count = 0U;
    int seed_descriptor = mkstemp(path);

    if (seed_descriptor < 0) {
        emit("OS_CHROMIUM_FD_RACE_FAIL_MKSTEMP\n");
        return 0;
    }
    if (libc_close_descriptor(seed_descriptor) != 0) {
        (void)unlink(path);
        emit("OS_CHROMIUM_FD_RACE_FAIL_CLOSE_SEED\n");
        return 0;
    }
    for (uint32_t index = 0U; index < WORKER_COUNT; ++index) {
        workers[index].context = &context;
        workers[index].use_native_socket = (int)(index & 1U);
        if (pthread_create(&threads[index], 0, linux_fd_race_worker_main,
                           &workers[index]) != 0) {
            linux_fd_race_record_failure(&context, 3U);
            break;
        }
        ++thread_count;
    }
    while (__atomic_load_n(&context.ready, __ATOMIC_ACQUIRE) < thread_count) {
        __asm__ volatile("pause");
    }
    __atomic_store_n(&context.start, 1U, __ATOMIC_RELEASE);
    for (uint32_t index = 0U; index < thread_count; ++index) {
        (void)pthread_join(threads[index], 0);
    }
    (void)unlink(path);

    uint32_t failure = __atomic_load_n(&context.failure, __ATOMIC_ACQUIRE);
    static const char *const failure_markers[] = {
        "OS_CHROMIUM_FD_RACE_PASS\n",
        "OS_CHROMIUM_FD_RACE_FAIL_OPEN\n",
        "OS_CHROMIUM_FD_RACE_FAIL_SOCKET\n",
        "OS_CHROMIUM_FD_RACE_FAIL_THREAD_CREATE\n",
        "OS_CHROMIUM_FD_RACE_FAIL_F_GETFL\n",
        "OS_CHROMIUM_FD_RACE_FAIL_ACCESS_MODE\n",
        "OS_CHROMIUM_FD_RACE_FAIL_F_GETFD\n",
        "OS_CHROMIUM_FD_RACE_FAIL_CLOEXEC\n",
        "OS_CHROMIUM_FD_RACE_FAIL_FILE_CLOSE\n",
        "OS_CHROMIUM_FD_RACE_FAIL_NATIVE_SOCKET_CLOSE\n",
        "OS_CHROMIUM_FD_RACE_FAIL_NATIVE_SOCKET_CLOSE_OTHER\n",
        "OS_CHROMIUM_FD_RACE_FAIL_NATIVE_SOCKET_CLASSIFICATION\n",
    };
    if (failure == 0U) emit(failure_markers[0]);
    else if (failure == 3U) emit(failure_markers[3]);
    else if (failure == 4U) emit(failure_markers[1]);
    else if (failure == 5U) emit(failure_markers[2]);
    else if (failure == 6U) emit(failure_markers[4]);
    else if (failure == 7U) emit(failure_markers[5]);
    else if (failure == 8U) emit(failure_markers[6]);
    else if (failure == 9U) emit(failure_markers[7]);
    else if (failure == 10U) emit(failure_markers[8]);
    else if (failure == 11U) emit(failure_markers[9]);
    else if (failure == 12U) emit(failure_markers[10]);
    else if (failure == 13U) emit(failure_markers[11]);
    return failure == 0U;
}

/* 对已占用的 Linux 描述符执行 dup2 时，在内核 dup3 操作原子安装替换项后，不得再将其关闭。 */
static int test_linux_libc_dup2_replacement(void) {
    int source = open("/dev/null", O_RDWR | O_CLOEXEC);
    int target = open("/dev/null", O_RDONLY);
    int success = 0;
    int access_flags;

    if (source < 0) {
        emit("OS_CHROMIUM_DUP2_FAIL_SOURCE_OPEN\n");
        goto cleanup;
    }
    if (target < 0) {
        emit("OS_CHROMIUM_DUP2_FAIL_TARGET_OPEN\n");
        goto cleanup;
    }
    if (dup2(source, target) != target) {
        emit("OS_CHROMIUM_DUP2_FAIL_DUP2\n");
        goto cleanup;
    }
    access_flags = fcntl(target, F_GETFL);
    if (access_flags < 0) {
        emit("OS_CHROMIUM_DUP2_FAIL_F_GETFL\n");
        goto cleanup;
    }
    if ((access_flags & O_ACCMODE) != O_RDWR) {
        emit("OS_CHROMIUM_DUP2_FAIL_ACCESS_MODE\n");
        goto cleanup;
    }
    success = 1;

cleanup:
    if (target >= 0) (void)libc_close_descriptor(target);
    if (source >= 0) (void)libc_close_descriptor(source);
    return success;
}

static int test_tmp_directory_open(void) {
    const int flags[] = {
        O_RDONLY | O_CLOEXEC,
        O_RDONLY | O_DIRECTORY,
        O_RDONLY | O_CLOEXEC,
    };
    for (size_t index = 0U; index < sizeof(flags) / sizeof(flags[0]); ++index) {
        struct linux_stat_abi directory_status = {0};
        int descriptor = (int)syscall(SYS_openat, -100, "/tmp", flags[index], 0);
        int success = descriptor >= 0 &&
            syscall(SYS_fstat, descriptor, &directory_status) == 0 &&
            (directory_status.mode & 0170000U) == 0040000U;
        if (descriptor >= 0) (void)linux_raw_close(descriptor);
        if (!success) return 0;
    }
    int write_descriptor = (int)syscall(SYS_openat, -100, "/tmp",
                                        O_WRONLY | O_CLOEXEC, 0);
    if (write_descriptor >= 0) {
        (void)linux_raw_close(write_descriptor);
        return 0;
    }
    return errno == EISDIR;
}

/* 测试 Chromium 在 PlatformSharedMemoryRegion 回退到临时文件时使用的文件后备共享内存流程。
 * 返回不同阶段标记，以便 QEMU 失败时准确定位不支持的操作。 */
static int test_tmp_shared_memory_file(void) {
    static const size_t region_size = 256U * 1024U;
    char path[] = "/tmp/system-shmem-XXXXXX";
    struct linux_stat_abi writable_status = {0};
    struct linux_stat_abi readonly_status = {0};
    uint8_t byte = 0;
    uint8_t *writable_mapping = MAP_FAILED;
    const uint8_t *readonly_mapping = MAP_FAILED;
    size_t block_size;
    int descriptor = -1;
    int readonly_descriptor = -1;
    int transfer_sockets[2] = {-1, -1};
    int failure_stage = 0;

    descriptor = mkstemp(path);
    if (descriptor < 0) {
        failure_stage = 1;
        goto cleanup;
    }
    if (syscall(SYS_fstat, descriptor, &writable_status) != 0) {
        failure_stage = 2;
        goto cleanup;
    }
    if (syscall(SYS_ftruncate, descriptor, region_size) != 0) {
        failure_stage = 3;
        goto cleanup;
    }

    block_size = writable_status.block_size > 0 ?
        (size_t)writable_status.block_size : 512U;
    if (block_size == 0U || block_size > region_size ||
        (block_size & (block_size - 1U)) != 0U) {
        block_size = 512U;
    }
    for (size_t offset = 0U; offset < region_size; offset += block_size) {
        byte = 0xffU;
        if (pread(descriptor, &byte, 1U, (off_t)offset) != 1) {
            failure_stage = 4;
            goto cleanup;
        }
        if (byte != 0U) {
            failure_stage = 5;
            goto cleanup;
        }
        if (pwrite(descriptor, &byte, 1U, (off_t)offset) != 1) {
            failure_stage = 6;
            goto cleanup;
        }
    }

    readonly_descriptor = open(path, O_RDONLY);
    if (readonly_descriptor < 0) {
        failure_stage = 7;
        goto cleanup;
    }
    if (syscall(SYS_fstat, descriptor, &writable_status) != 0 ||
        syscall(SYS_fstat, readonly_descriptor, &readonly_status) != 0) {
        failure_stage = 8;
        goto cleanup;
    }
    if (writable_status.size != (int64_t)region_size ||
        readonly_status.size != (int64_t)region_size ||
        writable_status.device != readonly_status.device ||
        writable_status.inode != readonly_status.inode) {
        failure_stage = 9;
        goto cleanup;
    }

    writable_mapping = mmap(0, region_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, descriptor, 0);
    if (writable_mapping == MAP_FAILED) {
        failure_stage = 10;
        goto cleanup;
    }
    readonly_mapping = mmap(0, region_size, PROT_READ, MAP_SHARED,
                            readonly_descriptor, 0);
    if (readonly_mapping == MAP_FAILED) {
        failure_stage = 11;
        goto cleanup;
    }
    writable_mapping[123U] = 0xa5U;
    if (readonly_mapping[123U] != 0xa5U) {
        failure_stage = 12;
        goto cleanup;
    }

    pid_t child = fork();
    if (child < 0) {
        failure_stage = 13;
        goto cleanup;
    }
    if (child == 0) {
        /* 另一个进程中的新映射必须解析到相同页面。 */
        (void)munmap(writable_mapping, region_size);
        (void)munmap((void *)readonly_mapping, region_size);
        uint8_t *child_mapping = mmap(0, region_size,
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED, descriptor, 0);
        if (child_mapping == MAP_FAILED || child_mapping[123U] != 0xa5U)
            _exit(1);
        child_mapping[124U] = 0x5aU;
        (void)munmap(child_mapping, region_size);
        _exit(0);
    }
    int child_status = 0;
    if (waitpid(child, &child_status, 0) != child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        failure_stage = 14;
        goto cleanup;
    }
    if (readonly_mapping[124U] != 0x5aU) {
        failure_stage = 15;
        goto cleanup;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, transfer_sockets) != 0) {
        failure_stage = 16;
        goto cleanup;
    }
    pid_t transfer_child = fork();
    if (transfer_child < 0) {
        failure_stage = 17;
        goto cleanup;
    }
    if (transfer_child == 0) {
        char received_byte = 0;
        struct iovec receive_vector = {&received_byte, 1U};
        uint64_t control[(CMSG_SPACE(sizeof(int)) + sizeof(uint64_t) - 1U) /
                         sizeof(uint64_t)] = {0};
        struct msghdr receive_message = {
            .msg_iov = &receive_vector,
            .msg_iovlen = 1U,
            .msg_control = control,
            .msg_controllen = sizeof(control),
        };
        (void)close(transfer_sockets[0]);
        (void)close(descriptor);
        (void)close(readonly_descriptor);
        (void)munmap(writable_mapping, region_size);
        (void)munmap((void *)readonly_mapping, region_size);
        if (recvmsg(transfer_sockets[1], &receive_message,
                    MSG_CMSG_CLOEXEC) != 1 || received_byte != 'S')
            _exit(1);
        struct cmsghdr *received_header = CMSG_FIRSTHDR(&receive_message);
        if (received_header == 0 ||
            received_header->cmsg_len < CMSG_LEN(sizeof(int)) ||
            received_header->cmsg_level != SOL_SOCKET ||
            received_header->cmsg_type != SCM_RIGHTS)
            _exit(2);
        int received_fd = -1;
        memcpy(&received_fd, CMSG_DATA(received_header), sizeof(received_fd));
        uint8_t *received_mapping = mmap(0, region_size,
                                         PROT_READ | PROT_WRITE,
                                         MAP_SHARED, received_fd, 0);
        if (received_fd < 0 || received_mapping == MAP_FAILED ||
            received_mapping[124U] != 0x5aU)
            _exit(3);
        received_mapping[125U] = 0xc3U;
        (void)munmap(received_mapping, region_size);
        (void)close(received_fd);
        (void)close(transfer_sockets[1]);
        _exit(0);
    }
    (void)close(transfer_sockets[1]);
    transfer_sockets[1] = -1;
    char sent_byte = 'S';
    struct iovec send_vector = {&sent_byte, 1U};
    uint64_t control[(CMSG_SPACE(sizeof(int)) + sizeof(uint64_t) - 1U) /
                     sizeof(uint64_t)] = {0};
    struct msghdr send_message = {
        .msg_iov = &send_vector,
        .msg_iovlen = 1U,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    struct cmsghdr *send_header = CMSG_FIRSTHDR(&send_message);
    if (send_header != 0) {
        send_header->cmsg_len = CMSG_LEN(sizeof(int));
        send_header->cmsg_level = SOL_SOCKET;
        send_header->cmsg_type = SCM_RIGHTS;
        memcpy(CMSG_DATA(send_header), &descriptor, sizeof(descriptor));
    }
    ssize_t sent = send_header != 0 ?
                   sendmsg(transfer_sockets[0], &send_message, 0) : -1;
    (void)close(transfer_sockets[0]);
    transfer_sockets[0] = -1;
    if (sent != 1) {
        (void)waitpid(transfer_child, 0, 0);
        failure_stage = 18;
        goto cleanup;
    }
    int transfer_status = 0;
    if (waitpid(transfer_child, &transfer_status, 0) != transfer_child ||
        !WIFEXITED(transfer_status) || WEXITSTATUS(transfer_status) != 0) {
        failure_stage = 19;
        goto cleanup;
    }
    if (readonly_mapping[125U] != 0xc3U) {
        failure_stage = 20;
        goto cleanup;
    }

cleanup:
    if (transfer_sockets[0] >= 0) (void)close(transfer_sockets[0]);
    if (transfer_sockets[1] >= 0) (void)close(transfer_sockets[1]);
    if (readonly_mapping != MAP_FAILED)
        (void)munmap((void *)readonly_mapping, region_size);
    if (writable_mapping != MAP_FAILED)
        (void)munmap(writable_mapping, region_size);
    if (readonly_descriptor >= 0)
        (void)libc_close_descriptor(readonly_descriptor);
    if (descriptor >= 0) (void)libc_close_descriptor(descriptor);
    if (path[0] != '\0') (void)unlink(path);
    if (failure_stage != 0) {
        static const char *const failure_markers[] = {
            "",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_MKSTEMP\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_FSTAT_INITIAL\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_FTRUNCATE\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_PREAD\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_ZERO_FILL\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_PWRITE\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_OPEN_READONLY\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_FSTAT_REOPEN\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_INODE_OR_SIZE\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_MMAP_WRITABLE\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_MMAP_READONLY\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_SHARED_VISIBILITY\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_FORK\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_CHILD_MAPPING\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_CROSS_PROCESS_VISIBILITY\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_TRANSFER_SOCKETPAIR\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_TRANSFER_FORK\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_TRANSFER_SENDMSG\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_TRANSFER_CHILD\n",
            "OS_CHROMIUM_TMP_SHMEM_FAIL_TRANSFER_VISIBILITY\n",
        };
        emit(failure_markers[failure_stage]);
    }
    return failure_stage == 0;
}

static int parse_inherited_descriptor(const char *text) {
    unsigned int descriptor = 0U;
    if (text == 0 || *text == '\0') return -1;
    for (; *text != '\0'; ++text) {
        if (*text < '0' || *text > '9') return -1;
        descriptor = descriptor * 10U + (unsigned int)(*text - '0');
        if (descriptor >= 1024U) return -1;
    }
    return (int)descriptor;
}

static void format_inherited_descriptor(int descriptor, char output[12]) {
    char reversed[12];
    unsigned int digits = 0U;
    unsigned int value = (unsigned int)descriptor;
    do {
        reversed[digits++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U && digits < 11U);
    for (unsigned int index = 0U; index < digits; ++index)
        output[index] = reversed[digits - index - 1U];
    output[digits] = '\0';
}

/* Chromium 的易失配置文件必须能超过 VFS 原有的 256 节点表容量，且不得误报 ENOMEM。
 * 此项与常规事件测试分开，因为它会有意创建数百个临时文件。 */
static int test_tmp_vfs_scale(bool raw) {
    char path[] = "/tmp/system-vfs-scale-0000";
    char *digits = path + sizeof(path) - 5U;
    const unsigned int count = 384U;
    int success = 1;
    const char *failure_stage = "";
    unsigned int failure_index = 0U;
    int failure_errno = 0;
    for (unsigned int index = 0U; index < count; ++index) {
        unsigned int value = index;
        for (unsigned int digit = 4U; digit != 0U;) {
            digits[--digit] = (char)('0' + value % 10U);
            value /= 10U;
        }
        int descriptor = raw ? (int)syscall(
            SYS_openat, -100, path, O_CREAT | O_EXCL | O_RDWR, 0600) :
            open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (descriptor < 0) {
            success = 0;
            failure_stage = "OPEN";
            failure_index = index;
            failure_errno = errno;
            break;
        }
        if ((raw ? syscall(SYS_write, descriptor, "x", 1U) :
                   libc_write_descriptor(descriptor, "x", 1U)) != 1) {
            success = 0;
            failure_stage = "WRITE";
            failure_index = index;
            failure_errno = errno;
        }
        if ((raw ? syscall(SYS_close, descriptor) :
                   libc_close_descriptor(descriptor)) != 0 && success) {
            success = 0;
            failure_stage = "CLOSE";
            failure_index = index;
            failure_errno = errno;
        }
        if (!success) break;
    }
    if (success) {
        digits[0] = '0'; digits[1] = '3'; digits[2] = '8'; digits[3] = '3';
        int descriptor = raw ?
            (int)syscall(SYS_openat, -100, path, O_RDONLY, 0) :
            open(path, O_RDONLY);
        char byte = 0;
        success = descriptor >= 0 &&
                  (raw ? syscall(SYS_read, descriptor, &byte, 1U) :
                         libc_read_descriptor(descriptor, &byte, 1U)) == 1 &&
                  byte == 'x';
        if (!success) {
            failure_stage = "VERIFY";
            failure_index = count - 1U;
            failure_errno = errno;
        }
        if (descriptor >= 0) {
            if (raw) (void)syscall(SYS_close, descriptor);
            else (void)libc_close_descriptor(descriptor);
        }
    }
    for (unsigned int index = 0U; index < count; ++index) {
        unsigned int value = index;
        for (unsigned int digit = 4U; digit != 0U;) {
            digits[--digit] = (char)('0' + value % 10U);
            value /= 10U;
        }
        (void)unlink(path);
    }
    if (success) {
        emit("OS_CHROMIUM_TMP_VFS_SCALE_PASS\n");
    } else {
        char number[12];
        emit("OS_CHROMIUM_TMP_VFS_SCALE_FAIL stage=");
        emit(failure_stage);
        emit(" index=");
        format_inherited_descriptor((int)failure_index, number);
        emit(number);
        emit(" errno=");
        format_inherited_descriptor(failure_errno, number);
        emit(number);
        emit("\n");
    }
    return success;
}

static void *linux_tls_probe_worker(void *argument) {
    uint64_t expected = (uint64_t)(uintptr_t)argument;
    if (g_linux_tls_probe != 0U) return (void *)(uintptr_t)1U;
    g_linux_tls_probe = expected;
    return g_linux_tls_probe == expected ? 0 : (void *)(uintptr_t)2U;
}

static int test_linux_thread_local_storage(void) {
    const uint64_t main_value = UINT64_C(0x4c4954454f53544c);
    pthread_t worker;
    void *result = 0;

    g_linux_tls_probe = main_value;
    if (pthread_create(&worker, 0, linux_tls_probe_worker,
                       (void *)(uintptr_t)0x12345678U) != 0 ||
        pthread_join(worker, &result) != 0 || result != 0 ||
        g_linux_tls_probe != main_value) {
        return 0;
    }
    return 1;
}

static int test_linux_main_initializers(int argc, char **argv, char **envp) {
    return g_linux_main_initializer_probe == LINUX_MAIN_INITIALIZER_MAGIC &&
           g_linux_main_bss_probe == LINUX_MAIN_INITIALIZER_MAGIC &&
           g_linux_main_initializer_count == 1U &&
           g_linux_main_argc_probe == (uintptr_t)argc &&
           g_linux_main_argv_probe == (uintptr_t)argv &&
           g_linux_main_envp_probe == (uintptr_t)envp &&
           g_linux_tls_probe == LINUX_MAIN_TLS_INITIALIZER_MAGIC;
}

static int test_linux_memory_info(void) {
    struct sysinfo info;
    long physical_pages = sysconf(_SC_PHYS_PAGES);
    long available_pages = sysconf(_SC_AVPHYS_PAGES);
    uint64_t total_bytes;
    if (physical_pages <= 0L || available_pages < 0L ||
        available_pages > physical_pages || sysinfo(&info) != 0 ||
        info.mem_unit == 0U ||
        info.totalram > UINT64_MAX / (uint64_t)info.mem_unit) {
        return 0;
    }
    total_bytes = (uint64_t)info.totalram * (uint64_t)info.mem_unit;
    return total_bytes % 4096U == 0U &&
           total_bytes / 4096U == (uint64_t)physical_pages &&
           info.freeram <= info.totalram;
}

int main(int argc, char **argv, char **envp) {
    static const char pipe_message[] = "system-pipe2\n";
    static const char socket_message[] = "system-socketpair\n";
    static const char socket_reply[] = "socketpair-reply\n";
    static const char msg_part_one[] = "msg-one-";
    static const char msg_part_two[] = "msg-two\n";
    char received[sizeof(pipe_message)];
    char socket_received[sizeof(socket_message)];
    char socket_reply_received[sizeof(socket_reply)];
    char msg_received[sizeof(msg_part_one) + sizeof(msg_part_two) - 1U];
    struct epoll_event event;
    struct epoll_event result;
    struct pollfd poll_descriptor;
    struct linux_iovec send_vectors[2];
    struct linux_iovec receive_vectors[2];
    struct linux_msghdr send_message;
    struct linux_msghdr receive_message;
    uint64_t counter = 1U;
    uint64_t value = 0U;
    char inherited_fd_text[12];
    char inherited_socket_text[2][12];
    char *self_exec_arguments[] = {
        "/proc/self/exe", "--self-exec-child", inherited_fd_text,
        inherited_socket_text[0], inherited_socket_text[1], 0,
    };
    char *empty_environment[] = {0};
    int pipe_descriptors[2] = {-1, -1};
    int event_descriptor = -1;
    int epoll_descriptor = -1;
    int socket_descriptors[2] = {-1, -1};
    int result_count;

    if (argc == 2 && strcmp(argv[1], "--tcp-proxy-probe") == 0)
        return test_tcp_proxy_connect(false) ? 0 : 49;
    if (argc == 2 && strcmp(argv[1], "--tcp-proxy-late-epoll") == 0)
        return test_tcp_proxy_connect(true) ? 0 : 49;
    if (argc == 2 && strcmp(argv[1], "--epoll-fairness") == 0)
        return test_epoll_ready_fairness() ? 0 : 69;
    if (argc == 2 && strcmp(argv[1], "--epoll-oneshot") == 0)
        return test_epoll_oneshot_rearm() ? 0 : 70;
    if (argc == 2 && strcmp(argv[1], "--vfs-scale-probe") == 0)
        return test_tmp_vfs_scale(false) ? 0 : 50;
    if (argc == 2 && strcmp(argv[1], "--vfs-scale-probe-raw") == 0)
        return test_tmp_vfs_scale(true) ? 0 : 50;
    if (argc == 2 && strcmp(argv[1], "--epoll-close-reuse") == 0)
        return test_epoll_close_and_fd_reuse() ? 0 : 68;

    g_failure_code = 31;
    if (argc == 5 && strcmp(argv[1], "--self-exec-child") == 0) {
        int inherited_fd = parse_inherited_descriptor(argv[2]);
        int first_socket = parse_inherited_descriptor(argv[3]);
        int second_socket = parse_inherited_descriptor(argv[4]);
        char sent = 'M';
        char received = 0;
        struct iovec send_vector = {&sent, 1U};
        struct iovec receive_vector = {&received, 1U};
        struct msghdr send_message = {.msg_iov = &send_vector, .msg_iovlen = 1U};
        struct msghdr receive_message = {
            .msg_iov = &receive_vector, .msg_iovlen = 1U,
        };
        struct pollfd readable = {.fd = first_socket, .events = POLLIN};
        int socket_type = 0;
        int socket_domain = 0;
        socklen_t type_length = sizeof(socket_type);
        socklen_t domain_length = sizeof(socket_domain);
        if (inherited_fd < 0 || fcntl(inherited_fd, F_GETFL) < 0)
            return fail("OS_CHROMIUM_INHERITED_FD_FAIL\n");
        emit("OS_CHROMIUM_INHERITED_FD_PASS\n");
        if (first_socket < 0 || second_socket < 0 ||
            sendmsg(first_socket, &send_message, 0) != 1 ||
            recvmsg(second_socket, &receive_message, 0) != 1 ||
            received != sent ||
            sendmsg(second_socket, &send_message, 0) != 1 ||
            libc_poll_descriptor(&readable, 1U, 0) != 1 ||
            (readable.revents & POLLIN) == 0 ||
            recvmsg(first_socket, &receive_message, 0) != 1 ||
            received != sent ||
            getsockopt(second_socket, SOL_SOCKET, SO_TYPE,
                       &socket_type, &type_length) != 0 ||
            socket_type != SOCK_STREAM ||
            getsockopt(second_socket, SOL_SOCKET, SO_DOMAIN,
                       &socket_domain, &domain_length) != 0 ||
            socket_domain != AF_UNIX ||
            !__libc_descriptor_is_linux_fd(first_socket) ||
            !__libc_descriptor_is_linux_fd(second_socket))
            return fail("OS_CHROMIUM_INHERITED_SOCKET_FAIL\n");
        emit("OS_CHROMIUM_INHERITED_SOCKET_PASS\n");
        emit("OS_CHROMIUM_SELF_EXEC_PASS\n");
        return 0;
    }

    emit("OS_CHROMIUM_LINUX_EVENTS_BEGIN\n");

    /* Chromium 现有启动桩使用旧版 envp-only 钩子。 */
    __libc_init_environment(envp);

    g_failure_code = 74;
    if (!test_linux_main_initializers(argc, argv, envp))
        return fail("OS_CHROMIUM_MAIN_INIT_ARRAY_FAIL\n");
    emit("OS_CHROMIUM_MAIN_INIT_ARRAY_PASS\n");

    g_failure_code = 72;
    if (!test_linux_thread_local_storage())
        return fail("OS_CHROMIUM_PTHREAD_TLS_FAIL\n");
    emit("OS_CHROMIUM_PTHREAD_TLS_PASS\n");

    g_failure_code = 73;
    if (!test_linux_memory_info())
        return fail("OS_CHROMIUM_MEMORY_INFO_FAIL\n");
    emit("OS_CHROMIUM_MEMORY_INFO_PASS\n");

    g_failure_code = 47;
    if (!test_realtime_clock_matches_native())
        return fail("OS_CHROMIUM_REALTIME_CLOCK_FAIL\n");
    emit("OS_CHROMIUM_REALTIME_CLOCK_PASS\n");
    g_failure_code = 48;
    if (!test_futex_wake_without_waiter())
        return fail("OS_CHROMIUM_FUTEX_WAKE_NO_WAITER_FAIL\n");
    emit("OS_CHROMIUM_FUTEX_WAKE_NO_WAITER_PASS\n");
    if (!test_realtime_futex_past_deadline())
        return fail("OS_CHROMIUM_REALTIME_FUTEX_FAIL\n");
    emit("OS_CHROMIUM_REALTIME_FUTEX_PASS\n");

    g_failure_code = 9;
    if (!test_dev_null()) return fail("OS_CHROMIUM_DEV_NULL_FAIL\n");
    emit("OS_CHROMIUM_DEV_NULL_PASS\n");

    g_failure_code = 10;
    if (syscall(SYS_pipe2, pipe_descriptors,
                O_NONBLOCK | O_CLOEXEC) != 0) {
        return fail("OS_CHROMIUM_PIPE2_FAIL\n");
    }
    emit("OS_CHROMIUM_PIPE2_PASS\n");

    g_failure_code = 20;
    event_descriptor = syscall(SYS_eventfd2, 0U,
                                EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_descriptor < 0) {
        (void)close(pipe_descriptors[0]);
        (void)close(pipe_descriptors[1]);
        return fail("OS_CHROMIUM_EVENTFD_FAIL\n");
    }
    emit("OS_CHROMIUM_EVENTFD_PASS\n");

    g_failure_code = 30;
    epoll_descriptor = syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
    if (epoll_descriptor < 0) {
        (void)close(event_descriptor);
        (void)close(pipe_descriptors[0]);
        (void)close(pipe_descriptors[1]);
        return fail("OS_CHROMIUM_EPOLL_CREATE_FAIL\n");
    }

    event = (struct epoll_event){
        .events = EPOLLIN,
        .data = UINT64_C(0x454644),
    };
    g_failure_code = 31;
    if (syscall(SYS_epoll_ctl, epoll_descriptor, EPOLL_CTL_ADD,
                event_descriptor, &event) != 0) {
        (void)close(epoll_descriptor);
        (void)close(event_descriptor);
        (void)close(pipe_descriptors[0]);
        (void)close(pipe_descriptors[1]);
        return fail("OS_CHROMIUM_EPOLL_CTL_FAIL\n");
    }
    g_failure_code = 32;
    if (write(event_descriptor, &counter, sizeof(counter)) !=
        (ssize_t)sizeof(counter)) {
        return fail("OS_CHROMIUM_EVENTFD_WRITE_FAIL\n");
    }
    result_count = syscall(SYS_epoll_wait, epoll_descriptor, &result, 1, 0);
    g_failure_code = 33;
    if (result_count != 1 || (result.events & EPOLLIN) == 0U ||
        result.data != event.data ||
        read(event_descriptor, &value, sizeof(value)) !=
            (ssize_t)sizeof(value) || value != counter) {
        return fail("OS_CHROMIUM_EPOLL_EVENTFD_FAIL\n");
    }
    emit("OS_CHROMIUM_EPOLL_PASS\n");

    g_failure_code = 34;
    if (!test_epoll_libc_inet_socket()) {
        return fail("OS_CHROMIUM_EPOLL_LIBC_INET_FAIL\n");
    }
    emit("OS_CHROMIUM_EPOLL_LIBC_INET_PASS\n");

    g_failure_code = 51;
    if (!test_epoll_control_change_wakes_waiter()) {
        return fail("OS_CHROMIUM_EPOLL_CONTROL_WAKE_FAIL\n");
    }
    emit("OS_CHROMIUM_EPOLL_CONTROL_WAKE_PASS\n");

    g_failure_code = 52;
    if (!test_epoll_source_wake_survives_del_and_close()) {
        return fail("OS_CHROMIUM_EPOLL_DEL_CLOSE_FAIL\n");
    }
    emit("OS_CHROMIUM_EPOLL_DEL_CLOSE_PASS\n");

    g_failure_code = 53;
    if (!test_epoll_add_then_signal_resnapshots()) {
        return fail("OS_CHROMIUM_EPOLL_ADD_RESNAPSHOT_FAIL\n");
    }
    emit("OS_CHROMIUM_EPOLL_ADD_RESNAPSHOT_PASS\n");

    g_failure_code = 54;
    if (!test_epoll_false_control_wakes_keep_original_deadline()) {
        return fail("OS_CHROMIUM_EPOLL_TIMEOUT_DEADLINE_FAIL\n");
    }
    emit("OS_CHROMIUM_EPOLL_TIMEOUT_DEADLINE_PASS\n");

    g_failure_code = 55;
    if (!test_epoll_unix_socket_wakes_on_delayed_write()) {
        return fail("OS_CHROMIUM_EPOLL_UNIX_WAKE_FAIL\n");
    }
    emit("OS_CHROMIUM_EPOLL_UNIX_WAKE_PASS\n");

    g_failure_code = 67;
    if (!test_poll_unix_socket_wakes_on_delayed_write()) {
        return fail("OS_CHROMIUM_POLL_UNIX_WAKE_FAIL\n");
    }
    emit("OS_CHROMIUM_POLL_UNIX_WAKE_PASS\n");

    g_failure_code = 66;
    if (!test_epoll_watch_holes_and_tail_reuse()) {
        return fail("OS_CHROMIUM_EPOLL_WATCH_REUSE_FAIL\n");
    }
    emit("OS_CHROMIUM_EPOLL_WATCH_REUSE_PASS\n");

    g_failure_code = 68;
    if (!test_epoll_close_and_fd_reuse()) {
        return fail("OS_CHROMIUM_EPOLL_CLOSE_REUSE_FAIL\n");
    }
    emit("OS_CHROMIUM_EPOLL_CLOSE_REUSE_PASS\n");

    g_failure_code = 69;
    if (!test_epoll_ready_fairness())
        return fail("OS_CHROMIUM_EPOLL_FAIRNESS_FAIL\n");

    g_failure_code = 70;
    if (!test_epoll_oneshot_rearm())
        return fail("OS_CHROMIUM_EPOLL_ONESHOT_FAIL\n");

    g_failure_code = 56;
    if (!test_unix_sendto_recvfrom()) {
        return fail("OS_CHROMIUM_UNIX_SEND_RECV_FAIL\n");
    }
    emit("OS_CHROMIUM_UNIX_SEND_RECV_PASS\n");

    g_failure_code = 57;
    if (!test_unix_libc_send_recv()) {
        return fail("OS_CHROMIUM_UNIX_LIBC_SEND_RECV_FAIL\n");
    }
    emit("OS_CHROMIUM_UNIX_LIBC_SEND_RECV_PASS\n");

    g_failure_code = 58;
    if (!test_unix_ancillary_stream_offsets()) {
        return fail("OS_CHROMIUM_UNIX_ANCILLARY_OFFSETS_FAIL\n");
    }
    emit("OS_CHROMIUM_UNIX_ANCILLARY_OFFSETS_PASS\n");

    event = (struct epoll_event){
        .events = EPOLLIN,
        .data = UINT64_C(0x50495045),
    };
    g_failure_code = 60;
    if (syscall(SYS_epoll_ctl, epoll_descriptor, EPOLL_CTL_ADD,
                pipe_descriptors[0], &event) != 0 ||
        write(pipe_descriptors[1], pipe_message, sizeof(pipe_message) - 1U) !=
            (ssize_t)(sizeof(pipe_message) - 1U)) {
        return fail("OS_CHROMIUM_PIPE_EPOLL_FAIL\n");
    }
    poll_descriptor = (struct pollfd){
        .fd = pipe_descriptors[0],
        .events = POLLIN,
        .revents = 0,
    };
    if (poll(&poll_descriptor, 1, 0) != 1 ||
        (poll_descriptor.revents & POLLIN) == 0U ||
        syscall(SYS_epoll_wait, epoll_descriptor, &result, 1, 0) != 1 ||
        (result.events & EPOLLIN) == 0U ||
        result.data != event.data ||
        read(pipe_descriptors[0], received, sizeof(received) - 1U) !=
            (ssize_t)(sizeof(pipe_message) - 1U) ||
        memcmp(received, pipe_message, sizeof(pipe_message) - 1U) != 0) {
        return fail("OS_CHROMIUM_PIPE_POLL_FAIL\n");
    }
    emit("OS_CHROMIUM_POLL_PASS\n");

    if (syscall(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0,
                socket_descriptors) != 0) {
        return fail("OS_CHROMIUM_SOCKETPAIR_CREATE_FAIL\n");
    }
    event = (struct epoll_event){
        .events = EPOLLIN,
        .data = UINT64_C(0x534F434B),
    };
    if (syscall(SYS_epoll_ctl, epoll_descriptor, EPOLL_CTL_ADD,
                socket_descriptors[1], &event) != 0 ||
        write(socket_descriptors[0], socket_message,
              sizeof(socket_message) - 1U) !=
            (ssize_t)(sizeof(socket_message) - 1U)) {
        return fail("OS_CHROMIUM_SOCKETPAIR_WRITE_FAIL\n");
    }
    poll_descriptor = (struct pollfd){
        .fd = socket_descriptors[1],
        .events = POLLIN,
        .revents = 0,
    };
    result_count = syscall(SYS_epoll_wait, epoll_descriptor, &result, 1, 0);
    if (poll(&poll_descriptor, 1, 0) != 1 ||
        (poll_descriptor.revents & POLLIN) == 0U || result_count != 1 ||
        (result.events & EPOLLIN) == 0U ||
        result.data != event.data ||
        read(socket_descriptors[1], socket_received,
             sizeof(socket_received) - 1U) !=
            (ssize_t)(sizeof(socket_message) - 1U) ||
        memcmp(socket_received, socket_message,
               sizeof(socket_message) - 1U) != 0 ||
        write(socket_descriptors[1], socket_reply,
              sizeof(socket_reply) - 1U) !=
            (ssize_t)(sizeof(socket_reply) - 1U) ||
        read(socket_descriptors[0], socket_reply_received,
             sizeof(socket_reply_received) - 1U) !=
            (ssize_t)(sizeof(socket_reply) - 1U) ||
        memcmp(socket_reply_received, socket_reply,
               sizeof(socket_reply) - 1U) != 0) {
        return fail("OS_CHROMIUM_SOCKETPAIR_IO_FAIL\n");
    }
    emit("OS_CHROMIUM_SOCKETPAIR_PASS\n");

    send_vectors[0] = (struct linux_iovec){
        .base = (uint64_t)(uintptr_t)msg_part_one,
        .length = sizeof(msg_part_one) - 1U,
    };
    send_vectors[1] = (struct linux_iovec){
        .base = (uint64_t)(uintptr_t)msg_part_two,
        .length = sizeof(msg_part_two) - 1U,
    };
    send_message = (struct linux_msghdr){
        .iov_pointer = (uint64_t)(uintptr_t)send_vectors,
        .iov_length = 2U,
    };
    receive_vectors[0] = (struct linux_iovec){
        .base = (uint64_t)(uintptr_t)msg_received,
        .length = 5U,
    };
    receive_vectors[1] = (struct linux_iovec){
        .base = (uint64_t)(uintptr_t)(msg_received + 5U),
        .length = sizeof(msg_received) - 1U - 5U,
    };
    receive_message = (struct linux_msghdr){
        .iov_pointer = (uint64_t)(uintptr_t)receive_vectors,
        .iov_length = 2U,
    };
    if (syscall(SYS_sendmsg, socket_descriptors[0], &send_message, 0) !=
            (long)(sizeof(msg_part_one) + sizeof(msg_part_two) - 2U) ||
        syscall(SYS_recvmsg, socket_descriptors[1], &receive_message, 0) !=
            (long)(sizeof(msg_part_one) + sizeof(msg_part_two) - 2U) ||
        memcmp(msg_received, "msg-one-msg-two\n",
               sizeof(msg_received) - 1U) != 0) {
        return fail("OS_CHROMIUM_SENDMSG_FAIL\n");
    }
    emit("OS_CHROMIUM_SENDMSG_RECVMSG_PASS\n");

    g_failure_code = 60;
    if (!test_tmp_directory_open())
        return fail("OS_CHROMIUM_TMP_DIRECTORY_FAIL\n");
    emit("OS_CHROMIUM_TMP_DIRECTORY_PASS\n");

    g_failure_code = 61;
    if (!test_tmp_file_scm_rights())
        return fail("OS_CHROMIUM_TMP_SCM_RIGHTS_FAIL\n");
    emit("OS_CHROMIUM_TMP_SCM_RIGHTS_PASS\n");

    g_failure_code = 63;
    if (!test_libc_scm_rights_fcntl())
        return fail("OS_CHROMIUM_SCM_LIBC_FAIL\n");
    emit("OS_CHROMIUM_SCM_LIBC_FCNTL_PASS\n");

    g_failure_code = 65;
    if (!test_linux_libc_dup2_replacement())
        return fail("OS_CHROMIUM_DUP2_FAIL\n");
    emit("OS_CHROMIUM_LINUX_DUP2_REPLACEMENT_PASS\n");

    g_failure_code = 64;
    if (!test_concurrent_fd_resource_install())
        return fail("OS_CHROMIUM_FD_RACE_FAIL\n");

    g_failure_code = 62;
    if (!test_tmp_shared_memory_file())
        return fail("OS_CHROMIUM_TMP_SHMEM_FAIL\n");
    emit("OS_CHROMIUM_TMP_SHMEM_PASS\n");

    (void)close(epoll_descriptor);
    (void)close(event_descriptor);
    (void)close(pipe_descriptors[0]);
    (void)close(pipe_descriptors[1]);
    (void)close(socket_descriptors[0]);
    (void)close(socket_descriptors[1]);
    emit("OS_CHROMIUM_LINUX_EVENTS_PASS\n");
    int inherited_fd = open("/dev/null", O_RDWR);
    if (inherited_fd < 0) return fail("OS_CHROMIUM_INHERITED_FD_OPEN_FAIL\n");
    int inherited_sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, inherited_sockets) != 0)
        return fail("OS_CHROMIUM_INHERITED_SOCKET_OPEN_FAIL\n");
    format_inherited_descriptor(inherited_fd, inherited_fd_text);
    format_inherited_descriptor(inherited_sockets[0], inherited_socket_text[0]);
    format_inherited_descriptor(inherited_sockets[1], inherited_socket_text[1]);
    emit("OS_CHROMIUM_SELF_EXEC_BEGIN\n");
    (void)syscall(SYS_execve, "/proc/self/exe", self_exec_arguments,
                  empty_environment);
    return fail("OS_CHROMIUM_SELF_EXEC_FAIL\n");
}
