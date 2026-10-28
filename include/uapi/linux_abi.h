#pragma once

#include <stdint.h>
#include "window_surface.h"

/*
 * Linux x86-64 兼容 ABI 常量。这些定义与 include/uapi/syscall.h 分开：
 * System 原生系统调用号刻意不采用 Linux 系统调用号。
 */

/* ELF 加载器映射的初始用户栈；栈底包含在映射范围内，栈顶不包含，
 * 与 pthread_attr_getstack() 的约定一致。 */
#define OS_LINUX_USER_STACK_TOP  0x00007FFFFFF00000ULL
#define OS_LINUX_USER_STACK_SIZE (8ULL * 1024ULL * 1024ULL)
#define OS_LINUX_USER_STACK_BASE \
    (OS_LINUX_USER_STACK_TOP - OS_LINUX_USER_STACK_SIZE)

#define LINUX_SYS_READ             0U
#define LINUX_SYS_WRITE            1U
#define LINUX_SYS_OPEN             2U
#define LINUX_SYS_CLOSE            3U
#define LINUX_SYS_CREAT           85U
#define LINUX_SYS_STAT             4U
#define LINUX_SYS_FSTAT            5U
#define LINUX_SYS_LSTAT            6U
#define LINUX_SYS_POLL             7U
#define LINUX_SYS_EPOLL_CREATE   213U
#define LINUX_SYS_EPOLL_WAIT     232U
#define LINUX_SYS_EPOLL_CTL      233U
#define LINUX_SYS_LSEEK            8U
#define LINUX_SYS_MMAP             9U
#define LINUX_SYS_MPROTECT        10U
#define LINUX_SYS_MUNMAP          11U
#define LINUX_SYS_BRK             12U
#define LINUX_SYS_RT_SIGACTION    13U
#define LINUX_SYS_RT_SIGPROCMASK  14U
#define LINUX_SYS_IOCTL            16U
#define LINUX_SYS_PREAD64          17U
#define LINUX_SYS_PWRITE64         18U
#define LINUX_SYS_READV            19U
#define LINUX_SYS_WRITEV           20U
#define LINUX_SYS_ACCESS           21U
#define LINUX_SYS_PIPE             22U
#define LINUX_SYS_SCHED_YIELD      24U
#define LINUX_SYS_DUP              32U
#define LINUX_SYS_DUP2             33U
#define LINUX_SYS_NANOSLEEP        35U
#define LINUX_SYS_GETPID           39U
#define LINUX_SYS_SOCKET           41U
#define LINUX_SYS_CONNECT          42U
#define LINUX_SYS_ACCEPT           43U
#define LINUX_SYS_SENDTO           44U
#define LINUX_SYS_RECVFROM         45U
#define LINUX_SYS_SENDMSG          46U
#define LINUX_SYS_RECVMSG          47U
#define LINUX_SYS_SHUTDOWN         48U
#define LINUX_SYS_BIND             49U
#define LINUX_SYS_LISTEN           50U
#define LINUX_SYS_GETSOCKNAME      51U
#define LINUX_SYS_GETPEERNAME      52U
#define LINUX_SYS_SOCKETPAIR       53U
#define LINUX_SYS_SETSOCKOPT       54U
#define LINUX_SYS_GETSOCKOPT       55U
#define LINUX_SYS_CLONE            56U
#define LINUX_SYS_FORK             57U
#define LINUX_SYS_VFORK            58U
#define LINUX_SYS_EXECVE           59U
#define LINUX_SYS_EXIT             60U
#define LINUX_SYS_WAIT4            61U
#define LINUX_SYS_KILL             62U
#define LINUX_SYS_UNAME            63U
#define LINUX_SYS_FCNTL            72U
#define LINUX_SYS_FSYNC            74U
#define LINUX_SYS_FDATASYNC        75U
#define LINUX_SYS_FTRUNCATE        77U
#define LINUX_SYS_GETCWD           79U
#define LINUX_SYS_CHDIR            80U
#define LINUX_SYS_RENAME           82U
#define LINUX_SYS_RENAMEAT         264U
#define LINUX_SYS_MKDIR            83U
#define LINUX_SYS_RMDIR            84U
#define LINUX_SYS_UNLINK           87U
#define LINUX_SYS_SYMLINK          88U
#define LINUX_SYS_READLINK         89U
#define LINUX_SYS_SYSINFO          99U
#define LINUX_SYS_GETUID           102U
#define LINUX_SYS_GETGID           104U
#define LINUX_SYS_GETEUID          107U
#define LINUX_SYS_GETEGID          108U
#define LINUX_SYS_GETRESUID        118U
#define LINUX_SYS_GETRESGID        120U
#define LINUX_SYS_STATFS           137U
#define LINUX_SYS_FSTATFS          138U
#define LINUX_SYS_PRCTL            157U
#define LINUX_SYS_SCHED_GETPARAM   143U
#define LINUX_SYS_SCHED_GETSCHEDULER 145U
#define LINUX_SYS_ARCH_PRCTL      158U
#define LINUX_SYS_GETTID           186U
#define LINUX_SYS_FUTEX            202U
#define LINUX_SYS_SCHED_GETAFFINITY 204U
#define LINUX_SYS_CLOCK_GETTIME   228U
#define LINUX_SYS_CLOCK_NANOSLEEP 230U
#define LINUX_SYS_EXIT_GROUP      231U
#define LINUX_SYS_OPENAT          257U
#define LINUX_SYS_MKDIRAT         258U
#define LINUX_SYS_UNLINKAT        263U
#define LINUX_SYS_NEWFSTATAT      262U
#define LINUX_SYS_SYMLINKAT       266U
#define LINUX_SYS_READLINKAT      267U
#define LINUX_SYS_SET_ROBUST_LIST 273U
#define LINUX_SYS_SET_TID_ADDRESS 218U
#define LINUX_SYS_EPOLL_PWAIT     281U
#define LINUX_SYS_EVENTFD2        290U
#define LINUX_SYS_EPOLL_CREATE1  291U
#define LINUX_SYS_PIPE2          293U
#define LINUX_SYS_PRLIMIT64       302U
#define LINUX_SYS_GETRANDOM       318U
#define LINUX_SYS_EVENTFD         284U
#define LINUX_SYS_RSEQ            334U
#define LINUX_SYS_STATX           332U
#define LINUX_SYS_GETDENTS64      217U
#define LINUX_SYS_MREMAP           25U
#define LINUX_SYS_MADVISE          28U
#define LINUX_SYS_DUP3             292U
#define LINUX_SYS_PREADV           295U
#define LINUX_SYS_PWRITEV          296U
#define LINUX_SYS_FACCESSAT2       439U
#define LINUX_SYS_ACCEPT4          288U

