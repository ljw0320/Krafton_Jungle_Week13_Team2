// route.c와 http.c의 프록시 함수 단위 테스트. src/http.c, src/route.c를 직접 포함하므로 src/*.c와 함께 링크하지 않는다.
#include "../src/http.c"
#include "../src/route.c"

static int passed, failed;

static void check(const char *name, long long want, long long got) {
    if (want == got) { passed++; return; }
    failed++;
    fprintf(stderr, "FAIL %s: 기대 %lld, 실제 %lld\n", name, want, got);
}

static void add(const char *arg, int want) {
    check(arg, want, route_add(arg));
}

static void find(const char *name, const char *target, size_t len, const char *want_prefix) {
    struct route *r = route_find(target, len);
    char got[32] = ""; if (r) snprintf(got, sizeof got, "%.*s", (int)r->prefix_len, r->prefix);
    if ((!r && !want_prefix) || (r && want_prefix && strcmp(got, want_prefix) == 0)) {
        passed++;
        return;
    }
    failed++;
    fprintf(stderr, "FAIL %s: 기대 %s, 실제 %s\n", name, want_prefix ? want_prefix : "NULL", r ? got : "NULL");
}

static void test_reject(void) {
    const char *bad[] = {
        "/api=127.0.0.1:8001", "api/=127.0.0.1:8001", "=127.0.0.1:8001", "/a/../b/=127.0.0.1:8001",
        "/a/./b/=127.0.0.1:8001", "//=127.0.0.1:8001", "/a//b/=127.0.0.1:8001", "/a%20/=127.0.0.1:8001",
        "/a?/=127.0.0.1:8001", "/a#/=127.0.0.1:8001", "/a b/=127.0.0.1:8001", "/a\x7F/=127.0.0.1:8001",
        "/a\x80/=127.0.0.1:8001", "/a\t/=127.0.0.1:8001", "/../=127.0.0.1:8001", "/./=127.0.0.1:8001",
        "/api/=localhost:8001", "/api/=127.0.0.1", "/api/=127.0.0.1:", "/api/=127.0.0.1:0",
        "/api/=127.0.0.1:65536", "/api/=127.0.0.1:8001x", "/api/=127.0.0.1:-1", "/api/=127.0.0.1:+1",
        "/api/=127.0.0.1: 80", "/api/=127.0.0.1:99999999999999999999", "/api/=:8001",
        "/api/=127.0.0.1.1:8001", "/api/=127.0.0.1:80:80", "/api/", "=", "",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) add(bad[i], -1);
}

static void test_accept(void) {
    check("accept /api/", 0, route_add("/api/=127.0.0.1:8001"));
    struct route *r = route_find("/api/x", 6);
    check("find 등록 직후", 1, r != NULL);
    if (r) {
        check("prefix_len", 5, (long long)r->prefix_len);
        check("addr 포트", 8001, ntohs(r->b.addr.sin_port));
        check("addr 주소", (long long)htonl(INADDR_LOOPBACK), r->b.addr.sin_addr.s_addr);
        check("name", 0, strcmp(r->b.name, "127.0.0.1:8001"));
        check("pick", 1, route_pick(r) == &r->b);
    }
    add("/api/=127.0.0.1:8002", -1);          // 같은 접두사 두 번
    add("/=127.0.0.1:65535", 0);
    add("/x/=127.0.0.1:1", 0);
    add("/p0/=127.0.0.1:000001", 0);          // 앞자리 0
}

static void test_find(void) {
    // 규칙: /api/, /, /x/. 규칙 `/`가 있으면 전부 맞으므로 `/`는 마지막에 확인한다
    find("/api/", "/api/", 5, "/api/");
    find("/api/users?x=1", "/api/users?x=1", 14, "/api/");
    find("/api/?x", "/api/?x", 7, "/api/");
    find("len보다 뒤를 보지 않음", "/api/users", 4, "/");
    find("/api", "/api", 4, "/");
    find("먼저 적은 /가 이김", "/x/y", 4, "/");
}

static void test_find_no_root(void) {
    // 규칙 `/` 없이 따로 확인하려고 배열을 비운다
    nroutes = 0;
    check("v2 뒤에 add", 0, route_add("/api/=127.0.0.1:8001"));
    check("v2 add", 0, route_add("/api/v2/=127.0.0.1:8002"));
    find("/api/v2/x는 먼저 적은 /api/", "/api/v2/x", 9, "/api/");
    find("/apix", "/apix/", 6, NULL);
    find("/api", "/api", 4, NULL);
    find("/api?x=/", "/api?x=/", 8, NULL);
    find("/%61pi/users", "/%61pi/users", 12, NULL);
    find("//api/users", "//api/users", 11, NULL);
    find("/API/users", "/API/users", 10, NULL);
    find("/", "/", 1, NULL);
    find("len 4", "/api/users", 4, NULL);
    find("len 0", "/api/", 0, NULL);
}

