#include "test_support.h"
#include "../server.c"
#include <string.h>
static struct server server;
int main(void)
{
    atomic_uint resources = 0;
    for (int i = 0; i < FILE_CAPACITY; i++) CHECK(resource_acquire(&resources, FILE_CAPACITY));
    CHECK(!resource_acquire(&resources, FILE_CAPACITY));
    atomic_fetch_sub(&resources, 1);
    CHECK(resource_acquire(&resources, FILE_CAPACITY));
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
    CHECK(server.timer_count == 2 && server.timer_heap[0] == b && a->timer_position == 2);
    b->progress += 200; timer_update(&server, b);
    CHECK(server.timer_count == 2 && server.timer_heap[0] == a && b->timer_position == 2);
    timer_cancel(&server, a); CHECK(server.timer_count == 1 && server.timer_heap[0] == b);
    a->progress = b->progress = now_ms() - RECV_TIMEOUT_MS - 1;
    timer_update(&server, a); timer_update(&server, b);
    timer_expire(&server);
    CHECK(a->state == FREE && b->state == FREE && server.timer_count == 0);
    CHECK(server.free_clients == b);
    for (int i = 2; i < MAX_CONNECTIONS; i++) connection_close(&server.clients[i]);
    CHECK(server.contexts.wait_count == 0);
    a->resource_server = &server; a->file_fd = -1; a->file_reserved = true;
    atomic_store(&server.active_files, 1);
    file_release(a); file_release(a);
    CHECK(atomic_load(&server.active_files) == 0);
    a->cgi_reserved = true; a->cgi.pid = 0;
    atomic_store(&server.active_cgi, 1);
    cgi_close(a); cgi_close(a);
    CHECK(atomic_load(&server.active_cgi) == 0);
    puts("scheduler: slots, saturation, retry, duplicate, timer order/update/cancel/expiry passed");
    context_pool_destroy(&server.contexts);
    free(server.timer_heap); free(server.clients);
    return 0;
}