/* System-owned extension used by native Linux-personality desktop clients. */
#define LINUX_SYS_WINDOW_CREATE 450U
#define LINUX_SYS_DISPLAY_GET_INFO 451U

#define LINUX_AT_FDCWD          (-100)
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100U
#define LINUX_AT_REMOVEDIR        0x200U
#define LINUX_AT_EMPTY_PATH       0x1000U

#define LINUX_O_WRONLY       00000001U
#define LINUX_O_RDWR         00000002U
#define LINUX_O_NOCTTY       00000400U
#define LINUX_O_CREAT        00000100U
#define LINUX_O_EXCL         00000200U
#define LINUX_O_TRUNC        00001000U
#define LINUX_O_APPEND       00002000U
#define LINUX_O_NONBLOCK     00004000U
#define LINUX_O_DIRECTORY    00200000U
#define LINUX_O_NOFOLLOW     00400000U
#define LINUX_O_CLOEXEC      02000000U

#define LINUX_AF_UNIX        1U
#define LINUX_AF_INET        2U
#define LINUX_AF_INET6       10U
#define LINUX_SOCK_STREAM    1U
#define LINUX_SOCK_DGRAM     2U
#define LINUX_SOCK_SEQPACKET 5U
#define LINUX_SOCK_NONBLOCK  00004000U
#define LINUX_SOCK_CLOEXEC   02000000U

