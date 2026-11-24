#define _POSIX_C_SOURCE 200809L
#include "proxy.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int control = -1;

struct proxy_failure {
    const char *reason;
    int error;
};

static int fail(struct proxy_failure *failure, const char *reason, int error)
{
    *failure = (struct proxy_failure){reason, error};
    return -1;
}

void proxy_set_control(int fd)
{
    control = fd;
}

static double now_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return -1;
    return now.tv_sec + now.tv_nsec / 1e9;
}

static int wait_socket(int fd, short events, double deadline, short *returned, int client,
                       struct proxy_failure *failure)
{
    for (;;) {
        double now = now_seconds();
        if (now < 0)
            return fail(failure, "clock_failed", errno);
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return fail(failure, "timeout", 0);
        }
        struct pollfd fds[] = {
            {.fd = fd, .events = events},
            {.fd = client, .events = 0},
        };
        int result = poll(fds, 2, (int)((deadline - now) * 1000) + 1);
        if (result < 0 && errno == EINTR)
            continue;
        if (!result) {
            errno = ETIMEDOUT;
            return fail(failure, "timeout", 0);
        }
        if (result < 0)
            return fail(failure, "poll_failed", errno);
        /* FIN may be a valid write-half close; only errors/full hangup cancel. */
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = ECANCELED;
            return fail(failure, "client_cancelled", 0);
        }
        if (returned)
            *returned = fds[0].revents;
        return 0;
    }
}

/* One sequenced packet and acknowledgement per acquire/release; no queued admission. */
static int limit_exchange(int backend_id, int client, struct proxy_failure *failure)
{
    if (control < 0)
        return 0;
    double now = now_seconds();
    if (now < 0) {
        fail(failure, "clock_failed", errno);
        return 500;
    }
    double deadline = now + 5;
    if (wait_socket(control, POLLOUT, deadline, NULL, client, failure))
        return 500;
    ssize_t n = send(control, &backend_id, sizeof(backend_id), 0);
    if (n != sizeof(backend_id)) {
        fail(failure, "control_failed", n < 0 ? errno : 0);
        return 500;
    }
    if (wait_socket(control, POLLIN, deadline, NULL, client, failure))
        return 500;
    int status;
    n = recv(control, &status, sizeof(status), MSG_TRUNC);
    if (n != sizeof(status)) {
        fail(failure, "control_failed", n < 0 ? errno : 0);
        return 500;
    }
    if (status == 503)
        fail(failure, "backend_limit", 0);
    else if (status != 0) {
        fail(failure, "invalid_control_response", 0);
        return 500;
    }
    return status;
}

static int connect_backend(const struct proxy_route *route, int *status, int client,
                           struct proxy_failure *failure)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    *status = 500;
    if (fd < 0)
        return fail(failure, "socket_setup_failed", errno);
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        fail(failure, "socket_setup_failed", errno);
        goto failed;
    }
    *status = 502;
    double start = now_seconds();
    if (start < 0) {
        fail(failure, "clock_failed", errno);
        goto failed;
    }
    if (connect(fd, (const struct sockaddr *)&route->address, sizeof(route->address)) < 0) {
        if (errno != EINPROGRESS) {
            fail(failure, "connect_failed", errno);
            goto failed;
        }
        if (wait_socket(fd, POLLOUT, start + 5, NULL, client, failure)) {
            if (errno == ETIMEDOUT)
                *status = 504;
            goto failed;
        }
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error) {
            if (error)
                errno = error;
            if (errno == ETIMEDOUT)
                *status = 504;
            fail(failure, "connect_failed", errno);
            goto failed;
        }
    }
    return fd;
failed:
    {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
}

/* Upstream readability during a send can be an early final response. Do not deadlock
 * trying to send a POST to a backend that has already stopped reading it. */
