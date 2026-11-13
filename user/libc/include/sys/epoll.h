#pragma once

#include <stdint.h>

/* Linux 兼容的 epoll 接口。System libc 实现使用相同的事件 ABI，但首次等待通过原生 poll 路径执行。 */
typedef union epoll_data {
    void *ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t events;
    epoll_data_t data;
} __attribute__((packed));

#define EPOLLIN       0x00000001
#define EPOLLPRI      0x00000002
#define EPOLLOUT      0x00000004
#define EPOLLERR      0x00000008
#define EPOLLHUP      0x00000010
#define EPOLLRDNORM   0x00000040
#define EPOLLRDBAND   0x00000080
#define EPOLLWRNORM   0x00000100
#define EPOLLWRBAND   0x00000200
#define EPOLLMSG      0x00000400
#define EPOLLRDHUP    0x00002000
#define EPOLLEXCLUSIVE (1U << 28)
#define EPOLLWAKEUP  (1U << 29)
#define EPOLLONESHOT (1U << 30)
#define EPOLLET      (1U << 31)

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#ifndef EPOLL_CLOEXEC
#define EPOLL_CLOEXEC 02000000
#endif
#ifndef EPOLL_NONBLOCK
#define EPOLL_NONBLOCK 00004000
#endif

#ifdef __cplusplus
extern "C" {
#endif
int epoll_create(int size);
int epoll_create1(int flags);
int epoll_ctl(int epoll_descriptor, int operation, int target_descriptor,
              struct epoll_event *event);
int epoll_wait(int epoll_descriptor, struct epoll_event *events,
               int maxevents, int timeout);
int epoll_pwait(int epoll_descriptor, struct epoll_event *events,
                int maxevents, int timeout, const void *signal_mask);
#ifdef __cplusplus
}
#endif
