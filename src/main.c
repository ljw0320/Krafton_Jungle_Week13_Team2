#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    long port = 0;
    char *end = argv[argc > 1 ? 1 : 0];
    if (argc == 2) {
        errno = 0;
        port = strtol(argv[1], &end, 10);
    }
    if (argc != 2 || end == argv[1] || *end != '\0' || errno != 0 || port < 1 || port > 65535) {
        fprintf(stderr, "usage: ./server <port>\n");
        return 1;
    }
    printf("port=%ld\n", port);
    return 0;
}