static int send_bytes(int fd, const char *data, size_t length, uintmax_t *sent,
                       bool upstream, int client, struct proxy_failure *failure)
{
    double start = now_seconds();
    if (start < 0)
        return fail(failure, "clock_failed", errno);
    double deadline = start + 10;
    while (length) {
        short events;
        if (wait_socket(fd, POLLOUT | (upstream ? POLLIN : 0), deadline, &events, client, failure))
            return -1;
        if (upstream && (events & (POLLIN | POLLHUP)))
            return 1;
        ssize_t n = send(fd, data, length, 0);
        if (n > 0) {
            *sent += (size_t)n;
            data += n;
            length -= (size_t)n;
            start = now_seconds();
            if (start < 0)
                return fail(failure, "clock_failed", errno);
            deadline = start + 10;
        } else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            const char *reason = upstream ? "request_send_failed" : "client_send_failed";
            if (!upstream && n < 0 && (errno == ECONNRESET || errno == EPIPE))
                reason = "client_cancelled";
            return fail(failure, reason, n < 0 ? errno : 0);
        }
    }
    return 0;
}

static ssize_t read_bytes(int fd, char *buffer, size_t size, int flags, double deadline,
                          int client, struct proxy_failure *failure)
{
    for (;;) {
        if (wait_socket(fd, POLLIN, deadline, NULL, client, failure))
            return -1;
        ssize_t n = recv(fd, buffer, size, flags);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n == 0)
            fail(failure, "early_eof", 0);
        else if (n < 0)
            fail(failure, "upstream_read_failed", errno);
        return n;
    }
}

static int read_header(int fd, char *header, int client, struct proxy_failure *failure)
{
    double start = now_seconds();
    if (start < 0) {
        fail(failure, "clock_failed", errno);
        return 500;
    }
    double deadline = start + 10;
    size_t used = 0;
    while (used < HEADER_LIMIT) {
        size_t room = HEADER_LIMIT - used;
        if (room > 4096)
            room = 4096;
        ssize_t n = read_bytes(fd, header + used, room, MSG_PEEK, deadline, client, failure);
        if (n <= 0)
            return strcmp(failure->reason, "timeout") == 0 ? 504 : 502;
        size_t end = 0;
        size_t total = used + (size_t)n;
        for (size_t i = used > 3 ? used - 3 : 0; i + 4 <= total; ++i) {
            if (memcmp(header + i, "\r\n\r\n", 4) == 0) {
                end = i + 4;
                break;
            }
        }
        size_t take = end ? end - used : (size_t)n;
        if (memchr(header + used, '\0', take)) {
            fail(failure, "invalid_response_header", 0);
            return 502;
        }
        size_t consumed = 0;
        while (consumed < take) {
            n = read_bytes(fd, header + used + consumed, take - consumed, 0, deadline,
                           client, failure);
            if (n <= 0)
                return strcmp(failure->reason, "timeout") == 0 ? 504 : 502;
            consumed += (size_t)n;
        }
        used += take;
        if (end) {
            header[used] = '\0';
            return 0;
        }
    }
    fail(failure, "response_header_too_large", 0);
    return 502;
}

static void log_failure(const struct proxy_route *route, const char *phase, int status,
                         struct proxy_failure failure, double elapsed,
                         bool known, uintmax_t expected, uintmax_t received, uintmax_t sent)
{
    if (strcmp(failure.reason, "timeout") == 0) {
        if (strncmp(phase, "client_", 7) == 0)
            failure.reason = "client_timeout";
        else if (strcmp(phase, "admission") == 0 || strcmp(phase, "slot_release") == 0)
            failure.reason = "control_timeout";
        else
            failure.reason = "upstream_timeout";
    }
    char address[INET_ADDRSTRLEN] = "unknown";
    inet_ntop(AF_INET, &route->address.sin_addr, address, sizeof(address));
    char length[32];
    if (known)
        snprintf(length, sizeof(length), "%ju", expected);
    else
        strcpy(length, "unknown");
    /* Bound the configured route so concurrent workers use one small pipe-atomic write. */
    const char *path = route->path ? route->path : "unknown";
    char line[1024];
    int size = snprintf(line, sizeof(line),
        "proxy phase=%s status=%d errno=%d expected=%s received=%ju sent=%ju "
        "backend=%s:%u route=%.256s route_truncated=%d elapsed_ms=%.0f reason=%s\n",
        phase, status, failure.error, length, received, sent, address,
        (unsigned)ntohs(route->address.sin_port), path, strlen(path) > 256,
        elapsed, failure.reason);
    if (size > 0 && (size_t)size < sizeof(line))
        while (write(STDERR_FILENO, line, (size_t)size) < 0 && errno == EINTR) {}
}

