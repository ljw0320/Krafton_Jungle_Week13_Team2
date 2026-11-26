#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/server.h"
#include "live_wire.h"

#define BACKENDS 2

struct peer {
    struct live_record input;
    size_t offset;
    int writing;
};

static int relay(int listener) {
    struct pollfd fds[1 + BACKENDS] = {{.fd = listener, .events = POLLIN}};
    struct peer peers[1 + BACKENDS] = {0};
    struct live_record message = {0};
    int next = 0;
    for (int i = 1; i <= BACKENDS; i++) fds[i].fd = -1;
    for (;;) {
        int busy = 0;
        for (int i = 1; i <= BACKENDS; i++) busy |= peers[i].writing;
        for (int i = 1; i <= BACKENDS; i++)
            fds[i].events = peers[i].writing ? POLLOUT : busy ? 0 : POLLIN;
        if (poll(fds, 1 + BACKENDS, -1) < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        int first = next;
        for (int step = 0; step < BACKENDS; step++) {
            int i = 1 + (first + step) % BACKENDS;
            short events = fds[i].revents;
            if (!events) continue;
            struct peer *peer = &peers[i];
            if (!(events & (POLLERR | POLLHUP | POLLNVAL))) {
                if (events & POLLOUT) {
                    int rc = live_write(fds[i].fd, &message, &peer->offset);
                    if (rc >= 0) {
                        peer->writing = !rc;
                        continue;
                    }
                } else if (!busy && (events & POLLIN)) {
                    int rc = live_read(fds[i].fd, &peer->input);
                    if (!rc) continue;
                    if (rc > 0) {
                        /* Finish this fanout before selecting the next sender. */
                        message = peer->input;
                        peer->input.used = 0;
                        for (int j = 1; j <= BACKENDS; j++) {
                            peers[j].writing = fds[j].fd >= 0;
                            peers[j].offset = 0;
                        }
                        busy = 1;
                        next = i % BACKENDS;
                        continue;
                    }
                } else continue;
            }
            close(fds[i].fd);
            fds[i].fd = -1;
            *peer = (struct peer){0};
        }
        if (fds[0].revents & POLLIN) {
            int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK);
            if (fd < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                return -1;
            }
            int i = 1, one = 1;
            while (i <= BACKENDS && fds[i].fd >= 0) i++;
            if (i > BACKENDS || setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0 ||
                send(fd, "+", 1, 0) != 1) close(fd);
            else fds[i].fd = fd;
        }
    }
}

int main(int argc, char **argv) {
    int port = argc == 2 ? live_port(argv[1]) : -1;
    if (port < 0) {
        fprintf(stderr, "usage: ./livehub <port>\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    int listener = server_listen(INADDR_LOOPBACK, port);
    if (listener < 0) return 1;
    if (relay(listener) < 0) perror("livehub");
    close(listener);
    return 1;
}
