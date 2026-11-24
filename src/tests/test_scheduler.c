#include "test_support.h"
#include "../server.c"
#include <string.h>
static struct server server;
int main(void)
{
    slots_init(&server);
    context_pool_init(&server.contexts);
    check_thread(pthread_mutex_init(&server.mutex, NULL));
    check_thread(pthread_cond_init(&server.ready, NULL));
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        int pair[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        register_client(&server, pair[0]);
        close(pair[1]);
    }
    CHECK(server.free_clients == NULL);
    struct connection *a = &server.clients[0], *b = &server.clients[1];
    for (int i = 0; i < MAX_CONNECTIONS; i++) enqueue_turn(&server, &server.clients[i]);
    CHECK(server.count == QUEUE_CAPACITY);
    CHECK(server.clients[QUEUE_CAPACITY].pending);
    enqueue_turn(&server, a);
    CHECK(server.count == QUEUE_CAPACITY);
    /* Simulate a dequeue under the scheduler lock; retry the deferred job. */
    server.count--; server.head = (server.head + 1) % QUEUE_CAPACITY;
    a->state = WAITING;
    enqueue_turn(&server, &server.clients[QUEUE_CAPACITY]);
    CHECK(!server.clients[QUEUE_CAPACITY].pending);
    for (int i = 0; i < MAX_CONNECTIONS; i++) timer_cancel(&server, &server.clients[i]);
    a->state = b->state = WAITING;
    a->progress = now_ms(); b->progress = a->progress - 100;
    timer_update(&server, a); timer_update(&server, b);
    CHECK(server.timers == b && b->timer_next == a);
    b->progress += 200; timer_update(&server, b);
    CHECK(server.timers == a && a->timer_next == b);
    timer_cancel(&server, a); CHECK(server.timers == b);
    a->progress = b->progress = now_ms() - RECV_TIMEOUT_MS - 1;
    timer_update(&server, a); timer_update(&server, b);
    timer_expire(&server);
    CHECK(a->state == FREE && b->state == FREE && server.timers == NULL);
    CHECK(server.free_clients == b);
    for (int i = 2; i < MAX_CONNECTIONS; i++) connection_close(&server.clients[i]);
    CHECK(server.contexts.wait_count == 0);
    puts("scheduler: slots, saturation, retry, duplicate, timer order/update/cancel/expiry passed");
    return 0;
}
