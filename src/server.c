#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <limits.h>
#include <unistd.h>

/* 연결 소켓은 메인만 close합니다. RUNNING 자원은 join 전까지 건드리지 않습니다. */
void connection_close(struct connection *client)
{
    if (client->state == FREE)
        return;
    if (client->server)
        timer_cancel(client->server, client);
    if (client->server && client->server->epoll_active && client->watch_registered)
        epoll_ctl(client->server->epoll_fd, EPOLL_CTL_DEL, client->watch_fd, NULL);
    client->watch_registered = client->watch_armed = false;
    client->watch_ticket = 0;
    cgi_close(client);
    file_release(client);

    close(client->fd);
    context_release(client);

    printf("[close fd=%d] %s\n", client->fd,
           client->reason ? client->reason : "server_shutdown");

    client->fd = client->file_fd = -1;
    client->state = FREE;
    if (client->server) {
        client->free_next = client->server->free_clients;
        client->server->free_clients = client;
    }
}

static void register_client(struct server *server, int fd)
{
    if (nonblocking(fd) == -1) {
        perror("nonblocking client");
        close(fd);
        return;
    }

    if (!server->slots_initialized)
        slots_init(server);
    struct connection *client = server->free_clients;
    if (client) {
        server->free_clients = client->free_next;
        memset(client, 0, sizeof(*client));
        client->server = server;
        client->context_pool = &server->contexts;
        client->state = WAITING;
        client->phase = READ_HEADER;
        client->fd = fd;
        client->file_fd = -1;
        client->progress = now_ms();
        timer_update(server, client);
        printf("[accept fd=%d]\n", fd);
        return;
    }

    printf("[reject fd=%d] connection_limit\n", fd);
    close(fd);
}

static void accept_clients(struct server *server, int listener)
{
    for (int i = 0; i < ACCEPT_BUDGET && !stop_requested; i++) {
        int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (fd == -1) {
            if (errno == EINTR)
                continue;
            if (!would_block()) {
                perror("accept");
                stop_requested = 1;
            }
            break;
        }

        check_thread(server_profile_lock(&server->mutex));
        register_client(server, fd);
        check_thread(pthread_mutex_unlock(&server->mutex));
    }
}

/* Main owns registrations. ONESHOT disables delivered sockets during work.
 * Remove CGI watches before workers can close/reuse their descriptors. */
void server_io_suspend(struct connection *client)
{
    struct server *server = client->server;
    if (!server || !server->epoll_active || !client->watch_registered) return;
    if (client->watch_armed || client->watch_fd != client->fd) {
        if (epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, client->watch_fd, NULL) == -1) {
            perror("epoll DEL");
            server->io_failed = true;
            stop_requested = 1;
        }
        client->watch_registered = false;
    }
    client->watch_armed = false;
    client->watch_ticket = 0;
}

static bool arm_client(struct server *server, struct connection *client)
{
    if (client->watch_armed) return true;
    if (server->next_ticket == UINT32_MAX) { errno = EOVERFLOW; return false; }
    int fd = client->phase == CGI_OUTPUT ? client->cgi.fd : client->fd;
    if (client->watch_registered && client->watch_fd != fd) {
        if (epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, client->watch_fd, NULL) == -1) return false;
        client->watch_registered = false;
    }
    /* CGI pipe EOF may precede child exit; the existing timer rechecks it. */
    if (fd < 0) return true;
    uint32_t events = client->context_wait_deadline ? 0 :
        (client->phase == WRITE_RESPONSE ? EPOLLOUT : EPOLLIN);
    uint32_t ticket = ++server->next_ticket;
    struct epoll_event event = {.events = events | EPOLLONESHOT,
        .data.u64 = ((uint64_t)ticket << 32) | (uint32_t)(client - server->clients + 3)};
    int operation = client->watch_registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (epoll_ctl(server->epoll_fd, operation, fd, &event) == -1) return false;
    client->watch_registered = client->watch_armed = true;
    client->watch_fd = fd;
    client->watch_ticket = ticket;
    return true;
}

