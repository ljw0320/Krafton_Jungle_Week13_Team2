#define main proxy_program_main
#include "../main.c"
#undef main
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

enum { TEST_CLIENTS = 6 };
static int peers[TEST_CLIENTS];

static void setup(void)
{
    active_capacity = 1; client_capacity = TEST_CLIENTS;
    pool = runtime_alloc(1, sizeof(*pool));
    clients = runtime_alloc(TEST_CLIENTS, sizeof(*clients));
    nodes = runtime_alloc(TEST_CLIENTS, sizeof(*nodes));
    buffer_wait_head = buffer_wait_tail = free_clients = NULL;
    buffer_wait_count = 0;
    CHECK(reactor_init(&reactor, TEST_CLIENTS, 0));
    for (int i = 0; i < TEST_CLIENTS; i++) {
        int pair[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
        peers[i] = pair[1];
        clients[i] = (struct exchange){.node=&nodes[i], .client=pair[0],
            .upstream=-1, .phase=IDLE, .client_progress=now_ms()};
        nodes[i] = (struct reactor_node){.owner=&clients[i], .watch={-1,-1},
            .heap_index=-1, .life=1};
        refresh_exchange(&clients[i]);
    }
    CHECK(acquire_buffers(&clients[0]));
    clients[0].phase = REQUEST;
    clients[0].first = now_ms();
    refresh_exchange(&clients[0]);
}

static void cleanup(void)
{
    for (int i = 0; i < TEST_CLIENTS; i++) {
        dispose(&clients[i]); refresh_exchange(&clients[i]);
        if (peers[i] >= 0) close(peers[i]);
    }
    CHECK(!buffer_wait_head && !buffer_wait_tail && !buffer_wait_count);
    CHECK(!pool[0].used && !reactor.heap_count && !reactor.count);
    reactor_destroy(&reactor);
    free(pool); free(clients); free(nodes);
}

static void enqueue(int i)
{
    const char partial[] = "POST /echo HTTP/1.1\r\n";
    CHECK(write(peers[i], partial, sizeof(partial)-1) == (ssize_t)sizeof(partial)-1);
    dispatch(&clients[i], POLLIN, 0, 0);
    refresh_exchange(&clients[i]);
    CHECK(clients[i].phase == BUFFER_WAIT && !clients[i].buffers);
    CHECK(clients[i].request_used == 0);
    CHECK(reactor.fds[nodes[i].watch[0]].events == 0);
    char bytes[sizeof(partial)];
    CHECK(recv(clients[i].client, bytes, sizeof(bytes), MSG_PEEK) == (ssize_t)sizeof(partial)-1);
}

static void test_fifo_and_cancel(void)
{
    setup();
    enqueue(1); enqueue(2); enqueue(3);
    int64_t until = clients[1].buffer_wait_until, first = clients[1].first;
    dispatch(&clients[1], POLLIN, 0, 0); /* even stale readiness cannot enqueue twice */
    CHECK(buffer_wait_count == 3 && clients[1].buffer_wait_until == until);
    CHECK(reactor_wait(&reactor, 0) == 0); /* unread bytes do not spin while paused */
    dispose(&clients[2]); refresh_exchange(&clients[2]);
    CHECK(buffer_wait_count == 2 && clients[1].wait_next == &clients[3]);
    CHECK(!clients[2].wait_prev && !clients[2].wait_next && !clients[2].buffer_wait_until);
    release_buffers(&clients[0]);
    CHECK(!acquire_buffers(&clients[4])); /* free buffer cannot bypass the queue */
    CHECK(!resume_buffer_waiters());
    CHECK(clients[1].buffers && clients[1].phase == REQUEST && clients[1].first == first);
    CHECK(buffer_wait_head == &clients[3] && buffer_wait_count == 1);
    clients[1].client_progress = first + TOTAL_MS;
    CHECK(deadline(&clients[1]) == first + TOTAL_MS); /* wait is part of total time */
    dispose(&clients[1]); refresh_exchange(&clients[1]);
    resume_buffer_waiters();
    CHECK(clients[3].buffers && !buffer_wait_count);
    cleanup();
}

static void test_expiry_before_grant(void)
{
    setup(); enqueue(1); enqueue(2);
    int64_t first = clients[1].first;
    CHECK(deadline(&clients[1]) == clients[1].buffer_wait_until);
    clients[1].first = now_ms() - TOTAL_MS + 20;
    CHECK(deadline(&clients[1]) == clients[1].first + TOTAL_MS);
    clients[1].first = first;
    clients[1].buffer_wait_until = now_ms() - 1;
    refresh_exchange(&clients[1]);
    release_buffers(&clients[0]);
    resume_buffer_waiters();
    CHECK(clients[1].phase == ERROR_REPLY && !clients[1].buffers);
    CHECK(!clients[1].buffer_wait_until && clients[1].first == first);
    CHECK(clients[2].buffers && !buffer_wait_count);
    send_response(&clients[1]);
    char response[128] = {0};
    CHECK(recv(peers[1], response, sizeof(response)-1, 0) > 0);
    CHECK(strstr(response, "HTTP/1.1 503 Service Unavailable\r\n"));
    cleanup();
}

static void test_timer_and_hup_cleanup(void)
{
    setup(); enqueue(1);
    clients[1].buffer_wait_until = now_ms() - 1;
    refresh_exchange(&clients[1]);
    prepare_timers();
    CHECK(clients[1].phase == ERROR_REPLY && !buffer_wait_count);
    dispose(&clients[1]); refresh_exchange(&clients[1]);
    enqueue(2);
    close(peers[2]); peers[2] = -1;
    int ready = reactor_wait(&reactor, 100);
    CHECK(ready > 0);
    size_t count = reactor_collect(&reactor, ready, NULL);
    for (size_t i = 0; i < count; i++) {
        struct ready_event e = reactor.ready[i];
        struct exchange *c = e.node->owner;
        dispatch(c, e.events, 0, e.generation);
        refresh_exchange(c);
    }
    CHECK(clients[2].phase == UNUSED && !buffer_wait_count);
    cleanup();
}

int main(void)
{
    test_fifo_and_cancel();
    test_expiry_before_grant();
    test_timer_and_hup_cleanup();
    puts("proxy buffer wait: FIFO, pause, fixed deadline, 503, total budget, cancellation and HUP passed");
    return 0;
}
