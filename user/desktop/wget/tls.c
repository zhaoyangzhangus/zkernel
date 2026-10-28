#include "tls.h"

#include <errno.h>
#include <time.h>

#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#define WGET_TLS_RETRY_SLEEP_NS 10000000ULL
#define WGET_TLS_SOCKET_CHUNK    4096U
#define WGET_TLS_EAGAIN          (-11LL)
#define WGET_TLS_EBUSY           (-16LL)
#define WGET_TLS_INT_MAX         0x7FFFFFFF

extern bool wget_trust_store_add(X509_STORE *store);

static BIO_METHOD *g_tls_bio_method;

static int64_t tls_syscall_one(uint64_t number, uint64_t arg0) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = 0U;
    register uint64_t rdx __asm__("rdx") = 0U;
    register uint64_t r10 __asm__("r10") = 0U;
    register uint64_t r8 __asm__("r8") = 0U;
    register uint64_t r9 __asm__("r9") = 0U;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx), "+r"(r10), "+r"(r8), "+r"(r9) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t tls_syscall_three(uint64_t number, uint64_t arg0,
                                 uint64_t arg1, uint64_t arg2) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    register uint64_t r10 __asm__("r10") = 0U;
    register uint64_t r8 __asm__("r8") = 0U;
    register uint64_t r9 __asm__("r9") = 0U;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx), "+r"(r10), "+r"(r8), "+r"(r9) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t tls_syscall_six(uint64_t number, uint64_t arg0,
                               uint64_t arg1, uint64_t arg2,
                               uint64_t arg3, uint64_t arg4,
                               uint64_t arg5) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    register uint64_t r10 __asm__("r10") = arg3;
    register uint64_t r8 __asm__("r8") = arg4;
    register uint64_t r9 __asm__("r9") = arg5;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx), "+r"(r10), "+r"(r8), "+r"(r9) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static bool tls_sleep_ns(uint64_t delay_ns) {
    os_handle_t timer = OS_INVALID_HANDLE;
    os_wait_result_t result = {0};
    int64_t status = tls_syscall_three(OS_SYS_TIMER_CREATE, delay_ns, 0U,
                                       (uint64_t)(uintptr_t)&timer);
    if (status < 0 || timer == OS_INVALID_HANDLE) return false;
    status = tls_syscall_three(OS_SYS_WAIT_ONE, timer, OS_WAIT_INFINITE,
                               (uint64_t)(uintptr_t)&result);
    (void)tls_syscall_one(OS_SYS_HANDLE_CLOSE, timer);
    return status >= 0;
}

static int64_t tls_socket_send_raw(os_handle_t socket, const void *buffer,
                                   size_t length) {
    return tls_syscall_six(OS_SYS_SOCKET_SEND, socket,
                           (uint64_t)(uintptr_t)buffer, length,
                           0U, 0U, 0U);
}

static int64_t tls_socket_recv_raw(os_handle_t socket, void *buffer,
                                   size_t capacity, uint64_t timeout_ns) {
    uint32_t source_address = 0U;
    uint16_t source_port = 0U;
    return tls_syscall_six(OS_SYS_SOCKET_RECV, socket,
                           (uint64_t)(uintptr_t)buffer, capacity,
                           (uint64_t)(uintptr_t)&source_address,
                           (uint64_t)(uintptr_t)&source_port,
                           timeout_ns);
}

static int64_t tls_normalize_status(int64_t status) {
    /* 某些系统调用返回路径会将 32 位负 errno 保留为
       zero-extended value; make both ABI forms comparable. */
    if (status > INT32_MAX && (uint64_t)status <= UINT32_MAX) {
        return (int64_t)(int32_t)(uint32_t)status;
    }
    return status;
}

static void tls_set_errno(int64_t status) {
    if (status < 0 && status >= -INT32_MAX) errno = (int)-status;
}

