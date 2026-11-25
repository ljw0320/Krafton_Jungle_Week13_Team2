#include "test_support.h"
#include "../server.c"
#include <fcntl.h>

int main(void)
{
    struct server server = {0};
    slots_init(&server);
    context_pool_init(&server.contexts);
    CHECK(pthread_mutex_init(&server.mutex, NULL) == 0);
    CHECK(pthread_cond_init(&server.ready, NULL) == 0);
    server.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    CHECK(server.epoll_fd >= 0);
    server.epoll_active = true;
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    register_client(&server, pair[0]);
    struct connection *c = &server.clients[0];
    CHECK(arm_client(&server, c));
    CHECK(write(pair[1], "xy", 2) == 2);
    struct epoll_event event;
    CHECK(epoll_wait(server.epoll_fd, &event, 1, 100) == 1);
    struct epoll_event stale = event;
    CHECK(epoll_wait(server.epoll_fd, &event, 1, 0) == 0);
    dispatch_epoll(&server, &stale, 1);
    CHECK(c->state == QUEUED && server.count == 1);
    dispatch_epoll(&server, &stale, 1);
    CHECK(server.count == 1);
    CHECK(epoll_wait(server.epoll_fd, &event, 1, 0) == 0);
    /* Simulate worker completion; unread input must trigger after MOD rearm. */
    server.count = 0;
    c->state = WAITING;
    CHECK(arm_client(&server, c));
    dispatch_epoll(&server, &stale, 1);
    CHECK(server.count == 0);
    CHECK(epoll_wait(server.epoll_fd, &event, 1, 100) == 1);
    dispatch_epoll(&server, &event, 1);
    CHECK(server.count == 1);
    server.count = 0;
    c->state = WAITING;
    /* Socket -> CGI descriptor transition, then remove before worker closes. */
    int pipefd[2];
    CHECK(pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) == 0);
    c->phase = CGI_OUTPUT;
    c->cgi.fd = pipefd[0];
    CHECK(arm_client(&server, c));
    CHECK(write(pipefd[1], "a", 1) == 1);
    CHECK(epoll_wait(server.epoll_fd, &event, 1, 100) == 1);
    dispatch_epoll(&server, &event, 1);
    CHECK(!c->watch_registered);
    close(pipefd[0]); close(pipefd[1]);
    c->cgi.fd = -1;
    c->state = WAITING;
    CHECK(arm_client(&server, c)); /* EOF with child still alive: timer only. */
    c->phase = READ_HEADER;
    CHECK(arm_client(&server, c));
    CHECK(epoll_wait(server.epoll_fd, &event, 1, 100) == 1);
    stale = event;
    connection_close(c);
    close(pair[1]);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    register_client(&server, pair[0]);
    CHECK(&server.clients[0] == c);
    CHECK(arm_client(&server, c));
    server.count = 0;
    dispatch_epoll(&server, &stale, 1);
    CHECK(server.count == 0 && c->state == WAITING);
    /* Failed per-connection registration must reclaim only that connection. */
    server_io_suspend(c);
    int saved = c->fd;
    c->fd = -1;
    c->phase = CGI_OUTPUT;
    c->cgi.fd = open("/dev/null", O_RDONLY);
    CHECK(c->cgi.fd >= 0);
    CHECK(!arm_client(&server, c) && errno == EPERM);
    close(c->cgi.fd); c->cgi.fd = -1; c->fd = saved;
    connection_close(c); close(pair[1]);
    close(server.epoll_fd);
    context_pool_destroy(&server.contexts);
    free(server.clients);
    CHECK(pthread_mutex_destroy(&server.mutex) == 0);
    CHECK(pthread_cond_destroy(&server.ready) == 0);
    puts("server epoll: ONESHOT, rearm, duplicate/stale events, slot reuse, CGI switch/EOF, errors passed");
    return 0;
}
