#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

typedef uint16_t sa_family_t;
typedef uint32_t socklen_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    unsigned char ss_data[126];
};

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_INET   2
#define AF_INET6  10
#define AF_NETLINK 16

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK 0x00000800
#define SOCK_CLOEXEC  0x00080000

#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_ERROR 4
#define SO_TYPE 3
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9
#define SO_ACCEPTCONN 30
#define SO_PROTOCOL 38
#define SO_DOMAIN 39
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define SO_PEERCRED 17
#define SO_BROADCAST 6

#define MSG_OOB       0x0001
#define MSG_PEEK      0x0002
#define MSG_DONTROUTE 0x0004
#define MSG_WAITALL   0x0100
#define MSG_DONTWAIT  0x0040
#define MSG_NOSIGNAL  0x4000
#define MSG_CONFIRM   0x0800
#define MSG_CTRUNC    0x0008
#define MSG_TRUNC     0x0020
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0x40000000
#endif
#define SOMAXCONN     128
#define SCM_RIGHTS    1
#ifndef SCM_CREDENTIALS
#define SCM_CREDENTIALS 2
#endif

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

struct msghdr {
    void *msg_name;
    socklen_t msg_namelen;
    struct iovec *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
};

struct ucred {
    pid_t pid;
    uid_t uid;
    gid_t gid;
};

/* recvmsg()/sendmsg() 使用的辅助数据 ABI。 */
struct cmsghdr {
    size_t cmsg_len;
    int cmsg_level;
    int cmsg_type;
};

#ifndef CMSG_ALIGN
#define CMSG_ALIGN(length) \
    (((length) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#endif
#ifndef CMSG_DATA
#define CMSG_DATA(cmsg) \
    ((unsigned char *)(uintptr_t)((uintptr_t)(cmsg) + \
                                  CMSG_ALIGN(sizeof(struct cmsghdr))))
#endif
#ifndef CMSG_LEN
#define CMSG_LEN(length) \
    (CMSG_ALIGN(sizeof(struct cmsghdr)) + (length))
#endif
#ifndef CMSG_SPACE
#define CMSG_SPACE(length) \
    (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(length))
#endif
#ifndef CMSG_FIRSTHDR
#define CMSG_FIRSTHDR(mhdr) \
    ((mhdr)->msg_controllen >= sizeof(struct cmsghdr) ? \
         (struct cmsghdr *)(mhdr)->msg_control : (struct cmsghdr *)0)
#endif
#ifndef CMSG_NXTHDR
#define CMSG_NXTHDR(mhdr, cmsg)                                      \
    ((CMSG_ALIGN((cmsg)->cmsg_len) + sizeof(struct cmsghdr) >         \
      (mhdr)->msg_controllen -                                        \
          ((uintptr_t)(cmsg) - (uintptr_t)(mhdr)->msg_control))       \
         ? (struct cmsghdr *)0                                        \
         : (struct cmsghdr *)(uintptr_t)((uintptr_t)(cmsg) +          \
                                         CMSG_ALIGN((cmsg)->cmsg_len)))
#endif

#ifdef __cplusplus
extern "C" {
#endif
int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int descriptors[2]);
int bind(int descriptor, const struct sockaddr *address, socklen_t length);
int connect(int descriptor, const struct sockaddr *address, socklen_t length);
int listen(int descriptor, int backlog);
int accept(int descriptor, struct sockaddr *address, socklen_t *length);
int accept4(int descriptor, struct sockaddr *address, socklen_t *length,
            int flags);
ssize_t send(int descriptor, const void *buffer, size_t length, int flags);
ssize_t recv(int descriptor, void *buffer, size_t length, int flags);
ssize_t sendto(int descriptor, const void *buffer, size_t length, int flags,
               const struct sockaddr *address, socklen_t address_length);
ssize_t recvfrom(int descriptor, void *buffer, size_t length, int flags,
                 struct sockaddr *address, socklen_t *address_length);
ssize_t sendmsg(int descriptor, const struct msghdr *message, int flags);
ssize_t recvmsg(int descriptor, struct msghdr *message, int flags);
int shutdown(int descriptor, int how);
int getsockname(int descriptor, struct sockaddr *address, socklen_t *length);
int getpeername(int descriptor, struct sockaddr *address, socklen_t *length);
int setsockopt(int descriptor, int level, int option, const void *value,
               socklen_t length);
int getsockopt(int descriptor, int level, int option, void *value,
               socklen_t *length);
#ifdef __cplusplus
}
#endif
