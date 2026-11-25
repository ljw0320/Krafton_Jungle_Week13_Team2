#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../src/http.h"
#include "../src/server.h"
#include "../src/stat.h"
#include "room.h"
#include "ws.h"

#define MAX_USERS 6000
#define MAX_CONNS 6064
#define FRAME_MAX 16384
#define TEXT_MAX 4096
#define IN_CAP (FRAME_MAX + 14)
#define OUT_CAP 65536
#define OUT_RESERVE 1024
#define IMG_MAX 2097152
#define MATCH_MS 500
#define PING_SEC 10
#define DEAD_SEC 30
#define HANDSHAKE_SEC 10
#define SLOW_SEC 10
#define EVENTS 64

enum state { HANDSHAKE, WAITING, PAIRED, ALONE, CLOSING };
enum blocked { RUNNING, OWN_QUEUE, PEER_QUEUE };
struct user {
    int fd, dead, counted, wait_pos;
    uint32_t interest, trace;
    enum state state;
    struct user *peer, *dirty_next, **dirty_prev, *garbage_next;
    struct user *next, **prev;
    size_t used, first, end, text_len, image_left;
    int fragment, paired_at_start, outq, resume;
    enum blocked blocked;
    int64_t accepted, received, ping_at, progress;
    unsigned char in[IN_CAP], out[OUT_CAP], text[TEXT_MAX];
};

static int epfd, users, conns, listener_fd, accept_paused;
static struct user *dirty, *garbage, *active;
static struct user *waiting[MAX_USERS];
static int nwaiting;
static unsigned long long msgs, img_bytes, slow_closed, rejected;
static struct {
    uint32_t trace;
    char kind;
    int send;
    size_t bytes;
    uint64_t t_us;
} events[EVENTS];
static uint64_t seq;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int64_t now_ms(void) {
    return (int64_t)(now_us() / 1000);
}

static void chat_event(const struct user *u, int send, char kind, size_t bytes) {
    if (!u->trace) return;
    size_t i = seq++ % EVENTS;
    events[i].trace = u->trace;
    events[i].kind = kind;
    events[i].send = send;
    events[i].bytes = bytes;
    events[i].t_us = now_us();
}

static unsigned long rss_kb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long total, resident = 0;
    if (fscanf(f, "%lu %lu", &total, &resident) != 2) resident = 0;
    fclose(f);
    return resident * (unsigned long)(sysconf(_SC_PAGESIZE) / 1024);
}

static int chat_json(char *out, size_t cap) {
    int paired = 0, paused = 0;
    size_t queued = 0;
    for (const struct user *u = active; u; u = u->next) {
        if (u->dead) continue;
        paired += u->state == PAIRED;
        paused += u->blocked != RUNNING;
        queued += u->end - u->first;
    }
    int len = snprintf(out, cap, "{\"now_us\":%" PRIu64 ",\"users\":%d,\"waiting\":%d,"
                       "\"rooms\":%d,\"msgs\":%llu,\"img_bytes\":%llu,\"paused\":%d,\"queued\":%zu,"
                       "\"slow_closed\":%llu,\"rejected\":%llu,\"rss_kb\":%lu,\"events\":[",
                       now_us(), users, nwaiting, paired / 2, msgs, img_bytes, paused, queued,
                       slow_closed, rejected, rss_kb());
    if (len < 0 || (size_t)len >= cap) return -1;
    uint64_t start = seq > EVENTS ? seq - EVENTS : 0;
    for (uint64_t n = start; n < seq; n++) {
        size_t i = n % EVENTS;
        int written = snprintf(out + len, cap - (size_t)len, "%s{\"seq\":%" PRIu64 ",\"trace\":%" PRIu32
                               ",\"stage\":\"%s\",\"kind\":\"%c\",\"bytes\":%zu,\"t_us\":%" PRIu64 "}",
                               n == start ? "" : ",", n + 1, events[i].trace, events[i].send ? "send" : "recv",
                               events[i].kind, events[i].bytes, events[i].t_us);
        if (written < 0 || (size_t)written >= cap - (size_t)len) return -1;
        len += written;
    }
    if (cap - (size_t)len < sizeof "]}") return -1;
    snprintf(out + len, cap - (size_t)len, "]}");
    return len + 2;
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
    if (u->first == u->end && len) {
        u->progress = now_ms();
        u->outq = 0;
    }
    if (len > OUT_CAP - u->end) {
        memmove(u->out, u->out + u->first, u->end - u->first);
        u->end -= u->first;
        u->first = 0;
    }
    if (len) memcpy(u->out + u->end, data, len);
    u->end += len;
    if (!(u->interest & EPOLLOUT)) mark_dirty(u);
}

