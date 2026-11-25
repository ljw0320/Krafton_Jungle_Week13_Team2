#ifndef PROXY_REACTOR_H
#define PROXY_REACTOR_H
#include <assert.h>

/* Dense poll registry and one indexed heap entry per connection. */
struct reactor_node {
    void *owner;
    int watch[2], heap_index;
    uint64_t life;
    int64_t due;
};
struct watch_owner { struct reactor_node *node; unsigned generation; bool upstream; };
struct ready_event { struct reactor_node *node; uint64_t life; unsigned generation; short events; bool upstream; };
struct reactor {
    struct pollfd *fds;
    struct watch_owner *owners;
    struct ready_event *ready;
    struct reactor_node **heap;
    size_t count, reserved, heap_count, limit;
};
static void reactor_init(struct reactor *r, int capacity, size_t reserved)
{
    memset(r, 0, sizeof(*r));
    r->reserved = r->count = reserved;
    r->limit = reserved + (size_t)capacity * 2;
    r->fds = runtime_alloc(r->limit, sizeof(*r->fds));
    r->owners = runtime_alloc(r->limit, sizeof(*r->owners));
    r->ready = runtime_alloc(r->limit, sizeof(*r->ready));
    r->heap = runtime_alloc((size_t)capacity, sizeof(*r->heap));
    for (size_t i = 0; i < reserved; i++) r->fds[i].fd = -1;
}
static void reactor_destroy(struct reactor *r)
{
    free(r->fds); free(r->owners); free(r->ready); free(r->heap);
}
static void reactor_watch(struct reactor *r, struct reactor_node *n, bool upstream,
                          int fd, short events, unsigned generation)
{
    int index = n->watch[upstream];
    if (fd < 0) {
        if (index < 0) return;
        size_t last = --r->count;
        if ((size_t)index != last) {
            r->fds[index] = r->fds[last]; r->owners[index] = r->owners[last];
            struct watch_owner *moved = &r->owners[index];
            moved->node->watch[moved->upstream] = index;
        }
        n->watch[upstream] = -1;
        return;
    }
    if (index < 0) {
        assert(r->count < r->limit);
        index = (int)r->count++;
        n->watch[upstream] = index;
    }
    r->fds[index] = (struct pollfd){.fd=fd, .events=events};
    r->owners[index] = (struct watch_owner){n, generation, upstream};
}
static void heap_swap(struct reactor *r, size_t a, size_t b)
{
    struct reactor_node *n = r->heap[a]; r->heap[a] = r->heap[b]; r->heap[b] = n;
    r->heap[a]->heap_index = (int)a; r->heap[b]->heap_index = (int)b;
}
static size_t heap_up(struct reactor *r, size_t i)
{
    while (i && r->heap[(i-1)/2]->due > r->heap[i]->due) {
        size_t parent = (i-1)/2; heap_swap(r, i, parent); i = parent;
    }
    return i;
}
static void heap_down(struct reactor *r, size_t i)
{
    for (;;) {
        size_t child = i*2+1;
        if (child >= r->heap_count) return;
        if (child+1 < r->heap_count && r->heap[child+1]->due < r->heap[child]->due) child++;
        if (r->heap[i]->due <= r->heap[child]->due) return;
        heap_swap(r, i, child); i = child;
    }
}
static void reactor_timer(struct reactor *r, struct reactor_node *n, int64_t due)
{
    int index = n->heap_index;
    if (due < 0) {
        if (index < 0) return;
        size_t last = --r->heap_count;
        n->heap_index = -1;
        if ((size_t)index != last) {
            r->heap[index] = r->heap[last]; r->heap[index]->heap_index = index;
            heap_down(r, heap_up(r, (size_t)index));
        }
        return;
    }
    n->due = due;
    if (index < 0) {
        index = (int)r->heap_count++;
        r->heap[index] = n; n->heap_index = index;
    }
    heap_down(r, heap_up(r, (size_t)index));
}
/* Snapshot before callbacks: swap-removal or fd reuse cannot rewrite old events. */
static size_t reactor_collect(struct reactor *r, int ready, uint64_t *checked)
{
    size_t count = 0;
    for (size_t i = 0; i < r->count && ready > 0; i++) {
        if (checked) (*checked)++;
        short events = r->fds[i].revents;
        if (!events) continue;
        ready--;
        if (i < r->reserved) continue;
        struct watch_owner *w = &r->owners[i];
        r->ready[count++] = (struct ready_event){w->node, w->node->life, w->generation, events, w->upstream};
    }
    return count;
}
#endif
