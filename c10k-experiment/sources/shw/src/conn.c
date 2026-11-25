#include <errno.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>

#include "conn.h"
#include "event_loop.h"
#include "http.h"
#include "proxy.h"
#include "route.h"

static struct conn *head;
static int conn_count;

static int conn_process(struct conn *c);

struct conn *conn_new(int fd) {
    struct conn *c = calloc(1, sizeof(*c));
    if (!c) {
        perror("calloc");
        close(fd);
        return NULL;
    }
    // 헤더와 본문이 따로 나가면 Nagle + 지연 ACK로 약 44ms 늦는다. 실패해도 느려질 뿐이다
    int on = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) < 0) perror("setsockopt");
    c->w = (struct watch){W_CLIENT, fd, 0};
    c->up.w = (struct watch){W_BACKEND, -1, 0};
    c->up.c = c;
    c->file_fd = -1;  // calloc의 0은 stdin이라 conn_close가 fd 0을 닫는다
    c->state = CONN_READING;
    c->last_active = time(NULL);  // 아무것도 안 보내는 연결도 유휴로 닫히게
    c->next = head;
    if (head) head->prev = c;
    head = c;
    conn_count++;
    if (loop_watch(&c->w, EPOLLIN) < 0) {
        conn_close(c);
        return NULL;
    }
    return c;
}

void conn_close(struct conn *c) {
    proxy_close(c);
    loop_unwatch(&c->w);  // close 전에 해야 한다
    close(c->w.fd);
    loop_accept_resume();
    free(c->wbuf);
    if (c->file_fd >= 0) close(c->file_fd);
    if (c->prev) c->prev->next = c->next;
    else head = c->next;
    if (c->next) c->next->prev = c->prev;
    conn_count--;
    fprintf(stderr, "close fd=%d conn_count=%d\n", c->w.fd, conn_count);
    free(c);
}

int conn_on_readable(struct conn *c) {
    ssize_t n = recv(c->w.fd, c->rbuf + c->rlen, RBUF_SIZE - c->rlen, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        conn_close(c);
        return -1;
    }
    if (n == 0) {
        conn_close(c);
        return -1;
    }
    c->last_active = time(NULL);
    if (c->rlen == 0) c->req_start = c->last_active;
    c->rlen += (size_t)n;
    fprintf(stderr, "fd=%d recv %zd bytes\n", c->w.fd, n);
    return conn_process(c);
}

// send나 sendfile 한 번. -1: 닫힘, 0: 남음(WRITING 유지), 1: 다 보내고 READING으로 돌아옴
static int conn_send(struct conn *c) {
    ssize_t n;
    if (c->woff < c->wlen) {
        n = send(c->w.fd, c->wbuf + c->woff, c->wlen - c->woff, 0);
        if (n > 0) c->woff += (size_t)n;
    } else {
        n = sendfile(c->w.fd, c->file_fd, &c->file_off, (size_t)(c->file_len - c->file_off));
        if (n == 0) {  // 전송 중 파일이 줄어듦. Content-Length를 채울 수 없다
            conn_close(c);
            return -1;
        }
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            conn_close(c);
            return -1;
        }
    } else {
        c->last_active = time(NULL);
        fprintf(stderr, "fd=%d send %zd bytes\n", c->w.fd, n);
    }
    if (c->woff < c->wlen || c->file_off < c->file_len) {
        if (c->up.w.events) {  // backpressure. 낮추기만 하면 B의 RST가 LT로 계속 온다
            loop_unwatch(&c->up.w);
            fprintf(stderr, "fd=%d backend fd=%d off\n", c->w.fd, c->up.w.fd);
        }
        if (!(c->w.events & EPOLLOUT)) {
            if (loop_watch(&c->w, EPOLLOUT) < 0) {
                conn_close(c);
                return -1;
            }
            fprintf(stderr, "fd=%d EPOLLOUT on\n", c->w.fd);
        }
        return 0;
    }
    if (c->up.w.fd >= 0) return proxy_resume(c);  // B가 열려 있으면 보낼 것이 더 있다
    if (c->file_fd >= 0) close(c->file_fd);
    c->file_fd = -1;
    free(c->wbuf);
    c->wbuf = NULL;
    if (!c->keep_alive) {
        conn_close(c);
        return -1;
    }
    c->state = CONN_READING;
    // 앞 응답을 보낸 시간이 다음 요청의 타이머에 들어가지 않게 여기서 기록한다
    if (c->rlen > 0) c->req_start = time(NULL);
    if (c->w.events != EPOLLIN) {
        int had_out = c->w.events & EPOLLOUT;
        if (loop_watch(&c->w, EPOLLIN) < 0) {
            conn_close(c);
            return -1;
        }
        if (had_out) fprintf(stderr, "fd=%d EPOLLOUT off\n", c->w.fd);
    }
    return 1;
}

