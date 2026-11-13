#pragma once

/* glibc 的 <sys/poll.h> 使用此保护宏。此处定义后，当 POSIX 源码同时包含 <poll.h> 和
 * <sys/poll.h> 时，可防止宿主/sysroot 兼容头文件重复声明 pollfd。 */
#ifndef _SYS_POLL_H
#define _SYS_POLL_H 1

#include <stddef.h>

typedef size_t nfds_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN       0x0001
#define POLLPRI      0x0002
#define POLLOUT      0x0004
#define POLLERR      0x0008
#define POLLHUP      0x0010
#define POLLNVAL     0x0020
#define POLLRDNORM   0x0040
#define POLLRDBAND   0x0080
#define POLLWRNORM   0x0100
#define POLLWRBAND   0x0200
#ifndef POLLRDHUP
#define POLLRDHUP    0x2000
#endif

#ifdef __cplusplus
extern "C" {
#endif
int poll(struct pollfd *fds, nfds_t count, int timeout);
#ifdef __cplusplus
}
#endif

#endif /* _SYS_POLL_H */
