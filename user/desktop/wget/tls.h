#ifndef OS_WGET_TLS_H
#define OS_WGET_TLS_H

#include <stdint.h>
#include <stdbool.h>

#include <uapi/all.h>

typedef struct bio_st BIO;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

typedef struct wget_tls_transport {
    os_handle_t socket;
    uint64_t timeout_ns;
    SSL_CTX *context;
    SSL *session;
    bool active;
} wget_tls_transport_t;

/* 在已连接的 socket 上启动 TLS 1.2/1.3 客户端会话。 */
bool wget_tls_start(wget_tls_transport_t *transport, os_handle_t socket,
                    const char *server_name, uint64_t timeout_ns,
                    bool no_check_certificate);

/* 通过 TLS 会话发送一条完整的明文请求。 */
bool wget_tls_send_all(wget_tls_transport_t *transport,
                       const void *buffer, size_t length);

/* 读取解密后的应用数据。EOF 时返回 0，出错时返回负值。 */
int64_t wget_tls_recv(wget_tls_transport_t *transport, void *buffer,
                      size_t capacity);

/* 释放 TLS 状态。socket 仍由调用方持有并负责关闭。 */
void wget_tls_stop(wget_tls_transport_t *transport);

#endif
