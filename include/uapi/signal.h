#pragma once

#include <stdint.h>

/* System 信号 ABI：每个信号编号占一位，编号范围为 1..64。 */
#define OS_SIGNAL_COUNT 64U
#define OS_SIGNAL_MASK_ALL UINT64_MAX

#define OS_SIGHUP    1U
#define OS_SIGINT    2U
#define OS_SIGQUIT   3U
#define OS_SIGILL    4U
#define OS_SIGTRAP   5U
#define OS_SIGABRT   6U
#define OS_SIGBUS    7U
#define OS_SIGFPE    8U
#define OS_SIGKILL   9U
#define OS_SIGUSR1   10U
#define OS_SIGSEGV   11U
#define OS_SIGUSR2   12U
#define OS_SIGPIPE   13U
#define OS_SIGALRM   14U
#define OS_SIGTERM   15U
#define OS_SIGCHLD   17U
#define OS_SIGCONT   18U
#define OS_SIGSTOP   19U
#define OS_SIGTSTP   20U
#define OS_SIGTTIN   21U
#define OS_SIGTTOU   22U
#define OS_SIGURG    23U
#define OS_SIGXCPU   24U
#define OS_SIGXFSZ   25U
#define OS_SIGVTALRM 26U
#define OS_SIGPROF   27U
#define OS_SIGWINCH  28U
#define OS_SIGIO     29U
#define OS_SIGPWR    30U
#define OS_SIGSYS    31U

#define OS_SIG_DFL 0ULL
#define OS_SIG_IGN 1ULL

#define OS_SIG_BLOCK   0
#define OS_SIG_UNBLOCK 1
#define OS_SIG_SETMASK 2

#define OS_SIG_FLAG_RESETHAND (1U << 0)
#define OS_SIG_FLAG_NODEFER   (1U << 1)

#define OS_SIGNAL_UNBLOCKABLE_MASK \
    ((1ULL << (OS_SIGKILL - 1U)) | (1ULL << (OS_SIGSTOP - 1U)))

typedef struct os_signal_action {
    uint64_t handler;
    uint64_t mask;
    uint64_t restorer;
    uint32_t flags;
    uint32_t reserved;
} os_signal_action_t;