static void test_count(void) {
    nroutes = 0;
    static char args[MAX_ROUTES][32];
    for (int i = 0; i < MAX_ROUTES; i++) {
        char *arg = args[i];
        snprintf(arg, sizeof(args[i]), "/p%d/=127.0.0.1:%d", i, 9000 + i);
        check(arg, 0, route_add(arg));
    }
    check("17번째", -1, route_add("/p99/=127.0.0.1:9100"));
    check("16번째까지는 남음", 1, route_find("/p15/x", 6) != NULL);
}

static void want_head(const char *name, const struct http_req *req, const char *part, int keep) {
    struct http_resp r;
    if (http_handle(req, &r) < 0) { check(name, 0, -1); return; }
    check(name, 1, memmem(r.head, r.head_len, part, strlen(part)) != NULL);
    check(name, keep, r.keep_alive);
    free(r.head);
}

static void test_error_resp(void) {
    struct http_req r502 = {.status = 502, .minor = 1}, r504 = {.status = 504, .minor = 1};
    want_head("502 상태 라인", &r502, "HTTP/1.1 502 Bad Gateway\r\n", 0);
    want_head("502 close", &r502, "\r\nConnection: close\r\n", 0);
    want_head("502 본문", &r502, "\r\n\r\n<h1>502 Bad Gateway</h1>\n", 0);
    want_head("504 상태 라인", &r504, "HTTP/1.1 504 Gateway Timeout\r\n", 0);
    want_head("504 close", &r504, "\r\nConnection: close\r\n", 0);
    want_head("504 본문", &r504, "\r\n\r\n<h1>504 Gateway Timeout</h1>\n", 0);
    struct http_req b = {.status = 405, .minor = 1, .backend = 1}, s = {.status = 405, .minor = 1};
    want_head("405 백엔드", &b, "\r\nAllow: GET, POST\r\n", 1);
    want_head("405 정적", &s, "\r\nAllow: GET\r\n", 1);
}

static void test_keep_alive(void) {
    static const int want[2][2][2] = {   // [minor][close][keep-alive]
        {{0, 1}, {0, 0}},
        {{1, 1}, {0, 0}},
    };
    for (int i = 0; i < 8; i++) {
        struct http_req r = {.minor = i >> 2, .conn_close = i >> 1 & 1, .conn_keep_alive = i & 1};
        check("keep_alive", want[i >> 2][i >> 1 & 1][i & 1], http_keep_alive(&r));
    }
}

#define BACKEND "127.0.0.1:8001"
#define GET11 "GET /api/x HTTP/1.1\r\n"
#define HOST "Host: h\r\n"

// 입력과 출력을 딱 맞게 할당해 ASan이 버퍼 밖 접근을 본다. 파서를 통과하지 못하면 -100
static int preq(const char *in, size_t len, size_t cap, char **out, size_t *out_len) {
    struct http_req req;
    char *copy = malloc(len);
    memcpy(copy, in, len);
    *out = malloc(cap);
    *out_len = 0;
    ssize_t n = http_parse(copy, len, 8192, &req);
    int st = n == (ssize_t)len ? http_proxy_request(&req, copy, BACKEND, *out, cap, out_len) : -100;
    free(copy);
    return st;
}

static void same(const char *name, const char *want, size_t wn, const char *got, size_t gn) {
    if (wn == gn && memcmp(want, got, wn) == 0) { passed++; return; }
    failed++;
    fprintf(stderr, "FAIL %s:\n--- 기대\n%.*s\n--- 실제\n%.*s\n---\n", name, (int)wn, want, (int)gn, got);
}

static void req_ok(const char *name, const char *in, size_t len, const char *want, size_t wn) {
    char *out;
    size_t n;
    check(name, 0, preq(in, len, 16384, &out, &n));
    same(name, want, wn, out, n);
    free(out);
    int bad = 0;
    for (size_t cap = 0; cap <= wn; cap++) {   // 1바이트라도 모자라면 500
        bad += preq(in, len, cap, &out, &n) != (cap < wn ? 500 : 0);
        free(out);
    }
    check(name, 0, bad);
}

static void req_st(const char *name, const char *in, size_t len, int want) {
    char *out;
    size_t n;
    check(name, want, preq(in, len, 16384, &out, &n));
    free(out);
}

