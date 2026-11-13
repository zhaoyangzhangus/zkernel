#pragma once

#include <stddef.h>
#include <sys/types.h>

/* Chromium POSIX 沙箱和进程辅助函数使用的 Linux clone 标志。 */
#ifndef CLONE_VM
#define CLONE_VM             0x00000100UL
#define CLONE_FS             0x00000200UL
#define CLONE_FILES          0x00000400UL
#define CLONE_SIGHAND        0x00000800UL
#define CLONE_PIDFD          0x00001000UL
#define CLONE_PTRACE         0x00002000UL
#define CLONE_VFORK          0x00004000UL
#define CLONE_PARENT         0x00008000UL
#define CLONE_THREAD         0x00010000UL
#define CLONE_NEWNS          0x00020000UL
#define CLONE_SYSVSEM        0x00040000UL
#define CLONE_SETTLS         0x00080000UL
#define CLONE_PARENT_SETTID  0x00100000UL
#define CLONE_CHILD_CLEARTID 0x00200000UL
#define CLONE_DETACHED       0x00400000UL
#define CLONE_UNTRACED       0x00800000UL
#define CLONE_CHILD_SETTID   0x01000000UL
#define CLONE_NEWCGROUP      0x02000000UL
#define CLONE_NEWUTS         0x04000000UL
#define CLONE_NEWIPC         0x08000000UL
#define CLONE_NEWUSER        0x10000000UL
#define CLONE_NEWPID         0x20000000UL
#define CLONE_NEWNET         0x40000000UL
#define CLONE_IO             0x80000000UL
#endif

/* Chromium 线程辅助函数使用的 Linux 兼容固定大小 CPU 集合。 */
#ifndef CPU_SETSIZE
#define CPU_SETSIZE 1024
#endif

typedef struct {
    unsigned long bits[CPU_SETSIZE / (8U * sizeof(unsigned long))];
} cpu_set_t;

/* POSIX 客户端使用的 Linux 调度常量和参数布局。 */
#ifndef SCHED_OTHER
#define SCHED_OTHER 0
#define SCHED_FIFO 1
#define SCHED_RR 2
#define SCHED_BATCH 3
#define SCHED_IDLE 5
#define SCHED_DEADLINE 6
#endif

struct sched_param {
    int sched_priority;
};

#define CPU_ZERO(set) \
    do { \
        unsigned char *bytes = (unsigned char *)(set); \
        for (size_t i = 0; i < sizeof(*(set)); ++i) \
            bytes[i] = 0; \
    } while (0)

#define CPU_SET(cpu, set) \
    do { \
        size_t index = (size_t)(cpu); \
        if (index < CPU_SETSIZE) \
            (set)->bits[index / (8U * sizeof(unsigned long))] |= \
                1UL << (index % (8U * sizeof(unsigned long))); \
    } while (0)

#define CPU_CLR(cpu, set) \
    do { \
        size_t index = (size_t)(cpu); \
        if (index < CPU_SETSIZE) \
            (set)->bits[index / (8U * sizeof(unsigned long))] &= \
                ~(1UL << (index % (8U * sizeof(unsigned long)))); \
    } while (0)

#define CPU_ISSET(cpu, set) \
    (((size_t)(cpu) < CPU_SETSIZE) && \
     (((set)->bits[(size_t)(cpu) / (8U * sizeof(unsigned long))] & \
       (1UL << ((size_t)(cpu) % (8U * sizeof(unsigned long))))) != 0))

#ifdef __cplusplus
extern "C" {
#endif
int sched_yield(void);
int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask);
int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask);
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param);
int sched_getscheduler(pid_t pid);
int sched_setparam(pid_t pid, const struct sched_param *param);
int sched_getparam(pid_t pid, struct sched_param *param);
int sched_get_priority_min(int policy);
int sched_get_priority_max(int policy);
int sched_getcpu(void);
#ifdef __cplusplus
}
#endif
