#ifndef CONTEXT_FIXTURE_H
#define CONTEXT_FIXTURE_H

/* 실제 정적 함수는 테스트 번역 단위에만 포함합니다. 운영 훅은 없습니다. */
#include "../server.c"
#include "../connection.c"
#include "test_support.h"
#include <fcntl.h>

static struct server fixture_server;

struct test_client {
    struct connection *connection;
    int peer;
};

static inline void fixture_start(void)
{
    memset(&fixture_server, 0, sizeof(fixture_server));
    context_pool_init(&fixture_server.contexts);
}

static inline void fixture_finish(void)
{
    expect_count("owners after cleanup", used_contexts(&fixture_server.contexts), 0);
    context_pool_destroy(&fixture_server.contexts);
    free(fixture_server.clients);
}

static inline struct test_client open_client(void)
{
    int sockets[2];
    int status = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
    CHECK(status == 0);
    status = nonblocking(sockets[1]);
    CHECK(status == 0);
    register_client(&fixture_server, sockets[0]);
    for (int i = 0; i < MAX_CONNECTIONS; i++)
        if (fixture_server.clients[i].fd == sockets[0] && fixture_server.clients[i].state != FREE)
            return (struct test_client){&fixture_server.clients[i], sockets[1]};
    CHECK(false);
    return (struct test_client){NULL, -1};
}

static inline void close_client(struct test_client *client)
{
    if (client->connection->state != FREE) connection_close(client->connection);
    if (client->peer >= 0) close(client->peer);
    client->peer = -1;
}

static inline void send_text(struct test_client *client, const char *text)
{
    size_t size = strlen(text);
    ssize_t sent = send(client->peer, text, size, 0);
    CHECK(sent == (ssize_t)size);
}

struct observed_response {
    char bytes[BODY_LIMIT + 2048];
    size_t length;
};

static inline void collect_response(struct test_client *client, struct observed_response *response)
{
    ssize_t received = recv(client->peer, response->bytes + response->length,
                            sizeof(response->bytes) - response->length, 0);
    if (received > 0) response->length += (size_t)received;
    else CHECK(received == -1 && would_block());
}

static inline void expect_body(const struct observed_response *response, const void *body, size_t size)
{
    size_t end = http_header_end(response->bytes, response->length);
    CHECK(end > 0);
    expect_count("response body bytes", response->length - end, size);
    CHECK(memcmp(response->bytes + end, body, size) == 0);
}

#endif
