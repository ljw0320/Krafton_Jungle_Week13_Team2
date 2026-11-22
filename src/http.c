#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"

#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

#define BIG_SIZE 8388608

static char root_real[PATH_MAX];
static size_t root_len;

int http_init(const char *root) {
    if (!realpath(root, root_real)) {
        fprintf(stderr, "realpath: %s: %s\n", root, strerror(errno));
        return -1;
    }
    struct stat st;
    if (stat(root_real, &st) < 0) {
        fprintf(stderr, "stat: %s: %s\n", root, strerror(errno));
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "root: %s: Not a directory\n", root);
        return -1;
    }
    if (strcmp(root_real, "/") == 0) {
        fprintf(stderr, "root: /: not allowed\n");
        return -1;
    }
    root_len = strlen(root_real);
    return 0;
}

// stub: 헤더 끝까지만 요청으로 본다. HTTP 파서로 바꿀 때 본문 길이를 더한다
ssize_t http_request_len(const char *buf, size_t len) {
    const char *end = memmem(buf, len, "\r\n\r\n", 4);
    return end ? end - buf + 4 : 0;
}

// stub: 요청이 GET /big으로 시작하면 8MB, 아니면 hello\n (부분 send 테스트용)
int http_handle(const char *req, size_t req_len,
                char **out, size_t *out_len, int *keep_alive) {
    int big = req_len >= 8 && memcmp(req, "GET /big", 8) == 0;
    size_t body_len = big ? BIG_SIZE : 6;
    char head[128];
    int hlen = snprintf(head, sizeof(head),
                        "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                        "Connection: keep-alive\r\n\r\n", body_len);
    char *buf = malloc((size_t)hlen + body_len);
    if (!buf) {
        perror("malloc");
        return -1;
    }
    memcpy(buf, head, (size_t)hlen);
    if (big) memset(buf + hlen, 'a', body_len);
    else memcpy(buf + hlen, "hello\n", body_len);
    *out = buf;
    *out_len = (size_t)hlen + body_len;
    *keep_alive = 1;
    return 0;
}
