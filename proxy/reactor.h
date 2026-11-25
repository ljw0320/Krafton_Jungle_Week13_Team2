#ifndef PROXY_REACTOR_H
#define PROXY_REACTOR_H
#include <assert.h>

#include <sys/epoll.h>
#include <errno.h>
#include <unistd.h>

enum { REACTOR_BATCH = 64 };
struct reactor_node {
    void *owner;
    int watch[2], heap_index;
    uint64_t life;
    int64_t due;
};
struct watch_owner { struct reactor_node *node; unsigned generation; bool upstream; };
struct ready_event { struct reactor_node *node; uint64_t life; unsigned generation; short events; bool upstream; };
struct reactor {
    int epoll_fd, free_watch;
    struct pollfd *fds; /* interest metadata, never passed to poll() */
    struct watch_owner *owners;
    uint32_t *tickets, serial;
    int *next_free;
    struct ready_event ready[REACTOR_BATCH];
    struct epoll_event events[REACTOR_BATCH];
    struct reactor_node **heap;
    size_t count, reserved, heap_count, limit;
};
static bool reactor_init(struct reactor *r, int capacity, size_t reserved)
{
    memset(r, 0, sizeof(*r));
    r->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (r->epoll_fd < 0) return false;
    r->reserved = reserved;
    r->limit = reserved + (size_t)capacity * 2;
    r->fds = runtime_alloc(r->limit, sizeof(*r->fds));
    r->owners = runtime_alloc(r->limit, sizeof(*r->owners));
    r->tickets = runtime_alloc(r->limit, sizeof(*r->tickets));
    r->next_free = runtime_alloc(r->limit, sizeof(*r->next_free));
    r->heap = runtime_alloc((size_t)capacity, sizeof(*r->heap));
    for (size_t i = 0; i < r->limit; i++) {
        r->fds[i].fd = -1;
        r->next_free[i] = i+1 < r->limit ? (int)i+1 : -1;
    }
    r->free_watch = (int)reserved;
    return true;
}
static void reactor_destroy(struct reactor *r)
{
    close(r->epoll_fd);
    free(r->fds); free(r->owners); free(r->tickets); free(r->next_free); free(r->heap);
}
/* A ticket identifies a registration, not a mutable pointer or a reused fd number. */
static bool reactor_set(struct reactor *r, size_t index, int fd, short events, struct watch_owner owner)
{
    int previous = r->fds[index].fd;
    if (previous >= 0 && (fd < 0 || previous != fd)) {
        int result = epoll_ctl(r->epoll_fd, EPOLL_CTL_DEL, previous, NULL);
        int error = errno;
        r->fds[index].fd = -1; r->tickets[index] = 0; r->count--;
        if (result < 0 && error != ENOENT && error != EBADF) { errno = error; return false; }
    }
    if (fd < 0) return true;
    bool present = r->fds[index].fd >= 0;
    if (present && r->fds[index].events == events &&
        r->owners[index].generation == owner.generation) return true;
    /* Never wrap a token into the identity of an old event. Fail closed instead. */
    if (r->serial == UINT32_MAX) { errno = EOVERFLOW; return false; }
    uint32_t ticket = ++r->serial;
    struct epoll_event event = {.events = 0};
    if (events & POLLIN) event.events |= EPOLLIN;
    if (events & POLLOUT) event.events |= EPOLLOUT;
    event.data.u64 = ((uint64_t)ticket << 32) | (index + 1);
    int op = present ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (epoll_ctl(r->epoll_fd, op, fd, &event) < 0) return false;
    if (!present) r->count++;
    r->fds[index] = (struct pollfd){.fd=fd, .events=events};
    r->owners[index] = owner; r->tickets[index] = ticket;
    return true;
}
static bool reactor_watch(struct reactor *r, struct reactor_node *n, bool upstream,
                          int fd, short events, unsigned generation)
{
    int index = n->watch[upstream];
    if (fd < 0 && index < 0) return true;
    bool fresh = index < 0;
    if (fresh) {
        index = r->free_watch;
        if (index < 0) { errno = ENOSPC; return false; }
        r->free_watch = r->next_free[index];
    }
    bool result = reactor_set(r, (size_t)index, fd, events, (struct watch_owner){n, generation, upstream});
    if (fd < 0 || (fresh && !result)) {
        r->next_free[index] = r->free_watch; r->free_watch = index;
        n->watch[upstream] = -1;
    } else n->watch[upstream] = index;
    return result;
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

static int reactor_wait(struct reactor *r, int timeout)
{
    for (size_t i = 0; i < r->reserved; i++) r->fds[i].revents = 0;
    return epoll_wait(r->epoll_fd, r->events, REACTOR_BATCH, timeout);
}
static size_t reactor_collect(struct reactor *r, int ready, uint64_t *checked)
{
    size_t count = 0;
    for (int i = 0; i < ready; i++) {
        if (checked) (*checked)++;
        uint64_t token = r->events[i].data.u64;
        uint32_t id = (uint32_t)token;
        if (!id || (size_t)(id-1) >= r->limit) continue;
        size_t index = id-1;
        if (r->fds[index].fd < 0 || r->tickets[index] != (uint32_t)(token >> 32)) continue;
        uint32_t event = r->events[i].events;
        short flags = 0;
        if (event & EPOLLIN) flags |= POLLIN;
        if (event & EPOLLOUT) flags |= POLLOUT;
        if (event & EPOLLERR) flags |= POLLERR;
        if (event & EPOLLHUP) flags |= POLLHUP;
        if (index < r->reserved) { r->fds[index].revents |= flags; continue; }
        struct watch_owner *w = &r->owners[index];
        r->ready[count++] = (struct ready_event){w->node, w->node->life, w->generation, flags, w->upstream};
    }
    return count;
}
#endif
