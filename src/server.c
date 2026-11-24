#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* 연결 소켓은 메인만 close합니다. RUNNING 자원은 join 전까지 건드리지 않습니다. */
void connection_close(struct connection *client)
{
    if (client->state == FREE)
        return;
    if (client->server)
        timer_cancel(client->server, client);
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

        check_thread(pthread_mutex_lock(&server->mutex));
        register_client(server, fd);
        check_thread(pthread_mutex_unlock(&server->mutex));
    }
}

/* CGI stdout 준비와 자식 종료 재확인 시각 중 먼저 도착하는 사건을 기다립니다. */
static bool poll_cgi(struct connection *client, struct pollfd *fd, int *timeout, int64_t now)
{
    (void)timeout;
    (void)now;
    *fd = (struct pollfd){.fd = client->cgi.fd, .events = POLLIN};
    return true;
}

/* 정리 중에는 POLLIN을 기다리고, 상대가 아무것도 보내지 않아도 기한에 회수합니다. */
static bool poll_client(struct connection *client, struct pollfd *fd, int *timeout, int64_t now)
{
    if (client->phase == CGI_OUTPUT)
        return poll_cgi(client, fd, timeout, now);
    bool writing = client->phase == WRITE_RESPONSE;
    (void)now;
    if (connection_buffered(client))
        *timeout = 0;

    *fd = (struct pollfd){.fd = client->fd,
                         .events = client->context_wait_deadline ? 0 : (writing ? POLLOUT : POLLIN)};

    return true;
}

/* WAITING만 읽으므로 작업자가 수정 중인 버퍼·시각과 경쟁하지 않습니다. */
static nfds_t prepare_poll(struct server *server, int listener,
                           struct pollfd *fds, struct connection **owners,
                           int *timeout)
{
    nfds_t count = 2;
    int64_t now = now_ms();

    *timeout = timer_expire(server);
    fds[0] = (struct pollfd){.fd = listener, .events = POLLIN};
    fds[1] = (struct pollfd){.fd = server->wake_fd, .events = POLLIN};

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        struct connection *client = &server->clients[i];
        if (client->state == FINISHED)
            connection_close(client);
        if (client->state != WAITING)
            continue;

        if (client->pending) {
            enqueue_turn(server, client);
            continue;
        }
        /* 슬롯당 하나의 대기 기록: 최대 MAX_CONNECTIONS개, 중복 없음.
         * 반환 작업자의 wake 알림으로 재검사하며 절대 2초 기한을 유지합니다. */
        if (client->context_wait_deadline && now < client->context_wait_deadline &&
            context_acquire(client)) {
            client->context_wait_deadline = 0;
            client->progress = now;
            enqueue_turn(server, client);
            continue;
        }
        if (poll_client(client, &fds[count], timeout, now))
            owners[count++] = client;
    }

    /* 슬롯 순서가 FIFO와 다르면 앞 슬롯의 대기자를 다음 루프에서 재검사합니다. */

    return count;
}

static void dispatch(struct server *server, struct pollfd *fds, struct connection **owners, nfds_t count)
{
    check_thread(pthread_mutex_lock(&server->mutex));

    for (nfds_t i = 2; i < count; i++) {
        if (owners[i]->state != WAITING)
            continue;
        if (fds[i].revents & POLLNVAL) {
            owners[i]->reason = "invalid_socket";
            connection_close(owners[i]);
        } else if (owners[i]->context_wait_deadline) {
            if (fds[i].revents & (POLLHUP | POLLERR)) {
                owners[i]->reason = "context_wait_closed";
                connection_close(owners[i]);
            }
        } else if (fds[i].revents || connection_buffered(owners[i]) || (owners[i]->phase == CGI_OUTPUT &&
                   (now_ms() >= owners[i]->cgi.next_check || now_ms() >= owners[i]->cgi.deadline))) {
            enqueue_turn(server, owners[i]);
        }
    }

    check_thread(pthread_mutex_unlock(&server->mutex));
}

/* 메인은 준비 대기·작업 배정만 하고 실제 HTTP I/O는 작업자가 맡습니다. */
void server_run(struct server *server, int listener)
{
    if (!server->slots_initialized)
        slots_init(server);
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
