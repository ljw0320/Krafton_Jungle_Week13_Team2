#define main proxy_program_main
#include "../main.c"
#undef main
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
int main(void)
{
    pool = runtime_alloc(PROXY_ACTIVE, sizeof(*pool));
    clients = runtime_alloc(PROXY_CLIENTS, sizeof(*clients));
    struct exchange c = {.client = -1, .upstream = -1, .phase = RELAY,
                         .keep_alive = true, .requests = 7};
    CHECK(acquire_buffers(&c));
    c.first = now_ms() - TOTAL_MS + 1;
    finish(&c);
    CHECK(c.phase == IDLE && !c.buffers && !c.first && c.requests == 7);
    int64_t start = now_ms();
    c.first = start; c.phase = REQUEST;
    c.client_progress = start + TOTAL_MS; /* isolate the absolute limit */
    CHECK(deadline(&c) == start + TOTAL_MS);
    struct exchange active[PROXY_ACTIVE + 1] = {0};
    for (int i = 0; i < PROXY_ACTIVE; i++) CHECK(acquire_buffers(&active[i]));
    CHECK(!acquire_buffers(&active[PROXY_ACTIVE]));
    release_buffers(&active[0]);
    CHECK(acquire_buffers(&active[PROXY_ACTIVE]));
    for (int i = 0; i <= PROXY_ACTIVE; i++) {
        release_buffers(&active[i]); release_buffers(&active[i]);
    }
    for (int i = 0; i < PROXY_ACTIVE; i++) CHECK(!pool[i].used);
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    struct exchange upstream = {.client = -1, .upstream = pair[0], .keep_alive = true,
        .backend_reusable = true, .backend_requests = BACKEND_KEEP_MAX - 1};
    backend_complete(&upstream);
    CHECK(upstream.upstream == pair[0] && upstream.backend_idle_at > 0);
    upstream.backend_requests++;
    backend_complete(&upstream);
    CHECK(upstream.upstream == -1);
    close(pair[1]);
    for (int get = 0; get <= 1; get++) {
        struct exchange failed = {.client = -1, .upstream = -1, .phase = RESPONSE,
            .first = now_ms() - 500, .reused_attempt = true, .is_get = get};
        int64_t original_first = failed.first;
        backend_failed(&failed);
        CHECK(failed.retried == (bool)get);
        CHECK(failed.first == original_first);
        dispose(&failed);
    }
    CHECK(proxy_backend_reusable("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
        strlen("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")));
    CHECK(!proxy_backend_reusable("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n",
        strlen("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n")));
    CHECK(!proxy_backend_reusable("HTTP/1.1 200 OK\r\nConnection: Keep-Alive, ClOsE\r\n\r\n",
        strlen("HTTP/1.1 200 OK\r\nConnection: Keep-Alive, ClOsE\r\n\r\n")));
    puts("proxy state: idle release, per-request deadline reset, pool cap/reuse passed");
    free(pool); free(clients);
    return 0;
}
