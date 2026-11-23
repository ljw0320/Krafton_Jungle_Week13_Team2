#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>

#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

static int tchar(unsigned char c) {
    return isalnum(c) || (c && strchr("!#$%&'*+-.^_`|~", c));
}

static int ieq(const char *s, size_t n, const char *lit) {
    return strlen(lit) == n && strncasecmp(s, lit, n) == 0;
}

static ssize_t perr(struct http_req *req, int status) {
    req->status = status;
    return -1;
}

// Connection 값의 쉼표 토큰에서 close / keep-alive를 찾는다
static void conn_tokens(const char *v, const char *end, struct http_req *req) {
    while (v <= end) {
        const char *comma = memchr(v, ',', (size_t)(end - v));
        const char *te = comma ? comma : end;
        while (v < te && (*v == ' ' || *v == '\t')) v++;
        const char *t = te;
        while (t > v && (t[-1] == ' ' || t[-1] == '\t')) t--;
        if (ieq(v, (size_t)(t - v), "close")) req->conn_close = 1;
        if (ieq(v, (size_t)(t - v), "keep-alive")) req->conn_keep_alive = 1;
        if (!comma) break;
        v = comma + 1;
    }
}

ssize_t http_parse(const char *buf, size_t len, size_t cap, struct http_req *req) {
    memset(req, 0, sizeof(*req));

    // 1. 헤더 끝(\r\n\r\n)을 찾으며 bare LF, bare CR, NUL을 거른다
    size_t head_len = 0, line_len = 0;
    int have_line = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0) return perr(req, 400);
        if (c == '\r') {
            if (i + 1 < len && buf[i + 1] != '\n') return perr(req, 400);
        } else if (c == '\n') {
            if (i == 0 || buf[i - 1] != '\r') return perr(req, 400);
            if (!have_line) {
                if (i == 1) return perr(req, 400);   // 앞의 빈 줄
                have_line = 1;
                line_len = i - 1;
            }
            if (i >= 3 && memcmp(buf + i - 3, "\r\n\r\n", 4) == 0) {
                head_len = i + 1;
                break;
            }
        }
    }
    // 2. 헤더 끝이 없다
    if (!head_len) {
        if (len >= cap) return perr(req, have_line ? 431 : 414);
        return 0;
    }

    // 3. 요청 라인: 메서드 SP 대상 SP HTTP/d.d
    const char *p = buf, *le = buf + line_len;
    size_t mlen = 0;
    while (p + mlen < le && tchar((unsigned char)p[mlen])) mlen++;
    if (mlen == 0 || p + mlen >= le || p[mlen] != ' ') return perr(req, 400);
    const char *t = p + mlen + 1, *te = t;
    while (te < le && *te != ' ') te++;
    if (te == t || *t != '/' || te >= le) return perr(req, 400);
    const char *v = te + 1;
    if (le - v != 8 || memcmp(v, "HTTP/", 5) != 0 || !isdigit((unsigned char)v[5]) ||
        v[6] != '.' || !isdigit((unsigned char)v[7]))
        return perr(req, 400);

    if (mlen == 3 && memcmp(p, "GET", 3) == 0) req->method = M_GET;
    else if (mlen == 4 && memcmp(p, "POST", 4) == 0) req->method = M_POST;
    else {
        static const char *const known[] = {"HEAD", "PUT", "DELETE", "CONNECT", "OPTIONS", "TRACE"};
        req->method = M_UNKNOWN;   // 메서드는 대소문자를 구분한다
        for (size_t k = 0; k < sizeof(known) / sizeof(known[0]); k++)
            if (strlen(known[k]) == mlen && memcmp(known[k], p, mlen) == 0) req->method = M_KNOWN;
    }
    req->target = t;
    req->target_len = (size_t)(te - t);

    // 4. 버전
    if (v[5] != '1' || (v[7] != '0' && v[7] != '1')) return perr(req, 505);
    req->minor = v[7] - '0';
    req->head_len = head_len;

    // 5. 헤더
    int hosts = 0, has_te = 0;
    const char *h = buf + line_len + 2, *hend = buf + head_len;
    while (h < hend && *h != '\r') {
        const char *e = memchr(h, '\r', (size_t)(hend - h));
        if (*h == ' ' || *h == '\t') return perr(req, 400);   // obs-fold
        const char *colon = memchr(h, ':', (size_t)(e - h));
        if (!colon || colon == h) return perr(req, 400);
        for (const char *q = h; q < colon; q++)
            if (!tchar((unsigned char)*q)) return perr(req, 400);   // 콜론 앞 공백 포함
        size_t nlen = (size_t)(colon - h);
        const char *vs = colon + 1, *ve = e;
        while (vs < ve && (*vs == ' ' || *vs == '\t')) vs++;
        while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;

        if (ieq(h, nlen, "host")) hosts++;
        else if (ieq(h, nlen, "transfer-encoding")) has_te = 1;
        else if (ieq(h, nlen, "connection")) conn_tokens(vs, ve, req);
        else if (ieq(h, nlen, "content-length")) {
            if (req->has_cl || vs == ve) return perr(req, 400);
            size_t cl = 0;
            for (const char *q = vs; q < ve; q++) {
                if (!isdigit((unsigned char)*q)) return perr(req, 400);
                size_t d = (size_t)(*q - '0');
                if (cl > (SIZE_MAX - d) / 10) return perr(req, 400);
                cl = cl * 10 + d;
            }
            req->has_cl = 1;
            req->content_length = cl;
        }
        h = e + 2;
    }
    if (req->minor == 1 ? hosts != 1 : hosts > 1) return perr(req, 400);
    if (has_te && req->has_cl) return perr(req, 400);

    // 6~7. TE는 501, 본문이 한도를 넘으면 기다리지 않고 413
    if (has_te) return perr(req, 501);
    if (req->content_length > cap - head_len) return perr(req, 413);
    if (len - head_len < req->content_length) return 0;
    return (ssize_t)(head_len + req->content_length);
}

