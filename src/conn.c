#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "conn.h"
#include "event_loop.h"

static struct conn *head;
static int conn_count;

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
    // 꽉 찬 채로 돌아가면 다음 recv 크기가 0이라 FIN과 구분되지 않는다
    if (c->rlen == RBUF_SIZE) {
        conn_close(c);
        return -1;
    }
    return 0;
}

int conn_on_writable(struct conn *c) {
    (void)c;
    return 0;
}
