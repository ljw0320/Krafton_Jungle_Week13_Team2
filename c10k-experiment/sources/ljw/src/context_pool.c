#include "server.h"

#include <string.h>

void context_pool_init(struct context_pool *pool)
{
    memset(pool, 0, sizeof(*pool));
    check_thread(pthread_mutex_init(&pool->mutex, NULL));
}

void context_pool_destroy(struct context_pool *pool)
{
    check_thread(pthread_mutex_destroy(&pool->mutex));
}

/* pool mutex 아래 등록 순서가 FIFO 순서입니다. 신규 연결도 선두를 추월하지 못합니다. */
static void remove_waiter(struct context_pool *pool, struct connection *client)
{
    for (size_t i = 0; i < pool->wait_count; i++) {
        if (pool->waiters[i] != client)
            continue;
        memmove(&pool->waiters[i], &pool->waiters[i + 1],
                (--pool->wait_count - i) * sizeof(pool->waiters[0]));
        break;
    }
    client->context_wait_deadline = 0;
}

bool context_acquire(struct connection *client)
{
    if (client->context)
        return true;
    struct context_pool *pool = client->context_pool;
    check_thread(pthread_mutex_lock(&pool->mutex));
    if (!pool->wait_count || pool->waiters[0] == client) {
        for (int i = 0; i < CONTEXT_CAPACITY; i++) {
            if (pool->owners[i])
                continue;
            pool->owners[i] = client;
            client->context = &pool->entries[i];
            remove_waiter(pool, client);
            break;
        }
    }
    if (!client->context && !client->context_wait_deadline) {
        if (pool->wait_count == MAX_CONNECTIONS)
            client->reason = "context_wait_limit";
        else {
            pool->waiters[pool->wait_count++] = client;
            client->context_wait_deadline = now_ms() + CONTEXT_WAIT_TIMEOUT_MS;
        }
    }
    check_thread(pthread_mutex_unlock(&pool->mutex));
    if (client->context)
        memset(client->context, 0, sizeof(*client->context));
    return client->context != NULL;
}

/* 같은 연결의 중복 반환은 아무 일도 하지 않으며 데이터는 재확보 시 초기화합니다. */
void context_release(struct connection *client)
{
    struct context_pool *pool = client->context_pool;
    check_thread(pthread_mutex_lock(&pool->mutex));
    remove_waiter(pool, client);
    for (int i = 0; i < CONTEXT_CAPACITY; i++) {
        if (pool->owners[i] != client)
            continue;
        pool->owners[i] = NULL;
        client->context = NULL;
        break;
    }
    check_thread(pthread_mutex_unlock(&pool->mutex));
}
