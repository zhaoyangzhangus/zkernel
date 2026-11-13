/* REFACTOR_P8_SOCKET_INTERNAL：私有 socket/测试约定。 */
#pragma once

#include <kernel/socket.h>

/* 协议测试结束后，测试夹具会恢复网络输出钩子。 */
void socket_tcp_output_get(socket_tcp_ipv4_output_fn *output, void **context);
void socket_tcp6_output_get(socket_tcp_ipv6_output_fn *output, void **context);
