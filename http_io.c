#define _POSIX_C_SOURCE 200809L
#include "http_internal.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

enum { REQUEST_SECONDS = 30, SEND_SECONDS = 10 };

static double monotonic_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
        return -1;
    return now.tv_sec + now.tv_nsec / 1e9;
}

/* A fixed deadline prevents trickling bytes from extending the request lifetime. */
static int wait_ready(int fd, short events, double deadline)
{
    for (;;) {
        double now = monotonic_seconds();
        if (now < 0)
            return -1;
        double remaining = deadline - now;
        if (remaining <= 0)
            return 0;
        struct pollfd ready = {.fd = fd, .events = events};
        int result = poll(&ready, 1, (int)(remaining * 1000) + 1);
        if (result < 0 && errno == EINTR)
            continue;
        return result;
    }
}

int send_all(int fd, const void *data, size_t length)
{
    const char *bytes = data;
    double now = monotonic_seconds();
    if (now < 0)
        return -1;
    double deadline = now + SEND_SECONDS;
    while (length) {
        if (wait_ready(fd, POLLOUT, deadline) <= 0)
            return -1;
        ssize_t n = send(fd, bytes, length, 0);
        if (n > 0) {
            bytes += n;
            length -= (size_t)n;
            now = monotonic_seconds();
            if (now < 0)
                return -1;
            deadline = now + SEND_SECONDS;
        } else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            return -1;
        }
    }
    return 0;
}

static enum receive_result receive_bytes(int fd, void *buffer, size_t capacity,
                                         int flags, double deadline, size_t *received)
{
    for (;;) {
        int ready = wait_ready(fd, POLLIN, deadline);
        if (ready <= 0)
            return ready == 0 ? RECEIVE_TIMEOUT : RECEIVE_ERROR;
        ssize_t n = recv(fd, buffer, capacity, flags);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n < 0)
            return RECEIVE_ERROR;
        if (n == 0)
            return RECEIVE_CLOSED;
        *received = (size_t)n;
        return RECEIVE_OK;
    }
}

static int receive_error_status(enum receive_result result)
{
    if (result == RECEIVE_TIMEOUT)
        return 408;
    if (result == RECEIVE_CLOSED)
        return 400;
    return RECEIVE_ERROR;
}

/* Consumes exactly length bytes, leaving the next request in the socket. */
static int receive_exact(int fd, char *buffer, size_t length, double deadline)
{
    size_t received = 0;
    while (received < length) {
        size_t n;
        enum receive_result result = receive_bytes(fd, buffer + received,
            length - received, 0, deadline, &n);
        if (result != RECEIVE_OK)
            return receive_error_status(result);
        received += n;
    }
    return RECEIVE_OK;
}

/* Only the last three old bytes can start a boundary completed by new bytes. */
static size_t find_header_end(const char *header, size_t previous, size_t length)
{
    size_t start = previous > 3 ? previous - 3 : 0;
    for (size_t i = start; i + 4 <= length; ++i)
        if (memcmp(header + i, "\r\n\r\n", 4) == 0)
            return i + 4;
    return 0;
}

static int check_target_length(const char *header)
{
    const char *space = strchr(header, ' ');
    if (!space)
        return 0;
    const char *end = strchr(space + 1, ' ');
    size_t length = end ? (size_t)(end - space - 1) : strlen(space + 1);
    return length > URI_LIMIT ? 414 : 0;
}

/* The first byte is already consumed; capacity includes the terminating NUL. */
static int receive_header_content(int fd, char *header, size_t capacity, double deadline)
{
    size_t limit = capacity - 1 < HEADER_LIMIT ? capacity - 1 : HEADER_LIMIT;
    size_t used = 1;
    while (used < limit) {
        size_t available;
        size_t room = limit - used < 4096 ? limit - used : 4096;
        enum receive_result result = receive_bytes(fd, header + used, room,
            MSG_PEEK, deadline, &available);
        if (result != RECEIVE_OK)
            return receive_error_status(result);
        size_t end = find_header_end(header, used, used + available);
        size_t take = end ? end - used : available;
        if (memchr(header, '\0', used + take))
            return 400;
        int status = receive_exact(fd, header + used, take, deadline);
        if (status)
            return status;
        used += take;
        header[used] = '\0';
        status = check_target_length(header);
        if (status)
            return status;
        if (end)
            return RECEIVE_OK;
    }
    return 431;
}

/* Returns OK, CLOSED for idle/EOF, ERROR for I/O, or a positive HTTP error. */
int receive_header(int fd, char *header, size_t capacity, int idle_seconds, double *deadline)
{
    if (capacity < 2)
        return RECEIVE_ERROR;
    double now = monotonic_seconds();
    if (now < 0)
        return RECEIVE_ERROR;
    size_t received;
    enum receive_result first = receive_bytes(fd, header, 1, 0, now + idle_seconds, &received);
    if (first != RECEIVE_OK)
        return first == RECEIVE_TIMEOUT ? RECEIVE_CLOSED : first;
    now = monotonic_seconds();
    if (now < 0)
        return RECEIVE_ERROR;
    *deadline = now + REQUEST_SECONDS;
    return receive_header_content(fd, header, capacity, *deadline);
}

/* Discards exactly content_length bytes under the original request deadline. */
int receive_body(int fd, size_t content_length, double deadline)
{
    char buffer[16384];
    size_t left = content_length;
    while (left) {
        size_t size = left < sizeof(buffer) ? left : sizeof(buffer);
        int status = receive_exact(fd, buffer, size, deadline);
        if (status)
            return status;
        left -= size;
    }
    return RECEIVE_OK;
}