#define ROK(name, in, want) req_ok(name, in, sizeof(in) - 1, want, sizeof(want) - 1)
#define RST(name, in, st)   req_st(name, in, sizeof(in) - 1, st)
#define CLOSE "Connection: close\r\n"
#define OUT11 GET11 HOST CLOSE
#define POST11 "POST /api/x HTTP/1.1\r\n"

static void test_req_rewrite(void) {
    ROK("설계의 예시",
        "POST /api/users?x=1 HTTP/1.1\r\nHost: shop.com\r\nConnection: keep-alive, X-Debug\r\nX-Debug: 1\r\n"
        "X-Forwarded-For: 127.0.0.1\r\nAccept: */*\r\nContent-Length: 5\r\n\r\nhello",
        "POST /api/users?x=1 HTTP/1.1\r\nHost: shop.com\r\nConnection: close\r\nContent-Length: 5\r\n"
        "Accept: */*\r\n\r\nhello");
    ROK("뺄 헤더 전부",
        GET11 "hOsT: h\r\nCONNECTION: keep-alive\r\nkeep-Alive: timeout=5\r\nProxy-connection: x\r\ntE: trailers\r\n"
        "uPGRADE: h2c\r\nEXPECT: 100-continue\r\ntrailer: X\r\nx-forwarded-FOR: 1\r\nX-Forwarded-: 1\r\n"
        "X-FORWARDED-Proto: https\r\nX-Forwarded-Host: a\r\nforwarded: for=1\r\nx-REAL-ip: 1\r\nX-Keep: 1\r\n\r\n",
        OUT11 "X-Keep: 1\r\n\r\n");
    ROK("이름이 비슷한 것은 남음",
        GET11 HOST "X-Forwarded: 1\r\nX-Forwarde: 1\r\nTEX: 1\r\nX-Real-IPs: 1\r\nForwarde: 1\r\nHosts: 1\r\n\r\n",
        OUT11 "X-Forwarded: 1\r\nX-Forwarde: 1\r\nTEX: 1\r\nX-Real-IPs: 1\r\nForwarde: 1\r\nHosts: 1\r\n\r\n");
    ROK("순서, 글자, 여러 줄 유지", GET11 "b: 1\r\nA: 2\r\n" HOST "b: 3\r\nCookie: x=1; y=2\r\n\r\n",
        OUT11 "b: 1\r\nA: 2\r\nb: 3\r\nCookie: x=1; y=2\r\n\r\n");
    ROK("Connection 여러 줄",
        GET11 "X-A: 1\r\n" HOST "Connection: x-a\r\nX-B: 1\r\nconnection: X-B, close\r\nX-C: 1\r\n\r\n",
        OUT11 "X-C: 1\r\n\r\n");
    ROK("Connection 빈 토큰", GET11 HOST "Connection: x-a ,, X-B\r\nX-A: 1\r\nx-b: 1\r\nX-AB: 1\r\n\r\n",
        OUT11 "X-AB: 1\r\n\r\n");
    ROK("Connection 빈 값", GET11 HOST "Connection:\r\nX-A: 1\r\n\r\n", OUT11 "X-A: 1\r\n\r\n");
    ROK("Connection: Content-Length, Host, Transfer-Encoding",
        POST11 HOST "Connection: Content-Length, Host, Transfer-Encoding\r\nContent-Length: 3\r\n\r\nabc",
        POST11 HOST CLOSE "Content-Length: 3\r\n\r\nabc");
    ROK("1.0 Host 없음", "GET /api/x HTTP/1.0\r\nA: 1\r\n\r\n",
        "GET /api/x HTTP/1.0\r\nHost: " BACKEND "\r\n" CLOSE "A: 1\r\n\r\n");
    ROK("1.0 헤더 없음", "GET /api/x HTTP/1.0\r\n\r\n", "GET /api/x HTTP/1.0\r\nHost: " BACKEND "\r\n" CLOSE "\r\n");
    ROK("1.0 Host 있음", "GET /api/x HTTP/1.0\r\nhost: h:80\r\n\r\n", "GET /api/x HTTP/1.0\r\nHost: h:80\r\n" CLOSE "\r\n");
    ROK("본문 있는 GET", GET11 HOST "Content-Length: 3\r\n\r\na\0b", OUT11 "Content-Length: 3\r\n\r\na\0b");
    ROK("CL 0", POST11 HOST "Content-Length: 0\r\n\r\n", POST11 HOST CLOSE "Content-Length: 0\r\n\r\n");
    ROK("CL은 파싱한 값", POST11 HOST "Content-Length: 003\r\n\r\nabc", POST11 HOST CLOSE "Content-Length: 3\r\n\r\nabc");
    ROK("값 공백 정리", GET11 "Host: \t h \t\r\nA:\t1\t\r\nB:\r\nC:   \r\nD:x  y\r\nE: a\tb\r\n\r\n",
        OUT11 "A: 1\r\nB: \r\nC: \r\nD: x  y\r\nE: a\tb\r\n\r\n");
    ROK("값에 0x80", GET11 HOST "A: \x80\xff\r\n\r\n", OUT11 "A: \x80\xff\r\n\r\n");
    ROK("대상은 원래 글자", "GET /api/a%2Fb/./c//d?x=%20 HTTP/1.1\r\n" HOST "\r\n",
        "GET /api/a%2Fb/./c//d?x=%20 HTTP/1.1\r\n" HOST CLOSE "\r\n");
}

