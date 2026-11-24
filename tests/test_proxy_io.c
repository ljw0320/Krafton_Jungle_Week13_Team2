#define _POSIX_C_SOURCE 200809L
/* Keep fault injection in the test binary; production deadlines are unchanged. */
#include "../proxy/proxy.c"
#include <assert.h>

static int injected_timeout;
static int observed_wait;
int __real_poll(struct pollfd *fds, nfds_t count, int timeout);
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    if (!injected_timeout)
        return __real_poll(fds, count, timeout);
    observed_wait = timeout;
    return 0;
}

/* Fortified libc redirects poll to __poll_chk under the sanitizer build. */
int __wrap___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t bytes)
{
    assert(count <= bytes / sizeof(*fds));
    return __wrap_poll(fds, count, timeout);
}

int __real_connect(int fd, const struct sockaddr *address, socklen_t size);
int __wrap_connect(int fd, const struct sockaddr *address, socklen_t size)
{
    if (!injected_timeout)
        return __real_connect(fd, address, size);
    errno = EINPROGRESS;
    return -1;
}

int main(void)
{
    struct proxy_route route = {.address.sin_family = AF_INET};
    int status;
    injected_timeout = 1;
    assert(connect_backend(&route, &status) == -1);
    assert(status == 504 && observed_wait > 4900 && observed_wait <= 5001);
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    uintmax_t sent = 0;
    assert(send_bytes(pair[0], "x", 1, &sent, true) == -1);
    assert(errno == ETIMEDOUT && sent == 0 && observed_wait > 9900 && observed_wait <= 10001);
    char header[HEADER_LIMIT + 1];
    assert(read_header(pair[0], header) == 504);
    assert(observed_wait > 9900 && observed_wait <= 10001);
    assert(send_bytes(pair[0], "x", 1, &sent, false) == -1);
    assert(errno == ETIMEDOUT && observed_wait > 9900 && observed_wait <= 10001);
    close(pair[0]);
    close(pair[1]);
    puts("proxy I/O timeout tests passed");
    return 0;
}
