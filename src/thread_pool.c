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
    check_thread(pthread_mutex_unlock(&server->mutex));

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
        server_profile_end(SP_PUBLISH, stamp, 1);
        check_thread(pthread_mutex_unlock(&server->mutex));

        stamp = server_profile_begin();
        notify_main(server->wake_fd);
        server_profile_end(SP_NOTIFY, stamp, 1);
    }

    server_profile_flush();
    return NULL;
}

/* 메인이 mutex를 보유하고 WAITING 연결만 큐에 넣습니다. */
void enqueue_turn(struct server *server, struct connection *client)
{
    struct server_stamp stamp = server_profile_begin();
    if (client->state != WAITING) { server_profile_end(SP_QUEUE_ENQUEUE, stamp, 0); return; }
    if (server->count == QUEUE_CAPACITY) {
        client->pending = true;
        server_profile_end(SP_QUEUE_ENQUEUE, stamp, 0);
        return;
    }
    client->pending = false;
    timer_cancel(server, client);

    client->profile_queued_at = server_profile_wall();
    server->jobs[server->tail] = client;
    server->tail = (server->tail + 1) % QUEUE_CAPACITY;
    server->count++;
    client->state = QUEUED;

    check_thread(pthread_cond_signal(&server->ready));
    server_profile_end(SP_QUEUE_ENQUEUE, stamp, 1);
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