#define TGT(t, st) RST("대상 " t, "GET " t " HTTP/1.1\r\n" HOST "\r\n", st)

static void test_req_check(void) {
    RST("1 모르는 메서드", "BREW /api/x HTTP/1.1\r\n" HOST "\r\n", 501);
    RST("1이 5보다 앞", "BREW /api/../x HTTP/1.1\r\n" HOST "\r\n", 501);
    RST("2 POST CL 없음", POST11 HOST "\r\n", 411);
    RST("2가 5보다 앞", "POST /api/../x HTTP/1.1\r\n" HOST "\r\n", 411);
    RST("3 HEAD", "HEAD /api/x HTTP/1.1\r\n" HOST "\r\n", 405);
    RST("3 PUT", "PUT /api/x HTTP/1.1\r\n" HOST "Content-Length: 0\r\n\r\n", 405);
    RST("3이 5보다 앞", "HEAD /api/../x HTTP/1.1\r\n" HOST "\r\n", 405);

    TGT("/api/%zz", 400); TGT("/api/%", 400); TGT("/api/%4", 400); TGT("/api/%00", 400);
    TGT("/api/x#y", 400); TGT("/api/x?a#b", 400); TGT("/api/x\ty", 400);
    TGT("/api/\x01", 400); TGT("/api/\x7f", 400); TGT("/api/\x80", 400); TGT("/api/x?\x80", 400);
    TGT("/api/../admin", 400); TGT("/api/%2e%2e/admin", 400); TGT("/api/a%2f..%2f..%2fadmin", 400);
    TGT("/api/.%2E", 400); TGT("/api/..", 400); TGT("/api/..?x", 400); TGT("/..", 400); TGT("/api/a/../b", 400);
    TGT("/api/a..b", 0); TGT("/api/x?y=../z", 0); TGT("/api/x?/../", 0); TGT("/api/...", 0);
    TGT("/api/./x", 0); TGT("/api//x", 0); TGT("/api/..a/b..", 0); TGT("/api/x?%zz", 0); TGT("/api/%252e%252e/x", 0);
    RST("%zz + 나쁜 Host", "GET /api/%zz HTTP/1.1\r\nHost: a b/c\r\n\r\n", 400);

    for (int c = 1; c < 256; c++) {
        if (c == '\r' || c == '\n') continue;   // 파서가 거른다
        char in[128];
        int st = (c < 0x20 && c != '\t') || c == 0x7F ? 400 : 0;
        int n = snprintf(in, sizeof in, GET11 HOST "A: b%cd\r\n\r\n", c);
        req_st("6 값의 글자", in, (size_t)n, st);
        n = snprintf(in, sizeof in, GET11 HOST "X-Real-IP: %c\r\n\r\n", c);
        req_st("6 빼는 헤더도 검사", in, (size_t)n, st);
        n = snprintf(in, sizeof in, GET11 "Host: a%cb\r\n\r\n", c);
        int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 strchr("-._~%!$&'()*+,;=:[]", c) != NULL;
        req_st("7 Host의 글자", in, (size_t)n, ok ? 0 : 400);
    }
    RST("7 빈 Host", GET11 "Host:\r\n\r\n", 400);
    RST("7 공백 Host", GET11 "Host: \t \r\n\r\n", 400);
    RST("7 1.0 빈 Host", "GET /api/x HTTP/1.0\r\nHost:\r\n\r\n", 400);
    RST("7 a b/c", GET11 "Host: a b/c\r\n\r\n", 400);
    RST("7 뒤에 있는 Host", GET11 "A: 1\r\nhost: a/b\r\n\r\n", 400);
    RST("7 IPv6", GET11 "Host: [::1]:8080\r\n\r\n", 0);
}

// start + first + a:\r\n 줄들 + 빈 줄. 헤더는 first까지 합쳐 lines줄이다. first는 헤더 한 줄
static size_t lines_head(char *buf, const char *start, const char *first, int lines) {
    size_t n = (size_t)sprintf(buf, "%s%s", start, first);
    for (int i = 1; i < lines; i++) n += (size_t)sprintf(buf + n, "a:\r\n");
    return n + (size_t)sprintf(buf + n, "\r\n");
}