#define LINUX_MSG_DONTWAIT  0x40U
#define LINUX_MSG_NOSIGNAL  0x4000U
#define LINUX_MSG_PEEK     0x2U
#define LINUX_MSG_WAITALL  0x100U
#define LINUX_MSG_CTRUNC  0x8U
#define LINUX_MSG_CMSG_CLOEXEC 0x40000000U
#define LINUX_EOPNOTSUPP   (-95LL)

#define LINUX_EFD_SEMAPHORE 0x1U

#define LINUX_EPOLL_CTL_ADD 1U
#define LINUX_EPOLL_CTL_DEL 2U
#define LINUX_EPOLL_CTL_MOD 3U

#define LINUX_EPOLLIN      0x00000001U
#define LINUX_EPOLLPRI     0x00000002U
#define LINUX_EPOLLOUT     0x00000004U
#define LINUX_EPOLLERR     0x00000008U
#define LINUX_EPOLLHUP     0x00000010U
#define LINUX_EPOLLRDNORM  0x00000040U
#define LINUX_EPOLLWRNORM  0x00000100U
#define LINUX_EPOLLMSG     0x00000400U
#define LINUX_EPOLLRDHUP   0x00002000U
#define LINUX_EPOLLONESHOT (1U << 30)
#define LINUX_EPOLLET      (1U << 31)

#define LINUX_SOL_SOCKET    1U
#define LINUX_SO_ERROR      4U
#define LINUX_SO_SNDBUF     7U
#define LINUX_SO_RCVBUF     8U
#define LINUX_SO_REUSEADDR  2U
#define LINUX_SO_TYPE       3U
#define LINUX_SO_DOMAIN     39U
#define LINUX_SO_ACCEPTCONN 30U

#define LINUX_POLLIN        0x0001U
#define LINUX_POLLPRI       0x0002U
#define LINUX_POLLOUT       0x0004U
#define LINUX_POLLERR       0x0008U
#define LINUX_POLLHUP       0x0010U
#define LINUX_POLLNVAL      0x0020U
#define LINUX_POLLRDNORM    0x0040U
#define LINUX_POLLRDBAND    0x0080U
#define LINUX_POLLWRNORM    0x0100U
#define LINUX_POLLWRBAND    0x0200U

#define LINUX_PROT_READ      0x1U
#define LINUX_PROT_WRITE     0x2U
#define LINUX_PROT_EXEC      0x4U

#define LINUX_MAP_SHARED     0x01U
#define LINUX_MAP_PRIVATE    0x02U
#define LINUX_MAP_FIXED      0x10U
#define LINUX_MAP_ANONYMOUS  0x20U
#define LINUX_MAP_GROWSDOWN  0x00100U
#define LINUX_MAP_DENYWRITE  0x00800U
#define LINUX_MAP_STACK      0x20000U
#define LINUX_MAP_FIXED_NOREPLACE 0x100000U

#define LINUX_F_DUPFD        0U
#define LINUX_F_GETFD        1U
#define LINUX_F_SETFD        2U
#define LINUX_F_GETFL        3U
#define LINUX_F_SETFL        4U
#define LINUX_F_GETLK        5U
#define LINUX_F_SETLK        6U
#define LINUX_F_SETLKW       7U
#define LINUX_F_RDLCK        0
#define LINUX_F_WRLCK        1
#define LINUX_F_UNLCK        2
typedef struct linux_flock {
    int16_t type, whence;
    uint32_t padding;
    int64_t start, length;
    int32_t pid;
    uint32_t trailing_padding;
} linux_flock_t;
#define LINUX_F_DUPFD_CLOEXEC 1030U
#define LINUX_FD_CLOEXEC     1U

