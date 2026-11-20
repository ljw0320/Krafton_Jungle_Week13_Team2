#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "conn.h"
#include "event_loop.h"
#include "http.h"

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
    c->fd = fd;
    c->state = CONN_READING;
    c->last_active = time(NULL);  // 아무것도 안 보내는 연결도 유휴로 닫히게
    c->next = head;
    if (head) head->prev = c;
    head = c;
    conn_count++;
    if (loop_watch(c, EPOLLIN) < 0) {
        conn_close(c);
        return NULL;
    }
    return c;
}

void conn_close(struct conn *c) {
    loop_unwatch(c);  // close 전에 해야 한다
    close(c->fd);
    free(c->wbuf);
    if (c->prev) c->prev->next = c->next;
    else head = c->next;
    if (c->next) c->next->prev = c->prev;
    conn_count--;
    fprintf(stderr, "close fd=%d conn_count=%d\n", c->fd, conn_count);
    free(c);
}

int conn_on_readable(struct conn *c) {
    ssize_t n = recv(c->fd, c->rbuf + c->rlen, RBUF_SIZE - c->rlen, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        conn_close(c);
        return -1;
    }
    if (n == 0) {
        conn_close(c);
        return -1;
    }
    c->rlen += (size_t)n;
    c->last_active = time(NULL);
    fprintf(stderr, "fd=%d recv %zd bytes\n", c->fd, n);
    return conn_process(c);
}

// send 한 번. -1: 닫힘, 0: 남음(WRITING 유지), 1: 다 보내고 READING으로 돌아옴
static int conn_send(struct conn *c) {
    ssize_t n = send(c->fd, c->wbuf + c->woff, c->wlen - c->woff, 0);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            conn_close(c);
            return -1;
        }
    } else {
        c->woff += (size_t)n;
        c->last_active = time(NULL);
        fprintf(stderr, "fd=%d send %zd bytes\n", c->fd, n);
    }
    if (c->woff < c->wlen) {
        if (!(c->events & EPOLLOUT)) {
            if (loop_watch(c, EPOLLOUT) < 0) {
                conn_close(c);
                return -1;
            }
            fprintf(stderr, "fd=%d EPOLLOUT on\n", c->fd);
        }
        return 0;
    }
    free(c->wbuf);
    c->wbuf = NULL;
    if (!c->keep_alive) {
        conn_close(c);
        return -1;
    }
    c->state = CONN_READING;
    if (c->events != EPOLLIN) {
        if (loop_watch(c, EPOLLIN) < 0) {
            conn_close(c);
            return -1;
        }
        fprintf(stderr, "fd=%d EPOLLOUT off\n", c->fd);
    }
    return 1;
}

// rbuf의 완성된 요청을 차례로 처리한다. 재귀 대신 루프로 파이프라이닝을 처리한다
static int conn_process(struct conn *c) {
    for (;;) {
        ssize_t n = http_request_len(c->rbuf, c->rlen);
        if (n == 0) break;
        if (n < 0 || http_handle(c->rbuf, (size_t)n, &c->wbuf, &c->wlen,
                                 &c->keep_alive) < 0) {
            conn_close(c);
            return -1;
        }
        c->rlen -= (size_t)n;
        memmove(c->rbuf, c->rbuf + n, c->rlen);
        c->woff = 0;
        c->state = CONN_WRITING;
        int r = conn_send(c);
        if (r <= 0) return r;
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
        if (now - c->last_active > IDLE_TIMEOUT_SEC) {
            fprintf(stderr, "fd=%d idle timeout\n", c->fd);
            conn_close(c);
        }
    }
}

int conn_on_writable(struct conn *c) {
    int r = conn_send(c);
    return r == 1 ? conn_process(c) : r;
}