static void test_hdr_max(void) {
    char in[1024];
    size_t n = lines_head(in, GET11, HOST, 100);
    req_st("요청 100줄", in, n, 0);
    n = lines_head(in, GET11, HOST, 101);
    req_st("요청 101줄", in, n, 431);
    n = lines_head(in, GET11, "Host: a\x01\r\n", 101);
    req_st("앞쪽의 제어 문자가 먼저", in, n, 400);
    n = lines_head(in, GET11, "Host: a/b\r\n", 101);
    req_st("앞쪽의 나쁜 Host가 먼저", in, n, 400);
    n = lines_head(in, "GET /api/x HTTP/1.1\r\n" HOST, "a:\r\n", 100);   // Host까지 101줄
    req_st("Host도 센다", in, n, 431);
    n = lines_head(in, GET11, HOST, 100) - 2;   // 빈 줄을 빼고 101번째 줄을 붙인다
    n += (size_t)sprintf(in + n, "b: \x01\r\n\r\n");
    req_st("101번째 줄의 제어 문자는 431", in, n, 431);
    n = lines_head(in, "BREW /api/x HTTP/1.1\r\n", HOST, 101);
    req_st("메서드가 먼저", in, n, 501);
    n = lines_head(in, "GET /api/.. HTTP/1.1\r\n", HOST, 101);
    req_st("대상이 먼저", in, n, 400);
}

// 가장 많이 늘어나는 요청: 콜론 뒤에 공백이 없는 100줄, 8192바이트, Host 없음
static void test_req_grow(void) {
    static char in[8192];
    size_t n = (size_t)sprintf(in, "GET /ab HTTP/1.0\r\n");
    for (int i = 0; i < 99; i++) n += (size_t)sprintf(in + n, "a:\r\n");
    in[n++] = 'b';
    in[n++] = ':';
    while (n < sizeof in - 4) in[n++] = 'x';
    memcpy(in + n, "\r\n\r\n", 4);
    char *out;
    size_t got, want = sizeof in + 100 + strlen("Host: " BACKEND "\r\n" CLOSE);
    check("grow", 0, preq(in, sizeof in, 16384, &out, &got));
    check("grow 길이", (long long)want, (long long)got);
    free(out);
    check("grow 정확한 cap", 0, preq(in, sizeof in, want, &out, &got));
    free(out);
    check("grow 1바이트 모자람", 500, preq(in, sizeof in, want - 1, &out, &got));
    free(out);
}

static ssize_t presp(const char *in, size_t len, int minor, int keep, size_t cap, char **out, struct http_upresp *r) {
    char *copy = malloc(len);
    memcpy(copy, in, len);
    *out = malloc(cap);
    ssize_t n = http_proxy_response(copy, len, minor, keep, *out, cap, r);
    free(copy);
    return n;
}

static void resp_ret(const char *name, const char *in, size_t len, int minor, ssize_t want) {
    char *out;
    struct http_upresp r;
    check(name, want, presp(in, len, minor, 1, 16384, &out, &r));
    free(out);
}

#define BAD(name, in)   resp_ret(name, in, sizeof(in) - 1, 1, -1)
#define GOOD(name, in)  resp_ret(name, in, sizeof(in) - 1, 1, sizeof(in) - 1)
#define OK200 "HTTP/1.1 200 OK\r\n"
#define LINE(s) s "\r\n\r\n"

