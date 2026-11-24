// route.c 단위 테스트. src/http.c, src/route.c를 직접 포함하므로 src/*.c와 함께 링크하지 않는다.
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

int main(void) {
    test_reject();
    test_accept();
    test_find();
    test_find_no_root();
    test_count();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
