#include "proxy.h"
#include <assert.h>
#include <poll.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/time.h>
#include "reactor.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static volatile sig_atomic_t interrupted;
static void interrupt_wait(int signal_number) { (void)signal_number; interrupted = 1; }
int main(void)
{
    struct reactor r;
    struct reactor_node nodes[64];
    CHECK(reactor_init(&r, 64, 1));
    for (int i = 0; i < 64; i++) nodes[i] = (struct reactor_node){.watch={-1,-1}, .heap_index=-1, .life=1};
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
    CHECK(reactor_watch(&r, &nodes[0], false, pair[0], POLLIN, 7));
    CHECK(reactor_watch(&r, &nodes[0], false, pair[0], POLLOUT, 9));
    CHECK(r.count == 1 && r.fds[nodes[0].watch[0]].events == POLLOUT);
    CHECK(reactor_watch(&r, &nodes[0], false, pair[0], POLLIN, 9));
    CHECK(reactor_wait(&r, 0) == 0); /* no permanent EPOLLOUT busy loop */
    CHECK(write(pair[1], "x", 1) == 1);
    CHECK(reactor_wait(&r, 100) == 1);
    CHECK(reactor_collect(&r, 1, NULL) == 1);
    struct ready_event old = r.ready[0];
    CHECK(old.node == &nodes[0] && old.generation == 9);
    /* The kernel batch was already returned: remove/re-register invalidates its token. */
    CHECK(reactor_watch(&r, &nodes[0], false, -1, 0, 0));
    nodes[0].life++;
    CHECK(reactor_watch(&r, &nodes[0], false, pair[0], POLLIN, 10));
    CHECK(reactor_collect(&r, 1, NULL) == 0);
    CHECK(old.life != old.node->life && old.generation == 9);
    CHECK(reactor_wait(&r, 100) == 1 && reactor_collect(&r, 1, NULL) == 1);
    CHECK(r.ready[0].generation == 10);
    char byte;
    CHECK(read(pair[0], &byte, 1) == 1);
    CHECK(reactor_watch(&r, &nodes[0], false, -1, 0, 0));
    CHECK(write(pair[1], "y", 1) == 1);
    CHECK(shutdown(pair[1], SHUT_WR) == 0);
    CHECK(reactor_wait(&r, 0) == 0); /* paused fd cannot spin on HUP */
    CHECK(reactor_watch(&r, &nodes[0], false, pair[0], POLLIN, 11));
    CHECK(reactor_wait(&r, 100) == 1);
    CHECK(read(pair[0], &byte, 1) == 1 && byte == 'y');
    CHECK(read(pair[0], &byte, 1) == 0);
    CHECK(reactor_watch(&r, &nodes[0], false, -1, 0, 0));
    close(pair[0]); close(pair[1]);
    CHECK(reactor_watch(&r, &nodes[0], false, -1, 0, 0));
    CHECK(r.count == 0);
    CHECK(!reactor_watch(&r, &nodes[0], false, 999999, POLLIN, 0));
    CHECK(nodes[0].watch[0] == -1 && r.count == 0);
    int regular = open("/dev/null", O_RDONLY);
    CHECK(regular >= 0);
    CHECK(!reactor_watch(&r, &nodes[0], false, regular, POLLIN, 0) && errno == EPERM);
    close(regular);
    /* More than a batch of level-ready sockets: no events are dropped. */
    int pairs[70][2], processed = 0;
    for (int i = 0; i < 70; i++) {
        CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pairs[i]) == 0);
        CHECK(reactor_watch(&r, &nodes[i/2], i%2, pairs[i][0], POLLIN, (unsigned)i));
        CHECK(write(pairs[i][1], "z", 1) == 1);
    }
    while (processed < 70) {
        int ready = reactor_wait(&r, 100);
        CHECK(ready > 0 && ready <= REACTOR_BATCH);
        size_t count = reactor_collect(&r, ready, NULL);
        for (size_t i = 0; i < count; i++) {
            struct ready_event e = r.ready[i];
            CHECK(read(r.fds[e.node->watch[e.upstream]].fd, &byte, 1) == 1);
            CHECK(reactor_watch(&r, e.node, e.upstream, -1, 0, 0));
            processed++;
        }
    }
    CHECK(r.count == 0 && reactor_wait(&r, 0) == 0);
    for (int i = 0; i < 70; i++) { close(pairs[i][0]); close(pairs[i][1]); }
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
    struct sigaction action = {.sa_handler=interrupt_wait}, previous;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGALRM, &action, &previous) == 0);
    struct itimerval alarm = {.it_value={.tv_sec=0, .tv_usec=1000}};
    CHECK(setitimer(ITIMER_REAL, &alarm, NULL) == 0);
    CHECK(reactor_wait(&r, 1000) == -1 && errno == EINTR && interrupted);
    CHECK(reactor_wait(&r, 0) == 0);
    CHECK(sigaction(SIGALRM, &previous, NULL) == 0);
    struct rlimit limits;
    CHECK(getrlimit(RLIMIT_NOFILE, &limits) == 0);
    struct rlimit low = limits; low.rlim_cur = 0;
    CHECK(setrlimit(RLIMIT_NOFILE, &low) == 0);
    struct reactor failed;
    bool created = reactor_init(&failed, 1, 1);
    CHECK(setrlimit(RLIMIT_NOFILE, &limits) == 0);
    CHECK(!created);
    close(r.epoll_fd); r.epoll_fd = -1;
    CHECK(reactor_wait(&r, 0) == -1 && errno == EBADF);
    reactor_destroy(&r);
    puts("reactor: LT watch add/modify/remove/pause/resume, stale tokens, 64-event batches, failure isolation, heap order passed");
    return 0;
}
