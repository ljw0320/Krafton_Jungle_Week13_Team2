#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "conn.h"
#include "event_loop.h"
#include "http.h"
#include "proxy.h"
#include "route.h"
#include "tunnel.h"

static void log_error(struct conn *c, int status, const char *why) {
    fprintf(stderr, "fd=%d proxy error %d: %s\n", c->w.fd, status, why);
}

void proxy_close(struct conn *c) {
    if (c->up.w.fd < 0) return;
    loop_unwatch(&c->up.w);  // close 전에 해야 한다
    close(c->up.w.fd);
    fprintf(stderr, "fd=%d backend fd=%d close\n", c->w.fd, c->up.w.fd);
    c->up.w.fd = -1;
    loop_accept_resume();
}

// proxy_start 안의 실패. 응답은 부른 쪽이 원래 req로 만든다
static int start_fail(struct conn *c, int status, const char *why) {
    if (why) log_error(c, status, why);
    proxy_close(c);
    free(c->wbuf);
    c->wbuf = NULL;
    return status;
}

// 응답을 시작했으면 A를 닫고, 아니면 오류 응답을 보낸다. 부른 뒤 u를 쓰지 않는다
static int proxy_fail(struct conn *c, int status, const char *why) {
    if (c->up.head_done) {
        fprintf(stderr, "fd=%d proxy error: %s\n", c->w.fd, why);
        conn_close(c);
        return -1;
    }
    log_error(c, status, why);
    c->last_active = time(NULL);  // B를 기다린 시간 때문에 다음 sweep이 응답을 보내기 전에 닫지 않게
    return conn_error(c, status);
}

