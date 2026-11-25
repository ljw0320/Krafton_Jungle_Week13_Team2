#include "server.h"

/* 정리 단계는 마지막 수신 시각 대신 최초에 정한 절대 마감 시각을 사용합니다. */
static int64_t connection_deadline(const struct connection *client)
{
    if (client->context_wait_deadline)
        return client->context_wait_deadline;
    if (client->phase == DRAIN_INPUT)
        return client->drain_deadline;
    if (connection_idle(client))
        return client->progress + KEEP_ALIVE_TIMEOUT_MS;

    int limit = client->phase == WRITE_RESPONSE ? SEND_TIMEOUT_MS : RECV_TIMEOUT_MS;
    return client->progress + limit;
}

static const char *timeout_reason(const struct connection *client)
{
    if (client->context_wait_deadline)
        return "context_wait_timeout";
    if (client->phase == DRAIN_INPUT)
        return "drain_timeout";
    if (connection_idle(client))
        return "keep_alive_timeout";

    return client->phase == WRITE_RESPONSE ? "send_timeout" : "recv_timeout";
}


void slots_init(struct server *server)
{
    server->capacity = runtime_limit("JUNGLE_CONNECTIONS", MAX_CONNECTIONS, 65536);
    server->clients = runtime_alloc((size_t)server->capacity, sizeof(*server->clients));
    server->timer_heap = runtime_alloc((size_t)server->capacity, sizeof(*server->timer_heap));
    server->timer_count = 0;
    server->free_clients = NULL;
    for (int i = server->capacity - 1; i >= 0; i--) {
        server->clients[i].server = server;
        server->clients[i].free_next = server->free_clients;
        server->free_clients = &server->clients[i];
    }
    server->slots_initialized = true;
}

/* Callers hold server->mutex. Heap positions are 1-based to survive memset(0).
 * Comparisons are counted for profiling; equal deadlines need no stable order. */
static bool timer_less(struct server *server, size_t a, size_t b, uint64_t *visits)
{
    (*visits)++;
    return server->timer_heap[a]->expires < server->timer_heap[b]->expires;
}

static void timer_swap(struct server *server, size_t a, size_t b)
{
    struct connection *client = server->timer_heap[a];
    server->timer_heap[a] = server->timer_heap[b];
    server->timer_heap[b] = client;
    server->timer_heap[a]->timer_position = a + 1;
    server->timer_heap[b]->timer_position = b + 1;
}

static void timer_fix(struct server *server, size_t at, uint64_t *visits)
{
    if (at && timer_less(server, at, (at - 1) / 2, visits)) {
        do {
            size_t parent = (at - 1) / 2;
            if (!timer_less(server, at, parent, visits)) break;
            timer_swap(server, at, parent);
            at = parent;
        } while (at);
    } else {
        while (at * 2 + 1 < server->timer_count) {
            size_t child = at * 2 + 1;
            if (child + 1 < server->timer_count && timer_less(server, child + 1, child, visits)) child++;
            if (!timer_less(server, child, at, visits)) break;
            timer_swap(server, at, child);
            at = child;
        }
    }
}

void timer_cancel(struct server *server, struct connection *client)
{
    struct server_stamp stamp = server_profile_begin();
    uint64_t visits = 0;
    if (client->timer_position) {
        size_t at = client->timer_position - 1;
        size_t last = --server->timer_count;
        client->timer_position = 0;
        if (at != last) {
            server->timer_heap[at] = server->timer_heap[last];
            server->timer_heap[at]->timer_position = at + 1;
        }
        server->timer_heap[last] = NULL;
        if (at != last) timer_fix(server, at, &visits);
    }
    server_profile_end(SP_TIMER_CANCEL, stamp, visits);
}

void timer_update(struct server *server, struct connection *client)
{
    struct server_stamp stamp = server_profile_begin();
    if (client->state != WAITING) {
        timer_cancel(server, client);
        server_profile_end(SP_TIMER_UPDATE, stamp, 0);
        return;
    }
    client->expires = connection_deadline(client);
    if (client->phase == CGI_OUTPUT)
        client->expires = client->cgi.next_check < client->cgi.deadline ?
                          client->cgi.next_check : client->cgi.deadline;
    if (!client->timer_position) {
        /* Only server-owned slots can register, at most one timer per slot. */
        if (server->timer_count >= (size_t)server->capacity) {
            fprintf(stderr, "timer heap capacity invariant failed\n");
            abort();
        }
        server->timer_heap[server->timer_count++] = client;
        client->timer_position = server->timer_count;
    }
    uint64_t visits = 0;
    timer_fix(server, client->timer_position - 1, &visits);
    server_profile_end(SP_TIMER_UPDATE, stamp, visits);
}

int timer_expire(struct server *server)
{
    struct server_stamp stamp = server_profile_begin();
    uint64_t expired = 0;
    int64_t now = now_ms();
    while (server->timer_count && server->timer_heap[0]->expires <= now) {
        expired++;
        struct connection *client = server->timer_heap[0];
        timer_cancel(server, client);
        if (client->phase == CGI_OUTPUT) enqueue_turn(server, client);
        else {
            client->reason = timeout_reason(client);
            connection_close(client);
        }
    }
    server_profile_end(SP_TIMER_EXPIRE, stamp, expired);
    return server->timer_count ? (int)(server->timer_heap[0]->expires - now) : -1;
}