static int queue_frame_from(struct user *u, int op, const void *data, size_t len, size_t reserve,
                            struct user *sender) {
    unsigned char head[10];
    size_t h = ws_head(head, 1, op, len);
    if (h + len + reserve > OUT_CAP - (u->end - u->first)) return -1;
    char kind = op == 2 ? 'b' : op == 1 && len ? *(const char *)data : 0;
    if (sender) chat_event(sender, 0, kind, len);
    append(u, head, h);
    append(u, data, len);
    if (kind) chat_event(u, 1, kind, len);
    return 0;
}

static int queue_frame(struct user *u, int op, const void *data, size_t len, size_t reserve) {
    return queue_frame_from(u, op, data, len, reserve, NULL);
}

static void unqueue_waiter(struct user *u) {
    if (u->wait_pos < 0) return;
    waiting[u->wait_pos] = waiting[--nwaiting];
    waiting[u->wait_pos]->wait_pos = u->wait_pos;
    waiting[nwaiting] = NULL;
    u->wait_pos = -1;
}

static void drop(struct user *u);

static void resume_input(struct user *u) {
    u->blocked = RUNNING;
    // 읽기를 꺼 둔 시간은 무응답 제한에 포함하지 않는다.
    u->received = now_ms();
    u->resume = 1;
    mark_dirty(u);
}

static void watch_listener(int enabled) {
    struct epoll_event ev = { .events = enabled ? EPOLLIN : 0, .data.ptr = NULL };
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, listener_fd, &ev) < 0) {
        perror("epoll_ctl listener");
        exit(1);
    }
    accept_paused = !enabled;
}

static void leave_room(struct user *u) {
    unqueue_waiter(u);
    struct user *peer = u->peer;
    if (!peer) return;
    u->peer = NULL;
    peer->peer = NULL;
    u->image_left = peer->image_left = 0;
    u->paired_at_start = peer->paired_at_start = 0;
    u->blocked = RUNNING;
    if (peer->blocked != RUNNING) resume_input(peer);
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
    if (accept_paused) watch_listener(1);
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
    u->blocked = RUNNING;
    u->resume = 0;
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

static size_t image_size(const unsigned char *p, size_t len) {
    if (len < 6 || len > 12 ||
        (memcmp(p, "ijpg ", 5) && memcmp(p, "ipng ", 5))) return 0;
    size_t size = 0;
    for (size_t i = 5; i < len; i++) {
        if (p[i] < '0' || p[i] > '9') return 0;
        size = size * 10 + (p[i] - '0');
    }
    return size <= IMG_MAX ? size : 0;
}

static int forward(struct user *u, int op, const unsigned char *p, size_t len, int ack) {
    if (ack && OUT_CAP - (u->end - u->first) < 3 + OUT_RESERVE)
        u->blocked = OWN_QUEUE;
    else if (queue_frame_from(u->peer, op, p, len, OUT_RESERVE, u) < 0)
        u->blocked = PEER_QUEUE;
    else {
        if (ack) {
            msgs++;
            queue_frame(u, 1, "a", 1, 0);
        }
        if (op == 2) img_bytes += len;
        return 1;
    }
    interest(u, u->interest & ~EPOLLIN);
    return 0;
}

static void handle_text(struct user *u, const unsigned char *p, size_t len, int paired) {
    if (!ws_utf8(p, len)) {
        violation(u, 1007);
        return;
    }
    if (len && (p[0] == 'm' || p[0] == 'i') && !paired) {
        chat_event(u, 0, (char)p[0], len);
        return;
    }
    if (len == 1 && p[0] == 'n') {
        chat_event(u, 0, 'n', len);
        if (u->state != WAITING) {
            leave_room(u);
            join_waiting(u);
        }
    } else if (len > 1 && p[0] == 'm') {
        forward(u, 1, p, len, 1);
    } else if (len && p[0] == 'i') {
        size_t size = image_size(p, len);
        if (!size || u->image_left) violation(u, 1008);
        else if (forward(u, 1, p, len, 0)) u->image_left = size;
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
    if (type == 2) {
        if (paired && f->len) {
            if (f->len > u->image_left) violation(u, 1008);
            else if (forward(u, 2, p, f->len, 0)) u->image_left -= f->len;
        } else chat_event(u, 0, 'b', f->len);
    } else if (f->len > TEXT_MAX - u->text_len) {
        violation(u, 1009);
    } else if (f->op && f->fin) {
        handle_text(u, p, f->len, paired);
    } else {
        memcpy(u->text + u->text_len, p, f->len);
        size_t total = u->text_len + f->len;
        if (f->fin) handle_text(u, u->text, total, paired);
        if (u->blocked == RUNNING) u->text_len = f->fin ? 0 : total;
    }
    if (u->blocked == RUNNING) u->fragment = f->fin ? 0 : type;
}

static void http_reply(struct user *u, int status) {
    const char *reason = "Bad Request", *extra = "";
    char body[STAT_JSON_CAP];
    int length = status == 200 ? chat_json(body, sizeof(body)) : 0;
    if (length < 0) {
        status = 500;
        length = 0;
    }
    switch (status) {
    case 200:
        reason = "OK";
        extra = "Content-Type: application/json\r\n";
        break;
    case 404: reason = "Not Found"; break;
    case 405: reason = "Method Not Allowed"; extra = "Allow: GET\r\n"; break;
    case 413: reason = "Content Too Large"; break;
    case 414: reason = "URI Too Long"; break;
    case 426: reason = "Upgrade Required"; extra = "Sec-WebSocket-Version: 13\r\n"; break;
    case 431: reason = "Request Header Fields Too Large"; break;
    case 500: reason = "Internal Server Error"; break;
    case 501: reason = "Not Implemented"; break;
    case 503: reason = "Service Unavailable"; rejected++; break;
    case 505: reason = "HTTP Version Not Supported"; break;
    }
    char reply[256];
    int len = snprintf(reply, sizeof(reply), "HTTP/1.1 %d %s\r\n%sContent-Length: %d\r\n"
                       "Connection: close\r\n\r\n", status, reason, extra, length);
    append(u, reply, len);
    if (length) append(u, body, (size_t)length);
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
        u->trace = http_trace(r.target, r.target_len);
        u->counted = 1;
        users++;
        u->ping_at = now_ms() + 1000 * PING_SEC;
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
        if (u->blocked != RUNNING) {
            ws_unmask(buf + f.head, f.len, f.mask);
            break;
        }
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
    u->received = now_ms();
    process_input(u);
}

static void flush(struct user *u) {
    if (u->resume) {
        u->resume = 0;
        interest(u, u->interest | EPOLLIN);
        if (u->dead) return;
        process_input(u);
        if (u->dead) return;
    }
    if (u->first == u->end) return;
    ssize_t n = send(u->fd, u->out + u->first, u->end - u->first, 0);
    if (n > 0) {
        u->first += n;
        u->progress = now_ms();
        if (u->first < u->end) ioctl(u->fd, SIOCOUTQ, &u->outq);
    } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        drop(u);
        return;
    }
    // 입력 재개 중에 추가한 출력도 이번 send에 포함됐다.
    unmark_dirty(u);
    if (u->first < u->end) interest(u, u->interest | EPOLLOUT);
    else if (u->state == CLOSING) drop(u);
    else {
        u->first = u->end = 0;
        interest(u, u->interest & ~EPOLLOUT);
    }
    if (!u->dead && u->end - u->first < OUT_CAP / 2) {
        if (u->blocked == OWN_QUEUE) resume_input(u);
        if (u->peer && u->peer->blocked == PEER_QUEUE) resume_input(u->peer);
    }
}

static void accept_users(int listener) {
    // 고정된 배치로 기존 사용자의 이벤트와 매칭 타이머도 진행한다.
    for (int i = 0; i < 128; i++) {
        int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EMFILE || errno == ENFILE) watch_listener(0);
            return;
        }
        if (conns >= MAX_CONNS) {
            rejected++;
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
        u->accepted = u->received = now_ms();
        u->next = active;
        u->prev = &active;
        if (active) active->prev = &u->next;
        active = u;
    }
}