int proxy_start(struct conn *c, const struct http_req *req, struct route *r) {
    struct upstream *u = &c->up;
    c->wbuf = malloc(PROXY_BUF_SIZE);
    if (!c->wbuf) return start_fail(c, 500, strerror(errno));
    const struct backend *b = route_pick(r);
    int st = http_proxy_request(req, c->rbuf, b->name, c->wbuf, PROXY_BUF_SIZE, &c->wlen);
    if (st != 0) return start_fail(c, st, st == 500 ? "cannot build request" : NULL);
    route_advance(r);
    c->woff = 0;
    u->w.fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (u->w.fd < 0) return start_fail(c, 500, strerror(errno));
    u->ws = req->ws;
    if (u->ws) {
        int size = TUNNEL_SOCKBUF, on = 1;
        if (setsockopt(u->w.fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0 ||
            setsockopt(u->w.fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0 ||
            setsockopt(u->w.fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) < 0)
            return start_fail(c, 500, strerror(errno));
    }
    fprintf(stderr, "fd=%d backend fd=%d connect %s\n", c->w.fd, u->w.fd, b->name);
    if (connect(u->w.fd, (const struct sockaddr *)&b->addr, sizeof(b->addr)) < 0 && errno != EINPROGRESS)
        return start_fail(c, 502, strerror(errno));
    // connect보다 먼저 등록하면 OUT|HUP이 온다
    if (loop_watch(&u->w, EPOLLOUT) < 0) return start_fail(c, 500, "epoll_ctl");
    u->state = UP_CONNECTING;
    u->since = time(NULL);
    u->minor = req->minor;
    u->head_done = 0;
    c->keep_alive = http_keep_alive(req);
    c->state = CONN_PROXYING;
    if (loop_watch(&c->w, EPOLLERR) < 0) {
        conn_close(c);
        return -1;
    }
    return 0;
}

static int up_send(struct upstream *u) {
    struct conn *c = u->c;
    ssize_t n = send(u->w.fd, c->wbuf + c->woff, c->wlen - c->woff, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        return proxy_fail(c, 502, strerror(errno));
    }
    fprintf(stderr, "fd=%d backend fd=%d send %zd bytes\n", c->w.fd, u->w.fd, n);
    u->since = time(NULL);
    c->woff += (size_t)n;
    if (c->woff < c->wlen) return 0;
    c->wlen = c->woff = 0;
    u->state = UP_RECEIVING;
    if (loop_watch(&u->w, EPOLLIN) < 0) return proxy_fail(c, 500, "epoll_ctl");
    return 0;
}

// wbuf에 담은 응답을 A로 보낸다. 본문을 다 받았으면 B를 먼저 닫는다(B가 열려 있으면 보낼 것이 더 있다)
static int relay(struct conn *c) {
    c->last_active = time(NULL);
    c->state = CONN_WRITING;
    if (c->up.has_len && c->up.remain == 0) proxy_close(c);
    return conn_on_writable(c);
}

// wbuf에 모은 응답 헤더를 검사하고, 통과하면 재작성한 헤더와 이미 받은 본문을 A로 보낸다
static int up_head(struct upstream *u) {
    struct conn *c = u->c;
    char *out = malloc(PROXY_BUF_SIZE);
    if (!out) return proxy_fail(c, 500, strerror(errno));
    struct http_upresp r;
    ssize_t n;
    for (;;) {
        n = http_proxy_response(c->wbuf, c->wlen, u->minor, c->keep_alive, u->ws, out, PROXY_BUF_SIZE, &r);
        if (n <= 0 || r.status == 101 || r.status >= 200) break;
        c->wlen -= (size_t)n;  // 1xx는 버린다
        memmove(c->wbuf, c->wbuf + n, c->wlen);
    }
    if (n == 0) {
        free(out);
        return 0;
    }
    if (n < 0) {
        free(out);
        return proxy_fail(c, 502, "bad response header");
    }
    size_t body = c->wlen - (size_t)n;
    if (r.has_len && body > r.body_len) body = r.body_len;  // 나머지를 보내면 다음 응답으로 읽힌다
    if (body > PROXY_BUF_SIZE - r.out_len) {
        free(out);
        return proxy_fail(c, 502, "response header too large");
    }
    memcpy(out + r.out_len, c->wbuf + n, body);
    free(c->wbuf);
    c->wbuf = out;
    c->wlen = r.out_len + body;
    c->woff = 0;
    u->head_done = 1;
    u->has_len = r.has_len;
    u->remain = r.has_len ? r.body_len - body : 0;
    c->keep_alive = r.keep_alive;
    c->outq = 0;  // 앞 응답의 값이 남으면 첫 sweep이 줄어든 것으로 잘못 본다
    if (r.status == 101) return tunnel_start(c);
    return relay(c);
}

static int up_recv(struct upstream *u) {
    struct conn *c = u->c;
    size_t max;
    if (u->head_done) {  // A로 다 보낸 뒤에만 여기 온다
        c->wlen = c->woff = 0;
        max = u->has_len && u->remain < PROXY_BUF_SIZE ? u->remain : PROXY_BUF_SIZE;
    } else {
        max = HTTP_HEAD_MAX - c->wlen;  // 꽉 차면 up_head가 502로 끝내므로 0이 아니다
    }
    ssize_t n = recv(u->w.fd, c->wbuf + c->wlen, max, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        return proxy_fail(c, 502, strerror(errno));
    }
    if (n == 0) {
        if (u->head_done && !u->has_len) {  // 길이를 모르는 응답의 정상 끝
            conn_close(c);
            return -1;
        }
        return proxy_fail(c, 502, "backend closed early");
    }
    u->since = time(NULL);
    fprintf(stderr, "fd=%d backend fd=%d recv %zd bytes\n", c->w.fd, u->w.fd, n);
    c->wlen += (size_t)n;
    if (!u->head_done) return up_head(u);
    if (u->has_len) u->remain -= (size_t)n;
    return relay(c);
}

// ERR|HUP과 함께 응답이 남아 있을 수 있어 이벤트 종류가 아니라 send, recv의 반환값으로 판단한다
int proxy_event(struct upstream *u, uint32_t events) {
    if (u->state == UP_TUNNEL) return tunnel_on_backend(u->c, events);
    if (u->state == UP_CONNECTING) {
        int err;
        socklen_t len = sizeof(err);
        if (getsockopt(u->w.fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) err = errno;
        if (err != 0) return proxy_fail(u->c, 502, strerror(err));
        u->state = UP_SENDING;
        u->since = time(NULL);
    }
    return u->state == UP_SENDING ? up_send(u) : up_recv(u);
}

// B를 끈 동안(A가 WRITING)은 sweep이 부르지 않는다. 늦은 쪽이 클라이언트라 A의 유휴 검사가 맡는다
int proxy_timeout(struct conn *c, time_t now) {
    struct upstream *u = &c->up;
    int connecting = u->state == UP_CONNECTING;
    if (now - u->since <= (connecting ? CONNECT_TIMEOUT_SEC : RESPONSE_TIMEOUT_SEC)) return 0;
    return proxy_fail(c, 504, connecting ? "connect timeout" : "response timeout");
}

int proxy_resume(struct conn *c) {
    struct upstream *u = &c->up;
    c->state = CONN_PROXYING;
    u->since = time(NULL);
    if (c->w.events != EPOLLERR) {
        if (loop_watch(&c->w, EPOLLERR) < 0) {
            conn_close(c);
            return -1;
        }
        fprintf(stderr, "fd=%d EPOLLOUT off\n", c->w.fd);
    }
    if (u->w.events == 0) {
        if (loop_watch(&u->w, EPOLLIN) < 0) return proxy_fail(c, 500, "epoll_ctl");
        fprintf(stderr, "fd=%d backend fd=%d on\n", c->w.fd, u->w.fd);
    }
    return 0;
}
