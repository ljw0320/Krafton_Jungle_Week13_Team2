#define _POSIX_C_SOURCE 200809L
#include "http_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* 파일 접근 오류를 404, 403 또는 500 상태 코드로 변환한다. */
static int file_error(void)
{
    if (errno == ENOENT)
        return 404;
    if (errno == EACCES || errno == EPERM || errno == ELOOP || errno == ENOTDIR ||
        errno == ENAMETOOLONG)
        return 403;
    return 500;
}

/* 파일 확장자로 Content-Type을 결정하며 알 수 없는 형식은 application/octet-stream으로 처리한다. */
static const char *mime_type(const char *name)
{
    const char *extension = strrchr(name, '.');
    if (!extension) return "application/octet-stream";
    static const struct { const char *extension; const char *type; } types[] = {
        {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
        {".json", "application/json"}, {".txt", "text/plain; charset=utf-8"},
        {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
        {".gif", "image/gif"}, {".svg", "image/svg+xml"}, {".ico", "image/x-icon"},
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
        if (strcasecmp(extension, types[i].extension) == 0)
            return types[i].type;
    return "application/octet-stream";
}

/* Mutates path; borrows root_fd and transfers file_fd to the caller only on success. */
/* 정적 루트에서 경로를 열고 숨김 경로·심볼릭 링크를 차단하며 일반 파일인지 검사한다. 디렉터리는 index.html을 선택한다. */
static int open_static(int root_fd, char *path, int *file_fd, struct stat *info,
                       const char **type)
{
    size_t path_length = strlen(path);
    bool trailing_slash = path_length && path[path_length - 1] == '/';
    /* Validate every component before traversal, including components after a missing path. */
    for (char *p = path; *p;) {
        while (*p == '/') ++p;
        char *end = strchr(p, '/');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        if (length && *p == '.')
            return 403;
        p += length;
    }
    int current_fd = dup(root_fd);
    if (current_fd == -1)
        return 500;
    char *save = NULL;
    char *part = strtok_r(path, "/", &save);
    const char *name = "index.html";
    while (part) {
        char *next = strtok_r(NULL, "/", &save);
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
        if (next)
            flags |= O_DIRECTORY;
        int opened = openat(current_fd, part, flags);
        if (opened == -1) {
            int status = file_error();
            close(current_fd);
            return status;
        }
        close(current_fd);
        current_fd = opened;
        name = part;
        part = next;
    }
    if (fstat(current_fd, info) == -1) {
        close(current_fd);
        return 500;
    }
    if (S_ISDIR(info->st_mode)) {
        int opened = openat(current_fd, "index.html", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        int status = opened == -1 ? file_error() : 0;
        close(current_fd);
        if (opened == -1)
            return status == 404 ? 403 : status;
        current_fd = opened;
        name = "index.html";
        if (fstat(current_fd, info) == -1) {
            close(current_fd);
            return 500;
        }
    } else if (trailing_slash) {
        close(current_fd);
        return 403;
    }
    if (!S_ISREG(info->st_mode) || info->st_size < 0) {
        close(current_fd);
        return 403;
    }
    *type = mime_type(name);
    *file_fd = current_fd;
    return 0;
}

/* 파일을 열고 응답 헤더와 파일 내용을 나누어 전송한 뒤 파일을 닫는다. */
int serve_static(int fd, int root_fd, const struct request *request, char *path)
{
    int file_fd;
    struct stat info;
    const char *type;
    int status = open_static(root_fd, path, &file_fd, &info, &type);
    if (status)
        return send_error(fd, request, status);
    int result = send_headers(fd, request, 200, type, (uintmax_t)info.st_size);
    off_t left = info.st_size;
    char buffer[16384];
    while (result == 0 && left) {
        size_t size = left < (off_t)sizeof(buffer) ? (size_t)left : sizeof(buffer);
        ssize_t n = read(file_fd, buffer, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            result = -1;
            break;
        }
        result = send_all(fd, buffer, (size_t)n);
        left -= n;
    }
    close(file_fd);
    return result;
}
