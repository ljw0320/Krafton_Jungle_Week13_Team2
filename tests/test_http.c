#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../http/http.h"
#include "../http/http_internal.h"

static void receive_exact(int fd, char *buffer, size_t length)
{
    size_t received = 0;
    while (received < length) {
        ssize_t n = recv(fd, buffer + received, length - received, 0);
        if (n < 0 && errno == EINTR)
            continue;
        assert(n > 0);
        received += (size_t)n;
    }
}

static int parse(const char *text, struct request *request)
{
    static char header[HEADER_LIMIT + 1];
    assert(strlen(text) <= HEADER_LIMIT);
    strcpy(header, text);
    return parse_request(header, request);
}

static void test_receive_results(void)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    char header[HEADER_LIMIT + 1];
    double deadline;
    const char *request = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(send(pair[1], request, strlen(request), 0) == (ssize_t)strlen(request));
    assert(receive_header(pair[0], header, sizeof(header), 1, &deadline) == RECEIVE_OK);
    assert(strcmp(header, request) == 0);
    assert(receive_body(pair[0], 1, 0) == 408);
    assert(shutdown(pair[1], SHUT_WR) == 0);
    assert(receive_body(pair[0], 1, deadline) == 400);
    assert(receive_header(pair[0], header, sizeof(header), 1, &deadline) == RECEIVE_CLOSED);
    close(pair[0]);
    assert(receive_header(pair[0], header, sizeof(header), 1, &deadline) == RECEIVE_ERROR);
    close(pair[1]);
}

static void test_error_response_failure(void)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const char *request = "INVALID\r\n\r\n";
    assert(send(pair[1], request, strlen(request), 0) == (ssize_t)strlen(request));
    assert(shutdown(pair[1], SHUT_RD) == 0);
    void (*previous)(int) = signal(SIGPIPE, SIG_IGN);
    assert(previous != SIG_ERR);
    assert(serve_http(pair[0], -1, 1) == 1);
    assert(signal(SIGPIPE, previous) != SIG_ERR);
    close(pair[0]);
    close(pair[1]);
}

static void test_request_responses(void)
{
    const char *methods[] = {"POST", "POST", "GET"};
    const char *targets[] = {"/%00", "/", "/"};
    const char *expected[] = {"HTTP/1.1 400 ", "HTTP/1.1 501 ", "HTTP/1.1 500 "};
    for (size_t i = 0; i < 3; ++i) {
        int pair[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        char buffer[256];
        int length = snprintf(buffer, sizeof(buffer),
            "%s %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", methods[i], targets[i]);
        assert(length > 0 && (size_t)length < sizeof(buffer));
        assert(send(pair[1], buffer, (size_t)length, 0) == length);
        assert(serve_http(pair[0], -1, 1) == 0);
        receive_exact(pair[1], buffer, strlen(expected[i]));
        assert(strncmp(buffer, expected[i], strlen(expected[i])) == 0);
        close(pair[0]);
        close(pair[1]);
    }
}

static void test_content_length(void)
{
    const char *values[] = {"0", "0004", "", "+1", "-1", "1x"};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        char header[128];
        snprintf(header, sizeof(header), "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: %s\r\n\r\n", values[i]);
        struct request request;
        assert(parse(header, &request) == (i < 2 ? 0 : 400));
        if (i < 2)
            assert(request.content_length == (i == 0 ? 0 : 4));
    }
    char maximum[3 * sizeof(size_t) + 1];
    int n = snprintf(maximum, sizeof(maximum), "%zu", SIZE_MAX);
    assert(n > 0 && (size_t)n < sizeof(maximum));
    char overflow[sizeof(maximum) + 1];
    assert(snprintf(overflow, sizeof(overflow), "%s0", maximum) == n + 1);
    char header[128];
    snprintf(header, sizeof(header), "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: %s\r\n\r\n", maximum);
    struct request request;
    assert(parse(header, &request) == 413 && request.content_length == SIZE_MAX);
    snprintf(header, sizeof(header), "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: %s\r\n\r\n", overflow);
    assert(parse(header, &request) == 400);
}

static void test_request_line(void)
{
    static const struct { const char *line; int status; } cases[] = {
        {"GET / HTTP/1.1", 0}, {"POST /a?b=c HTTP/1.0", 0},
        {"GET / HTTP/2.0", 505}, {"GET / HTTP/no", 400},
        {"GET  / HTTP/1.1", 400}, {"GET / HTTP/1.1 extra", 400},
        {" / HTTP/1.1", 400}, {"GET\t/ HTTP/1.1", 400},
        {"G(ET / HTTP/1.1", 400}, {"GET /\t HTTP/1.1", 400},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char header[128];
        snprintf(header, sizeof(header), "%s\r\nHost: x\r\n\r\n", cases[i].line);
        struct request request;
        assert(parse_request(header, &request) == cases[i].status);
        if (!cases[i].status) {
            assert(request.method == header && request.target > header);
            assert(request.close == !request.http11);
        }
    }
    struct request request;
    assert(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n"
                 "Transfer-Encoding: chunked\r\n\r\n", &request) == 400);
    assert(parse("POST / HTTP/1.1\r\nContent-Length: 1048577\r\n\r\n", &request) == 400);
}

