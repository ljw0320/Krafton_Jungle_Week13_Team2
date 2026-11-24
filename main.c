#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "server/server.h"
#include "proxy/routes.h"

static int parse_positive_integer(const char *text, int maximum)
{
    char *end;
    errno = 0;
    long n = strtol(text, &end, 10);
    if (errno || !*text || *end || n < 1 || n > maximum) {
        fprintf(stderr, "Invalid positive integer: %s\n", text);
        return -1;
    }
    return (int)n;
}

int main(int argc, char **argv)
{
    const char *routes_file = NULL;
    int backend_limit = 16;
    int positional = 1;
    while (positional < argc && strncmp(argv[positional], "--", 2) != 0)
        ++positional;
    int seen_limit = 0;
    for (int i = positional; i < argc; i += 2) {
        if (i + 1 == argc)
            goto usage;
        if (strcmp(argv[i], "--routes") == 0 && !routes_file)
            routes_file = argv[i + 1];
        else if (strcmp(argv[i], "--backend-limit") == 0 && !seen_limit) {
            seen_limit = 1;
            backend_limit = parse_positive_integer(argv[i + 1], 4096);
            if (backend_limit < 0)
                return EXIT_FAILURE;
        } else
            goto usage;
    }
    argc = positional;
    if (argc > 6) {
        goto usage;
    }

    int port = argc > 1 ? parse_positive_integer(argv[1], 65535) : 8080; /* 서버 포트 */
    const char *root = argc > 2 ? argv[2] : "public";
    int limit = argc > 3 ? parse_positive_integer(argv[3], 4096) : 64; /* 최대 자식 수 */
    int idle = argc > 4 ? parse_positive_integer(argv[4], 86400) : 5; /* 다음 요청 대기 제한 시간 */
    int grace = argc > 5 ? parse_positive_integer(argv[5], 3600) : 5; /* 종료 시 자식에게 주는 유예 시간 */
    if (port < 0 || limit < 0 || idle < 0 || grace < 0)
        return EXIT_FAILURE;

    if (routes_file && routes_load(routes_file) == -1)
        return EXIT_FAILURE;
    int result = server_run(port, root, limit, idle, grace, backend_limit);
    routes_free();
    return result;
usage:
    fprintf(stderr, "Usage: %s [port [root [max_children [idle_seconds [grace_seconds]]]]] [--routes FILE] [--backend-limit N]\n", argv[0]);
    return EXIT_FAILURE;
}
