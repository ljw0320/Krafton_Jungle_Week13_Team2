// http.c 단위 테스트. src/http.c를 직접 포함하므로 src/*.c와 함께 링크하지 않는다.
#include "../src/http.c"

#include <sys/resource.h>

#define CAP 8192

static int passed, failed;

static void check(const char *name, long long want, long long got) {
    if (want == got) { passed++; return; }
    failed++;
    fprintf(stderr, "FAIL %s: 기대 %lld, 실제 %lld\n", name, want, got);
}

// 반환값과 status를 한 케이스로 센다
static void run(const char *name, const char *buf, size_t len, ssize_t ret, int status) {
    struct http_req req;
    ssize_t got = http_parse(buf, len, CAP, &req);
    if (got == ret && req.status == status) { passed++; return; }
    failed++;
    fprintf(stderr, "FAIL %s: 기대 %zd/%d, 실제 %zd/%d\n", name, ret, status, got, req.status);
}

#define OK(name, s)       run(name, s, sizeof(s) - 1, (ssize_t)(sizeof(s) - 1), 0)
#define WAIT(name, s)     run(name, s, sizeof(s) - 1, 0, 0)
#define ERR(name, s, st)  run(name, s, sizeof(s) - 1, -1, st)

#define H11 "GET / HTTP/1.1\r\n"
#define HOST "Host: x\r\n"