static const char *reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default:  return "Internal Server Error";
    }
}

static int hexval(unsigned char c) {
    if (isdigit(c)) return c - '0';
    return isxdigit(c) ? tolower(c) - 'a' + 10 : -1;
}

// 대상의 바이트를 검사하고 쿼리를 떼어 한 번만 디코딩한다. out은 len + 1바이트 이상. 0 또는 400
static int path_decode(const char *t, size_t len, char *out, int *slash) {
    for (size_t i = 0; i < len; i++)
        if (t[i] < 0x21 || t[i] > 0x7E || t[i] == '#') return 400;
    const char *q = memchr(t, '?', len);
    if (q) len = (size_t)(q - t);

    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (t[i] != '%') {
            out[n++] = t[i];
            continue;
        }
        if (len - i < 3) return 400;
        int hi = hexval((unsigned char)t[i + 1]), lo = hexval((unsigned char)t[i + 2]);
        if (hi < 0 || lo < 0 || hi * 16 + lo == 0) return 400;
        out[n++] = (char)(hi * 16 + lo);
        i += 2;
    }
    out[n] = '\0';
    *slash = n > 0 && out[n - 1] == '/';
    return 0;
}

// 빈 칸, '.', '..'을 정리해 앞뒤 '/'가 없는 상대 경로로 제자리에서 바꾼다. 0 또는 403
static int path_normalize(char *s) {
    size_t n = 0;
    char *p = s;
    while (*p) {
        size_t seg = strcspn(p, "/");
        char *next = p + seg + (p[seg] == '/');
        if (seg == 2 && memcmp(p, "..", 2) == 0) {
            if (n == 0) return 403;
            while (n > 0 && s[n - 1] != '/') n--;
            if (n > 0) n--;
        } else if (seg > 0 && !(seg == 1 && *p == '.')) {
            if (n > 0) s[n++] = '/';
            memmove(s + n, p, seg);
            n += seg;
        }
        p = next;
    }
    s[n] = '\0';
    return 0;
}