static void test_resp_check(void) {
    BAD("NUL", OK200 "A: \0\r\n\r\n");
    BAD("상태 라인 NUL", "HTTP/1.1 200 \0\r\n\r\n");
    BAD("LF만", "HTTP/1.1 200 OK\nA: 1\n\n");
    BAD("홀로 LF", OK200 "A: 1\n\r\n");
    BAD("홀로 CR", OK200 "A: 1\rB: 2\r\n\r\n");
    BAD("앞의 빈 줄", "\r\n" OK200 "\r\n");

    GOOD("정상", LINE("HTTP/1.1 200 OK"));
    GOOD("1.0 응답", LINE("HTTP/1.0 200 OK"));
    GOOD("이유 없음", LINE("HTTP/1.1 200"));
    GOOD("빈 이유", LINE("HTTP/1.1 200 "));
    GOOD("이유에 탭", LINE("HTTP/1.1 200 a\tb"));
    GOOD("이유에 0x80", LINE("HTTP/1.1 200 \x80"));
    GOOD("100", LINE("HTTP/1.1 100 Continue"));
    GOOD("103", LINE("HTTP/1.1 103 Early Hints\r\nLink: x"));
    GOOD("599", LINE("HTTP/1.1 599 x"));
    BAD("99", LINE("HTTP/1.1 99"));
    BAD("099", LINE("HTTP/1.1 099 x"));
    BAD("600", LINE("HTTP/1.1 600 x"));
    BAD("2000", LINE("HTTP/1.1 2000 OK"));
    BAD("2x0", LINE("HTTP/1.1 2x0 OK"));
    BAD("코드 뒤 탭", LINE("HTTP/1.1 200\tOK"));
    BAD("SP 둘", LINE("HTTP/1.1  200 OK"));
    BAD("버전 뒤 탭", LINE("HTTP/1.1\t200 OK"));
    BAD("코드 없음", LINE("HTTP/1.1"));
    BAD("코드 없음 SP", LINE("HTTP/1.1 "));
    BAD("HTTP/2.0", LINE("HTTP/2.0 200 OK"));
    BAD("HTTP/1.2", LINE("HTTP/1.2 200 OK"));
    BAD("HTTP/1.10", LINE("HTTP/1.10 200 OK"));
    BAD("소문자", LINE("http/1.1 200 OK"));
    BAD("HTTP 아님", LINE("hello"));
    BAD("한 글자", LINE("H"));
    BAD("이유에 0x01", LINE("HTTP/1.1 200 \x01"));
    BAD("이유에 0x7F", LINE("HTTP/1.1 200 O\x7f"));
    BAD("101", LINE("HTTP/1.1 101 Switching Protocols"));

    BAD("이름에 공백", OK200 LINE("A B: 1"));
    BAD("이름 비 tchar", OK200 LINE("A(: 1"));
    BAD("콜론 앞 공백", OK200 LINE("A : 1"));
    BAD("콜론 없음", OK200 LINE("A"));
    BAD("빈 이름", OK200 LINE(": 1"));
    BAD("obs-fold", OK200 LINE("A: 1\r\n b"));
    BAD("obs-fold 탭", OK200 LINE("A: 1\r\n\tb"));
    BAD("1xx도 헤더 검사", LINE("HTTP/1.1 100 C\r\nA"));
    for (int c = 1; c < 256; c++) {
        if (c == '\r' || c == '\n') continue;
        char in[64];
        int bad = (c < 0x20 && c != '\t') || c == 0x7F;
        int n = snprintf(in, sizeof in, OK200 "A: b%cd\r\n\r\n", c);
        resp_ret("값의 글자", in, (size_t)n, 1, bad ? -1 : n);
        n = snprintf(in, sizeof in, OK200 "Upgrade: %c\r\n\r\n", c);
        resp_ret("빼는 헤더도 검사", in, (size_t)n, 1, bad ? -1 : n);
    }

    BAD("CL 1, 1", OK200 LINE("Content-Length: 1, 1"));
    BAD("CL +5", OK200 LINE("Content-Length: +5"));
    BAD("CL -1", OK200 LINE("Content-Length: -1"));
    BAD("CL 빈 값", OK200 LINE("Content-Length:"));
    BAD("CL 0x5", OK200 LINE("Content-Length: 0x5"));
    BAD("CL 2^64", OK200 LINE("Content-Length: 18446744073709551616"));
    GOOD("CL 2^64-1", OK200 LINE("Content-Length: 18446744073709551615"));
    BAD("CL 같은 값 둘", OK200 LINE("Content-Length: 1\r\ncontent-length: 1"));
    BAD("CL 다른 값 둘", OK200 LINE("Content-Length: 1\r\nContent-Length: 2"));
    BAD("204 CL 둘", LINE("HTTP/1.1 204 N\r\nContent-Length: 0\r\nContent-Length: 0"));
    BAD("TE + CL", OK200 LINE("Transfer-Encoding: chunked\r\nContent-Length: 1"));
    BAD("CL + TE", OK200 LINE("Content-Length: 1\r\ntransfer-encoding: x"));
    BAD("204 TE + CL", LINE("HTTP/1.1 204 N\r\nTransfer-Encoding: chunked\r\nContent-Length: 0"));
    BAD("1.0 응답 + TE", LINE("HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked"));
    static const char te[] = OK200 LINE("Transfer-Encoding: chunked");
    GOOD("TE", OK200 LINE("Transfer-Encoding: chunked"));
    resp_ret("minor 0 + TE", te, sizeof te - 1, 0, -1);
    resp_ret("minor 0 + TE 없음", OK200 "\r\n", 19, 0, 19);

    static char big[8300];   // 크기 한도
    memset(big, 'a', sizeof big);
    memcpy(big, OK200 "A: ", 20);
    resp_ret("8191 끝 없음", big, 8191, 1, 0);
    resp_ret("8192 끝 없음", big, 8192, 1, -1);
    resp_ret("8300 끝 없음", big, 8300, 1, -1);
    memcpy(big + 8188, "\r\n\r\n", 4);
    resp_ret("8192 중 8191", big, 8191, 1, 0);
    resp_ret("정확히 8192", big, 8192, 1, 8192);
    resp_ret("정확히 8192 + 본문", big, 8300, 1, 8192);
    memcpy(big + 8188, "a\r\n\r\n", 5);
    resp_ret("8193", big, 8193, 1, -1);

    size_t n = lines_head(big, OK200, "A: 1\r\n", 100);   // 줄 수 한도
    resp_ret("100줄", big, n, 1, (ssize_t)n);
    n = lines_head(big, OK200, "A: 1\r\n", 101);
    resp_ret("101줄", big, n, 1, -1);
    n = lines_head(big, "HTTP/1.1 100 Continue\r\n", "A: 1\r\n", 100);
    resp_ret("1xx 100줄", big, n, 1, (ssize_t)n);
    n = lines_head(big, "HTTP/1.1 100 Continue\r\n", "A: 1\r\n", 101);
    resp_ret("1xx 101줄", big, n, 1, -1);
}

