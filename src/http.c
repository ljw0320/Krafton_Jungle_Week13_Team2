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

// Connection 값 하나의 쉼표 토큰 중에 name이 있나
static int has_token(const char *v, const char *end, const char *name, size_t n) {
    while (v <= end) {
        const char *comma = memchr(v, ',', (size_t)(end - v));
        const char *te = comma ? comma : end;
        while (v < te && (*v == ' ' || *v == '\t')) v++;
        const char *t = te;
        while (t > v && (t[-1] == ' ' || t[-1] == '\t')) t--;
        if ((size_t)(t - v) == n && strncasecmp(v, name, n) == 0) return 1;
        if (!comma) break;
        v = comma + 1;
    }
    return 0;
}

// 헤더 끝(\r\n\r\n)을 찾으며 bare LF, bare CR, NUL, 앞의 빈 줄을 거른다. 어기면 -1.
// 0이면 head_len(끝을 못 찾으면 0)과 첫 줄의 길이 line_len(첫 줄이 덜 왔으면 0)을 채운다
static int head_end(const char *buf, size_t len, size_t *head_len, size_t *line_len) {
    *head_len = *line_len = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0) return -1;
        if (c == '\r') {
            if (i + 1 < len && buf[i + 1] != '\n') return -1;
        } else if (c == '\n') {
            if (i == 0 || buf[i - 1] != '\r') return -1;
            if (!*line_len) {
                if (i == 1) return -1;   // 앞의 빈 줄
                *line_len = i - 1;
            }
            if (i >= 3 && memcmp(buf + i - 3, "\r\n\r\n", 4) == 0) {
                *head_len = i + 1;
                return 0;
            }
        }
    }
    return 0;
}

struct hdr { const char *name; size_t nlen; const char *v, *ve; };   // 값 [v, ve)은 앞뒤 공백을 뺐다

// *h의 헤더 한 줄을 읽고 다음 줄로 옮긴다. 1: 읽음, 0: 빈 줄(끝), -1: 형식 오류
static int hdr_next(const char **h, const char *hend, struct hdr *o) {
    const char *p = *h;
    if (p >= hend || *p == '\r') return 0;
    const char *e = memchr(p, '\r', (size_t)(hend - p));
    if (*p == ' ' || *p == '\t') return -1;   // obs-fold
    const char *colon = memchr(p, ':', (size_t)(e - p));
    if (!colon || colon == p) return -1;
    for (const char *q = p; q < colon; q++)
        if (!tchar((unsigned char)*q)) return -1;   // 콜론 앞 공백 포함
    o->name = p;
    o->nlen = (size_t)(colon - p);
    o->v = colon + 1;
    o->ve = e;
    while (o->v < o->ve && (*o->v == ' ' || *o->v == '\t')) o->v++;
    while (o->ve > o->v && (o->ve[-1] == ' ' || o->ve[-1] == '\t')) o->ve--;
    *h = e + 2;
    return 1;
}

// Content-Length 값. 숫자만이고 넘치지 않으면 0, 아니면 -1
static int parse_cl(const char *v, const char *end, size_t *out) {
    if (v == end) return -1;
    size_t cl = 0;
    for (; v < end; v++) {
        if (!isdigit((unsigned char)*v)) return -1;
        size_t d = (size_t)(*v - '0');
        if (cl > (SIZE_MAX - d) / 10) return -1;
        cl = cl * 10 + d;
    }
    *out = cl;
    return 0;
}

