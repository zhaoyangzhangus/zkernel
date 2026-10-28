/* REFACTOR_P8_SOCKET_INTERNAL：私有 socket/测试约定。 */
#pragma once

#include <kernel/socket.h>

/* Test fixtures restore the network output hooks after protocol exercises. */
void socket_tcp_output_get(socket_tcp_ipv4_output_fn *output, void **context);
void socket_tcp6_output_get(socket_tcp_ipv6_output_fn *output, void **context);
