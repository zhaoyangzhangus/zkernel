#pragma once

#include <libc/libc.h>

#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0x40
#define O_EXCL     0x80
#define O_NOCTTY   0x100
#define O_TRUNC    0x200
#define O_APPEND   0x400
#define O_NONBLOCK 0x800
#define O_NDELAY O_NONBLOCK
#ifndef O_ASYNC
#define O_ASYNC    0x2000
#endif
#ifndef O_DIRECT
#define O_DIRECT   0x4000
#endif
#ifndef O_LARGEFILE
#define O_LARGEFILE 0x8000
#endif
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW  0x20000
#ifndef O_NOATIME
#define O_NOATIME  0x40000
#endif
#define O_CLOEXEC   0x80000
#ifndef O_SYNC
#define O_SYNC     0x101000
#endif
#define O_ACCMODE   3

#define AT_FDCWD (-100)
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef AT_EACCESS
#define AT_EACCESS 0x200
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif

#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1

/* Linux memfd 密封标志取值。System 当前尚未实现 memfd 或这些 fcntl 操作；
 * 保留定义是为了兼容 Linux Mojo 通道实现的源代码，该实现会在 System 上
 * 禁用此可选快速路径。 */
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008

/* SQLite Unix VFS 使用的 POSIX 建议锁 ABI。内核侧 fcntl 实现尚不支持这些操作；
 * 在此保留布局和常量可避免目标构建意外使用宿主 glibc 定义。 */
#define F_GETLK  5
#define F_SETLK  6
#define F_SETLKW 7
#define F_RDLCK  0
#define F_WRLCK  1
#define F_UNLCK  2

struct flock {
    short l_type;
    short l_whence;
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
};

#ifdef __cplusplus
extern "C" {
#endif
int openat(int directory_descriptor, const char *path, int flags, ...);
int open64(const char *path, int flags, ...);
int openat64(int directory_descriptor, const char *path, int flags, ...);
int creat(const char *path, unsigned int mode);
int creat64(const char *path, unsigned int mode);
int dup3(int old_descriptor, int new_descriptor, int flags);
#ifdef __cplusplus
}
#endif