#define LINUX_FUTEX_WAIT         0U
#define LINUX_FUTEX_WAKE         1U
#define LINUX_FUTEX_WAIT_BITSET  9U
#define LINUX_FUTEX_WAKE_BITSET 10U
#define LINUX_FUTEX_BITSET_MATCH_ANY 0xffffffffU
#define LINUX_FUTEX_PRIVATE_FLAG 128U
#define LINUX_FUTEX_CLOCK_REALTIME 256U

#define LINUX_ARCH_SET_GS       0x1001U
#define LINUX_ARCH_SET_FS       0x1002U
#define LINUX_ARCH_GET_FS       0x1003U
#define LINUX_ARCH_GET_GS       0x1004U

#define LINUX_PR_SET_NAME        15U
#define LINUX_PR_GET_NAME        16U

#define LINUX_CLONE_VM             0x00000100ULL
#define LINUX_CLONE_FS             0x00000200ULL
#define LINUX_CLONE_FILES         0x00000400ULL
#define LINUX_CLONE_SIGHAND       0x00000800ULL
#define LINUX_CLONE_THREAD        0x00010000ULL
#define LINUX_CLONE_SETTLS        0x00080000ULL
#define LINUX_CLONE_PARENT_SETTID 0x00100000ULL
#define LINUX_CLONE_CHILD_CLEARTID 0x00200000ULL
#define LINUX_CLONE_DETACHED      0x00400000ULL
#define LINUX_CLONE_CHILD_SETTID  0x01000000ULL

#define LINUX_RLIMIT_STACK       3U
#define LINUX_RLIMIT_DATA        2U
#define LINUX_RLIMIT_NLIMITS    16U
#define LINUX_RLIM_INFINITY      UINT64_MAX

#define OS_LINUX_CLOCK_REALTIME  0U
#define OS_LINUX_CLOCK_MONOTONIC 1U

#define LINUX_ARCH_ELF_PLATFORM  "x86_64"

/* System-owned Linux-personality window bridge.  The device is exposed only
 * to the owner process of the window created by the Phase 12 runner. */
#define LINUX_WINDOW_DEVICE_PATH "/dev/system-window"

/*
 * Create a System window from a Linux-personality process.  The syscall
 * returns the new Linux fd; address, identifier and buffer_size are filled
 * by the kernel.  A zero address lets the VM choose an address, while a
 * non-zero address requests a page-aligned user mapping.
 */
typedef struct linux_window_create {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint32_t background;
    uint64_t address;
    uint64_t buffer_size;
    uint32_t identifier;
    uint32_t reserved;
    char title[32];
} linux_window_create_t;

#define LINUX_WINDOW_IOCTL_MAGIC 'W'
#define LINUX_WINDOW_IOCTL_BASE  0x50U

enum linux_window_operation {
    LINUX_WINDOW_OPERATION_PRESENT = 1U,
    LINUX_WINDOW_OPERATION_MOVE = 2U,
    LINUX_WINDOW_OPERATION_RESIZE = 3U,
    LINUX_WINDOW_OPERATION_CLOSE = 4U,
    LINUX_WINDOW_OPERATION_VISIBILITY = 5U,
};

typedef struct linux_window_command {
    uint32_t operation;
    uint32_t reserved;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t reserved2;
    uint64_t address;
    uint64_t length;
} linux_window_command_t;

#define LINUX_WINDOW_IOC_NRBITS    8U
#define LINUX_WINDOW_IOC_SIZEBITS  14U
#define LINUX_WINDOW_IOC_NRSHIFT   0U
#define LINUX_WINDOW_IOC_TYPESHIFT (LINUX_WINDOW_IOC_NRSHIFT + \
                                    LINUX_WINDOW_IOC_NRBITS)
