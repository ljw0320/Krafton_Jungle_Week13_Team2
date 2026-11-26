#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "event_loop.h"
#include "http.h"
#include "route.h"
#include "server.h"

int main(int argc, char **argv) {
    long port = 0;
    char *end = NULL;
    if (argc >= 3) {
        errno = 0;
        port = strtol(argv[1], &end, 10);
    }
    if (argc < 3 || end == argv[1] || *end != '\0' || errno != 0 || port < 1 || port > 65535) {
        fprintf(stderr, "usage: ./server <port> <root> [--loopback] [<prefix>=<ip>:<port> ...]\n");
        return 1;
    }
    if (http_init(argv[2]) < 0) exit(1);
    int local = argc > 3 && !strcmp(argv[3], "--loopback");
    for (int i = 3 + local; i < argc; i++)
        if (route_add(argv[i]) < 0) exit(1);
    signal(SIGPIPE, SIG_IGN);
    int listen_fd = server_listen(local ? INADDR_LOOPBACK : INADDR_ANY, (uint16_t)port);
    if (listen_fd < 0) exit(1);
    if (loop_run(listen_fd) < 0) exit(1);
    return 0;
}
