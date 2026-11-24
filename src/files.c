#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';

    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;

    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return -1;
}

/* 쿼리를 제외하고 URL을 한 번만 디코딩합니다. */
static int decode_path(const char *target, char *path)
{
    size_t length = 0;

    for (size_t i = 1; target[i] && target[i] != '?'; i++) {
        unsigned char c = (unsigned char)target[i];
        if (c == '%') {
            if (!target[i + 1] || !target[i + 2])
                return 400;
            int high = hex_digit(target[i + 1]), low = hex_digit(target[i + 2]);
            if (high < 0 || low < 0)
                return 400;
            c = (unsigned char)(high * 16 + low);
            i += 2;
        }

        if (c < 32 || c == 127 || c == '\\')
            return 400;
        if (length >= TARGET_LIMIT - sizeof("index.html"))
            return 400;
        path[length++] = (char)c;
    }

    path[length] = '\0';
    if (!length || path[length - 1] == '/')
        strcpy(path + length, "index.html");
    return 0;
}

static int open_error(void)
{
    if (errno == ENOENT || errno == ENOTDIR)
        return 404;

    if (errno == EACCES || errno == ELOOP)
        return 403;

    return 500;
}

/* 각 경로 조각을 root fd 기준으로 열어 ..와 심볼릭 링크를 차단합니다. */
static int walk_path(int root, char *path, int *status)
{
    int directory = fcntl(root, F_DUPFD_CLOEXEC, 3);
    if (directory == -1) {
        *status = 500;
        return -1;
    }

    char *part = path;
    while (true) {
        char *slash = strchr(part, '/');
        if (slash)
            *slash = '\0';
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) {
            close(directory);
            *status = 403;
            return -1;
        }

        int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
        int next = openat(directory, part, flags | (slash ? O_DIRECTORY : 0));
        *status = next == -1 ? open_error() : 200;
        close(directory);
        if (next == -1 || !slash)
            return next;
        directory = next;
        part = slash + 1;
    }
}

/* 검증된 일반 파일 fd와 응답에 사용할 크기를 반환합니다. */
int file_open(int root, const char *target, off_t *size, int *status)
{
    char path[TARGET_LIMIT];
    *status = decode_path(target, path);
    if (*status)
        return -1;

    int fd = walk_path(root, path, status);
    if (fd == -1)
        return -1;

    struct stat info;
    if (fstat(fd, &info) == -1) {
        *status = 500;
    } else if (!S_ISREG(info.st_mode) || info.st_size < 0) {
        *status = 403;
    } else {
        *size = info.st_size;
        return fd;
    }

    close(fd);
    return -1;
}

const char *file_type(const char *target)
{
    char path[TARGET_LIMIT];
    if (decode_path(target, path))
        return "application/octet-stream";

    const char *ext = strrchr(path, '.');
    if (!ext)
        return "application/octet-stream";

    if (!strcasecmp(ext, ".html") || !strcasecmp(ext, ".htm"))
        return "text/html; charset=utf-8";
    if (!strcasecmp(ext, ".css"))
        return "text/css; charset=utf-8";
    if (!strcasecmp(ext, ".js"))
        return "text/javascript; charset=utf-8";
    if (!strcasecmp(ext, ".txt"))
        return "text/plain; charset=utf-8";
    if (!strcasecmp(ext, ".svg"))
        return "image/svg+xml";
    if (!strcasecmp(ext, ".png"))
        return "image/png";
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg"))
        return "image/jpeg";
    if (!strcasecmp(ext, ".gif"))
        return "image/gif";
    if (!strcasecmp(ext, ".ico"))
        return "image/x-icon";
    return "application/octet-stream";
}
