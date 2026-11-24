#include "../proxy/headers.h"
#include "../proxy/routes.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static size_t comparisons;
static size_t allocations, fail_at;
static bool allocation_failed;
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size)
{
    if (fail_at && ++allocations == fail_at) {
        allocation_failed = true;
        errno = ENOMEM;
        return NULL;
    }
    return __real_malloc(size);
}
int __real_strcasecmp(const char *left, const char *right);
int __wrap_strcasecmp(const char *left, const char *right)
{
    ++comparisons;
    return __real_strcasecmp(left, right);
}

static void test_connection_filtering_preserves_order(void)
{
    struct request request = {.method = "GET", .target = "/api", .http11 = true};
    char *out = NULL;
    const char *raw = "GET /api HTTP/1.1\r\nHost: example.com\r\nX-Keep: one\r\n"
        "X-Drop: secret\r\ncOnnection: x-drop \t, X-Two\r\n"
        "Connection: X-DROP, keep-alive\r\nX-Two: secret\r\nX-Keep: two\r\n\r\n";
    assert(proxy_request_header(raw, &request, &out) == 0);
    assert(strcmp(out, "GET /api HTTP/1.1\r\nHost:example.com\r\nX-Keep:one\r\n"
                       "X-Keep:two\r\nContent-Length:0\r\nConnection:close\r\n\r\n") == 0);
    free(out);
    raw = "HTTP/1.1 200 OK\r\nContent-Length:0\r\nSet-Cookie:a=1\r\n"
        "X-Drop:secret\r\nConnection:x-drop, X-Two\r\nSet-Cookie:b=2\r\n"
        "Connection:X-DROP\r\nX-Two:secret\r\n\r\n";
    uintmax_t length;
    const char *reason;
    assert(proxy_response_header(raw, &request, &out, &length, &reason) == 0);
    assert(length == 0 && reason == NULL);
    assert(strcmp(out, "HTTP/1.1 200 OK\r\nSet-Cookie:a=1\r\nSet-Cookie:b=2\r\n"
                       "Content-Length:0\r\nConnection:keep-alive\r\n\r\n") == 0);
    free(out);
}

static void test_connection_validation(void)
{
    const char *values[] = {"", ",x", "x,", "x,,y", "x y", "x;y", "x,\t",
        "Host", "safe, content-LENGTH", "transfer-Encoding", "x\r\nConnection:hOsT"};
    struct request request = {.method = "GET", .target = "/api", .http11 = true};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        char raw[512], *out = NULL;
        snprintf(raw, sizeof(raw), "GET /api HTTP/1.1\r\nHost:example.com\r\n"
                 "Connection:%s\r\n\r\n", values[i]);
        assert(proxy_request_header(raw, &request, &out) == 400 && out == NULL);
        snprintf(raw, sizeof(raw), "HTTP/1.1 200 OK\r\nContent-Length:0\r\n"
                 "Connection:%s\r\n\r\n", values[i]);
        uintmax_t length;
        const char *reason;
        assert(proxy_response_header(raw, &request, &out, &length, &reason) == 502);
        assert(out == NULL && strcmp(reason, "invalid_response_header") == 0);
    }
}

static void test_many_fields_avoid_quadratic_comparisons(size_t fields, size_t tokens)
{
    for (int response = 0; response < 2; ++response) {
        char raw[HEADER_LIMIT + 1];
        strcpy(raw, response ? "HTTP/1.1 200 OK\r\nContent-Length:0\r\n" :
                               "GET /api HTTP/1.1\r\nHost:example.com\r\n");
        size_t used = strlen(raw);
        for (size_t i = 0; i < fields; ++i) {
            memcpy(raw + used, "X:y\r\n", 5);
            used += 5;
        }
        if (tokens) {
            memcpy(raw + used, "Connection:", 11);
            used += 11;
            for (size_t i = 0; i < tokens; ++i) {
                raw[used++] = 'a';
                if (i + 1 < tokens)
                    raw[used++] = ',';
            }
            memcpy(raw + used, "\r\n", 2);
            used += 2;
        }
        assert(used + 2 <= HEADER_LIMIT);
        memcpy(raw + used, "\r\n", 3);
        struct request request = {.method = "GET", .target = "/api", .http11 = true};
        char *out = NULL;
        uintmax_t length;
        const char *reason;
        comparisons = 0;
        int status = response ? proxy_response_header(raw, &request, &out, &length, &reason) :
                                proxy_request_header(raw, &request, &out);
        assert(status == 0);
        assert(comparisons < 64 * (fields + tokens + 1));
        size_t kept = 0;
        for (const char *p = out; (p = strstr(p, "X:y\r\n")) != NULL; p += 5)
            ++kept;
        assert(kept == fields);
        free(out);
    }
}

static void test_allocation_failures_are_internal_errors(void)
{
    struct request request = {.method = "GET", .target = "/api", .http11 = true};
    for (int response = 0; response < 2; ++response) {
        bool completed = false;
        for (size_t attempt = 1; attempt < 8; ++attempt) {
            char *out = NULL;
            uintmax_t length;
            const char *reason;
            allocations = 0;
            allocation_failed = false;
            fail_at = attempt;
            int status = response ? proxy_response_header(
                "HTTP/1.1 200 OK\r\nContent-Length:0\r\nConnection:close\r\n\r\n",
                &request, &out, &length, &reason) : proxy_request_header(
                "GET /api HTTP/1.1\r\nHost:example.com\r\nConnection:close\r\n\r\n",
                &request, &out);
            fail_at = 0;
            if (!allocation_failed) {
                assert(status == 0);
                free(out);
                completed = true;
                break;
            }
            assert(status == 500 && out == NULL);
            if (response)
                assert(strcmp(reason, "allocation_failed") == 0);
        }
        assert(completed);
    }
}

int main(void)
{
    test_connection_filtering_preserves_order();
    test_connection_validation();
    test_allocation_failures_are_internal_errors();
    test_many_fields_avoid_quadratic_comparisons(6000, 0);
    test_many_fields_avoid_quadratic_comparisons(1000, 12000);
    struct request request = {.http11 = true};
    char raw[HEADER_LIMIT + 1];
    const char *start = "HTTP/1.1 200 OK\r\nContent-Length:0\r\n";
    strcpy(raw, start);
    size_t used = strlen(raw);
    while (used + 6 <= HEADER_LIMIT) {
        memcpy(raw + used, "X:y\r\n", 5);
        used += 5;
    }
    memcpy(raw + used, "\r\n", 3);
    char *out = NULL;
    uintmax_t length;
    const char *reason;
    assert(proxy_response_header(raw, &request, &out, &length, &reason) == 0);
    assert(reason == NULL);
    assert(length == 0);
    free(out);
    puts("proxy header tests passed");
    return 0;
}