int serve_proxy(int client, const struct proxy_route *route, struct request *request,
                const char *header, const char *body)
{
    int status;
    struct proxy_failure failure = {"internal_error", 0};
    double begin = now_seconds();
    int backend = -1;
    char raw[HEADER_LIMIT + 1];
    char buffer[16384];
    char *response = NULL;
    uintmax_t expected = 0, received = 0, sent = 0, upstream_sent = 0;
    bool known = false, started = false, acquired = false;
    const char *phase = "admission";
    if (begin < 0) {
        status = 500;
        fail(&failure, "clock_failed", errno);
        goto failed;
    }
    status = limit_exchange(route->backend_id, client, &failure);
    if (status)
        goto failed;
    acquired = true;
    phase = "connect";
    backend = connect_backend(route, &status, client, &failure);
    if (backend < 0)
        goto failed;
    phase = "request_send";
    int result = send_bytes(backend, header, strlen(header), &upstream_sent, true, client, &failure);
    if (!result)
        result = send_bytes(backend, body, request->content_length, &upstream_sent,
                            true, client, &failure);
    if (result < 0) {
        status = strcmp(failure.reason, "timeout") == 0 ? 504 : 502;
        goto failed;
    }
    if (result == 1)
        request->close = true;
    phase = "response_header";
    status = read_header(backend, raw, client, &failure);
    if (status)
        goto failed;
    const char *reason;
    status = proxy_response_header(raw, request, &response, &expected, &reason);
    if (status) {
        fail(&failure, reason, 0);
        goto failed;
    }
    known = true;
    phase = "client_header_send";
    uintmax_t header_sent = 0;
    result = send_bytes(client, response, strlen(response), &header_sent, false, -1, &failure);
    started = header_sent > 0;
    if (result) {
        status = 0; /* A downstream failure is not an upstream 504. */
        goto failed;
    }
    while (received < expected) {
        size_t take = expected - received < sizeof(buffer) ?
            (size_t)(expected - received) : sizeof(buffer);
        phase = "response_body";
        double now = now_seconds();
        if (now < 0) {
            status = 500;
            fail(&failure, "clock_failed", errno);
            goto failed;
        }
        ssize_t n = read_bytes(backend, buffer, take, 0, now + 10, client, &failure);
        if (n <= 0) {
            status = strcmp(failure.reason, "timeout") == 0 ? 504 : 502;
            goto failed;
        }
        received += (size_t)n;
        phase = "client_body_send";
        if (send_bytes(client, buffer, (size_t)n, &sent, false, -1, &failure)) {
            status = 0;
            goto failed;
        }
        /* Start the next receive timer only after downstream backpressure clears. */
    }
    free(response);
    response = NULL;
    close(backend);
    backend = -1;
    phase = "slot_release";
    status = limit_exchange(-1, -1, &failure);
    acquired = false;
    if (status)
        goto failed;
    return 0;
failed:
    {
        double end = now_seconds();
        double elapsed = begin >= 0 && end >= begin ? (end - begin) * 1000 : -1;
        if (strcmp(failure.reason, "client_cancelled") == 0)
            status = 0;
        if (backend >= 0)
            close(backend);
        if (acquired) {
            struct proxy_failure cleanup;
            limit_exchange(-1, -1, &cleanup);
        }
        free(response);
        request->close = true;
        log_failure(route, phase, status, failure, elapsed, known, expected, received, sent);
        return !started && status ? send_error(client, request, status) : -1;
    }
}