static int tls_bio_read(BIO *bio, char *data, int length) {
    wget_tls_transport_t *transport;
    int64_t received;
    size_t request;
    if (bio == 0 || data == 0 || length <= 0) return -1;
    transport = (wget_tls_transport_t *)BIO_get_data(bio);
    if (transport == 0 || !transport->active) return -1;
    BIO_clear_retry_flags(bio);
    request = (size_t)length > WGET_TLS_SOCKET_CHUNK ?
              WGET_TLS_SOCKET_CHUNK : (size_t)length;
    received = tls_normalize_status(
        tls_socket_recv_raw(transport->socket, data, request,
                            transport->timeout_ns));
    if (received > 0 && (size_t)received <= request) return (int)received;
    tls_set_errno(received);
    if (received == WGET_TLS_EAGAIN || received == WGET_TLS_EBUSY ||
        received == 0) BIO_set_retry_read(bio);
    return -1;
}

static int tls_bio_write(BIO *bio, const char *data, int length) {
    wget_tls_transport_t *transport;
    int64_t sent;
    size_t request;
    if (bio == 0 || data == 0 || length <= 0) return -1;
    transport = (wget_tls_transport_t *)BIO_get_data(bio);
    if (transport == 0 || !transport->active) return -1;
    BIO_clear_retry_flags(bio);
    request = (size_t)length > WGET_TLS_SOCKET_CHUNK ?
              WGET_TLS_SOCKET_CHUNK : (size_t)length;
    sent = tls_normalize_status(
        tls_socket_send_raw(transport->socket, data, request));
    if (sent > 0 && (size_t)sent <= request) return (int)sent;
    tls_set_errno(sent);
    if (sent == WGET_TLS_EAGAIN || sent == WGET_TLS_EBUSY || sent == 0) {
        BIO_set_retry_write(bio);
    }
    return -1;
}

static int tls_bio_read_ex(BIO *bio, char *data, size_t length,
                           size_t *read_bytes) {
    int result;
    if (read_bytes == 0 || length > WGET_TLS_INT_MAX) return 0;
    *read_bytes = 0U;
    if (length == 0U) return 1;
    result = tls_bio_read(bio, data, (int)length);
    if (result <= 0) return 0;
    *read_bytes = (size_t)result;
    return 1;
}

static int tls_bio_write_ex(BIO *bio, const char *data, size_t length,
                            size_t *written) {
    int result;
    if (written == 0 || length > WGET_TLS_INT_MAX) return 0;
    *written = 0U;
    if (length == 0U) return 1;
    result = tls_bio_write(bio, data, (int)length);
    if (result <= 0) return 0;
    *written = (size_t)result;
    return 1;
}

static long tls_bio_ctrl(BIO *bio, int command, long number, void *pointer) {
    (void)bio;
    (void)number;
    (void)pointer;
    switch (command) {
        case BIO_CTRL_FLUSH:
        case BIO_CTRL_SET_CLOSE:
            return 1;
        case BIO_CTRL_GET_CLOSE:
        case BIO_CTRL_EOF:
        case BIO_CTRL_PENDING:
        case BIO_CTRL_WPENDING:
            return 0;
        default:
            return 0;
    }
}

static int tls_bio_create(BIO *bio) {
    BIO_set_data(bio, 0);
    BIO_set_init(bio, 1);
    BIO_set_shutdown(bio, 0);
    return 1;
}

static int tls_bio_destroy(BIO *bio) {
    if (bio == 0) return 0;
    BIO_set_data(bio, 0);
    BIO_set_init(bio, 0);
    return 1;
}

static const BIO_METHOD *tls_bio_method(void) {
    BIO_METHOD *method;
    if (g_tls_bio_method != 0) return g_tls_bio_method;
    method = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "System socket");
    if (method == 0 ||
        !BIO_meth_set_read(method, tls_bio_read) ||
        !BIO_meth_set_write(method, tls_bio_write) ||
        !BIO_meth_set_read_ex(method, tls_bio_read_ex) ||
        !BIO_meth_set_write_ex(method, tls_bio_write_ex) ||
        !BIO_meth_set_ctrl(method, tls_bio_ctrl) ||
        !BIO_meth_set_create(method, tls_bio_create) ||
        !BIO_meth_set_destroy(method, tls_bio_destroy)) {
        if (method != 0) BIO_meth_free(method);
        return 0;
    }
    g_tls_bio_method = method;
    return method;
}

