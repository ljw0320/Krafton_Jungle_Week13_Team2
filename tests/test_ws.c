// ws.c의 내부 SHA-1과 Base64도 직접 검사한다.
#include "../chat/ws.c"
#include "../chat/room.h"

#include <stdio.h>
#include <stdlib.h>

static int passed, failed;

static void check(const char *name, long long want, long long got) {
    if (want == got) { passed++; return; }
    failed++;
    fprintf(stderr, "FAIL %s: 기대 %lld, 실제 %lld\n", name, want, got);
}

static void test_sha1(void) {
    static const struct {
        size_t len;
        const char *hex;
    } cases[] = {
        {0, "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
        {3, "a9993e364706816aba3e25717850c26c9cd0d89d"},
        {55, "c1c8bbdc22796e28c0e15163d20899b65621d65a"},
        {56, "c2db330f6083854c99d4b5bfb6e8f29f201be699"},
        {63, "03f09f5b158a7a8cdad920bddc29b81c18a551f5"},
        {64, "0098ba824b5c16427bd7a1122a5a442a25ec644d"},
        {65, "11655326c708d70319be2610e8a57d9a5b959d3b"},
    };
    unsigned char repeated[65];
    memset(repeated, 'a', sizeof(repeated));
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const unsigned char *input = cases[i].len == 3 ? (const unsigned char *)"abc" : repeated;
        unsigned char digest[20];
        char hex[41], name[32];
        sha1(input, cases[i].len, digest);
        for (size_t j = 0; j < sizeof(digest); j++)
            snprintf(hex + j * 2, 3, "%02x", (unsigned int)digest[j]);
        snprintf(name, sizeof(name), "SHA-1 %zu바이트", cases[i].len);
        check(name, 0, strcmp(cases[i].hex, hex));
    }
}

static void test_base64(void) {
    const unsigned char input[] = "foo";
    char out[5];
    base64(input, 0, out);
    check("Base64 빈 입력", 0, strcmp("", out));
    base64(input, 1, out);
    check("Base64 나머지 1바이트", 0, strcmp("Zg==", out));
    base64(input, 2, out);
    check("Base64 나머지 2바이트", 0, strcmp("Zm8=", out));
    base64(input, 3, out);
    check("Base64 나머지 0바이트", 0, strcmp("Zm9v", out));
}

static void test_accept(void) {
    char out[29];
    const char key[] = "dGhlIHNhbXBsZSBub25jZQ==";
    check("RFC 키", 0, ws_accept(key, sizeof(key) - 1, out));
    check("RFC Accept", 0, memcmp("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", out, sizeof(out)));
    check("키 23자", -1, ws_accept(key, 23, out));
    check("키 25자", -1, ws_accept(key, 25, out));
    check("키 패딩 하나", -1, ws_accept("dGhlIHNhbXBsZSBub25jZQA=", 24, out));
    check("키 알파벳 밖", -1, ws_accept("dGhlIHNhbXBsZSBub25jZ!==", 24, out));
    check("키 중간 패딩", -1, ws_accept("dGhl=HNhbXBsZSBub25jZQ==", 24, out));
    check("키 남는 비트", -1, ws_accept("dGhlIHNhbXBsZSBub25jZR==", 24, out));
    check("키 내부 NUL", -1, ws_accept("\0GhlIHNhbXBsZSBub25jZQ==", 24, out));
}

#define FRAME_MAX 16384

