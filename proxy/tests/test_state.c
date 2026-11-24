#define main proxy_program_main
#include "../main.c"
#undef main
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
int main(void)
{
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
    puts("proxy state: idle release, per-request deadline reset, pool cap/reuse passed");
    return 0;
}
