#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../src/http.h"
#include "../src/server.h"
#include "room.h"
#include "ws.h"

#define MAX_USERS 6000
#define MAX_CONNS 6064
#define FRAME_MAX 16384
#define TEXT_MAX 4096
#define IN_CAP (FRAME_MAX + 14)
#define OUT_CAP 65536
#define OUT_RESERVE 1024
#define MATCH_MS 500

enum state { HANDSHAKE, WAITING, PAIRED, ALONE, CLOSING };
struct user {
    int fd, dead, counted, wait_pos;
    uint32_t interest;
    enum state state;
    struct user *peer, *dirty_next, **dirty_prev, *garbage_next;
    size_t used, first, end, text_len;
    int fragment, paired_at_start;
    unsigned char in[IN_CAP], out[OUT_CAP], text[TEXT_MAX];
};

static int epfd, users, conns;
static struct user *dirty, *garbage;
static struct user *waiting[MAX_USERS];
static int nwaiting;

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void mark_dirty(struct user *u) {
    if (u->dirty_prev) return;
    u->dirty_next = dirty;
    if (dirty) dirty->dirty_prev = &u->dirty_next;
    dirty = u;
    u->dirty_prev = &dirty;
}

static void unmark_dirty(struct user *u) {
    if (!u->dirty_prev) return;
    *u->dirty_prev = u->dirty_next;
    if (u->dirty_next) u->dirty_next->dirty_prev = u->dirty_prev;
    u->dirty_prev = NULL;
    u->dirty_next = NULL;
}

static void append(struct user *u, const void *data, size_t len) {
    if (len > OUT_CAP - u->end) {
        memmove(u->out, u->out + u->first, u->end - u->first);
        u->end -= u->first;
        u->first = 0;
    }
    if (len) memcpy(u->out + u->end, data, len);
    u->end += len;
    if (!(u->interest & EPOLLOUT)) mark_dirty(u);
}

static int queue_frame(struct user *u, int op, const void *data, size_t len, size_t reserve) {
    unsigned char head[10];
    size_t h = ws_head(head, 1, op, len);
    if (h + len + reserve > OUT_CAP - (u->end - u->first)) return -1;
    append(u, head, h);
    append(u, data, len);
    return 0;
}

static void unqueue_waiter(struct user *u) {
    if (u->wait_pos < 0) return;
    waiting[u->wait_pos] = waiting[--nwaiting];
    waiting[u->wait_pos]->wait_pos = u->wait_pos;
    waiting[nwaiting] = NULL;
    u->wait_pos = -1;
}

static void drop(struct user *u);

static void leave_room(struct user *u) {
    unqueue_waiter(u);
    struct user *peer = u->peer;
    if (!peer) return;
    u->peer = NULL;
    peer->peer = NULL;
    peer->state = ALONE;
    if (queue_frame(peer, 1, "l", 1, 0) < 0) drop(peer);
}

static void drop(struct user *u) {
    leave_room(u);
    unmark_dirty(u);
    users -= u->counted;
    conns--;
    u->dead = 1;
    epoll_ctl(epfd, EPOLL_CTL_DEL, u->fd, NULL);
    close(u->fd);
    u->garbage_next = garbage;
    garbage = u;
}

static void interest(struct user *u, uint32_t mask) {
    if (mask == u->interest) return;
    struct epoll_event ev = { .events = mask, .data.ptr = u };
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, u->fd, &ev) < 0) {
        drop(u);
        return;
    }
    u->interest = mask;
}

static void closing(struct user *u) {
    leave_room(u);
    users -= u->counted;
    u->counted = 0;
    u->state = CLOSING;
    interest(u, u->interest & EPOLLOUT);
}

static void close_frame(struct user *u, const void *p, size_t n) {
    if (queue_frame(u, 8, p, n, 0) < 0) drop(u);
    else closing(u);
}

static void violation(struct user *u, int code) {
    uint16_t wire = htons(code);
    close_frame(u, &wire, sizeof(wire));
}

static void join_waiting(struct user *u) {
    if (queue_frame(u, 1, "w", 1, 0) < 0) {
        drop(u);
        return;
    }
    u->state = WAITING;
    u->wait_pos = nwaiting;
    waiting[nwaiting++] = u;
}

static void matchmaking(void) {
    int n = nwaiting;
    if (n < 2) return;
    struct user *batch[MAX_USERS];
    int order[MAX_USERS];
    memcpy(batch, waiting, n * sizeof(batch[0]));
    for (int i = 0; i < n; i++) order[i] = i;
    shuffle(order, n);
    for (int i = 1; i < n; i += 2) {
        struct user *a = batch[order[i - 1]], *b = batch[order[i]];
        unqueue_waiter(a);
        unqueue_waiter(b);
        a->state = b->state = PAIRED;
        a->peer = b;
        b->peer = a;
        int a_ok = queue_frame(a, 1, "c", 1, 0);
        int b_ok = queue_frame(b, 1, "c", 1, 0);
        if (a_ok < 0) drop(a);
        if (b_ok < 0 && !b->dead) drop(b);
    }
}

