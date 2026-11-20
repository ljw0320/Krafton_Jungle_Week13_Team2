#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "conn.h"
#include "event_loop.h"

static int epfd;
static int lfd;  // loop_unwatch가 재개할 listen fd
static int accept_paused;

int loop_watch(struct conn *c, uint32_t events) {
    struct epoll_event ev = {.events = events, .data.ptr = c};
    int op = c->events == 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    if (epoll_ctl(epfd, op, c->fd, &ev) < 0) {
        perror("epoll_ctl");
        return -1;
    }
    c->events = events;
    return 0;
}

void loop_unwatch(struct conn *c) {
    if (c->events != 0) {
        if (epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL) < 0) perror("epoll_ctl");
        c->events = 0;
    }
    // fd가 하나 비므로 accept 재개. 실제 accept는 다음 epoll_wait라 그때는 close된 뒤다
    if (accept_paused) {
        struct epoll_event ev = {.events = EPOLLIN, .data.ptr = NULL};
        if (epoll_ctl(epfd, EPOLL_CTL_MOD, lfd, &ev) < 0) {
            perror("epoll_ctl");
            return;
        }
        accept_paused = 0;
        fprintf(stderr, "accept resumed\n");
    }
}

static void accept_all(int listen_fd) {
    for (;;) {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EMFILE || errno == ENFILE) {
                // LT라 대기열이 남으면 계속 깨어나므로 감시를 끈다
                struct epoll_event ev = {.events = 0, .data.ptr = NULL};
                if (epoll_ctl(epfd, EPOLL_CTL_MOD, listen_fd, &ev) < 0) {
                    perror("epoll_ctl");
                    return;
                }
                accept_paused = 1;
                fprintf(stderr, "accept paused\n");
                return;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) perror("accept4");
            return;
        }
        fprintf(stderr, "accept fd=%d\n", fd);
        conn_new(fd);
    }
}

int loop_run(int listen_fd) {
    lfd = listen_fd;
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
    time_t last_sweep = 0;
    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            return -1;
        }
        for (int i = 0; i < n; i++) {
            struct conn *c = events[i].data.ptr;
            uint32_t e = events[i].events;
            if (c == NULL) {
                accept_all(listen_fd);
                continue;
            }
            if (e & (EPOLLERR | EPOLLHUP)) {
                conn_close(c);
                continue;
            }
            // -1이면 c는 이미 해제됐다
            if ((e & EPOLLIN) && conn_on_readable(c) < 0) continue;
            if (e & EPOLLOUT) conn_on_writable(c);
        }
        time_t now = time(NULL);
        if (now != last_sweep) {
            last_sweep = now;
            conn_sweep_idle(now);
        }
    }
}
