// http_parse 단위 테스트. src/http.c를 직접 포함하므로 src/*.c와 함께 링크하지 않는다.
#include "../src/http.c"

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

int main(void) {
    test_table();
    test_fields();
    test_limits();
    prefixes("prefix GET", "GET /index.html HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");
    prefixes("prefix POST", "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello");
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
