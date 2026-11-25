#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "server.h"

int server_listen(uint32_t address, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) { perror("setsockopt"); goto fail; }

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(address) };
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); goto fail; }
    if (listen(fd, SOMAXCONN) < 0) { perror("listen"); goto fail; }

    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) { perror("fcntl"); goto fail; }
    if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) { perror("fcntl"); goto fail; }
    return fd;

fail:
    close(fd);
    return -1;
}