static void test_header_boundaries(void)
{
    const char *header = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t length = strlen(header);
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(send(pair[1], header, length, 0) == (ssize_t)length);
    const char body[] = {'\0', '\r', '\n', '\r', '\n', 'x'};
    assert(send(pair[1], body, sizeof(body), 0) == (ssize_t)sizeof(body));
    char buffer[HEADER_LIMIT + 1];
    double deadline;
    assert(receive_header(pair[0], buffer, sizeof(buffer), 1, &deadline) == RECEIVE_OK);
    assert(strcmp(buffer, header) == 0);
    receive_exact(pair[0], buffer, sizeof(body));
    assert(memcmp(buffer, body, sizeof(body)) == 0);
    char small[8];
    memset(small, 'z', sizeof(small));
    assert(send(pair[1], header, length, 0) == (ssize_t)length);
    assert(receive_header(pair[0], small, 4, 1, &deadline) == 431);
    for (size_t i = 4; i < sizeof(small); ++i)
        assert(small[i] == 'z');
    close(pair[0]);
    close(pair[1]);
}

/* Fixed-seed mutations exercise parser and decoder memory boundaries under sanitizers. */
static void test_mutated_inputs(void)
{
    const char *seeds[] = {
        "GET /a%20b HTTP/1.1\r\nHost: x\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: close, keep-alive\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n\r\n",
        "/%2e%2e/a%00?query",
    };
    uint32_t random = 12345;
    for (size_t i = 0; i < 12000; ++i) {
        char input[256];
        strcpy(input, seeds[i % (sizeof(seeds) / sizeof(seeds[0]))]);
        size_t length = strlen(input);
        for (size_t j = 0; j < 1 + i % 4; ++j) {
            random = random * 1664525u + 1013904223u;
            size_t position = (random >> 8) % length;
            input[position] = (char)(random >> 24);
        }
        char parsed[sizeof(input)];
        strcpy(parsed, input);
        struct request request = {.http11 = true, .close = true};
        int status = parse_request(parsed, &request);
        assert(status || (request.method && request.target && request.content_length <= BODY_LIMIT));
        char path[64];
        memset(path, 'z', sizeof(path));
        size_t capacity = i % sizeof(path);
        status = decode_path(input, path, capacity);
        for (size_t j = capacity; j < sizeof(path); ++j)
            assert(path[j] == 'z');
        if (!status)
            assert(memchr(path, '\0', capacity));
    }
}

int main(void)
{
    test_content_length();
    test_request_line();
    test_header_boundaries();
    test_mutated_inputs();
    test_receive_results();
    test_error_response_failure();
    test_request_responses();
    struct request request;
    assert(parse("GET / HTTP/1.1\r\nhOsT: localhost\r\ncOnTeNt-LeNgTh: 0004\r\n\r\n", &request) == 0);
    assert(request.http11 && !request.close && request.content_length == 4);
    assert(parse("POST / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", &request) == 0);
    assert(!request.http11 && request.close);
    assert(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: CLOSE\r\nConnection: keep-alive\r\n\r\n", &request) == 0);
    assert(request.close);
    assert(parse("GET / HTTP/1.1\r\nHost: x\r\nX: value\nfolded\r\n\r\n", &request) == 400);
    assert(parse("GET / HTTP/1.1\r\nHost: x\r\n X: value\r\n\r\n", &request) == 400);
    assert(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive,\r\n\r\n", &request) == 400);
    assert(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: +1\r\n\r\n", &request) == 400);
    assert(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1, 1\r\n\r\n", &request) == 400);
    assert(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048576\r\n\r\n", &request) == 0);
    assert(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n", &request) == 413);
    assert(parse("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n", &request) == 400);
    char path[URI_LIMIT + 1];
    assert(decode_path("/a%20b.txt?version=1", path, sizeof(path)) == 0);
    assert(strcmp(path, "/a b.txt") == 0);
    assert(decode_path("/%252e%252e/file", path, sizeof(path)) == 0);
    assert(strcmp(path, "/%2e%2e/file") == 0);
    assert(decode_path("/%00", path, sizeof(path)) == 400);
    assert(decode_path("/%0a", path, sizeof(path)) == 400);
    assert(decode_path("http://host/file", path, sizeof(path)) == 400);
    puts("PASS: HTTP parser, path decoding, framing, response and I/O errors");
    return 0;
}