static void test_parse(void) {
    static const struct {
        size_t len, mask_at;
        unsigned char header[4];
    } cases[] = {
        {0, 2, {0x82, 0x80}},
        {125, 2, {0x82, 0xfd}},
        {126, 4, {0x82, 0xfe, 0x00, 0x7e}},
        {16384, 4, {0x82, 0xfe, 0x40, 0x00}},
    };
    const unsigned char mask[] = {0x12, 0x34, 0x56, 0x78};
    const unsigned char next[] = {0x89, 0x80, 0, 0, 0, 0};
    unsigned char buf[FRAME_MAX + 8 + sizeof(next)];
    unsigned char original[sizeof(buf)];
    struct ws_frame f;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t head = cases[i].mask_at + sizeof(mask);
        size_t total = head + cases[i].len;
        memcpy(buf, cases[i].header, cases[i].mask_at);
        memcpy(buf + cases[i].mask_at, mask, sizeof(mask));
        memset(buf + head, 0x5a, cases[i].len);
        memcpy(buf + total, next, sizeof(next));
        memcpy(original, buf, total + sizeof(next));
        for (size_t n = 0; n < total; n++) {
            ssize_t got = ws_parse(buf, n, FRAME_MAX, &f);
            if (got != 0) fprintf(stderr, "payload %zu, 입력 %zu\n", cases[i].len, n);
            check("미완성 프레임", 0, got);
        }
        check("완성 프레임", total, ws_parse(buf, total, FRAME_MAX, &f));
        check("프레임 FIN", 1, f.fin);
        check("프레임 opcode", 2, f.op);
        check("프레임 헤더 길이", head, f.head);
        check("프레임 payload 길이", cases[i].len, f.len);
        check("프레임 마스크", 0, memcmp(mask, f.mask, sizeof(mask)));
        check("뒤에 프레임이 붙음", total, ws_parse(buf, total + sizeof(next), FRAME_MAX, &f));
        check("파싱 후 입력 보존", 0, memcmp(original, buf, total + sizeof(next)));
    }

    const unsigned char extended[] = {0x02, 0xff, 0, 0, 0, 0, 0, 1, 0, 0};
    check("허용된 64비트 길이 헤더", 0, ws_parse(extended, sizeof(extended), 65536, &f));
}

static void parse_error(const char *name, const unsigned char *buf, size_t n, int close) {
    struct ws_frame f = {0};
    check(name, -1, ws_parse(buf, n, FRAME_MAX, &f));
    check(name, close, f.close);
}

static void test_parse_errors(void) {
    static const struct {
        const char *name;
        unsigned char buf[10];
        size_t n;
        int close;
    } cases[] = {
        {"길이 16385", {0x82, 0xfe, 0x40, 0x01}, 4, 1009},
        {"길이 65535", {0x82, 0xfe, 0xff, 0xff}, 4, 1009},
        {"길이 65536", {0x82, 0xff, 0, 0, 0, 0, 0, 1, 0, 0}, 10, 1009},
        {"마스크 없음", {0x82, 0x00}, 2, 1002},
        {"RSV1", {0xc2, 0x80}, 2, 1002},
        {"RSV2", {0xa2, 0x80}, 2, 1002},
        {"RSV3", {0x92, 0x80}, 2, 1002},
        {"126에 길이 125", {0x82, 0xfe, 0, 125}, 4, 1002},
        {"127에 길이 65535", {0x82, 0xff, 0, 0, 0, 0, 0, 0, 0xff, 0xff}, 10, 1002},
        {"64비트 최상위 비트", {0x82, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0}, 10, 1002},
        {"126바이트 ping", {0x89, 0xfe, 0, 126}, 4, 1002},
        {"조각난 pong", {0x0a, 0x80}, 2, 1002},
        {"1바이트 close", {0x88, 0x81, 1, 2, 3, 4, 0}, 7, 1002},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        parse_error(cases[i].name, cases[i].buf, cases[i].n, cases[i].close);
    for (int op = 3; op <= 15; op++) {
        if (op >= 8 && op <= 10) continue;
        unsigned char buf[] = {0x80 | op, 0x80};
        char name[32];
        snprintf(name, sizeof(name), "예약된 opcode %d", op);
        parse_error(name, buf, sizeof(buf), 1002);
    }
}

static void test_unmask(void) {
    const unsigned char mask[] = {1, 2, 3, 4};
    const unsigned char original[] = {0x10, 0x20, 0x30, 0x40, 0x50};
    const unsigned char masked[] = {0x11, 0x22, 0x33, 0x44, 0x51};
    const size_t lengths[] = {0, 1, 4, 5};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        size_t n = lengths[i];
        unsigned char buf[sizeof(original)];
        memcpy(buf, original, sizeof(buf));
        ws_unmask(buf, n, mask);
        check("마스크 적용", 0, memcmp(buf, masked, n));
        check("마스크 범위", 0, memcmp(buf + n, original + n, sizeof(buf) - n));
        ws_unmask(buf, n, mask);
        check("마스크 두 번 적용", 0, memcmp(buf, original, sizeof(buf)));
    }
}

