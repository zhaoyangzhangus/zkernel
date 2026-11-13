#pragma once

/* 可移植跟踪和崩溃辅助函数使用的 Linux prctl 选项。 */
#define PR_SET_PDEATHSIG 1
#define PR_GET_DUMPABLE 3
#define PR_SET_DUMPABLE 4
#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define PR_GET_TAGGED_ADDR_CTRL 56
#define PR_TAGGED_ADDR_ENABLE (1UL << 0)
#define PR_MTE_TCF_SYNC (1UL << 1)

int prctl(int option, ...);
