#include "proxy.h"
#include <assert.h>
#include <poll.h>
#include <string.h>
#include "reactor.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
int main(void)
{
    struct reactor r;
    struct reactor_node nodes[64];
    reactor_init(&r, 64, 1);
    for (int i = 0; i < 64; i++) nodes[i] = (struct reactor_node){.watch={-1,-1}, .heap_index=-1, .life=1};
    reactor_watch(&r, &nodes[0], false, 100, POLLIN, 7);
    reactor_watch(&r, &nodes[1], true, 101, POLLOUT, 8);
    reactor_watch(&r, &nodes[0], false, 100, POLLOUT, 9);
    CHECK(r.count == 3 && r.fds[nodes[0].watch[0]].events == POLLOUT);
    r.fds[2].revents = POLLOUT;
    CHECK(reactor_collect(&r, 1, NULL) == 1);
    struct ready_event old = r.ready[0];
    reactor_watch(&r, &nodes[0], false, -1, 0, 0);
    CHECK(r.count == 2 && nodes[1].watch[1] == 1);
    CHECK(old.node == &nodes[1] && old.generation == 8);
    nodes[1].life++;
    reactor_watch(&r, &nodes[1], true, 101, POLLIN, 10);
    CHECK(old.life != old.node->life && old.generation == 8);
    reactor_watch(&r, &nodes[1], true, -1, 0, 0);
    reactor_watch(&r, &nodes[1], true, -1, 0, 0);
    CHECK(r.count == 1);
    unsigned seed = 31;
    for (int step = 0; step < 3000; step++) {
        seed = seed*1664525u+1013904223u;
        int index = (int)(seed % 64);
        int64_t due = step % 4 ? (int64_t)((seed >> 8) % 10000) : -1;
        reactor_timer(&r, &nodes[index], due);
        int expected = 0;
        int64_t minimum = INT64_MAX;
        for (int i = 0; i < 64; i++) if (nodes[i].heap_index >= 0) {
            CHECK(r.heap[nodes[i].heap_index] == &nodes[i]);
            expected++; if (nodes[i].due < minimum) minimum = nodes[i].due;
        }
        CHECK(r.heap_count == (size_t)expected);
        if (expected) CHECK(r.heap[0]->due == minimum);
        for (size_t i = 1; i < r.heap_count; i++) CHECK(r.heap[(i-1)/2]->due <= r.heap[i]->due);
    }
    while (r.heap_count) reactor_timer(&r, r.heap[0], -1);
    CHECK(r.heap_count == 0);
    reactor_destroy(&r);
    puts("reactor: watch update/swap removal, immutable stale snapshots, heap update/cancel/order passed");
    return 0;
}