static bool tls_retry_sleep(const wget_tls_transport_t *transport,
                            uint64_t *waited) {
    uint64_t delay = WGET_TLS_RETRY_SLEEP_NS;
    if (transport == 0 || waited == 0 || transport->timeout_ns == 0U ||
        *waited >= transport->timeout_ns) return false;
    if (transport->timeout_ns != UINT64_MAX &&
        delay > transport->timeout_ns - *waited) {
        delay = transport->timeout_ns - *waited;
    }
    if (delay == 0U || !tls_sleep_ns(delay)) return false;
    if (*waited <= UINT64_MAX - delay) *waited += delay;
    return true;
}

static bool tls_wants_retry(SSL *session, int error) {
    BIO *bio;
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
        return true;
    }
    if (error != SSL_ERROR_SYSCALL || session == 0) return false;
    bio = SSL_get_rbio(session);
    return bio != 0 && BIO_should_retry(bio);
}

static uint32_t tls_date_digit(char value) {
    return value >= '0' && value <= '9' ? (uint32_t)(value - '0') : 0U;
}

static uint32_t tls_build_month(const char *date) {
    if (date[0] == 'J' && date[1] == 'a') return 1U;
    if (date[0] == 'F') return 2U;
    if (date[0] == 'M' && date[2] == 'r') return 3U;
    if (date[0] == 'A' && date[1] == 'p') return 4U;
    if (date[0] == 'M' && date[2] == 'y') return 5U;
    if (date[0] == 'J' && date[1] == 'u' && date[2] == 'n') return 6U;
    if (date[0] == 'J' && date[1] == 'u' && date[2] == 'l') return 7U;
    if (date[0] == 'A' && date[1] == 'u') return 8U;
    if (date[0] == 'S') return 9U;
    if (date[0] == 'O') return 10U;
    if (date[0] == 'N') return 11U;
    return 12U;
}