// full의 realpath가 루트 안인지 보고 연다. 0이면 real, fd, st가 채워졌고, 아니면 오류 코드(fd는 닫혔다)
static int path_open(const char *full, char *real, int *fd, struct stat *st) {
    if (!realpath(full, real)) {
        switch (errno) {
        case ENOENT: case ENOTDIR: case ENAMETOOLONG: return 404;
        case EACCES: case ELOOP: return 403;
        default: return 500;
        }
    }
    if (strncmp(real, root_real, root_len) != 0 || (real[root_len] != '/' && real[root_len] != '\0'))
        return 403;
    *fd = open(real, O_RDONLY | O_NONBLOCK);   // FIFO에서 멈추지 않는다
    if (*fd < 0) {
        switch (errno) {
        case ENOENT: return 404;
        case EACCES: case ENXIO: return 403;   // ENXIO: 유닉스 소켓 파일
        default: return 500;
        }
    }
    if (fstat(*fd, st) < 0) {
        close(*fd);
        return 500;
    }
    return 0;
}

static const char *mime_type(const char *path) {
    static const char *const types[][2] = {
        {".html", "text/html; charset=utf-8"}, {".css", "text/css"}, {".js", "text/javascript"},
        {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
    };
    const char *base = strrchr(path, '/');
    const char *ext = strrchr(base ? base : path, '.');
    for (size_t i = 0; ext && i < sizeof(types) / sizeof(types[0]); i++)
        if (strcasecmp(ext, types[i][0]) == 0) return types[i][1];
    return "application/octet-stream";
}

// 200이면 연 일반 파일을 fd, size, mime에 준다. 아니면 오류 코드이고 열린 fd가 없다
static int path_resolve(const char *target, size_t len, int *fd, off_t *size, const char **mime) {
    char *path = malloc(len + 1);   // 정규화 전에는 PATH_MAX보다 길 수 있다
    if (!path) {
        perror("malloc");
        return 500;
    }
    char full[PATH_MAX], real[PATH_MAX];
    int slash, n = 0;
    int status = path_decode(target, len, path, &slash);
    if (!status) status = path_normalize(path);
    if (!status) n = snprintf(full, sizeof(full), "%s/%s", root_real, path);
    free(path);
    if (status) return status;
    if (n < 0 || (size_t)n >= sizeof(full)) return 404;

    struct stat st;
    int f;
    status = path_open(full, real, &f, &st);
    if (status) return status;
    if (S_ISDIR(st.st_mode)) {
        close(f);
        if (!slash) return 404;
        n = snprintf(full, sizeof(full), "%s/index.html", real);
        if (n < 0 || (size_t)n >= sizeof(full)) return 404;
        status = path_open(full, real, &f, &st);   // index.html도 바깥 링크일 수 있다
        if (status) return status;
    }
    if (!S_ISREG(st.st_mode)) {
        close(f);
        return 403;
    }
    *fd = f;
    *size = st.st_size;
    *mime = mime_type(real);
    return 200;
}

int http_handle(const struct http_req *req, struct http_resp *resp) {
    int status = req->status, fd = -1;
    off_t size = 0;
    const char *mime = "text/html; charset=utf-8";
    if (!status) {
        if (req->method == M_UNKNOWN) status = 501;
        else if (req->method == M_POST && !req->has_cl) status = 411;
        else if (req->method != M_GET) status = 405;
        else status = path_resolve(req->target, req->target_len, &fd, &size, &mime);
    }

    int keep;
    switch (status) {
    case 400: case 408: case 411: case 413: case 414: case 431:
    case 500: case 501: case 505:
        keep = 0;
        break;
    default:
        keep = req->minor == 1 ? !req->conn_close
                               : req->conn_keep_alive && !req->conn_close;
    }

    char body[96] = "";
    long long clen = size;
    if (status != 200)
        clen = snprintf(body, sizeof(body), "<h1>%d %s</h1>\n", status, reason(status));
    char date[40];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);

    size_t cap = 512;
    char *buf = malloc(cap);
    if (!buf) {
        perror("malloc");
        if (fd >= 0) close(fd);
        return -1;
    }
    int n = snprintf(buf, cap,
                     "HTTP/1.1 %d %s\r\nDate: %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lld\r\nConnection: %s\r\n%s\r\n%s",
                     status, reason(status), date, mime, clen,
                     keep ? "keep-alive" : "close",
                     status == 405 ? "Allow: GET\r\n" : "", body);
    if (n < 0 || (size_t)n >= cap) {
        if (fd >= 0) close(fd);
        free(buf);
        return -1;
    }
    resp->head = buf;
    resp->head_len = (size_t)n;
    resp->file_fd = fd;
    resp->file_len = size;
    resp->keep_alive = keep;
    return 0;
}