static void test_table(void) {
    OK("basic GET", H11 HOST "\r\n");
    OK("1.0 Host 없음", "GET / HTTP/1.0\r\n\r\n");
    OK("1.0 Host 하나", "GET / HTTP/1.0\r\nHost: x\r\n\r\n");
    OK("값 앞뒤 공백", "GET / HTTP/1.1\r\nHost: \t x \t\r\n\r\n");
    OK("헤더 이름 대소문자", "GET / HTTP/1.1\r\nHOST: x\r\n\r\n");
    OK("모르는 메서드도 형식은 정상", "BREW / HTTP/1.1\r\nHost: x\r\n\r\n");
    OK("소문자 메서드", "get / HTTP/1.1\r\nHost: x\r\n\r\n");
    OK("POST CL 5 본문 포함", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello");
    OK("CL 0", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");

    WAIT("빈 입력", "");
    WAIT("요청 라인 도중", "GET / HT");
    WAIT("헤더 도중", H11 HOST);
    WAIT("끝의 CR만 옴", H11 HOST "\r");
    WAIT("본문 덜 옴", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhel");
    WAIT("본문 0바이트 옴", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n");

    ERR("bare LF", "GET / HTTP/1.1\nHost: x\n\n", 400);
    ERR("bare LF 하나", H11 "Host: x\n\r\n", 400);
    ERR("bare CR", "GET / HTTP/1.1\rHost: x\r\n\r\n", 400);
    ERR("CR 뒤 CR", "GET / HTTP/1.1\r\r\nHost: x\r\n\r\n", 400);
    ERR("헤더 안 NUL", "GET / HTTP/1.1\r\nHost: x\0y\r\n\r\n", 400);
    ERR("요청 라인 안 NUL", "GET /\0 HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ERR("SP 2개", "GET  / HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ERR("대상 뒤 SP 2개", "GET /  HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ERR("버전 없음", "GET /\r\nHost: x\r\n\r\n", 400);
    ERR("버전 없음 SP만", "GET / \r\nHost: x\r\n\r\n", 400);
    ERR("HTTX/1.1", "GET / HTTX/1.1\r\nHost: x\r\n\r\n", 400);
    ERR("소문자 http", "GET / http/1.1\r\nHost: x\r\n\r\n", 400);
    ERR("HTTP/1.10", "GET / HTTP/1.10\r\nHost: x\r\n\r\n", 400);
    ERR("HTTP/1.", "GET / HTTP/1.\r\nHost: x\r\n\r\n", 400);
    ERR("버전 뒤 SP", "GET / HTTP/1.1 \r\nHost: x\r\n\r\n", 400);
    ERR("대상 absolute-form", "GET http://x/ HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ERR("대상 *", "GET * HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ERR("메서드 비 tchar", "GE(T / HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ERR("메서드 없음", " / HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ERR("앞의 빈 줄", "\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    ERR("빈 줄만", "\r\n\r\n", 400);
    ERR("빈 줄 하나만 옴", "\r\n", 400);
    ERR("Host 앞 공백", "GET / HTTP/1.1\r\nHost : x\r\n\r\n", 400);
    ERR("공백으로 시작하는 줄", "GET / HTTP/1.1\r\nHost: x\r\n X: y\r\n\r\n", 400);
    ERR("TAB으로 시작하는 줄", "GET / HTTP/1.1\r\nHost: x\r\n\tX: y\r\n\r\n", 400);
    ERR("콜론 없는 줄", "GET / HTTP/1.1\r\nHost: x\r\nfoo\r\n\r\n", 400);
    ERR("빈 헤더 이름", "GET / HTTP/1.1\r\nHost: x\r\n: y\r\n\r\n", 400);
    ERR("1.1 Host 없음", "GET / HTTP/1.1\r\n\r\n", 400);
    ERR("1.1 Host 2개", "GET / HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n", 400);
    ERR("1.0 Host 2개", "GET / HTTP/1.0\r\nHost: x\r\nHost: y\r\n\r\n", 400);
    ERR("CL +5", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: +5\r\n\r\n", 400);
    ERR("CL -1", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n", 400);
    ERR("CL 0x5", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0x5\r\n\r\n", 400);
    ERR("CL 5 5", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5 5\r\n\r\n", 400);
    ERR("CL 빈 값", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length:\r\n\r\n", 400);
    ERR("CL 공백만", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length:  \r\n\r\n", 400);
    ERR("CL 20자리 오버플로", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999\r\n\r\n", 400);
    ERR("CL SIZE_MAX+1", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 18446744073709551616\r\n\r\n", 400);
    ERR("CL 2번 같은 값", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello", 400);
    ERR("CL 2번 다른 값", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", 400);
    ERR("TE + CL", "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n", 400);

    ERR("HTTP/2.0", "GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505);
    ERR("HTTP/1.2", "GET / HTTP/1.2\r\nHost: x\r\n\r\n", 505);
    ERR("HTTP/0.9", "GET / HTTP/0.9\r\nHost: x\r\n\r\n", 505);
    ERR("505가 헤더 검사보다 앞", "GET / HTTP/2.0\r\nbad\r\n\r\n", 505);

    ERR("TE chunked", "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", 501);
    ERR("TE gzip, chunked", "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", 501);
    ERR("TE 이름 대소문자", "GET / HTTP/1.1\r\nHost: x\r\ntransfer-encoding: x\r\n\r\n", 501);

    ERR("CL 1000000 본문 없음", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1000000\r\n\r\n", 413);
    ERR("모르는 메서드 CL 1000000", "FOO / HTTP/1.1\r\nHost: x\r\nContent-Length: 1000000\r\n\r\n", 413);
    ERR("CL SIZE_MAX", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 18446744073709551615\r\n\r\n", 413);
}

// 값 필드 확인
static void test_fields(void) {
    struct http_req r;
    const char *a = "GET /a.png?v=3 HTTP/1.0\r\nHost: x\r\nConnection: Keep-Alive, Upgrade\r\n\r\n";
    check("fields ret", (long long)strlen(a), http_parse(a, strlen(a), CAP, &r));
    check("fields method", M_GET, r.method);
    check("fields minor", 0, r.minor);
    check("fields target_len", 10, (long long)r.target_len);
    check("fields target", 0, memcmp(r.target, "/a.png?v=3", 10));
    check("fields keep_alive", 1, r.conn_keep_alive);
    check("fields close", 0, r.conn_close);
    check("fields head_len", (long long)strlen(a), (long long)r.head_len);
    check("fields has_cl", 0, r.has_cl);

    const char *b = "POST /x HTTP/1.1\r\nHost: x\r\nconnection: foo,  CLOSE \r\nContent-Length: 3\r\n\r\nabcGET";
    ssize_t n = (ssize_t)(strlen(b) - 3);
    check("fields2 ret", n, http_parse(b, strlen(b), CAP, &r));
    check("fields2 method", M_POST, r.method);
    check("fields2 minor", 1, r.minor);
    check("fields2 close", 1, r.conn_close);
    check("fields2 keep_alive", 0, r.conn_keep_alive);
    check("fields2 has_cl", 1, r.has_cl);
    check("fields2 cl", 3, (long long)r.content_length);
    check("fields2 head_len", n - 3, (long long)r.head_len);

    const char *ms[] = {"HEAD", "PUT", "DELETE", "CONNECT", "OPTIONS", "TRACE"};
    for (size_t i = 0; i < sizeof(ms) / sizeof(ms[0]); i++) {
        char req[64];
        int l = snprintf(req, sizeof(req), "%s / HTTP/1.1\r\nHost: x\r\n\r\n", ms[i]);
        http_parse(req, (size_t)l, CAP, &r);
        check(ms[i], M_KNOWN, r.method);
    }
    const char *c = "BREW / HTTP/1.1\r\nHost: x\r\n\r\n";
    http_parse(c, strlen(c), CAP, &r);
    check("BREW", M_UNKNOWN, r.method);
    c = "get / HTTP/1.1\r\nHost: x\r\n\r\n";
    http_parse(c, strlen(c), CAP, &r);
    check("get", M_UNKNOWN, r.method);
}

// 헤더 + 본문 한도 8192 경계, 414/431
static void test_limits(void) {
    static char buf[CAP + 64];
    const char *fmt = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n";
    size_t hl = strlen("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: \r\n\r\n") + 4;
    size_t n = CAP - hl;                       // 헤더 + 본문 = 8192
    for (int k = 0; k < 2; k++, n++) {         // k=1: 8193
        int l = snprintf(buf, sizeof(buf), fmt, n);
        memset(buf + l, 'a', CAP - (size_t)l);
        struct http_req r;
        ssize_t got = http_parse(buf, CAP, CAP, &r);
        if (k == 0) {
            check("8192 ret", CAP, got);
            check("8192 status", 0, r.status);
            check("8192 head_len", l, (long long)r.head_len);
            check("8191 대기", 0, http_parse(buf, CAP - 1, CAP, &r));
        } else {
            check("8193 ret", -1, got);
            check("8193 status", 413, r.status);
        }
    }

    memset(buf, 'a', CAP);
    memcpy(buf, "GET /", 5);
    struct http_req r;
    check("8192 CRLF 없음 ret", -1, http_parse(buf, CAP, CAP, &r));
    check("8192 CRLF 없음 status", 414, r.status);
    check("8191 CRLF 없음 대기", 0, http_parse(buf, CAP - 1, CAP, &r));

    memset(buf, 'a', CAP);
    memcpy(buf, "GET / HTTP/1.1\r\nX: ", 19);
    check("8192 헤더 끝 없음 ret", -1, http_parse(buf, CAP, CAP, &r));
    check("8192 헤더 끝 없음 status", 431, r.status);
    check("8191 헤더 끝 없음 대기", 0, http_parse(buf, CAP - 1, CAP, &r));
}

// 정상 요청의 모든 앞부분은 0, 전체는 길이
static void prefixes(const char *name, const char *s) {
    size_t n = strlen(s);
    struct http_req r;
    int bad = 0;
    for (size_t l = 1; l < n; l++)
        if (http_parse(s, l, CAP, &r) != 0 || r.status != 0) bad++;
    check(name, 0, bad);
    check(name, (long long)n, http_parse(s, n, CAP, &r));
}

static void check_str(const char *name, const char *want, const char *got) {
    if (strcmp(want, got) == 0) { passed++; return; }
    failed++;
    fprintf(stderr, "FAIL %s: 기대 %s, 실제 %s\n", name, want, got);
}

// 디코딩: 성공이면 결과와 끝 '/' 기록, 실패면 400
static void dec(const char *in, const char *want, int want_slash) {
    char *out = malloc(strlen(in) + 1);   // 크기를 딱 맞춰 ASan이 넘침을 잡게 한다
    int slash = -1;
    check(in, want ? 0 : 400, path_decode(in, strlen(in), out, &slash));
    if (want) {
        check_str(in, want, out);
        check(in, want_slash, slash);
    }
    free(out);
}

static void norm(const char *in, const char *want) {
    char *s = strdup(in);
    check(in, want ? 0 : 403, path_normalize(s));
    if (want) check_str(in, want, s);
    free(s);
}

static void test_path(void) {
    dec("/", "/", 1);
    dec("/a%20b", "/a b", 0);
    dec("/a%2fb", "/a/b", 0);
    dec("/a%2Fb", "/a/b", 0);
    dec("/%41%7e%ff%01", "/A~\xff\x01", 0);
    dec("/%252e", "/%2e", 0);
    dec("/%2500", "/%00", 0);
    dec("/a+b", "/a+b", 0);
    dec("/a.png?v=3", "/a.png", 0);
    dec("/a/?b", "/a/", 1);
    dec("/a?b/", "/a", 0);
    dec("/a?b?c%zz", "/a", 0);
    dec("/?", "/", 1);
    dec("/a%3fb", "/a?b", 0);
    dec("/%23", "/#", 0);
    dec("/docs/", "/docs/", 1);
    dec("/docs", "/docs", 0);
    dec("/docs%2f", "/docs/", 1);
    dec("/!~", "/!~", 0);
    dec("/\x80", NULL, 0);
    dec("/\xea\xb0\x80", NULL, 0);
    dec("/\x7f", NULL, 0);
    dec("/a b", NULL, 0);
    dec("/a\tb", NULL, 0);
    dec("/a#b", NULL, 0);
    dec("/a?b#c", NULL, 0);
    dec("/a?\x80", NULL, 0);
    dec("/%zz", NULL, 0);
    dec("/%4g", NULL, 0);
    dec("/%g4", NULL, 0);
    dec("/%", NULL, 0);
    dec("/%0", NULL, 0);
    dec("/%4?1", NULL, 0);
    dec("/%?41", NULL, 0);
    dec("/%00", NULL, 0);
    dec("/a%00.html", NULL, 0);
    // 길이 밖의 바이트를 보지 않는다
    char out[8];
    int slash;
    check("길이 밖 16진수", 400, path_decode("/%41", 3, out, &slash));

    norm("/", "");
    norm("//", "");
    norm("/.", "");
    norm("/a", "a");
    norm("//a", "a");
    norm("/./a", "a");
    norm("/a/", "a");
    norm("/a//b/./c/", "a/b/c");
    norm("/img/../x", "x");
    norm("/a/b/../c", "a/c");
    norm("/a/..", "");
    norm("/a/../b/../c", "c");
    norm("/notes..txt", "notes..txt");
    norm("/...", "...");
    norm("/..a/.b", "..a/.b");
    norm("/..", NULL);
    norm("/../", NULL);
    norm("/../etc/passwd", NULL);
    norm("/a/../../", NULL);
    norm("/a/../../a", NULL);
    norm("/./..", NULL);
    norm("//..", NULL);
}

static void test_mime(void) {
    static const char *const t[][2] = {
        {"/r/a.html", "text/html; charset=utf-8"}, {"/r/a.HTML", "text/html; charset=utf-8"},
        {"/r/a.css", "text/css"}, {"/r/a.js", "text/javascript"},
        {"/r/a.png", "image/png"}, {"/r/a.PNG", "image/png"},
        {"/r/a.jpg", "image/jpeg"}, {"/r/a.jpeg", "image/jpeg"}, {"/r/a.Jpeg", "image/jpeg"},
        {"/r/a.min.js", "text/javascript"},
        {"/r/a", "application/octet-stream"}, {"/r/a.", "application/octet-stream"},
        {"/v1.2/readme", "application/octet-stream"}, {"/a.html/readme", "application/octet-stream"},
        {"/r/notes..txt", "application/octet-stream"}, {"/r/a.htm", "application/octet-stream"},
        {"/r/html", "application/octet-stream"}, {"/r/a.html.bak", "application/octet-stream"},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) check_str(t[i][0], t[i][1], mime_type(t[i][0]));
}

static char base[] = "/tmp/http_testXXXXXX";

// base 아래 경로. 한 문장에서 두 번까지 쓸 수 있다
static const char *at(const char *rel) {
    static char buf[2][PATH_MAX];
    static int k;
    k = !k;
    snprintf(buf[k], sizeof(buf[k]), "%s/%s", base, rel);
    return buf[k];
}

static void put(const char *rel, const char *data) {
    FILE *f = fopen(at(rel), "w");
    if (!f) { perror(rel); exit(1); }
    fputs(data, f);
    fclose(f);
}

static void must(int r, const char *what) {
    if (r < 0) { perror(what); exit(1); }
}

// GET 하나. 200이면 Content-Type, Content-Length, 빈 본문, fd 크기를, 오류면 fd가 없는지를 본다
static void get(const char *target, int status, const char *mime, long long size) {
    struct http_req req = {.method = M_GET, .target = target, .target_len = strlen(target), .minor = 1};
    struct http_resp r;
    char name[64];
    snprintf(name, sizeof(name), "GET %.50s", target);
    check(name, 0, http_handle(&req, &r));
    check(name, status, atoi(r.head + 9));
    if (status == 200) {
        char want[256];
        snprintf(want, sizeof(want), "\r\nContent-Type: %s\r\nContent-Length: %lld\r\nConnection: keep-alive\r\n\r\n",
                 mime, size);
        const char *ct = strstr(r.head, "\r\nContent-Type");
        check_str(name, want, ct ? ct : "(없음)");
        check(name, 0, strncmp(r.head, "HTTP/1.1 200 OK\r\nDate: ", 23));
        check(name, (long long)strlen(r.head), (long long)r.head_len);
        check(name, size, (long long)r.file_len);
        check(name, size, r.file_fd < 0 ? -1 : (long long)lseek(r.file_fd, 0, SEEK_END));
        if (r.file_fd >= 0) close(r.file_fd);
    } else {
        check(name, -1, r.file_fd);
        check(name, 0, (long long)r.file_len);
    }
    free(r.head);
}

static void test_handle(void) {
    const char *html = "text/html; charset=utf-8", *bin = "application/octet-stream";
    if (!mkdtemp(base)) { perror("mkdtemp"); exit(1); }
    must(mkdir(at("www"), 0755), "mkdir");
    must(mkdir(at("www2"), 0755), "mkdir");
    put("www2/index.html", "secret");
    put("secret.txt", "secret");
    must(http_init(at("www")), "http_init");

    int before = open("/dev/null", O_RDONLY);   // 새 fd 번호가 그대로면 새지 않았다
    close(before);

    get("/", 404, NULL, 0);                     // 빈 루트

    put("www/index.html", "<p>hi</p>");
    put("www/a.css", "body{}");
    put("www/zero.png", "");
    put("www/my file.txt", "x");
    put("www/%2e", "x");
    put("www/notes..txt", "x");
    must(mkdir(at("www/docs"), 0755), "mkdir");
    put("www/docs/index.html", "docs");
    must(mkdir(at("www/img"), 0755), "mkdir");
    must(mkdir(at("www/v1.2"), 0755), "mkdir");
    put("www/v1.2/readme", "x");
    get("/", 200, html, 9);
    get("/?q", 200, html, 9);
    get("/index.html", 200, html, 9);
    get("/a.css?v=1", 200, "text/css", 6);
    get("/zero.png", 200, "image/png", 0);
    get("/my%20file.txt", 200, bin, 1);
    get("/%252e", 200, bin, 1);
    get("/notes..txt", 200, bin, 1);
    get("/v1.2/readme", 200, bin, 1);
    get("//index.html", 200, html, 9);
    get("/./index.html", 200, html, 9);
    get("/img/../index.html", 200, html, 9);
    get("/docs/", 200, html, 4);
    get("/docs%2f", 200, html, 4);
    get("/docs/?q", 200, html, 4);
    get("/docs", 404, NULL, 0);
    get("/img/", 404, NULL, 0);
    get("/img", 404, NULL, 0);
    get("/nope", 404, NULL, 0);
    get("/nope/x", 404, NULL, 0);
    get("/index.html/x", 404, NULL, 0);         // ENOTDIR
    get("/%zz", 400, NULL, 0);
    get("/a#b", 400, NULL, 0);
    get("/../secret.txt", 403, NULL, 0);
    get("/%2e%2e/secret.txt", 403, NULL, 0);
    get("/..%2fsecret.txt", 403, NULL, 0);
    get("/img/../../x", 403, NULL, 0);
    get("/%252e%252e/secret.txt", 404, NULL, 0);

    must(symlink("../secret.txt", at("www/out")), "symlink");
    must(symlink("../www2", at("www/sib")), "symlink");
    must(symlink("..", at("www/up")), "symlink");
    must(symlink("docs/index.html", at("www/in")), "symlink");
    must(symlink("docs", at("www/indir")), "symlink");
    must(symlink(".", at("www/self")), "symlink");
    must(symlink("loop", at("www/loop")), "symlink");
    must(symlink("gone", at("www/dangling")), "symlink");
    get("/out", 403, NULL, 0);
    get("/sib/index.html", 403, NULL, 0);
    get("/sib/", 403, NULL, 0);
    get("/sib", 403, NULL, 0);
    get("/up/", 403, NULL, 0);
    get("/up/www/index.html", 200, html, 9);    // 나갔다 들어와도 최종 경로가 루트 안
    get("/in", 200, html, 4);                   // MIME은 링크가 아니라 최종 경로로 판단
    get("/indir/", 200, html, 4);
    get("/self/", 200, html, 9);
    get("/loop", 403, NULL, 0);                 // ELOOP
    get("/dangling", 404, NULL, 0);

    must(mkfifo(at("www/fifo"), 0644), "mkfifo");
    must(mknod(at("www/sock"), S_IFSOCK | 0644, 0), "mknod");
    get("/fifo", 403, NULL, 0);
    get("/sock", 403, NULL, 0);                 // open이 ENXIO

    must(mkdir(at("www/d1"), 0755), "mkdir");
    must(mkdir(at("www/d1/index.html"), 0755), "mkdir");
    must(mkdir(at("www/d2"), 0755), "mkdir");
    must(symlink("../../secret.txt", at("www/d2/index.html")), "symlink");
    must(mkdir(at("www/d3"), 0755), "mkdir");
    must(mkfifo(at("www/d3/index.html"), 0644), "mkfifo");
    must(mkdir(at("www/d4"), 0755), "mkdir");
    must(symlink("../../www2/index.html", at("www/d4/index.html")), "symlink");
    get("/d1/", 403, NULL, 0);
    get("/d2/", 403, NULL, 0);
    get("/d3/", 403, NULL, 0);
    get("/d4/", 403, NULL, 0);

    if (geteuid() != 0) {                       // root는 권한 검사를 받지 않는다
        put("www/noread", "x");
        must(chmod(at("www/noread"), 0), "chmod");
        must(mkdir(at("www/nox"), 0755), "mkdir");
        put("www/nox/a", "x");
        must(chmod(at("www/nox"), 0600), "chmod");
        get("/noread", 403, NULL, 0);
        get("/nox/a", 403, NULL, 0);
        must(chmod(at("www/nox"), 0755), "chmod");
    }

    // 32비트를 넘는 크기 (sparse)
    put("www/big.jpg", "");
    must(truncate(at("www/big.jpg"), 5000000001LL), "truncate");
    get("/big.jpg", 200, "image/jpeg", 5000000001LL);

    // 길이: 정규화 결과로 판단하므로 원래 대상이 길어도 정규화 뒤 짧으면 정상
    static char lng[8001];
    for (int i = 0; i < 8000; i++) lng[i] = i % 2 ? 'a' : '/';
    get(lng, 404, NULL, 0);
    memset(lng + 1, 'a', 300);                  // 세그먼트 하나가 NAME_MAX 초과
    lng[301] = '\0';
    get(lng, 404, NULL, 0);
    for (int i = 0; i < 7900; i++) lng[i] = "/."[i % 2];
    strcpy(lng + 7900, "/index.html");
    get(lng, 200, html, 9);
    // 전체 길이가 PATH_MAX - 1(받음), PATH_MAX(넘음)인 경계는 둘 다 404다
    for (size_t full = PATH_MAX - 1; full <= PATH_MAX; full++) {
        size_t n = full - root_len;
        for (size_t i = 0; i < n; i++) lng[i] = i % 2 ? 'a' : '/';
        lng[n - 1] = 'a';
        lng[n] = '\0';
        get(lng, 404, NULL, 0);
    }

    int after = open("/dev/null", O_RDONLY);
    close(after);
    check("fd 누수", before, after);

    // fd가 바닥나면 realpath는 되고 open이 EMFILE → 500
    struct rlimit old, lim;
    must(getrlimit(RLIMIT_NOFILE, &old), "getrlimit");
    lim = old;
    lim.rlim_cur = (rlim_t)before;
    must(setrlimit(RLIMIT_NOFILE, &lim), "setrlimit");
    get("/index.html", 500, NULL, 0);
    must(setrlimit(RLIMIT_NOFILE, &old), "setrlimit");

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
    check("임시 디렉토리 삭제", 0, system(cmd));
}

int main(void) {
    test_table();
    test_fields();
    test_limits();
    test_path();
    test_mime();
    test_handle();
    prefixes("prefix GET", "GET /index.html HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");
    prefixes("prefix POST", "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello");
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
