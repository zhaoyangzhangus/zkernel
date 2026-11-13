#pragma once

#include <sys/types.h>
#include <sys/resource.h>
#include <uapi/process.h>

struct libc_siginfo;
typedef struct libc_siginfo siginfo_t;

#define WNOHANG OS_PROCESS_WAIT_NOHANG

/* Linux waitid() 的选择器和选项值。System 当前通过 waitpid() 提供等待操作；libc 会将 waitid()
 * 转换为该 ABI。 */
#ifndef P_ALL
#define P_ALL 0
#define P_PID 1
#define P_PGID 2
#endif
#ifndef WEXITED
#define WEXITED 0x00000004
#define WSTOPPED 0x00000002
#define WCONTINUED 0x00000008
#define WNOWAIT 0x01000000
#endif

#define WIFEXITED(status) (((status) & 0x7F) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status) (((status) & 0x7F) != 0 && \
                             ((status) & 0x7F) != 0x7F)
#define WTERMSIG(status) ((status) & 0x7F)
#define WIFSTOPPED(status) (((status) & 0xFF) == 0x7F)
#define WSTOPSIG(status) WEXITSTATUS(status)

#ifdef __cplusplus
extern "C" {
#endif
pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait4(pid_t pid, int *status, int options, struct rusage *usage);
int waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options);
#ifdef __cplusplus
}
#endif
