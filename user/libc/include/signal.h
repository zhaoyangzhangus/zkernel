#pragma once

#include <libc/libc.h>
#include <uapi/signal.h>

#ifndef __sigset_t_defined
#define __sigset_t_defined 1
typedef uint64_t sigset_t;
#endif
typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);

typedef struct libc_siginfo {
    int si_signo;
    int si_errno;
    int si_code;
    pid_t si_pid;
    uid_t si_uid;
    int si_status;
    void *si_addr;
} siginfo_t;

/* Linux 兼容的备用信号栈结构。System 当前仍在线程常规栈上投递信号，
 * 但公开此 POSIX 类型可让可移植崩溃处理和跟踪代码按该 ABI 编译。 */
typedef struct {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
} stack_t;

struct sigaction {
    union {
        sighandler_t sa_handler;
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

#define SIGHUP    OS_SIGHUP
#define SIGINT    OS_SIGINT
#define SIGQUIT   OS_SIGQUIT
#define SIGILL    OS_SIGILL
#define SIGTRAP   OS_SIGTRAP
#define SIGABRT   OS_SIGABRT
#define SIGBUS    OS_SIGBUS
#define SIGFPE    OS_SIGFPE
#define SIGKILL   OS_SIGKILL
#define SIGUSR1   OS_SIGUSR1
#define SIGSEGV   OS_SIGSEGV
#define SIGUSR2   OS_SIGUSR2
#define SIGPIPE   OS_SIGPIPE
#define SIGALRM   OS_SIGALRM
#define SIGTERM   OS_SIGTERM
#define SIGCHLD   OS_SIGCHLD
#define SIGCONT   OS_SIGCONT
#define SIGSTOP   OS_SIGSTOP
#define SIGTSTP   OS_SIGTSTP
#define SIGTTIN   OS_SIGTTIN
#define SIGTTOU   OS_SIGTTOU
#define SIGURG    OS_SIGURG
#define SIGXCPU   OS_SIGXCPU
#define SIGXFSZ   OS_SIGXFSZ
#define SIGVTALRM OS_SIGVTALRM
#define SIGPROF   OS_SIGPROF
#define SIGWINCH  OS_SIGWINCH
#define SIGIO     OS_SIGIO
#define SIGPWR    OS_SIGPWR
#define SIGSYS    OS_SIGSYS

#define SIGIOT  SIGABRT
#define SIGCLD  SIGCHLD
#define SIGPOLL SIGIO
#define NSIG ((int)OS_SIGNAL_COUNT + 1)

#define SIG_DFL ((sighandler_t)OS_SIG_DFL)
#define SIG_IGN ((sighandler_t)OS_SIG_IGN)
#define SIG_ERR ((sighandler_t)-1)

/* 提供可移植 Linux 代码所需的信号标志取值。libc 系统调用适配器会在进入内核前，
 * 将 System 支持的两个标志转换回 OS_SIG_FLAG_*。其余标志仅作为 ABI 标记接受；
 * System 当前未实现它们的附加语义。
 */
#define SA_NOCLDSTOP  0x00000001U
#define SA_NOCLDWAIT  0x00000002U
#define SA_SIGINFO    0x00000004U
#define SA_ONSTACK    0x08000000U
#define SA_RESTART    0x10000000U
#define SA_NODEFER    0x40000000U
#define SA_RESETHAND  0x80000000U
#define SS_ONSTACK   1
#define SS_DISABLE   2
#ifndef SIGSTKSZ
#define SIGSTKSZ     65536
#endif
#ifndef MINSIGSTKSZ
#define MINSIGSTKSZ  2048
#endif
#define SIG_BLOCK    OS_SIG_BLOCK
#define SIG_UNBLOCK  OS_SIG_UNBLOCK
#define SIG_SETMASK  OS_SIG_SETMASK

/* Chromium 崩溃报告器使用的 POSIX siginfo si_code 取值。 */
#ifndef ILL_ILLOPC
#define ILL_ILLOPC 1
#define ILL_ILLOPN 2
#define ILL_ILLADR 3
#define ILL_ILLTRP 4
#define ILL_PRVOPC 5
#define ILL_PRVREG 6
#define ILL_COPROC 7
#define ILL_BADSTK 8
#define FPE_INTDIV 1
#define FPE_INTOVF 2
#define FPE_FLTDIV 3
#define FPE_FLTOVF 4
#define FPE_FLTUND 5
#define FPE_FLTRES 6
#define FPE_FLTINV 7
#define FPE_FLTSUB 8
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define BUS_ADRALN 1
#define BUS_ADRERR 2
#define BUS_OBJERR 3
#define SI_KERNEL 0x80
#endif

/* V8 陷阱处理器使用的 Linux siginfo si_code 取值。 */
#ifndef SI_USER
#define SI_USER 0
#define SI_QUEUE (-1)
#define SI_TIMER (-2)
#define SI_MESGQ (-3)
#define SI_ASYNCIO (-4)
#endif

/* waitid() 返回的子进程状态码。 */
#ifndef CLD_EXITED
#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_DUMPED 3
#define CLD_TRAPPED 4
#define CLD_STOPPED 5
#define CLD_CONTINUED 6
#endif

#ifdef __cplusplus
extern "C" {
#endif
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signal);
int sigdelset(sigset_t *set, int signal);
int sigismember(const sigset_t *set, int signal);
int sigaction(int signal, const struct sigaction *action,
              struct sigaction *old_action);
sighandler_t signal(int signal, sighandler_t handler);
int sigprocmask(int how, const sigset_t *set, sigset_t *old_set);
int sigaltstack(const stack_t *stack, stack_t *old_stack);
int kill(pid_t pid, int signal);
int raise(int signal);
char *strsignal(int signal);
#ifdef __cplusplus
}
#endif