static bool tls_is_leap_year(int32_t year) {
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static time_t tls_build_time(void) {
    static const uint8_t days_in_month[] = {
        31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U,
    };
    static const char date[] = __DATE__;
    static const char clock[] = __TIME__;
    int32_t year = (int32_t)tls_date_digit(date[7]) * 1000 +
                   (int32_t)tls_date_digit(date[8]) * 100 +
                   (int32_t)tls_date_digit(date[9]) * 10 +
                   (int32_t)tls_date_digit(date[10]);
    uint32_t month = tls_build_month(date);
    uint32_t day = date[4] == ' ' ?
                   (date[5] == ' ' ? tls_date_digit(date[6]) :
                    tls_date_digit(date[5])) :
                   tls_date_digit(date[4]) * 10U + tls_date_digit(date[5]);
    uint32_t seconds = tls_date_digit(clock[0]) * 36000U +
                       tls_date_digit(clock[1]) * 3600U +
                       tls_date_digit(clock[3]) * 600U +
                       tls_date_digit(clock[4]) * 60U +
                       tls_date_digit(clock[6]) * 10U +
                       tls_date_digit(clock[7]);
    int64_t days = 0;
    if (year >= 1970) {
        for (int32_t value = 1970; value < year; ++value) {
            days += tls_is_leap_year(value) ? 366 : 365;
        }
    } else {
        for (int32_t value = year; value < 1970; ++value) {
            days -= tls_is_leap_year(value) ? 366 : 365;
        }
    }
    for (uint32_t value = 1U; value < month; ++value) {
        days += days_in_month[value - 1U];
        if (value == 2U && tls_is_leap_year(year)) ++days;
    }
    days += (int64_t)day - 1;
    return (time_t)(days * 86400 + seconds);
}

static bool tls_connect(wget_tls_transport_t *transport) {
    uint64_t waited = 0U;
    for (;;) {
        int result = SSL_connect(transport->session);
        int error;
        if (result == 1) return true;
        error = SSL_get_error(transport->session, result);
        if (!tls_wants_retry(transport->session, error) ||
            !tls_retry_sleep(transport, &waited)) return false;
    }
}

bool wget_tls_start(wget_tls_transport_t *transport, os_handle_t socket,
                    const char *server_name, uint64_t timeout_ns,
                    bool no_check_certificate) {
    const SSL_METHOD *method;
    X509_VERIFY_PARAM *verify_param;
    BIO *bio;
    if (transport == 0 || server_name == 0 || server_name[0] == '\0' ||
        socket == OS_INVALID_HANDLE) return false;
    if (transport->active || transport->session != 0 || transport->context != 0) {
        wget_tls_stop(transport);
    }
    transport->socket = socket;
    transport->timeout_ns = timeout_ns;
    transport->context = 0;
    transport->session = 0;
    transport->active = false;

    if (OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, 0) != 1) return false;
    method = TLS_client_method();
    transport->context = SSL_CTX_new(method);
    if (transport->context == 0) goto fail;
    if (SSL_CTX_set_min_proto_version(transport->context, TLS1_2_VERSION) <= 0) {
        goto fail;
    }
    (void)SSL_CTX_set_options(transport->context, SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_verify(transport->context,
                       no_check_certificate ? SSL_VERIFY_NONE : SSL_VERIFY_PEER,
                       0);
    if (!no_check_certificate &&
        !wget_trust_store_add(SSL_CTX_get_cert_store(transport->context))) {
        goto fail;
    }
    verify_param = SSL_CTX_get0_param(transport->context);
    if (verify_param == 0) goto fail;
    X509_VERIFY_PARAM_set_time(verify_param, tls_build_time());

    transport->session = SSL_new(transport->context);
    if (transport->session == 0 ||
        SSL_set_tlsext_host_name(transport->session, server_name) <= 0) {
        goto fail;
    }
    if (!no_check_certificate && SSL_set1_host(transport->session, server_name) != 1) {
        goto fail;
    }
    bio = BIO_new(tls_bio_method());
    if (bio == 0) goto fail;
    BIO_set_data(bio, transport);
    BIO_set_init(bio, 1);
    BIO_set_shutdown(bio, 0);
    SSL_set_bio(transport->session, bio, bio);
    SSL_set_connect_state(transport->session);
    transport->active = true;
    if (!tls_connect(transport)) goto fail;
    return true;

fail:
    transport->active = false;
    if (transport->session != 0) SSL_free(transport->session);
    if (transport->context != 0) SSL_CTX_free(transport->context);
    transport->session = 0;
    transport->context = 0;
    return false;
}

bool wget_tls_send_all(wget_tls_transport_t *transport,
                       const void *buffer, size_t length) {
    const uint8_t *cursor = (const uint8_t *)buffer;
    uint64_t waited = 0U;
    if (transport == 0 || !transport->active ||
        (length != 0U && buffer == 0)) return false;
    while (length != 0U) {
        int chunk = length > WGET_TLS_INT_MAX ? WGET_TLS_INT_MAX : (int)length;
        int result = SSL_write(transport->session, cursor, chunk);
        if (result > 0 && result <= chunk) {
            cursor += (size_t)result;
            length -= (size_t)result;
            waited = 0U;
            continue;
        }
        if (!tls_wants_retry(transport->session,
                             SSL_get_error(transport->session, result)) ||
            !tls_retry_sleep(transport, &waited)) return false;
    }
    return true;
}

int64_t wget_tls_recv(wget_tls_transport_t *transport, void *buffer,
                      size_t capacity) {
    uint64_t waited = 0U;
    if (transport == 0 || !transport->active || buffer == 0 ||
        capacity == 0U) return -EINVAL;
    for (;;) {
        int chunk = capacity > WGET_TLS_INT_MAX ? WGET_TLS_INT_MAX : (int)capacity;
        int result = SSL_read(transport->session, buffer, chunk);
        int error;
        if (result > 0 && result <= chunk) return result;
        error = SSL_get_error(transport->session, result);
        if (error == SSL_ERROR_ZERO_RETURN) return 0;
        if (!tls_wants_retry(transport->session, error) ||
            !tls_retry_sleep(transport, &waited)) return -1;
    }
}

void wget_tls_stop(wget_tls_transport_t *transport) {
    SSL *session;
    SSL_CTX *context;
    if (transport == 0) return;
    transport->active = false;
    session = transport->session;
    context = transport->context;
    transport->session = 0;
    transport->context = 0;
    if (session != 0) SSL_free(session);
    if (context != 0) SSL_CTX_free(context);
}