#define LINUX_WINDOW_IOC_SIZESHIFT (LINUX_WINDOW_IOC_TYPESHIFT + 8U)
#define LINUX_WINDOW_IOC_DIRSHIFT  (LINUX_WINDOW_IOC_SIZESHIFT + \
                                    LINUX_WINDOW_IOC_SIZEBITS)
#define LINUX_WINDOW_IOC_READ      2U
#define LINUX_WINDOW_IOC_WRITE     1U
#define LINUX_WINDOW_IOC(dir, type, nr, size) \
    (((uint32_t)(dir) << LINUX_WINDOW_IOC_DIRSHIFT) | \
     ((uint32_t)(size) << LINUX_WINDOW_IOC_SIZESHIFT) | \
     ((uint32_t)(type) << LINUX_WINDOW_IOC_TYPESHIFT) | \
     ((uint32_t)(nr) << LINUX_WINDOW_IOC_NRSHIFT))
#define LINUX_WINDOW_IOWR(nr, type) \
    LINUX_WINDOW_IOC(LINUX_WINDOW_IOC_READ | LINUX_WINDOW_IOC_WRITE, \
                     LINUX_WINDOW_IOCTL_MAGIC, (nr), sizeof(type))
#define LINUX_WINDOW_IOCTL_COMMIT \
    LINUX_WINDOW_IOWR(LINUX_WINDOW_IOCTL_BASE, linux_window_command_t)
#define LINUX_WINDOW_IOCTL_SURFACE \
    LINUX_WINDOW_IOWR(LINUX_WINDOW_IOCTL_BASE + 1U, os_surface_command_t)
#define LINUX_WINDOW_IOCTL_NR(request) \
    (((uint32_t)(request) >> LINUX_WINDOW_IOC_NRSHIFT) & 0xFFU)
#define LINUX_WINDOW_IOCTL_SIZE(request) \
    (((uint32_t)(request) >> LINUX_WINDOW_IOC_SIZESHIFT) & 0x3FFFU)

_Static_assert(sizeof(linux_window_command_t) == 48U,
               "System Linux window command ABI changed");
_Static_assert(__builtin_offsetof(linux_window_command_t, address) == 32U,
               "System Linux window address offset changed");
_Static_assert(sizeof(linux_window_create_t) == 80U,
               "System Linux window create ABI changed");
_Static_assert(__builtin_offsetof(linux_window_create_t, address) == 24U,
               "System Linux window create address offset changed");

/* VISIBILITY: width = visible (0/1), height = inactive (0/1, show only). */
#define LINUX_WINDOW_VISIBILITY_INACTIVE 1U

#define LINUX_SO_KEEPALIVE 9U
#define LINUX_SOL_TCP 6U
#define LINUX_TCP_KEEPIDLE 4U
#define LINUX_TCP_KEEPINTVL 5U
#define LINUX_TCP_KEEPCNT 6U

typedef struct linux_sysinfo {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    uint16_t procs, pad;
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit, reserved;
} linux_sysinfo_t;

/* x86-64 statfs, also used by glibc's statfs64/statvfs64 wrappers. */
typedef struct linux_statfs {
    int64_t type, block_size;
    uint64_t blocks, blocks_free, blocks_available, files, files_free;
    int32_t filesystem_id[2];
    int64_t name_max, fragment_size, flags, spare[4];
} linux_statfs_t;
_Static_assert(sizeof(linux_statfs_t) == 120U, "Linux statfs size changed");
_Static_assert(__builtin_offsetof(linux_statfs_t, name_max) == 64U,
               "Linux statfs name_max offset changed");
_Static_assert(sizeof(linux_sysinfo_t) == 112U, "Linux x86-64 sysinfo size changed");
_Static_assert(__builtin_offsetof(linux_sysinfo_t, mem_unit) == 104U,
               "Linux sysinfo memory unit offset changed");