static void test_head(void) {
    static const struct {
        size_t len, head;
        unsigned char bytes[10];
    } cases[] = {
        {0, 2, {0x82, 0}},
        {125, 2, {0x82, 125}},
        {126, 4, {0x82, 126, 0, 126}},
        {16384, 4, {0x82, 126, 0x40, 0}},
        {65535, 4, {0x82, 126, 0xff, 0xff}},
        {65536, 10, {0x82, 127, 0, 0, 0, 0, 0, 1, 0, 0}},
    };
    unsigned char out[10];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        check("서버 헤더 길이", cases[i].head, ws_head(out, 1, 2, cases[i].len));
        check("서버 헤더 바이트", 0, memcmp(cases[i].bytes, out, cases[i].head));
    }
    check("FIN 없는 서버 헤더 길이", 2, ws_head(out, 0, 1, 0));
    check("FIN 없는 서버 첫 바이트", 0x01, out[0]);
}

static void test_utf8(void) {
    static const struct {
        const char *name, *bytes;
        size_t len;
        int valid;
    } cases[] = {
        {"빈 UTF-8", "", 0, 1},
        {"UTF-8 1바이트 최소와 최대", "\x00\x7f", 2, 1},
        {"UTF-8 2바이트 최소와 최대", "\xc2\x80\xdf\xbf", 4, 1},
        {"UTF-8 3바이트 최소와 최대", "\xe0\xa0\x80\xef\xbf\xbf", 6, 1},
        {"UTF-8 서로게이트 앞뒤", "\xed\x9f\xbf\xee\x80\x80", 6, 1},
        {"UTF-8 4바이트 최소와 최대", "\xf0\x90\x80\x80\xf4\x8f\xbf\xbf", 8, 1},
        {"홀로 있는 이어지는 바이트", "\x80", 1, 0},
        {"잘못된 이어지는 바이트", "\xe0\xa0\x7f", 3, 0},
        {"잘린 2바이트 글자", "\xc2", 1, 0},
        {"잘린 3바이트 글자", "\xe0\xa0", 2, 0},
        {"잘린 4바이트 글자", "\xf0\x90\x80", 3, 0},
        {"과장된 2바이트 표현", "\xc0\x80", 2, 0},
        {"과장된 3바이트 표현", "\xe0\x80\x80", 3, 0},
        {"과장된 4바이트 표현", "\xf0\x80\x80\x80", 4, 0},
        {"서로게이트", "\xed\xa0\x80", 3, 0},
        {"유니코드 상한 초과", "\xf4\x90\x80\x80", 4, 0},
        {"UTF-8 F5", "\xf5\x80\x80\x80", 4, 0},
        {"UTF-8 FF", "\xff", 1, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        check(cases[i].name, cases[i].valid,
              ws_utf8((const unsigned char *)cases[i].bytes, cases[i].len));
}

static void test_shuffle(void) {
    int a[1000], seen[1000];
    const int sizes[] = {0, 1, 2, 1000};
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        int n = sizes[k];
        memset(seen, 0, sizeof(seen));
        for (int i = 0; i < n; i++) a[i] = i;
        shuffle(a, n);
        for (int i = 0; i < n; i++)
            if (a[i] >= 0 && a[i] < n) seen[a[i]]++;
        for (int i = 0; i < n; i++) check("shuffle 원소 보존", 1, seen[i]);
    }
    int counts[3][3] = {{0}};
    srandom(1);
    for (int k = 0; k < 6000; k++) {
        int p[] = {0, 1, 2};
        shuffle(p, 3);
        if ((unsigned)p[0] < 3 && (unsigned)p[1] < 3) counts[p[0]][p[1]]++;
    }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            if (i != j) check("shuffle 순열 분포", 1, counts[i][j] >= 800 && counts[i][j] <= 1200);
}

int main(void) {
    test_shuffle();
    test_sha1();
    test_base64();
    test_accept();
    test_parse();
    test_parse_errors();
    test_unmask();
    test_head();
    test_utf8();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