static void waiting_status(void) {
    char status[32];
    int len = snprintf(status, sizeof(status), "s%d %d", users, nwaiting);
    for (int i = 0; i < nwaiting; i++)
        queue_frame(waiting[i], 1, status, len, 0);
}

static void handle_text(struct user *u, const unsigned char *p, size_t len, int paired) {
    if (!ws_utf8(p, len)) {
        violation(u, 1007);
        return;
    }
    if (len && (p[0] == 'm' || p[0] == 'i') && !paired) return;
    if (len == 1 && p[0] == 'n') {
        if (u->state != WAITING) {
            leave_room(u);
            join_waiting(u);
        }
    } else if (len > 1 && p[0] == 'm') {
        if (queue_frame(u->peer, 1, p, len, OUT_RESERVE) < 0 ||
            queue_frame(u, 1, "a", 1, 0) < 0) drop(u);
    } else {
        violation(u, 1008);
    }
}

static void handle_frame(struct user *u, const struct ws_frame *f, unsigned char *p) {
    switch (f->op) {
    case 8:
        close_frame(u, p, f->len ? 2 : 0);
        return;
    case 9:
        if (queue_frame(u, 10, p, f->len, 0) < 0) drop(u);
        return;
    case 10:
        return;
    }
    if ((f->op == 0) != (u->fragment != 0)) {
        violation(u, 1002);
        return;
    }
    int type = f->op ? f->op : u->fragment;
    if (f->op) u->paired_at_start = u->state == PAIRED;
    int paired = u->paired_at_start && u->state == PAIRED;
    u->fragment = f->fin ? 0 : type;
    if (type == 2) {
        if (paired) violation(u, 1008);
        return;
    }
    if (f->len > TEXT_MAX - u->text_len) {
        violation(u, 1009);
        return;
    }
    if (f->op && f->fin) {
        handle_text(u, p, f->len, paired);
        return;
    }
    memcpy(u->text + u->text_len, p, f->len);
    u->text_len += f->len;
    if (f->fin) {
        size_t total = u->text_len;
        u->text_len = 0;
        handle_text(u, u->text, total, paired);
    }
}

static void http_reply(struct user *u, int status) {
    const char *reason = "Bad Request", *extra = "";
    char body[64] = "";
    switch (status) {
    case 200:
        reason = "OK";
        extra = "Content-Type: application/json\r\n";
        snprintf(body, sizeof(body), "{\"users\":%d,\"waiting\":%d}", users, nwaiting);
        break;
    case 404: reason = "Not Found"; break;
    case 405: reason = "Method Not Allowed"; extra = "Allow: GET\r\n"; break;
    case 413: reason = "Content Too Large"; break;
    case 414: reason = "URI Too Long"; break;
    case 426: reason = "Upgrade Required"; extra = "Sec-WebSocket-Version: 13\r\n"; break;
    case 431: reason = "Request Header Fields Too Large"; break;
    case 501: reason = "Not Implemented"; break;
    case 503: reason = "Service Unavailable"; break;
    case 505: reason = "HTTP Version Not Supported"; break;
    }
    char reply[256];
    int len = snprintf(reply, sizeof(reply), "HTTP/1.1 %d %s\r\n%sContent-Length: %zu\r\n"
                       "Connection: close\r\n\r\n%s", status, reason, extra, strlen(body), body);
    append(u, reply, len);
    closing(u);
}