ssize_t http_parse(const char *buf, size_t len, size_t cap, struct http_req *req) {
    memset(req, 0, sizeof(*req));

    // 1. 헤더 끝
    size_t head_len, line_len;
    if (head_end(buf, len, &head_len, &line_len) < 0) return perr(req, 400);
    // 2. 헤더 끝이 없다
    if (!head_len) {
        if (len >= cap) return perr(req, line_len ? 431 : 414);
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
    struct hdr f;
    int got;
    while ((got = hdr_next(&h, hend, &f)) > 0) {
        if (ieq(f.name, f.nlen, "host")) hosts++;
        else if (ieq(f.name, f.nlen, "transfer-encoding")) has_te = 1;
        else if (ieq(f.name, f.nlen, "connection")) {
            if (has_token(f.v, f.ve, "close", 5)) req->conn_close = 1;
            if (has_token(f.v, f.ve, "keep-alive", 10)) req->conn_keep_alive = 1;
        } else if (ieq(f.name, f.nlen, "content-length")) {
            if (req->has_cl || parse_cl(f.v, f.ve, &req->content_length) < 0) return perr(req, 400);
            req->has_cl = 1;
        }
    }
    if (got < 0) return perr(req, 400);
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
    case 502: return "Bad Gateway";
    case 504: return "Gateway Timeout";
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

// 0 또는 501/411/405
static int method_check(const struct http_req *req, int allow_post) {
    if (req->method == M_UNKNOWN) return 501;
    if (req->method == M_POST && !req->has_cl) return 411;
    if (req->method == M_GET || (req->method == M_POST && allow_post)) return 0;
    return 405;
}

int http_keep_alive(const struct http_req *req) {
    return req->minor == 1 ? !req->conn_close : req->conn_keep_alive && !req->conn_close;
}

int http_handle(const struct http_req *req, struct http_resp *resp) {
    int status = req->status, fd = -1;
    off_t size = 0;
    const char *mime = "text/html; charset=utf-8";
    if (!status) {
        status = method_check(req, 0);
        if (!status) status = path_resolve(req->target, req->target_len, &fd, &size, &mime);
    }

    int keep;
    switch (status) {
    case 400: case 408: case 411: case 413: case 414: case 431:
    case 500: case 501: case 502: case 504: case 505:
        keep = 0;
        break;
    default:
        keep = http_keep_alive(req);
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
                     status != 405 ? "" : req->backend ? "Allow: GET, POST\r\n" : "Allow: GET\r\n", body);
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

struct outbuf { char *p; size_t cap, n; int full; };

// 자리가 모자라면 full을 켜고, 그 뒤로는 아무것도 쓰지 않는다
static void put_n(struct outbuf *o, const char *s, size_t n) {
    if (o->full || n > o->cap - o->n) {
        o->full = 1;
        return;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
}

static void put_str(struct outbuf *o, const char *s) {
    put_n(o, s, strlen(s));
}

static void put_hdr(struct outbuf *o, const struct hdr *f) {
    put_n(o, f->name, f->nlen);
    put_str(o, ": ");
    put_n(o, f->v, (size_t)(f->ve - f->v));
    put_str(o, "\r\n");
}

static void put_cl(struct outbuf *o, size_t cl) {
    char line[48];
    snprintf(line, sizeof(line), "Content-Length: %zu\r\n", cl);
    put_str(o, line);
}

// 탭을 뺀 제어 문자나 0x7F가 있나
static int has_ctl(const char *s, const char *end) {
    for (; s < end; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c < 0x20 && c != '\t') || c == 0x7F) return 1;
    }
    return 0;
}

// 헤더 [h, hend)의 Connection 줄 어딘가에 f의 이름이 적혀 있나
static int in_connection(const char *h, const char *hend, const struct hdr *f) {
    struct hdr c;
    while (hdr_next(&h, hend, &c) > 0)
        if (ieq(c.name, c.nlen, "connection") && has_token(c.v, c.ve, f->name, f->nlen)) return 1;
    return 0;
}

// f의 이름이 NULL로 끝나는 목록에 있나
static int name_in(const struct hdr *f, const char *const *list) {
    for (; *list; list++)
        if (ieq(f->name, f->nlen, *list)) return 1;
    return 0;
}

// 대상의 문법과, 디코딩한 경로의 ".." 세그먼트를 검사한다. 0 또는 400/500
static int target_check(const char *target, size_t len) {
    char *path = malloc(len + 1);
    if (!path) {
        perror("malloc");
        return 500;
    }
    int slash, status = path_decode(target, len, path, &slash);
    for (char *p = path; !status && *p; ) {
        size_t seg = strcspn(p, "/");
        if (seg == 2 && memcmp(p, "..", 2) == 0) status = 400;
        p += seg + (p[seg] == '/');
    }
    free(path);
    return status;
}

static int host_ok(const struct hdr *f) {
    for (const char *q = f->v; q < f->ve; q++)
        if (!isalnum((unsigned char)*q) && !strchr("-._~%!$&'()*+,;=:[]", *q)) return 0;
    return f->v < f->ve;
}

int http_proxy_request(const struct http_req *req, const char *buf, const char *host,
                       char *out, size_t cap, size_t *out_len) {
    static const char *const drop[] = {
        "host", "content-length",   // 새로 만든다
        "connection", "keep-alive", "proxy-connection", "te", "upgrade", "expect", "trailer",
        "forwarded", "x-real-ip", NULL,
    };
    int status = method_check(req, 1);
    if (!status) status = target_check(req->target, req->target_len);
    if (status) return status;

    const char *hend = buf + req->head_len;
    const char *first = (const char *)memchr(buf, '\n', req->head_len) + 1, *h = first;
    struct hdr f, host_hdr = {"Host", 4, host, host + strlen(host)};
    while (hdr_next(&h, hend, &f) > 0) {
        if (has_ctl(f.v, f.ve)) return 400;
        if (ieq(f.name, f.nlen, "host")) {
            if (!host_ok(&f)) return 400;
            host_hdr.v = f.v;
            host_hdr.ve = f.ve;
        }
    }

    struct outbuf o = {out, cap, 0, 0};
    put_n(&o, buf, (size_t)(req->target + req->target_len - buf));   // 메서드 SP 대상
    put_str(&o, req->minor ? " HTTP/1.1\r\n" : " HTTP/1.0\r\n");
    put_hdr(&o, &host_hdr);
    put_str(&o, "Connection: close\r\n");
    if (req->has_cl) put_cl(&o, req->content_length);
    for (h = first; hdr_next(&h, hend, &f) > 0; ) {
        if (name_in(&f, drop) || in_connection(first, hend, &f)) continue;
        if (f.nlen >= 12 && strncasecmp(f.name, "x-forwarded-", 12) == 0) continue;
        put_hdr(&o, &f);
    }
    put_str(&o, "\r\n");
    put_n(&o, hend, req->content_length);
    if (o.full) return 500;
    *out_len = o.n;
    return 0;
}

// 상태 라인이 HTTP/1.0 또는 1.1, SP, 100~599, (SP 이유)이면 상태 코드, 아니면 -1
static int status_line(const char *s, size_t len) {
    if (len < 12 || memcmp(s, "HTTP/1.", 7) != 0 || (s[7] != '0' && s[7] != '1') || s[8] != ' ') return -1;
    for (int i = 9; i < 12; i++)
        if (!isdigit((unsigned char)s[i])) return -1;
    if (len > 12 && s[12] != ' ') return -1;
    if (has_ctl(s + 12, s + len)) return -1;
    int status = (s[9] - '0') * 100 + (s[10] - '0') * 10 + (s[11] - '0');
    return status >= 100 && status <= 599 ? status : -1;
}

ssize_t http_proxy_response(const char *buf, size_t len, int minor, int keep,
                            char *out, size_t cap, struct http_upresp *r) {
    static const char *const drop[] = {
        "content-length",   // 새로 만든다
        "connection", "keep-alive", "proxy-connection", "upgrade", NULL,
    };
    static const char *const stay[] = {"transfer-encoding", "trailer", NULL};   // Connection에 적혀 있어도 남긴다
    memset(r, 0, sizeof(*r));

    size_t head_len, line_len, cl = 0;
    if (head_end(buf, len < HTTP_HEAD_MAX ? len : HTTP_HEAD_MAX, &head_len, &line_len) < 0) return -1;
    if (!head_len) return len >= HTTP_HEAD_MAX ? -1 : 0;
    int status = status_line(buf, line_len);
    if (status < 0 || status == 101) return -1;

    const char *first = buf + line_len + 2, *hend = buf + head_len, *h = first;
    int has_cl = 0, has_te = 0, got;
    struct hdr f;
    while ((got = hdr_next(&h, hend, &f)) > 0) {
        if (has_ctl(f.v, f.ve)) return -1;
        if (ieq(f.name, f.nlen, "transfer-encoding")) has_te = 1;
        else if (ieq(f.name, f.nlen, "content-length")) {
            if (has_cl || parse_cl(f.v, f.ve, &cl) < 0) return -1;
            has_cl = 1;
        }
    }
    if (got < 0) return -1;
    if (has_te && (has_cl || minor == 0 || buf[7] == '0')) return -1;
    r->status = status;
    if (status < 200) return (ssize_t)head_len;

    int no_body = status == 204 || status == 304;
    r->has_len = no_body || has_cl;   // TE와 CL이 함께 있는 응답은 위에서 걸렀다
    r->body_len = no_body ? 0 : cl;
    r->keep_alive = r->has_len && keep;

    struct outbuf o = {out, cap, 0, 0};
    put_str(&o, "HTTP/1.1");
    put_n(&o, buf + 8, line_len - 8);
    put_str(&o, line_len == 12 ? " \r\n" : "\r\n");   // 이유가 없어도 SP는 있어야 한다
    put_str(&o, r->keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
    if (has_cl && status != 204) put_cl(&o, cl);
    for (h = first; hdr_next(&h, hend, &f) > 0; ) {
        if (name_in(&f, drop)) continue;
        if (no_body && ieq(f.name, f.nlen, "transfer-encoding")) continue;
        if (!name_in(&f, stay) && in_connection(first, hend, &f)) continue;
        put_hdr(&o, &f);
    }
    put_str(&o, "\r\n");
    if (o.full) return -1;
    r->out_len = o.n;
    return (ssize_t)head_len;
}
