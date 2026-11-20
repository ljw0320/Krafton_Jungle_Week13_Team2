#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "event_loop.h"

static int epfd;

// 3단계 임시: accept한 fd는 로그만 찍고 닫는다
static void accept_all(int listen_fd) {
    for (;;) {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) perror("accept4");
            return;
        }
        fprintf(stderr, "accept fd=%d\n", fd);
        close(fd);
    }
}

int loop_run(int listen_fd) {
    epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        return -1;
    }
    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = NULL};
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        perror("epoll_ctl");
        return -1;
    }
    struct epoll_event events[MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            return -1;
        }
        for (int i = 0; i < n; i++)
            if (events[i].data.ptr == NULL) accept_all(listen_fd);
    }
}
