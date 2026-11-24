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
    server->free_clients = NULL;
    for (int i = server->capacity - 1; i >= 0; i--) {
        server->clients[i].server = server;
        server->clients[i].free_next = server->free_clients;
        server->free_clients = &server->clients[i];
    }
    server->slots_initialized = true;
}

void timer_cancel(struct server *server, struct connection *client)
{
    struct connection **at = &server->timers;
    while (*at && *at != client) at = &(*at)->timer_next;
    if (*at) *at = client->timer_next;
    client->timer_next = NULL;
}

void timer_update(struct server *server, struct connection *client)
{
    timer_cancel(server, client);
    if (client->state != WAITING) return;
    client->expires = connection_deadline(client);
    if (client->phase == CGI_OUTPUT)
        client->expires = client->cgi.next_check < client->cgi.deadline ?
                          client->cgi.next_check : client->cgi.deadline;
    struct connection **at = &server->timers;
    while (*at && (*at)->expires <= client->expires) at = &(*at)->timer_next;
    client->timer_next = *at;
    *at = client;
}

int timer_expire(struct server *server)
{
    int64_t now = now_ms();
    while (server->timers && server->timers->expires <= now) {
        struct connection *client = server->timers;
        timer_cancel(server, client);
        if (client->phase == CGI_OUTPUT) enqueue_turn(server, client);
        else {
            client->reason = timeout_reason(client);
            connection_close(client);
        }
    }
    return server->timers ? (int)(server->timers->expires - now) : -1;
}
