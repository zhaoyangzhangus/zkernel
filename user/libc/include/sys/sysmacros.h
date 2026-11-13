#pragma once

#include <sys/types.h>

/* 设备号辅助函数保持自包含。System 当前不会向原生应用公开 Linux 设备节点，但 Chromium 的 Linux
 * 进程指标代码仍需要标准 ABI 才能编译。 */
static inline unsigned int gnu_dev_major(dev_t device) {
    return (unsigned int)((device >> 8) & 0xffU);
}

static inline unsigned int gnu_dev_minor(dev_t device) {
    return (unsigned int)((device & 0xffU) | ((device >> 12) & 0xffffff00U));
}

static inline dev_t gnu_dev_makedev(unsigned int major_number,
                                    unsigned int minor_number) {
    return (dev_t)(((major_number & 0xfffU) << 8) |
                   (minor_number & 0xffU) |
                   ((minor_number & 0xffffff00U) << 12));
}

#define major(device) gnu_dev_major(device)
#define minor(device) gnu_dev_minor(device)
#define makedev(major_number, minor_number) \
    gnu_dev_makedev((major_number), (minor_number))
