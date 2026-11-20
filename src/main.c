#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "event_loop.h"
#include "server.h"

int main(int argc, char **argv) {
    long port = 0;
    char *end = NULL;
    if (argc == 2) {
        errno = 0;
        port = strtol(argv[1], &end, 10);
    }
    if (argc != 2 || end == argv[1] || *end != '\0' || errno != 0 || port < 1 || port > 65535) {
        fprintf(stderr, "usage: ./server <port>\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    int listen_fd = server_listen((uint16_t)port);
    if (listen_fd < 0) exit(1);
    if (loop_run(listen_fd) < 0) exit(1);
    return 0;
}
