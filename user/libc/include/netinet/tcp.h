#pragma once

/* 可移植客户端使用的 Linux 兼容 TCP 套接字选项名称。System 套接字层可以用 ENOPROTOOPT 拒绝尚未
 * 实现的选项；保留这些声明可让通用网络代码无需导入宿主 libc ABI 即可编译。 */
#define SOL_TCP 6
#define TCP_NODELAY 1
#define TCP_MAXSEG 2
#define TCP_KEEPIDLE 4
#define TCP_KEEPINTVL 5
#define TCP_KEEPCNT 6
#define TCP_SYNCNT 7
#define TCP_LINGER2 8
#define TCP_DEFER_ACCEPT 9
#define TCP_WINDOW_CLAMP 10
#define TCP_QUICKACK 12
#define TCP_CONGESTION 13
#define TCP_CORK 3
#define TCP_USER_TIMEOUT 18
