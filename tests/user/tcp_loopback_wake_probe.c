/* Linux-ABI integration probe: only opens private loopback TCP connections. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int pause_ms(long milliseconds) {
    /* glibc's nanosleep wrapper uses clock_nanosleep, which this guest does
     * not implement yet. Use the same supported poll timeout as the test. */
    while (poll(NULL, 0, (int)milliseconds) < 0)
        if (errno != EINTR) return -1;
    return 0;
}

static int test_family(int family) {
    int listener = -1, sender = -1, receiver = -1, epoll = -1, result = 1;
    const char *stage = "socket";
    pid_t child = -1;
    size_t filled = 0;
    unsigned char data[4096];
    memset(data, 0x5a, sizeof(data));
    struct sockaddr_storage address = {0};
    socklen_t length;
    if (family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&address;
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        length = sizeof(*a);
    } else {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&address;
        a->sin6_family = AF_INET6;
        a->sin6_addr = in6addr_loopback;
        length = sizeof(*a);
    }
    listener = socket(family, SOCK_STREAM, 0);
    sender = socket(family, SOCK_STREAM, 0);
    if (listener < 0 || sender < 0) goto done;
    stage = "bind";
    if (bind(listener, (struct sockaddr *)&address, length) < 0) goto done;
    stage = "listen";
    if (listen(listener, 1) < 0) goto done;
    stage = "getsockname";
    if (getsockname(listener, (struct sockaddr *)&address, &length) < 0) goto done;
    stage = "connect";
    if (connect(sender, (struct sockaddr *)&address, length) < 0) goto done;
    stage = "accept";
    receiver = accept(listener, NULL, NULL);
    if (receiver < 0) goto done;
    epoll = epoll_create1(0);
    struct epoll_event event = {.events = EPOLLOUT, .data.u64 = 0x57414b45};
    stage = "epoll-add";
    if (epoll < 0 || epoll_ctl(epoll, EPOLL_CTL_ADD, sender, &event) < 0) goto done;
    stage = "fill";
    int stable = 0;
    for (unsigned pass = 0; pass < 64 && !stable; ++pass) {
        for (;;) {
            ssize_t n = send(sender, data, sizeof(data), MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n <= 0 || filled + (size_t)n > 64U * 1024U * 1024U) goto done;
            filled += (size_t)n;
        }
        if (pause_ms(20) < 0) goto done;
        int ready = epoll_wait(epoll, &event, 1, 0);
        if (ready < 0) goto done;
        stable = ready == 0;
    }
    if (!stable || !filled) goto done;
    stage = "fork";
    child = fork();
    if (child < 0) goto done;
    if (!child) {
        /* Allow the parent to subscribe before freeing any send credit.
         * Neither process closes its sockets until the readiness check. */
        if (pause_ms(200) < 0) _exit(2);
        size_t drained = 0;
        while (drained < filled) {
            struct pollfd incoming = {.fd = receiver, .events = POLLIN};
            int readable;
            do { readable = poll(&incoming, 1, 2000); }
            while (readable < 0 && errno == EINTR);
            if (readable != 1 || !(incoming.revents & POLLIN)) _exit(3);
            size_t request = filled - drained;
            if (request > sizeof(data)) request = sizeof(data);
            ssize_t n = recv(receiver, data, request, MSG_DONTWAIT);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) _exit(3);
            for (ssize_t i = 0; i < n; ++i) if (data[i] != 0x5a) _exit(4);
            drained += (size_t)n;
        }
        _exit(0);
    }
    stage = "wait-for-write-credit";
    int ready;
    do { ready = epoll_wait(epoll, &event, 1, 2000); }
    while (ready < 0 && errno == EINTR);
    if (ready != 1 || event.data.u64 != 0x57414b45 || !(event.events & EPOLLOUT) ||
        (event.events & (EPOLLERR | EPOLLHUP))) goto done;
    stage = "resume-send";
    if (send(sender, "x", 1, MSG_DONTWAIT | MSG_NOSIGNAL) != 1) goto done;
    result = 0;
done:
    if (child > 0) {
        int status;
        pid_t waited;
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status)) result = 1;
    }
    if (epoll >= 0) close(epoll);
    if (receiver >= 0) close(receiver);
    if (sender >= 0) close(sender);
    if (listener >= 0) close(listener);
    printf("TCP_WAKE_%s_%s bytes=%zu stage=%s\n", family == AF_INET ? "IPV4" : "IPV6",
           result ? "FAIL" : "PASS", filled, stage);
    return result;
}

int main(int argc, char **argv) {
    /* Native launchers need not import their output handles as Linux fds. */
    if (argc == 2) {
        int output = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (output < 0) return 2;
        if (dup2(output, STDOUT_FILENO) < 0 ||
            dup2(output, STDERR_FILENO) < 0) {
            if (output > STDERR_FILENO) close(output);
            return 2;
        }
        if (output > STDERR_FILENO) close(output);
    } else if (argc != 1) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (test_family(AF_INET) || test_family(AF_INET6)) return 1;
    puts("TCP_LOOPBACK_WAKE_PASS");
    return 0;
}