static void resp_ok(const char *name, const char *in, size_t len, int keep, const char *want, size_t wn,
                    int status, int has_len, size_t body_len, int ka) {
    char *out;
    struct http_upresp r;
    check(name, (long long)len, presp(in, len, 1, keep, 16384, &out, &r));
    same(name, want, wn, out, r.out_len);
    check(name, status, r.status);
    check(name, has_len, r.has_len);
    check(name, (long long)body_len, (long long)r.body_len);
    check(name, ka, r.keep_alive);
    free(out);

    int bad = 0;
    char *more = malloc(len + 4);   // 덜 오면 0, 뒤에 본문이 붙어도 헤더 길이
    memcpy(more, in, len);
    memcpy(more + len, "\0\n\r\xff", 4);
    for (size_t k = 0; k <= len + 4; k++) {
        bad += presp(more, k, 1, keep, 16384, &out, &r) != (k < len ? 0 : (ssize_t)len);
        free(out);
    }
    free(more);
    check(name, 0, bad);
    for (size_t cap = 0; cap <= wn; cap++) {
        bad += presp(in, len, 1, keep, cap, &out, &r) != (cap < wn ? -1 : (ssize_t)len);
        free(out);
    }
    check(name, 0, bad);
}

#define POK(name, in, keep, want, st, hl, bl, ka) \
    resp_ok(name, in, sizeof(in) - 1, keep, want, sizeof(want) - 1, st, hl, bl, ka)
#define KA "Connection: keep-alive\r\n"
#define S204 "HTTP/1.1 204 No Content\r\n"
#define S304 "HTTP/1.1 304 Not Modified\r\n"
#define CL5 "Content-Length: 5\r\n"
#define TE "Transfer-Encoding: chunked\r\n"
#define PLAIN "Date: Mon, 01 Jan 2024 00:00:00 GMT\r\nServer: x\r\nContent-Type: text/plain\r\nSet-Cookie: a=1\r\n" \
    "Set-Cookie: b=2\r\nLocation: http://h/x\r\nTE: x\r\nForwarded: y\r\nHost: z\r\n"

