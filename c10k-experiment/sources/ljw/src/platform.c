#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t signal_fd = -1;

void fail(const char *operation)
{
    perror(operation);
    exit(EXIT_FAILURE);
}

void check_thread(int error)
{
    if (error) {
        fprintf(stderr, "pthread: %s\n", strerror(error));
        exit(EXIT_FAILURE);
    }
}

int64_t now_ms(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) == -1)
        fail("clock_gettime");

    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

bool would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

int nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1 || fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int listener_open(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1)
        fail("socket");

    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1)
        fail("SO_REUSEADDR");
    if (nonblocking(fd) == -1)
        fail("nonblocking listener");

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) == -1)
        fail("bind");
    if (listen(fd, MAX_CONNECTIONS) == -1)
        fail("listen");

    return fd;
}

/* eventfd에는 완료 사실만 알리고 실제 연결 상태는 mutex로 공유합니다. */
void notify_main(int fd)
{
    const uint64_t one = 1;
    ssize_t result;

    do {
        result = write(fd, &one, sizeof(one));
    } while (result == -1 && errno == EINTR);

    if (result == -1 && !would_block())
        fail("eventfd write");
}

void drain_notifications(int fd)
{
    uint64_t value;
    ssize_t result;

    do {
        result = read(fd, &value, sizeof(value));
    } while (result > 0 || (result == -1 && errno == EINTR));

    if (result == -1 && !would_block())
        fail("eventfd read");
}

/* 핸들러는 종료 플래그와 async-signal-safe write만 수행합니다. */
static void on_signal(int number)
{
    int saved = errno;
    const uint64_t one = 1;

    (void)number;
    stop_requested = 1;

    if (signal_fd >= 0) {
        ssize_t result;

        do {
            result = write((int)signal_fd, &one, sizeof(one));
        } while (result == -1 && errno == EINTR);
    }

    errno = saved;
}

static void termination_mask(int operation)
{
    sigset_t mask;

    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);

    check_thread(pthread_sigmask(operation, &mask, NULL));
}

void signals_start(int wake_fd)
{
    struct sigaction action = {0};

    signal_fd = wake_fd;
    sigemptyset(&action.sa_mask);
    action.sa_handler = on_signal;
    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1)
        fail("sigaction termination");

    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL) == -1)
        fail("sigaction SIGPIPE");

    termination_mask(SIG_BLOCK);
}

void signals_unblock(void)
{
    termination_mask(SIG_UNBLOCK);
}

void signals_detach(void)
{
    termination_mask(SIG_BLOCK);
    signal_fd = -1;
}
