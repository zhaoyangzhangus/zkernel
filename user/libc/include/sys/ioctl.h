#pragma once

#include <libc/libc.h>

/* Linux 兼容的 ioctl 请求编码。System 当前仅提供 Chromium 文件监视器使用的
 * 通用描述符查询；原生内核 ABI 尚不支持设备专用 ioctl 请求。 */
#ifndef _IOC_NRBITS
#define _IOC_NRBITS 8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_DIRBITS 2
#define _IOC_NRSHIFT 0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT (_IOC_SIZESHIFT + _IOC_SIZEBITS)
#define _IOC_NONE 0U
#define _IOC_WRITE 1U
#define _IOC_READ 2U
#define _IOC(dir, type, nr, size) \
    (((unsigned long)(dir) << _IOC_DIRSHIFT) | \
     ((unsigned long)(type) << _IOC_TYPESHIFT) | \
     ((unsigned long)(nr) << _IOC_NRSHIFT) | \
     ((unsigned long)(size) << _IOC_SIZESHIFT))
#define _IO(type, nr) _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, data_type) _IOC(_IOC_READ, (type), (nr), sizeof(data_type))
#define _IOW(type, nr, data_type) _IOC(_IOC_WRITE, (type), (nr), sizeof(data_type))
#define _IOWR(type, nr, data_type) \
    _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(data_type))
#endif

#ifndef FIONREAD
#define FIONREAD 0x541BUL
#endif

/* Chromium POSIX 接口名称辅助函数使用的 Linux 兼容网络接口查询。
 * 对应驱动 ioctl 实现前，System 可能返回 ENOTTY；但为保持源代码兼容，
 * 必须提供此公共请求编号。 */
#ifndef SIOCGIFNAME
#define SIOCGIFNAME 0x8910UL
#endif

#ifdef __cplusplus
extern "C" {
#endif
int ioctl(int descriptor, unsigned long request, ...) __THROW;
#ifdef __cplusplus
}
#endif