static void sweep(int64_t now) {
    // 시스템 전체의 fd 부족(ENFILE)은 이 프로세스 밖에서 해소될 수도 있다.
    if (accept_paused) watch_listener(1);
    for (struct user *u = active; u; u = u->next) {
        if (u->dead) continue;
        if (u->state == HANDSHAKE && now - u->accepted >= 1000 * HANDSHAKE_SEC) {
            drop(u);
            continue;
        }
        if (u->first < u->end) {
            int pending;
            if (ioctl(u->fd, SIOCOUTQ, &pending) == 0) {
                if (pending < u->outq) u->progress = now;
                u->outq = pending;
            }
            if (now - u->progress >= 1000 * SLOW_SEC) {
                slow_closed++;
                drop(u);
                continue;
            }
        }
        if (!u->counted) continue;
        if (u->blocked == RUNNING && now - u->received >= 1000 * DEAD_SEC) {
            drop(u);
        } else if (now >= u->ping_at) {
            queue_frame(u, 9, NULL, 0, 0);
            u->ping_at = now + 1000 * PING_SEC;
        }
    }
}

static int run(int listener) {
    listener_fd = listener;
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
            if ((mask & EPOLLIN) && (u->interest & EPOLLIN) && !u->resume) read_user(u);
            if (!u->dead && (mask & EPOLLOUT)) mark_dirty(u);
        }
        int64_t now = now_ms();
        if (now >= match_at) {
            matchmaking();
            match_at = now + MATCH_MS;
        }
        if (now >= status_at) {
            sweep(now);
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
            *u->prev = u->next;
            if (u->next) u->next->prev = u->prev;
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
    int listener = server_listen(INADDR_LOOPBACK, port);
    if (listener < 0) return 1;
    if (run(listener) < 0) {
        perror("chatd");
        return 1;
    }
    return 0;
}
