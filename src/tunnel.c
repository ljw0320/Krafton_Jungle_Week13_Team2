#include <errno.h>
#include <linux/sockios.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "conn.h"
#include "tunnel.h"

static int close_tunnel(struct conn *c) {
    conn_close(c);
    return -1;
}

static int watches(struct conn *c) {
    uint32_t a = (c->rlen == 0 ? EPOLLIN : 0) | (c->woff < c->wlen ? EPOLLOUT : 0);
    uint32_t b = (c->woff == c->wlen ? EPOLLIN : 0) | (c->roff < c->rlen ? EPOLLOUT : 0);
    if (a == 0) a = EPOLLERR;
    if (a != c->w.events && loop_watch(&c->w, a) < 0) return close_tunnel(c);
    if (b != c->up.w.events) {
        if (b == 0) loop_unwatch(&c->up.w);
        else if (loop_watch(&c->up.w, b) < 0) return close_tunnel(c);
    }
    return 0;
}

int tunnel_start(struct conn *c) {
    int size = TUNNEL_SOCKBUF;
    if (setsockopt(c->w.fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0 ||
        setsockopt(c->w.fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0)
        return close_tunnel(c);
    c->state = CONN_TUNNEL;
    c->up.state = UP_TUNNEL;
    c->up.since = c->last_active = time(NULL);
    fprintf(stderr, "fd=%d tunnel open backend fd=%d\n", c->w.fd, c->up.w.fd);
    return watches(c);
}

static int retry_later(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

// 각 방향은 버퍼 하나를 소유한다. 비어 있으면 한 번 읽고, 남은 바이트를 한 번 보낸다.
static int transfer(struct conn *c, int down, int readable) {
    int from = down ? c->up.w.fd : c->w.fd;
    int to = down ? c->w.fd : c->up.w.fd;
    char *buf = down ? c->wbuf : c->rbuf;
    size_t *len = down ? &c->wlen : &c->rlen;
    size_t *off = down ? &c->woff : &c->roff;
    if (readable) {
        if (*len != 0) return 0;  // 이번 배치의 오래된 IN
        ssize_t n = recv(from, buf, down ? PROXY_BUF_SIZE : RBUF_SIZE, 0);
        if (n < 0 && retry_later()) return 0;
        if (n <= 0) return close_tunnel(c);
        *len = (size_t)n;
        c->last_active = time(NULL);
        if (down) c->up.since = c->last_active;
    }
    if (*off == *len) return 0;
    ssize_t n = send(to, buf + *off, *len - *off, 0);
    if (n < 0 && retry_later()) return 0;
    if (n <= 0) return close_tunnel(c);
    *off += (size_t)n;
    if (*off == *len) *off = *len = 0;
    c->last_active = time(NULL);
    if (down) c->up.since = c->last_active;
    return 0;
}

int tunnel_on_client(struct conn *c, uint32_t events) {
    if ((events & EPOLLIN) && transfer(c, 0, 1) < 0) return -1;
    if ((events & EPOLLOUT) && transfer(c, 1, 0) < 0) return -1;
    return watches(c);
}

int tunnel_on_backend(struct conn *c, uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP))
        events = (c->up.w.events & EPOLLIN) ? EPOLLIN : EPOLLOUT;
    if ((events & EPOLLIN) && transfer(c, 1, 1) < 0) return -1;
    if ((events & EPOLLOUT) && transfer(c, 0, 0) < 0) return -1;
    return watches(c);
}

void tunnel_sweep(struct conn *c, time_t now) {
    int q;
    if (ioctl(c->w.fd, SIOCOUTQ, &q) == 0) {
        if (q < c->outq) c->up.since = c->last_active = now;
        c->outq = q;
    }
    if (now - c->last_active > 30 ||
        ((c->woff < c->wlen || c->outq > 0) && now - c->up.since > 10))
        conn_close(c);
}
