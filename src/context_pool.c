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

/* 소유권만 잠금 안에서 예약하고 큰 버퍼 초기화는 잠금 밖에서 수행합니다. */
bool context_acquire(struct connection *client)
{
    if (client->context)
        return true;
    struct context_pool *pool = client->context_pool;
    check_thread(pthread_mutex_lock(&pool->mutex));
    for (int i = 0; i < CONTEXT_CAPACITY; i++) {
        if (pool->owners[i])
            continue;
        pool->owners[i] = client;
        client->context = &pool->entries[i];
        break;
    }
    check_thread(pthread_mutex_unlock(&pool->mutex));
    if (client->context)
        memset(client->context, 0, sizeof(*client->context));
    return client->context != NULL;
}

/* 같은 연결의 중복 반환은 아무 일도 하지 않으며 데이터는 재확보 시 초기화합니다. */
void context_release(struct connection *client)
{
    if (!client->context)
        return;
    struct context_pool *pool = client->context_pool;
    check_thread(pthread_mutex_lock(&pool->mutex));
    for (int i = 0; i < CONTEXT_CAPACITY; i++) {
        if (pool->owners[i] != client)
            continue;
        pool->owners[i] = NULL;
        client->context = NULL;
        break;
    }
    check_thread(pthread_mutex_unlock(&pool->mutex));
}
