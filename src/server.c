#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* 메인만 close합니다. RUNNING 자원은 join 전까지 건드리지 않습니다. */
void connection_close(struct connection *client)
{
    if (client->file_fd >= 0)
        close(client->file_fd);

    close(client->fd);

    printf("[close fd=%d] %s\n", client->fd,
           client->reason ? client->reason : "server_shutdown");

    client->fd = client->file_fd = -1;
    client->state = FREE;
}

static void register_client(struct server *server, int fd)
{
    if (nonblocking(fd) == -1) {
        perror("nonblocking client");
        close(fd);
        return;
    }

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        struct connection *client = &server->clients[i];
        if (client->state != FREE)
            continue;

        memset(client, 0, sizeof(*client));
        client->state = WAITING;
        client->phase = READ_HEADER;
        client->fd = fd;
        client->file_fd = -1;
        client->progress = now_ms();

        printf("[accept fd=%d]\n", fd);
        return;
    }

    printf("[reject fd=%d] connection_limit\n", fd);
    close(fd);
}

static void accept_clients(struct server *server, int listener)
{
    for (int i = 0; i < ACCEPT_BUDGET && !stop_requested; i++) {
        int fd = accept(listener, NULL, NULL);

        if (fd == -1) {
            if (errno == EINTR)
                continue;
            if (!would_block()) {
                perror("accept");
                stop_requested = 1;
            }
            break;
        }

        check_thread(pthread_mutex_lock(&server->mutex));
        register_client(server, fd);
        check_thread(pthread_mutex_unlock(&server->mutex));
    }
}

static bool poll_client(struct connection *client, struct pollfd *fd, int *timeout, int64_t now)
{
    bool writing = client->phase == WRITE_RESPONSE;
    int limit = writing ? SEND_TIMEOUT_MS : RECV_TIMEOUT_MS;
    int64_t left = client->progress + limit - now;

    if (left <= 0) {
        client->reason = writing ? "send_timeout" : "recv_timeout";
        connection_close(client);
        return false;
    }

    if (*timeout < 0 || left < *timeout)
        *timeout = (int)left;

    *fd = (struct pollfd){.fd = client->fd,
                         .events = writing ? POLLOUT : POLLIN};

    return true;
}

/* WAITING만 읽으므로 작업자가 수정 중인 버퍼·시각과 경쟁하지 않습니다. */
static nfds_t prepare_poll(struct server *server, int listener,
                           struct pollfd *fds, struct connection **owners,
                           int *timeout)
{
    nfds_t count = 2;
    int64_t now = now_ms();

    *timeout = -1;
    fds[0] = (struct pollfd){.fd = listener, .events = POLLIN};
    fds[1] = (struct pollfd){.fd = server->wake_fd, .events = POLLIN};

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        struct connection *client = &server->clients[i];
        if (client->state == FINISHED)
            connection_close(client);
        if (client->state != WAITING)
            continue;

        if (poll_client(client, &fds[count], timeout, now))
            owners[count++] = client;
    }

    return count;
}

static void dispatch(struct server *server, struct pollfd *fds, struct connection **owners, nfds_t count)
{
    check_thread(pthread_mutex_lock(&server->mutex));

    for (nfds_t i = 2; i < count; i++) {
        if (fds[i].revents & POLLNVAL) {
            owners[i]->reason = "invalid_socket";
            connection_close(owners[i]);
        } else if (fds[i].revents) {
            enqueue_turn(server, owners[i]);
        }
    }

    check_thread(pthread_mutex_unlock(&server->mutex));
}

/* 메인은 준비 대기·작업 배정만 하고 실제 HTTP I/O는 작업자가 맡습니다. */
void server_run(struct server *server, int listener)
{
    while (!stop_requested) {
        struct pollfd fds[MAX_CONNECTIONS + 2];
        struct connection *owners[MAX_CONNECTIONS + 2] = {0};
        int timeout;

        check_thread(pthread_mutex_lock(&server->mutex));
        nfds_t count = prepare_poll(server, listener, fds, owners, &timeout);
        check_thread(pthread_mutex_unlock(&server->mutex));

        if (poll(fds, count, timeout) == -1) {
            if (errno == EINTR)
                continue;
            perror("poll");
            stop_requested = 1;
            break;
        }

        if (stop_requested)
            break;
        if (fds[1].revents & POLLIN)
            drain_notifications(server->wake_fd);
        dispatch(server, fds, owners, count);
        if (fds[0].revents & POLLIN)
            accept_clients(server, listener);
    }
}