/* Preserve the scheduling scan; this change only replaces I/O watching. */
static int prepare_epoll(struct server *server)
{
    struct server_stamp stamp = server_profile_begin();
    int timeout = timer_expire(server);
    int64_t now = now_ms();
    for (int i = 0; i < server->capacity; i++) {
        struct connection *client = &server->clients[i];
        if (client->state == FINISHED) connection_close(client);
        if (client->state != WAITING) continue;
        if (client->pending) { enqueue_turn(server, client); continue; }
        if (client->context_wait_deadline && now < client->context_wait_deadline &&
            context_acquire(client)) {
            client->context_wait_deadline = 0;
            client->progress = now;
            enqueue_turn(server, client);
            continue;
        }
        if (connection_buffered(client)) { enqueue_turn(server, client); continue; }
        if (!arm_client(server, client)) {
            perror("epoll arm client");
            client->reason = "epoll_registration_error";
            connection_close(client);
        }
    }
    server_profile_end(SP_PREPARE, stamp, (uint64_t)server->capacity);
    return timeout;
}

static void dispatch_epoll(struct server *server, struct epoll_event *events, int count)
{
    struct server_stamp stamp = server_profile_begin();
    for (int i = 0; i < count; i++) {
        uint64_t token = events[i].data.u64;
        uint32_t index = (uint32_t)token;
        if (index < 3 || index - 3 >= (uint32_t)server->capacity) continue;
        struct connection *client = &server->clients[index - 3];
        if (client->state != WAITING || !client->watch_armed ||
            client->watch_ticket != (uint32_t)(token >> 32)) continue;
        client->watch_armed = false;
        if (client->context_wait_deadline) {
            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                client->reason = "context_wait_closed";
                connection_close(client);
            }
        } else enqueue_turn(server, client);
    }
    server_profile_end(SP_DISPATCH, stamp, count);
}

void server_run(struct server *server, int listener)
{
    if (!server->slots_initialized) slots_init(server);
    server->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (server->epoll_fd == -1) goto failed;
    server->epoll_active = true;
    struct epoll_event event = {.events = EPOLLIN, .data.u64 = 1};
    if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, listener, &event) == -1) goto failed;
    event.data.u64 = 2;
    if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, server->wake_fd, &event) == -1) goto failed;
    while (!stop_requested) {
        check_thread(server_profile_lock(&server->mutex));
        int timeout = prepare_epoll(server);
        check_thread(pthread_mutex_unlock(&server->mutex));
        if (stop_requested) break;
        struct epoll_event events[64];
        struct server_stamp stamp = server_profile_begin();
        int count = epoll_wait(server->epoll_fd, events, 64, timeout);
        server_profile_end(SP_POLL, stamp, count > 0 ? (uint64_t)count : 0);
        if (count == -1) { if (errno == EINTR) continue; goto failed; }
        if (stop_requested) break;
        bool accept_ready = false;
        for (int i = 0; i < count; i++) {
            if (events[i].data.u64 == 1) accept_ready = true;
            if (events[i].data.u64 == 2) {
                stamp = server_profile_begin();
                drain_notifications(server->wake_fd);
                server_profile_end(SP_DRAIN, stamp, 1);
            }
        }
        check_thread(server_profile_lock(&server->mutex));
        dispatch_epoll(server, events, count);
        check_thread(pthread_mutex_unlock(&server->mutex));
        if (accept_ready) {
            stamp = server_profile_begin();
            accept_clients(server, listener);
            server_profile_end(SP_ACCEPT, stamp, 0);
        }
    }
    goto done;
failed:
    perror("server epoll");
    server->io_failed = true;
    stop_requested = 1;
done:
    if (server->epoll_active) close(server->epoll_fd);
    server->epoll_active = false;
}
