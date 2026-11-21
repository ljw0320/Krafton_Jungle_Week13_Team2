#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "server/server.h"

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
    if (argc > 6) {
        fprintf(stderr, "Usage: %s [port [root [max_children [idle_seconds [grace_seconds]]]]]\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = argc > 1 ? parse_positive_integer(argv[1], 65535) : 8080; /* 서버 포트 */
    const char *root = argc > 2 ? argv[2] : "public";
    int limit = argc > 3 ? parse_positive_integer(argv[3], 4096) : 64; /* 최대 자식 수 */
    int idle = argc > 4 ? parse_positive_integer(argv[4], 86400) : 5; /* 다음 요청 대기 제한 시간 */
    int grace = argc > 5 ? parse_positive_integer(argv[5], 3600) : 5; /* 종료 시 자식에게 주는 유예 시간 */
    if (port < 0 || limit < 0 || idle < 0 || grace < 0)
        return EXIT_FAILURE;

    return server_run(port, root, limit, idle, grace);
}
