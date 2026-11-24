#include "server.h"

#include <stdio.h>
#include <stdlib.h>



static struct connection *take_job(struct server *server)
{
    check_thread(pthread_mutex_lock(&server->mutex));

    while (!server->count && !server->stopping)
        check_thread(pthread_cond_wait(&server->ready, &server->mutex));

    struct connection *client = NULL;
    if (!server->stopping) {
        client = server->jobs[server->head];
        server->head = (server->head + 1) % QUEUE_CAPACITY;
        server->count--;
        client->state = RUNNING;
    }

    check_thread(pthread_mutex_unlock(&server->mutex));

    return client;
}

/* 연결 전체 대신 최대 64KiB의 한 차례만 맡고 메인으로 소유권을 돌립니다. */
static void *worker_main(void *argument)
{
    struct server *server = argument;
    struct connection *client;

    while ((client = take_job(server))) {
        process_turn(server, client);

        check_thread(pthread_mutex_lock(&server->mutex));
        client->state = client->reason ? FINISHED : WAITING;
        timer_update(server, client);
        check_thread(pthread_mutex_unlock(&server->mutex));

        notify_main(server->wake_fd);
    }

    return NULL;
}

/* 메인이 mutex를 보유하고 WAITING 연결만 큐에 넣습니다. */
void enqueue_turn(struct server *server, struct connection *client)
{
    if (client->state != WAITING)
        return;
    if (server->count == QUEUE_CAPACITY) {
        client->pending = true;
        return;
    }
    client->pending = false;
    timer_cancel(server, client);

    server->jobs[server->tail] = client;
    server->tail = (server->tail + 1) % QUEUE_CAPACITY;
    server->count++;
    client->state = QUEUED;

    check_thread(pthread_cond_signal(&server->ready));
}

void pool_start(struct server *server)
{
    for (int i = 0; i < WORKERS; i++)
        check_thread(pthread_create(&server->workers[i], NULL, worker_main, server));
}

/* 신규 작업을 멈추고 현재 차례가 반환된 뒤에만 연결 자원을 닫습니다. */
void pool_stop(struct server *server)
{
    check_thread(pthread_mutex_lock(&server->mutex));
    server->stopping = true;
    check_thread(pthread_cond_broadcast(&server->ready));
    check_thread(pthread_mutex_unlock(&server->mutex));

    for (int i = 0; i < WORKERS; i++)
        check_thread(pthread_join(server->workers[i], NULL));
}
