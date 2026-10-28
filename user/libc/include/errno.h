#pragma once

/* System 将稳定的负内核状态码用作 errno 值。 */

/* errno 按线程保存。主线程使用 libc 管理的后备槽位；pthread 创建的线程使用
 * 现有 FS 基址后方的槽位。 */
#ifdef __cplusplus
extern "C" {
#endif
extern int __libc_errno;
int *__errno_location(void);
#ifdef __cplusplus
}
#endif
#define errno (*__errno_location())

#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define ENXIO        6
#define E2BIG        7
#define ENOEXEC       8
#define EBADF         9
#define ECHILD       10
#define EAGAIN       11
#define ENOMEM       12
#define EACCES       13
#define EFAULT       14
#define EBUSY        16
#define EEXIST       17
#define EXDEV        18
#define ENODEV       19
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define ENFILE       23
#define EMFILE       24
#define ENOTTY       25
#define ETXTBSY      26
#define EFBIG        27
#define ENOSPC       28
#define ESPIPE       29
#define EROFS        30
#define EMLINK       31
#define EPIPE        32
#define EDOM         33
#define ERANGE       34
#define EDEADLK      35
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39
#define ELOOP        40
#define ENOMSG       42
#define ENOPKG       65
#define EIDRM        43
#define ENOLCK       37
#define ENOSR        63
#define EILSEQ       84
#define ENOTSOCK     88
#define EDESTADDRREQ 89
#define EMSGSIZE     90
#define EPROTOTYPE   91
#define ENOPROTOOPT  92
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EOPNOTSUPP   95
#define EPFNOSUPPORT 96
#define EAFNOSUPPORT 97
#define EADDRINUSE   98
#define EADDRNOTAVAIL 99
#define ENETDOWN     100
#define ENETUNREACH  101
#define ENETRESET    102
#define ECONNABORTED 103
#define ECONNRESET   104
#define ENOBUFS      105
#define EISCONN      106
#define ENOTCONN     107
#define EUSERS       87
#define ETIMEDOUT   110
#define EHOSTDOWN   112
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#define EALREADY     114
#define EINPROGRESS  115
#define ESTALE       116
#define EDQUOT       122
#define ECANCELED   125
#define EOVERFLOW    75
#define ENOTSUP      95
