#define _POSIX_C_SOURCE 200809L
#include "proxy.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return -1;
    return now.tv_sec + now.tv_nsec / 1e9;
}

static int wait_socket(int fd, short events, double deadline, short *returned, int client)
{
    for (;;) {
        double now = now_seconds();
        if (now < 0)
            return -1;
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
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
            return -1;
        }
        if (result < 0)
            return -1;
        /* FIN may be a valid write-half close; only errors/full hangup cancel. */
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = ECANCELED;
            return -1;
        }
        if (returned)
            *returned = fds[0].revents;
        return 0;
    }
}

static int connect_backend(const struct proxy_route *route, int *status, int client)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    *status = 500;
    if (fd < 0)
        return -1;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        goto failed;
    *status = 502;
    double start = now_seconds();
    if (start < 0)
        goto failed;
    if (connect(fd, (const struct sockaddr *)&route->address, sizeof(route->address)) < 0) {
        if (errno != EINPROGRESS)
            goto failed;
        if (wait_socket(fd, POLLOUT, start + 5, NULL, client)) {
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
            goto failed;
        }
    }
    return fd;
failed:
    close(fd);
    return -1;
}

/* Upstream readability during a send can be an early final response. Do not deadlock
 * trying to send a POST to a backend that has already stopped reading it. */
static int send_bytes(int fd, const char *data, size_t length, uintmax_t *sent,
                       bool upstream, int client)
{
    double start = now_seconds();
    if (start < 0)
        return -1;
    double deadline = start + 10;
    while (length) {
        short events;
        if (wait_socket(fd, POLLOUT | (upstream ? POLLIN : 0), deadline, &events, client))
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
                return -1;
            deadline = start + 10;
        } else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            return -1;
        }
    }
    return 0;
}

static ssize_t read_bytes(int fd, char *buffer, size_t size, int flags, double deadline,
                          int client)
{
    for (;;) {
        if (wait_socket(fd, POLLIN, deadline, NULL, client))
            return -1;
        ssize_t n = recv(fd, buffer, size, flags);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n == 0)
            errno = ECONNRESET;
        return n;
    }
}

static int read_header(int fd, char *header, int client)
{
    double start = now_seconds();
    if (start < 0)
        return 500;
    double deadline = start + 10;
    size_t used = 0;
    while (used < HEADER_LIMIT) {
        size_t room = HEADER_LIMIT - used;
        if (room > 4096)
            room = 4096;
        ssize_t n = read_bytes(fd, header + used, room, MSG_PEEK, deadline, client);
        if (n <= 0)
            return errno == ETIMEDOUT ? 504 : 502;
        size_t end = 0;
        size_t total = used + (size_t)n;
        for (size_t i = used > 3 ? used - 3 : 0; i + 4 <= total; ++i) {
            if (memcmp(header + i, "\r\n\r\n", 4) == 0) {
                end = i + 4;
                break;
            }
        }
        size_t take = end ? end - used : (size_t)n;
        if (memchr(header + used, '\0', take))
            return 502;
        size_t consumed = 0;
        while (consumed < take) {
            n = read_bytes(fd, header + used + consumed, take - consumed, 0, deadline, client);
            if (n <= 0)
                return errno == ETIMEDOUT ? 504 : 502;
            consumed += (size_t)n;
        }
        used += take;
        if (end) {
            header[used] = '\0';
            return 0;
        }
    }
    return 502;
}

int serve_proxy(int client, const struct proxy_route *route, struct request *request,
                const char *header, const char *body)
{
    int status;
    int backend = connect_backend(route, &status, client);
    char raw[HEADER_LIMIT + 1];
    char buffer[16384];
    char *response = NULL;
    uintmax_t expected = 0, received = 0, sent = 0, upstream_sent = 0;
    bool known = false, started = false;
    const char *phase = "connect";
    if (backend < 0)
        goto failed;
    phase = "request_send";
    int result = send_bytes(backend, header, strlen(header), &upstream_sent, true, client);
    if (!result)
        result = send_bytes(backend, body, request->content_length, &upstream_sent, true, client);
    if (result < 0) {
        status = errno == ETIMEDOUT ? 504 : 502;
        goto failed;
    }
    if (result == 1)
        request->close = true;
    phase = "response_header";
    status = read_header(backend, raw, client);
    if (status)
        goto failed;
    status = proxy_response_header(raw, request, &response, &expected);
    if (status)
        goto failed;
    known = true;
    phase = "client_header_send";
    uintmax_t header_sent = 0;
    result = send_bytes(client, response, strlen(response), &header_sent, false, -1);
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
            goto failed;
        }
        ssize_t n = read_bytes(backend, buffer, take, 0, now + 10, client);
        if (n <= 0) {
            status = errno == ETIMEDOUT ? 504 : 502;
            goto failed;
        }
        received += (size_t)n;
        phase = "client_body_send";
        if (send_bytes(client, buffer, (size_t)n, &sent, false, -1)) {
            status = 0;
            goto failed;
        }
        /* Start the next receive timer only after downstream backpressure clears. */
    }
    free(response);
    close(backend);
    return 0;
failed:
    {
        int error = errno;
        if (error == ECANCELED)
            status = 0;
        if (backend >= 0)
            close(backend);
        free(response);
        request->close = true;
        char length[32];
        if (known)
            snprintf(length, sizeof(length), "%ju", expected);
        else
            strcpy(length, "unknown");
        fprintf(stderr, "proxy phase=%s status=%d errno=%d expected=%s received=%ju sent=%ju\n",
                phase, status, error, length, received, sent);
        return !started && status ? send_error(client, request, status) : -1;
    }
}
