#include "test_support.h"
#include "../server.c"
#include <fcntl.h>

static void test_pending_fifo(void)
{
    struct server server = {0};
    slots_init(&server);
    context_pool_init(&server.contexts);
    CHECK(pthread_cond_init(&server.ready, NULL) == 0);
    int peers[MAX_CONNECTIONS];
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        int pair[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        register_client(&server, pair[0]);
        peers[i] = pair[1];
    }
    struct connection *a = &server.clients[4], *b = &server.clients[5];
    struct connection *c = &server.clients[6], *newcomer = &server.clients[7];
    for (int i = 0; i < 7; i++) enqueue_turn(&server, &server.clients[i]);
    CHECK(server.count == 4 && server.pending_head == a && server.pending_tail == c);
    enqueue_turn(&server, a);
    CHECK(a->pending_next == b && b->pending_next == c);
    server_changed(&server, b); /* Already on changed list: no duplicate. */
    connection_close(b); /* Remove middle entry from both queues. */
    CHECK(a->pending_next == c && c->pending_prev == a);
    CHECK(server.clients[4].control_next == c);
    /* Dequeue one old job, then let a newcomer arrive first. Older a must win. */
    struct connection *old = server.jobs[server.head];
    server.head = (server.head + 1) % QUEUE_CAPACITY;
    server.count--; old->state = RUNNING;
    enqueue_turn(&server, newcomer);
    CHECK(a->state == QUEUED && newcomer->pending && server.pending_head == c);
    CHECK(server.jobs[(server.tail + QUEUE_CAPACITY - 1) % QUEUE_CAPACITY] == a);
    connection_close(c); /* Head expiry/close must leave newcomer reachable. */
    CHECK(server.pending_head == newcomer && newcomer->pending_prev == NULL);
    /* Reuse freed slot: it must not inherit old queue membership. */
    int pair[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    register_client(&server, pair[0]);
    CHECK(!c->pending && c->control_queued && server.control_tail == c);
    close(pair[1]);
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        connection_close(&server.clients[i]); close(peers[i]);
    }
    CHECK(!server.pending_head && !server.pending_tail);
    CHECK(!server.control_head && !server.control_tail);
    context_pool_destroy(&server.contexts); free(server.clients);
    CHECK(pthread_cond_destroy(&server.ready) == 0);
}

static void test_buffer_resume_fifo(void)
{
    struct server server = {0};
    slots_init(&server); context_pool_init(&server.contexts);
    CHECK(pthread_cond_init(&server.ready, NULL) == 0);
    int peers[10];
    for (int i = 0; i < 10; i++) {
        int pair[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        register_client(&server, pair[0]); peers[i] = pair[1];
        if (i < 8) CHECK(context_acquire(&server.clients[i]));
    }
    struct connection *first = &server.clients[9], *second = &server.clients[8];
    CHECK(!context_acquire(first) && !context_acquire(second));
    int64_t deadline = second->context_wait_deadline;
    CHECK(resume_contexts(&server) == 1);
    CHECK(second->context_wait_deadline == deadline);
    context_release(&server.clients[0]);
    resume_contexts(&server);
    CHECK(first->context && first->state == QUEUED);
    CHECK(!second->context && second->context_wait_deadline == deadline);
    CHECK(server.jobs[server.head] == first);
    second->context_wait_deadline = now_ms() - 1;
    resume_contexts(&server);
    CHECK(second->state == FREE && server.contexts.wait_count == 0);
    for (int i = 0; i < 10; i++) { connection_close(&server.clients[i]); close(peers[i]); }
    context_pool_destroy(&server.contexts); free(server.clients);
    CHECK(pthread_cond_destroy(&server.ready) == 0);
}

int main(void)
{
    test_pending_fifo();
    test_buffer_resume_fifo();
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
    CHECK(prepare_epoll(&server) >= 0);
    CHECK(!server.control_head && c->watch_armed);
    uint32_t ticket = c->watch_ticket;
    CHECK(prepare_epoll(&server) >= 0 && c->watch_ticket == ticket);
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