// 0: WRITING 준비됨, -1: 닫음
static int conn_respond(struct conn *c, const struct http_req *req) {
    struct http_resp r;
    if (http_handle(req, &r) < 0) {
        conn_close(c);
        return -1;
    }
    c->wbuf = r.head;
    c->wlen = r.head_len;
    c->woff = 0;
    c->file_fd = r.file_fd;
    c->file_len = r.file_len;
    c->file_off = 0;
    c->keep_alive = r.keep_alive;
    c->outq = 0;  // 앞 응답의 값이 남으면 첫 sweep이 줄어든 것으로 잘못 본다
    c->state = CONN_WRITING;
    return 0;
}

int conn_error(struct conn *c, int status) {
    proxy_close(c);
    free(c->wbuf);
    c->wbuf = NULL;
    struct http_req req = {.status = status};
    if (conn_respond(c, &req) < 0) return -1;
    // 다 못 보내면 WRITING으로 남고 유휴 검사가 맡는다. 닫는 응답이라 1은 오지 않는다
    return conn_send(c);
}

// rbuf의 완성된 요청을 차례로 처리한다. 재귀 대신 루프로 파이프라이닝을 처리한다
static int conn_process(struct conn *c) {
    for (;;) {
        struct http_req req;
        ssize_t n = http_parse(c->rbuf, c->rlen, RBUF_SIZE, &req);  // rlen <= RBUF_SIZE
        if (n == 0) break;
        struct route *r = n > 0 ? route_find(req.target, req.target_len) : NULL;
        int st = r ? proxy_start(c, &req, r) : 0;
        if (st < 0) return -1;
        if (st > 0) {
            req.status = st;
            req.backend = 1;
        }
        if ((!r || st > 0) && conn_respond(c, &req) < 0) return -1;
        // req가 rbuf를 가리키므로 응답이나 전달할 요청을 만든 뒤에 지운다
        if (n > 0) {
            c->rlen -= (size_t)n;
            memmove(c->rbuf, c->rbuf + n, c->rlen);
        } else {
            c->rlen = 0;  // 형식 오류는 닫는 응답이므로 남은 바이트는 쓰지 않는다
        }
        if (r && st == 0) return 0;  // PROXYING. 다음 요청은 응답이 끝난 뒤
        int sent = conn_send(c);
        if (sent <= 0) return sent;
    }
    // 꽉 찬 채로 돌아가면 다음 recv 크기가 0이라 FIN과 구분되지 않는다
    if (c->rlen == RBUF_SIZE) {
        conn_close(c);
        return -1;
    }
    return 0;
}

// '>': time()이 초 단위라 '>='면 9초대에 닫힌다. 실제 타임아웃은 10~11초
void conn_sweep_idle(time_t now) {
    struct conn *next;
    for (struct conn *c = head; c; c = next) {
        next = c->next;  // conn_close가 c를 해제하므로 먼저 저장
        if (c->state == CONN_PROXYING) {  // 요청 10초와 유휴 10초는 보지 않는다
            proxy_timeout(c, now);
            continue;
        }
        // 송신 버퍼가 1/3쯤 비기 전에는 EPOLLOUT이 오지 않아, 느리게 읽는 중인지는 줄어든 양으로 안다
        int q;
        if (c->state == CONN_WRITING && ioctl(c->w.fd, SIOCOUTQ, &q) == 0) {
            if (q < c->outq) c->last_active = now;
            c->outq = q;
        }
        // 유휴보다 먼저 본다. 한 바이트 보내고 멈춘 연결도 408을 받는다
        if (c->state == CONN_READING && c->rlen > 0 && now - c->req_start > REQ_TIMEOUT_SEC) {
            fprintf(stderr, "fd=%d request timeout\n", c->w.fd);
            conn_error(c, 408);
        } else if (now - c->last_active > IDLE_TIMEOUT_SEC) {
            fprintf(stderr, "fd=%d idle timeout\n", c->w.fd);
            conn_close(c);
        }
    }
}

int conn_on_writable(struct conn *c) {
    int r = conn_send(c);
    return r == 1 ? conn_process(c) : r;
}