static ssize_t handshake(struct user *u) {
    struct http_req r;
    const char *buf = (const char *)u->in;
    size_t n = u->used < HTTP_HEAD_MAX ? u->used : HTTP_HEAD_MAX;
    ssize_t total = http_parse(buf, n, HTTP_HEAD_MAX, &r);
    if (total < 0) http_reply(u, r.status);
    if (total <= 0) return 0;
    const char *query = memchr(r.target, '?', r.target_len);
    size_t path_len = query ? (size_t)(query - r.target) : r.target_len;
    char accept[29];
    const char *v;
    size_t vlen;
    if (r.method != M_GET) http_reply(u, 405);
    else if (r.target_len == 8 && memcmp(r.target, "/ws/stat", 8) == 0) http_reply(u, 200);
    else if (path_len != 8 || memcmp(r.target, "/ws/chat", 8) != 0) http_reply(u, 404);
    else if (!r.ws || http_header(&r, buf, "Sec-WebSocket-Key", &v, &vlen) != 1 ||
             ws_accept(v, vlen, accept) < 0) http_reply(u, 400);
    else if (http_header(&r, buf, "Sec-WebSocket-Version", &v, &vlen) != 1 ||
             vlen != 2 || memcmp(v, "13", 2) != 0) http_reply(u, 426);
    else if (users >= MAX_USERS) http_reply(u, 503);
    else {
        char reply[192];
        int len = snprintf(reply, sizeof(reply), "HTTP/1.1 101 Switching Protocols\r\n"
                           "Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
        append(u, reply, len);
        u->counted = 1;
        users++;
        join_waiting(u);
    }
    return total;
}

static void process_input(struct user *u) {
    size_t pos = 0;
    if (u->state == HANDSHAKE) {
        pos = handshake(u);
        if (u->dead || u->state == HANDSHAKE || u->state == CLOSING) return;
    }
    while (pos < u->used) {
        struct ws_frame f;
        unsigned char *buf = u->in + pos;
        ssize_t n = ws_parse(buf, u->used - pos, FRAME_MAX, &f);
        if (n < 0) {
            violation(u, f.close);
            return;
        }
        if (!n) break;
        ws_unmask(buf + f.head, f.len, f.mask);
        handle_frame(u, &f, buf + f.head);
        if (u->dead || u->state == CLOSING) return;
        pos += n;
    }
    if (pos) {
        u->used -= pos;
        memmove(u->in, u->in + pos, u->used);
    }
}

static void read_user(struct user *u) {
    ssize_t n = recv(u->fd, u->in + u->used, IN_CAP - u->used, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
    if (n <= 0) {
        drop(u);
        return;
    }
    u->used += n;
    process_input(u);
}

static void flush(struct user *u) {
    ssize_t n = send(u->fd, u->out + u->first, u->end - u->first, 0);
    if (n > 0) u->first += n;
    else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        drop(u);
        return;
    }
    if (u->first < u->end) interest(u, u->interest | EPOLLOUT);
    else if (u->state == CLOSING) drop(u);
    else {
        u->first = u->end = 0;
        interest(u, EPOLLIN);
    }
}

static void accept_users(int listener) {
    // 고정된 배치로 기존 사용자의 이벤트와 매칭 타이머도 진행한다.
    for (int i = 0; i < 128; i++) {
        int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK);
        if (fd < 0) return;
        if (conns >= MAX_CONNS) {
            close(fd);
            continue;
        }
        int one = 1, size = 16384;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0) {
            close(fd);
            continue;
        }
        struct user *u = calloc(1, sizeof(*u));
        if (!u) { close(fd); continue; }
        u->fd = fd;
        u->wait_pos = -1;
        u->interest = EPOLLIN;
        struct epoll_event ev = { .events = EPOLLIN, .data.ptr = u };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            close(fd);
            free(u);
            continue;
        }
        conns++;
    }
}

static int run(int listener) {
    struct epoll_event event = { .events = EPOLLIN, .data.ptr = NULL };
    epfd = epoll_create1(0);
    if (epfd < 0 || epoll_ctl(epfd, EPOLL_CTL_ADD, listener, &event) < 0) return -1;
    int64_t match_at = now_ms() + MATCH_MS, status_at = now_ms() + 1000;
    for (;;) {
        struct epoll_event events[256];
        int64_t until = (match_at < status_at ? match_at : status_at) - now_ms();
        int count = epoll_wait(epfd, events, 256, until > 0 ? (int)until : 0);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        for (int i = 0; i < count; i++) {
            struct user *u = events[i].data.ptr;
            uint32_t mask = events[i].events;
            if (!u) { accept_users(listener); continue; }
            if (u->dead) continue;
            if (mask & (EPOLLERR | EPOLLHUP)) { drop(u); continue; }
            if ((mask & EPOLLIN) && u->state != CLOSING) read_user(u);
            if (!u->dead && (mask & EPOLLOUT)) mark_dirty(u);
        }
        int64_t now = now_ms();
        if (now >= match_at) {
            matchmaking();
            match_at = now + MATCH_MS;
        }
        if (now >= status_at) {
            waiting_status();
            status_at = now + 1000;
        }
        while (dirty) {
            struct user *u = dirty;
            unmark_dirty(u);
            flush(u);
        }
        while (garbage) {
            struct user *u = garbage;
            garbage = u->garbage_next;
            free(u);
        }
    }
}

int main(int argc, char **argv) {
    char *end = NULL;
    long port = argc == 2 ? strtol(argv[1], &end, 10) : 0;
    if (port < 1 || port > 65535 || *end) {
        fprintf(stderr, "usage: ./chatd <port>\n");
        return 1;
    }
    unsigned seed;
    ssize_t n;
    do n = getrandom(&seed, sizeof(seed), 0); while (n < 0 && errno == EINTR);
    if (n != sizeof(seed)) { perror("getrandom"); return 1; }
    srandom(seed);
    signal(SIGPIPE, SIG_IGN);
    int listener = server_listen(INADDR_LOOPBACK, port), size = 16384;
    if (listener < 0) return 1;
    if (setsockopt(listener, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0 ||
        run(listener) < 0) {
        perror("chatd");
        return 1;
    }
    return 0;
}
