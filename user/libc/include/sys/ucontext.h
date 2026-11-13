#pragma once

#include <signal.h>

/* 可移植栈报告代码使用的最小 x86_64 ucontext 布局。内核原生信号帧特意对用户代码保持不透明；
 * 这些字段提供 Chromium/Abseil 所需的常规 POSIX 名称。 */
typedef long greg_t;
typedef greg_t gregset_t[23];

typedef struct {
    gregset_t gregs;
    void *fpregs;
    unsigned long reserved[8];
} mcontext_t;

typedef struct ucontext {
    unsigned long uc_flags;
    struct ucontext *uc_link;
    stack_t uc_stack;
    mcontext_t uc_mcontext;
    sigset_t uc_sigmask;
} ucontext_t;

#ifndef REG_RIP
#define REG_RIP 16
#endif
#ifndef REG_R8
#define REG_R8 0
#endif
#ifndef REG_R9
#define REG_R9 1
#endif
#ifndef REG_R10
#define REG_R10 2
#endif
#ifndef REG_R11
#define REG_R11 3
#endif
#ifndef REG_RBP
#define REG_RBP 10
#endif
#ifndef REG_RSP
#define REG_RSP 15
#endif
#ifndef REG_RBX
#define REG_RBX 11
#endif
#ifndef REG_R12
#define REG_R12 4
#endif
#ifndef REG_R13
#define REG_R13 5
#endif
#ifndef REG_R14
#define REG_R14 6
#endif
#ifndef REG_R15
#define REG_R15 7
#endif
#ifndef REG_RDI
#define REG_RDI 8
#endif
#ifndef REG_RSI
#define REG_RSI 9
#endif
#ifndef REG_RDX
#define REG_RDX 12
#endif
#ifndef REG_RAX
#define REG_RAX 13
#endif
#ifndef REG_RCX
#define REG_RCX 14
#endif
#ifndef REG_EFL
#define REG_EFL 17
#endif
#ifndef REG_CSGSFS
#define REG_CSGSFS 18
#endif
#ifndef REG_ERR
#define REG_ERR 19
#endif
#ifndef REG_TRAPNO
#define REG_TRAPNO 20
#endif
#ifndef REG_OLDMASK
#define REG_OLDMASK 21
#endif
#ifndef REG_CR2
#define REG_CR2 22
#endif
