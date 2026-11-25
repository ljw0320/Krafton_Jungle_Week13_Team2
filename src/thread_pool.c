#include "server.h"

#include <stdio.h>
#include <stdlib.h>



static struct connection *take_job(struct server *server)
{
    check_thread(server_profile_lock(&server->mutex));

    while (!server->count && !server->stopping) {
        struct server_stamp idle = server_profile_begin();
        check_thread(pthread_cond_wait(&server->ready, &server->mutex));
        server_profile_end(SP_WORKER_IDLE, idle, 0);
    }
    struct server_stamp stamp = server_profile_begin();

    struct connection *client = NULL;
    if (!server->stopping) {
        client = server->jobs[server->head];
        server->head = (server->head + 1) % QUEUE_CAPACITY;
        server->count--;
        client->state = RUNNING;
        server_profile_residence(client->profile_queued_at);
    }

    server_profile_end(SP_QUEUE_TAKE, stamp, client != NULL);
    bool refill = client && server->pending_head;
    check_thread(pthread_mutex_unlock(&server->mutex));
    if (refill) notify_main(server->wake_fd);

    return client;
}

/* 연결 전체 대신 최대 64KiB의 한 차례만 맡고 메인으로 소유권을 돌립니다. */
static void *worker_main(void *argument)
{
    server_profile_worker();
    struct server *server = argument;
    struct connection *client;

    while ((client = take_job(server))) {
        struct server_stamp stamp = server_profile_begin();
        process_turn(server, client);
        server_profile_end(SP_REQUEST_TURN, stamp, 1);

        check_thread(server_profile_lock(&server->mutex));
        stamp = server_profile_begin();
        client->state = client->reason ? FINISHED : WAITING;
        timer_update(server, client);
        server_changed(server, client);
        server_profile_end(SP_PUBLISH, stamp, 1);
        check_thread(pthread_mutex_unlock(&server->mutex));

        stamp = server_profile_begin();
        notify_main(server->wake_fd);
        server_profile_end(SP_NOTIFY, stamp, 1);
    }

    server_profile_flush();
    return NULL;
}

/* Intrusive FIFO, one entry per connection. All operations hold server->mutex. */
void pending_remove(struct server *server, struct connection *client)
{
    if (!client->pending) return;
    if (client->pending_prev) client->pending_prev->pending_next = client->pending_next;
    else server->pending_head = client->pending_next;
    if (client->pending_next) client->pending_next->pending_prev = client->pending_prev;
    else server->pending_tail = client->pending_prev;
    client->pending_prev = client->pending_next = NULL;
    client->pending = false;
}

void dispatch_pending(struct server *server)
{
    while (server->pending_head && server->count < QUEUE_CAPACITY) {
        struct connection *client = server->pending_head;
        pending_remove(server, client);
        timer_cancel(server, client);
        client->profile_queued_at = server_profile_wall();
        server->jobs[server->tail] = client;
        server->tail = (server->tail + 1) % QUEUE_CAPACITY;
        server->count++;
        client->state = QUEUED;
        check_thread(pthread_cond_signal(&server->ready));
    }
}

/* New work never bypasses older pending work, even when a slot just became free. */
void enqueue_turn(struct server *server, struct connection *client)
{
    struct server_stamp stamp = server_profile_begin();
    if (client->state != WAITING) { server_profile_end(SP_QUEUE_ENQUEUE, stamp, 0); return; }
    if (!client->pending) {
        server_io_suspend(client);
        client->pending_prev = server->pending_tail;
        client->pending_next = NULL;
        if (server->pending_tail) server->pending_tail->pending_next = client;
        else server->pending_head = client;
        server->pending_tail = client;
        client->pending = true;
    }
    size_t before = server->count;
    dispatch_pending(server);
    server_profile_end(SP_QUEUE_ENQUEUE, stamp, server->count - before);
}

void pool_start(struct server *server)
{
    for (int i = 0; i < WORKERS; i++)
        check_thread(pthread_create(&server->workers[i], NULL, worker_main, server));
}

/* 신규 작업을 멈추고 현재 차례가 반환된 뒤에만 연결 자원을 닫습니다. */
void pool_stop(struct server *server)
{
    check_thread(server_profile_lock(&server->mutex));
    server->stopping = true;
    check_thread(pthread_cond_broadcast(&server->ready));
    check_thread(pthread_mutex_unlock(&server->mutex));

    for (int i = 0; i < WORKERS; i++)
        check_thread(pthread_join(server->workers[i], NULL));
}
