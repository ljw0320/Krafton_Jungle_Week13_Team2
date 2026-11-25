#include "test_support.h"
#include "../server.c"
#include <limits.h>

static void verify(struct server *server, const bool *present)
{
    size_t count = 0;
    int64_t minimum = INT64_MAX;
    for (int i = 0; i < server->capacity; i++) {
        struct connection *c = &server->clients[i];
        CHECK((c->timer_position != 0) == present[i]);
        if (present[i]) {
            count++;
            CHECK(c->timer_position <= server->timer_count);
            CHECK(server->timer_heap[c->timer_position - 1] == c);
            if (c->expires < minimum) minimum = c->expires;
        }
    }
    CHECK(count == server->timer_count);
    if (count) CHECK(server->timer_heap[0]->expires == minimum);
    for (size_t i = 1; i < count; i++)
        CHECK(server->timer_heap[(i - 1) / 2]->expires <= server->timer_heap[i]->expires);
    for (size_t i = count; i < (size_t)server->capacity; i++) CHECK(!server->timer_heap[i]);
}

int main(void)
{
    struct server server = {0};
    slots_init(&server);
    bool *present = calloc((size_t)server.capacity, sizeof(*present));
    CHECK(present);
    uint32_t random = 12345;
    /* Arbitrary updates in both directions, ties, duplicate insertion/cancellation. */
    for (int step = 0; step < 10000; step++) {
        random = random * 1664525u + 1013904223u;
        int index = (int)((random >> 8) % (unsigned)server.capacity);
        struct connection *c = &server.clients[index];
        if (random & 1) {
            c->state = WAITING; c->phase = READ_HEADER;
            c->progress = 100000 + (random % 17);
            timer_update(&server, c); timer_update(&server, c);
            present[index] = true;
        } else {
            timer_cancel(&server, c); timer_cancel(&server, c);
            present[index] = false;
        }
        verify(&server, present);
    }
    for (int i = 0; i < server.capacity; i++) timer_cancel(&server, &server.clients[i]);
    /* Fill to capacity and cancel a root, middle and tail; position must invalidate. */
    for (int i = 0; i < server.capacity; i++) {
        server.clients[i].state = WAITING;
        server.clients[i].progress = 100000 + i;
        timer_update(&server, &server.clients[i]); present[i] = true;
    }
    while (server.timer_count) {
        size_t at = server.timer_count / 2;
        struct connection *c = server.timer_heap[at];
        c->state = RUNNING; timer_update(&server, c);
        present[c - server.clients] = false;
        verify(&server, present);
    }
    /* Exact deadline selection, including fixed buffer/drain and CGI recheck. */
    struct connection *c = &server.clients[0];
    c->state = WAITING; c->phase = READ_HEADER; c->requests = 1; c->progress = now_ms();
    timer_update(&server, c); CHECK(c->expires == c->progress + KEEP_ALIVE_TIMEOUT_MS);
    c->context_wait_deadline = c->progress + 123;
    timer_update(&server, c); CHECK(c->expires == c->context_wait_deadline);
    c->context_wait_deadline = 0; c->phase = DRAIN_INPUT; c->drain_deadline = c->progress + 222;
    timer_update(&server, c); CHECK(c->expires == c->drain_deadline);
    c->phase = WRITE_RESPONSE; timer_update(&server, c);
    CHECK(c->expires == c->progress + SEND_TIMEOUT_MS);
    c->phase = CGI_OUTPUT; c->cgi.next_check = c->progress + 333; c->cgi.deadline = c->progress + 444;
    timer_update(&server, c); CHECK(c->expires == c->cgi.next_check);
    c->cgi.deadline = c->progress + 111;
    timer_update(&server, c); CHECK(c->expires == c->cgi.deadline);
    timer_cancel(&server, c);
    free(present); free(server.timer_heap); free(server.clients);
    puts("timer heap: 10000 randomized updates/cancels, indices, ties, capacity, deadline policies passed");
    return 0;
}
