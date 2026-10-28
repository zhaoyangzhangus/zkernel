#pragma once

/* System 既不是 glibc，也不是 uClibc。功能测试接口保持精简，但需提供少数可移植
 * POSIX 头文件所需的声明辅助项（例如 sysroot 中的 ifaddrs.h）。 */
#include <sys/cdefs.h>