static void test_resp_rewrite(void) {
    POK("CL keep 1", OK200 CL5 "A: 1\r\n\r\n", 1, OK200 KA CL5 "A: 1\r\n\r\n", 200, 1, 5, 1);
    POK("CL keep 0", OK200 CL5 "A: 1\r\n\r\n", 0, OK200 CLOSE CL5 "A: 1\r\n\r\n", 200, 1, 5, 0);
    POK("CL 0", OK200 "Content-Length: 0\r\n\r\n", 1, OK200 KA "Content-Length: 0\r\n\r\n", 200, 1, 0, 1);
    POK("CL 파싱한 값", OK200 "content-length:\t005 \r\n\r\n", 1, OK200 KA CL5 "\r\n", 200, 1, 5, 1);
    POK("길이 모름 keep 1", OK200 "\r\n", 1, OK200 CLOSE "\r\n", 200, 0, 0, 0);
    POK("길이 모름 keep 0", OK200 "\r\n", 0, OK200 CLOSE "\r\n", 200, 0, 0, 0);
    POK("chunked keep 1", OK200 TE "Trailer: X\r\n\r\n", 1, OK200 CLOSE TE "Trailer: X\r\n\r\n", 200, 0, 0, 0);
    POK("chunked keep 0", OK200 TE "\r\n", 0, OK200 CLOSE TE "\r\n", 200, 0, 0, 0);
    POK("204 keep 1", S204 "A: 1\r\n\r\n", 1, S204 KA "A: 1\r\n\r\n", 204, 1, 0, 1);
    POK("204 keep 0", S204 "\r\n", 0, S204 CLOSE "\r\n", 204, 1, 0, 0);
    POK("204 CL 0", S204 "Content-Length: 0\r\n\r\n", 1, S204 KA "\r\n", 204, 1, 0, 1);
    POK("204 CL 5", S204 CL5 "\r\n", 1, S204 KA "\r\n", 204, 1, 0, 1);
    POK("204 TE", S204 TE "\r\n", 1, S204 KA "\r\n", 204, 1, 0, 1);
    POK("304 keep 1", S304 "\r\n", 1, S304 KA "\r\n", 304, 1, 0, 1);
    POK("304 keep 0", S304 "\r\n", 0, S304 CLOSE "\r\n", 304, 1, 0, 0);
    POK("304 CL 5", S304 CL5 "\r\n", 1, S304 KA CL5 "\r\n", 304, 1, 0, 1);
    POK("304 TE", S304 "TRANSFER-ENCODING: x\r\nETag: x\r\n\r\n", 1, S304 KA "ETag: x\r\n\r\n", 304, 1, 0, 1);
    POK("204 TE가 Connection에 있음", S204 "Connection: Transfer-Encoding\r\n" TE "\r\n", 1, S204 KA "\r\n", 204, 1, 0, 1);
    POK("뺄 헤더 전부",
        OK200 "cONNECTION: keep-alive, x-hop\r\nKeep-alive: timeout=5\r\nPROXY-Connection: x\r\nupgrade: h2\r\nX-Hop: 1\r\n"
        CL5 "Connection: X-Two ,, x-none\r\nx-two: 2\r\nX-Keep: 3\r\n\r\n", 1,
        OK200 KA CL5 "X-Keep: 3\r\n\r\n", 200, 1, 5, 1);
    POK("백엔드의 close는 A와 무관", OK200 CLOSE CL5 "\r\n", 1, OK200 KA CL5 "\r\n", 200, 1, 5, 1);
    POK("Connection에 적힌 TE, Trailer는 남음",
        OK200 "Connection: transfer-encoding, TRAILER\r\n" TE "Trailer: X\r\n\r\n", 1,
        OK200 CLOSE TE "Trailer: X\r\n\r\n", 200, 0, 0, 0);
    POK("Connection에 적힌 CL은 새로 만듦", OK200 "Connection: Content-Length\r\n" CL5 "\r\n", 1,
        OK200 KA CL5 "\r\n", 200, 1, 5, 1);
    POK("그대로 두는 헤더", OK200 PLAIN CL5 "\r\n", 1, OK200 KA CL5 PLAIN "\r\n", 200, 1, 5, 1);
    POK("값 공백과 빈 값", OK200 "A:\t1\t\r\nB:\r\nC: \x80\r\n\r\n", 1, OK200 CLOSE "A: 1\r\nB: \r\nC: \x80\r\n\r\n", 200, 0, 0, 0);
    POK("1.0 응답은 1.1로", "HTTP/1.0 404 Nope  x\r\n" CL5 "\r\n", 1, "HTTP/1.1 404 Nope  x\r\n" KA CL5 "\r\n", 404, 1, 5, 1);
    POK("이유 없음", "HTTP/1.1 200\r\n\r\n", 0, "HTTP/1.1 200 \r\n" CLOSE "\r\n", 200, 0, 0, 0);
    POK("빈 이유", "HTTP/1.1 200 \r\n\r\n", 0, "HTTP/1.1 200 \r\n" CLOSE "\r\n", 200, 0, 0, 0);

    static const char c100[] = "HTTP/1.1 100 Continue\r\nA: 1\r\n\r\n" OK200 "\r\n";
    char out[4] = "zzz";
    struct http_upresp r;
    check("1xx는 원본 헤더 길이", 31, http_proxy_response(c100, sizeof c100 - 1, 1, 1, out, 0, &r));
    check("1xx status", 100, r.status);
    check("1xx out_len", 0, (long long)r.out_len);
    check("1xx out 그대로", 0, strcmp(out, "zzz"));
}

int main(void) {
    test_reject();
    test_accept();
    test_find();
    test_find_no_root();
    test_count();
    test_error_resp();
    test_keep_alive();
    test_req_rewrite();
    test_req_check();
    test_req_grow();
    test_hdr_max();
    test_resp_check();
    test_resp_rewrite();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
