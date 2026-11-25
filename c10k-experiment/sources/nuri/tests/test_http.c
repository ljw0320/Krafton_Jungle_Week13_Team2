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

#include "test_runner.h"

static void send_sample_request(int pair[2])
{
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const char *request = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(send(pair[1], request, strlen(request), 0) == (ssize_t)strlen(request));
}

static void test_receive_preserves_complete_header(void)
{
    int pair[2];
    send_sample_request(pair);
    char header[HEADER_LIMIT + 1];
    double deadline;
    assert(receive_header(pair[0], header, sizeof(header), 1, &deadline) == RECEIVE_OK);
    assert(strcmp(header, "GET / HTTP/1.1\r\nHost: x\r\n\r\n") == 0);
    close(pair[0]);
    close(pair[1]);
}

static void test_receive_body_reports_expired_deadline(void)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(receive_body(pair[0], 1, 0) == 408);
    close(pair[0]);
    close(pair[1]);
}

static void test_receive_body_reports_premature_eof(void)
{
    int pair[2];
    send_sample_request(pair);
    char header[HEADER_LIMIT + 1];
    double deadline;
    assert(receive_header(pair[0], header, sizeof(header), 1, &deadline) == RECEIVE_OK);
    assert(shutdown(pair[1], SHUT_WR) == 0);
    assert(receive_body(pair[0], 1, deadline) == 400);
    close(pair[0]);
    close(pair[1]);
}

static void test_receive_header_reports_eof(void)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(shutdown(pair[1], SHUT_WR) == 0);
    char header[HEADER_LIMIT + 1];
    double deadline;
    assert(receive_header(pair[0], header, sizeof(header), 1, &deadline) == RECEIVE_CLOSED);
    close(pair[0]);
    close(pair[1]);
}

static void test_receive_header_reports_invalid_fd(void)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    close(pair[0]);
    char header[HEADER_LIMIT + 1];
    double deadline;
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

static void assert_request_response(const char *method, const char *target, const char *expected)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    char buffer[256];
    int length = snprintf(buffer, sizeof(buffer),
        "%s %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", method, target);
    assert(length > 0 && (size_t)length < sizeof(buffer));
    assert(send(pair[1], buffer, (size_t)length, 0) == length);
    assert(serve_http(pair[0], -1, 1) == 0);
    receive_exact(pair[1], buffer, strlen(expected));
    assert(strncmp(buffer, expected, strlen(expected)) == 0);
    close(pair[0]);
    close(pair[1]);
}

static void test_invalid_path_returns_400(void)
{
    assert_request_response("POST", "/%00", "HTTP/1.1 400 ");
}

static void test_static_post_returns_501(void)
{
    assert_request_response("POST", "/", "HTTP/1.1 501 ");
}

static void test_invalid_root_returns_500(void)
{
    assert_request_response("GET", "/", "HTTP/1.1 500 ");
}

static int parse_length(const char *value, struct request *request)
{
    char header[128];
    snprintf(header, sizeof(header), "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: %s\r\n\r\n", value);
    return parse(header, request);
}

static void test_content_length_values(void)
{
    const struct { const char *value; int status; size_t length; } cases[] = {
        {"0", 0, 0}, {"0004", 0, 4}, {"", 400, 0},
        {"+1", 400, 0}, {"-1", 400, 0}, {"1x", 400, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct request request;
        assert(parse_length(cases[i].value, &request) == cases[i].status);
        if (cases[i].status == 0)
            assert(request.content_length == cases[i].length);
    }
}

static void test_content_length_rejects_size_max_as_too_large(void)
{
    char maximum[3 * sizeof(size_t) + 1];
    int n = snprintf(maximum, sizeof(maximum), "%zu", SIZE_MAX);
    assert(n > 0 && (size_t)n < sizeof(maximum));
    struct request request;
    assert(parse_length(maximum, &request) == 413 && request.content_length == SIZE_MAX);
}

static void test_content_length_rejects_numeric_overflow(void)
{
    char overflow[3 * sizeof(size_t) + 2];
    int n = snprintf(overflow, sizeof(overflow), "%zu0", SIZE_MAX);
    assert(n > 0 && (size_t)n < sizeof(overflow));
    struct request request;
    assert(parse_length(overflow, &request) == 400);
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
}

static void test_header_leaves_body_in_socket(void)
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
    close(pair[0]);
    close(pair[1]);
}

static void test_header_respects_buffer_capacity(void)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const char *header = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t length = strlen(header);
    double deadline;
    char small[8];
    memset(small, 'z', sizeof(small));
    assert(send(pair[1], header, length, 0) == (ssize_t)length);
    assert(receive_header(pair[0], small, 4, 1, &deadline) == 431);
    for (size_t i = 4; i < sizeof(small); ++i)
        assert(small[i] == 'z');
    close(pair[0]);
    close(pair[1]);
}

static void test_generated_valid_content_lengths(void)
{
    for (size_t length = 0; length < 128; ++length) {
        char header[256];
        struct request request;
        snprintf(header, sizeof(header),
            "POST /form?q=%zu HTTP/1.1\r\nhOsT: x\r\nContent-Length:\t%04zu \t\r\n\r\n",
            length, length);
        assert(parse(header, &request) == 0);
        assert(strcmp(request.method, "POST") == 0);
        char target[64];
        snprintf(target, sizeof(target), "/form?q=%zu", length);
        assert(strcmp(request.target, target) == 0);
        assert(request.http11 && !request.close && request.content_length == length);
    }
}

static void test_generated_duplicate_content_lengths(void)
{
    for (size_t length = 0; length < 128; ++length) {
        char header[256];
        struct request request;
        snprintf(header, sizeof(header),
            "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\ncontent-length: %zu\r\n\r\n",
            length, length);
        assert(parse(header, &request) == 400);
    }
}

static void test_decodes_all_percent_encoded_bytes(void)
{
    for (unsigned int byte = 0; byte < 256; ++byte) {
        char target[16], path[3];
        snprintf(target, sizeof(target), "/%%%02X", byte);
        bool forbidden = byte < 32 || byte == 127 || byte == '\\';
        assert(decode_path(target, path, sizeof(path)) == (forbidden ? 400 : 0));
        if (!forbidden) {
            assert(path[0] == '/' && (unsigned char)path[1] == byte && path[2] == '\0');
            assert(decode_path(target, path, sizeof(path) - 1) == 414);
        }
    }
}

static void mutate_input(size_t i, char *input, uint32_t *random)
{
    const char *seeds[] = {
        "GET /a%20b HTTP/1.1\r\nHost: x\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: close, keep-alive\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n\r\n",
        "/%2e%2e/a%00?query",
    };
    strcpy(input, seeds[i % (sizeof(seeds) / sizeof(seeds[0]))]);
    size_t length = strlen(input);
    for (size_t j = 0; j < 1 + i % 4; ++j) {
        *random = *random * 1664525u + 1013904223u;
        size_t position = (*random >> 8) % length;
        input[position] = (char)(*random >> 24);
    }
}

static void test_mutated_parser_memory_safety(void)
{
    uint32_t random = 12345;
    for (size_t i = 0; i < 12000; ++i) {
        char input[256];
        mutate_input(i, input, &random);
        char parsed[sizeof(input)];
        strcpy(parsed, input);
        struct request request = {.http11 = true, .close = true};
        int status = parse_request(parsed, &request);
        assert(status || (request.method && request.target && request.content_length <= BODY_LIMIT));
    }
}

static void test_mutated_decoder_memory_safety(void)
{
    uint32_t random = 12345;
    for (size_t i = 0; i < 12000; ++i) {
        char input[256];
        mutate_input(i, input, &random);
        char path[64];
        memset(path, 'z', sizeof(path));
        size_t capacity = i % sizeof(path);
        int status = decode_path(input, path, capacity);
        for (size_t j = capacity; j < sizeof(path); ++j)
            assert(path[j] == 'z');
        if (!status)
            assert(memchr(path, '\0', capacity));
    }
}

static void test_header_names_are_case_insensitive(void)
{
    struct request request;
    assert(parse("GET / HTTP/1.1\r\nhOsT: localhost\r\ncOnTeNt-LeNgTh: 0004\r\n\r\n", &request) == 0);
    assert(request.http11 && !request.close && request.content_length == 4);
}

static void test_http10_disables_keep_alive(void)
{
    struct request request;
    assert(parse("POST / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", &request) == 0);
    assert(!request.http11 && request.close);
}

static void test_close_token_overrides_repeated_connection_header(void)
{
    struct request request;
    assert(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: CLOSE\r\nConnection: keep-alive\r\n\r\n", &request) == 0);
    assert(request.close);
}

static void test_rejects_invalid_header_syntax(void)
{
    const char *cases[] = {
        "GET / HTTP/1.1\r\nHost: x\r\nX: value\nfolded\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\n X: value\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive,\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: +1\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1, 1\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct request request;
        assert(parse(cases[i], &request) == 400);
    }
}

static void test_body_size_boundary(void)
{
    struct request request;
    assert(parse_length("1048576", &request) == 0);
    assert(parse_length("1048577", &request) == 413);
}

static void test_header_conflicts_take_priority_over_body_size(void)
{
    struct request request;
    assert(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n"
                 "Transfer-Encoding: chunked\r\n\r\n", &request) == 400);
    assert(parse("POST / HTTP/1.1\r\nContent-Length: 1048577\r\n\r\n", &request) == 400);
}

static void test_http10_rejects_transfer_encoding(void)
{
    struct request request;
    assert(parse("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n", &request) == 400);
}

static void test_path_decoder_removes_query(void)
{
    char path[URI_LIMIT + 1];
    assert(decode_path("/a%20b.txt?version=1", path, sizeof(path)) == 0);
    assert(strcmp(path, "/a b.txt") == 0);
}

static void test_path_decoder_decodes_only_once(void)
{
    char path[URI_LIMIT + 1];
    assert(decode_path("/%252e%252e/file", path, sizeof(path)) == 0);
    assert(strcmp(path, "/%2e%2e/file") == 0);
}

static void test_path_decoder_rejects_invalid_targets(void)
{
    const char *cases[] = {"/%00", "/%0a", "http://host/file"};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char path[URI_LIMIT + 1];
        assert(decode_path(cases[i], path, sizeof(path)) == 400);
    }
}

int main(int argc, char **argv)
{
    const struct test_case tests[] = {
        TEST_CASE(test_receive_preserves_complete_header),
        TEST_CASE(test_receive_body_reports_expired_deadline),
        TEST_CASE(test_receive_body_reports_premature_eof),
        TEST_CASE(test_receive_header_reports_eof),
        TEST_CASE(test_receive_header_reports_invalid_fd),
        TEST_CASE(test_error_response_failure),
        TEST_CASE(test_invalid_path_returns_400),
        TEST_CASE(test_static_post_returns_501),
        TEST_CASE(test_invalid_root_returns_500),
        TEST_CASE(test_content_length_values),
        TEST_CASE(test_content_length_rejects_size_max_as_too_large),
        TEST_CASE(test_content_length_rejects_numeric_overflow),
        TEST_CASE(test_request_line),
        TEST_CASE(test_header_leaves_body_in_socket),
        TEST_CASE(test_header_respects_buffer_capacity),
        TEST_CASE(test_generated_valid_content_lengths),
        TEST_CASE(test_generated_duplicate_content_lengths),
        TEST_CASE(test_decodes_all_percent_encoded_bytes),
        TEST_CASE(test_mutated_parser_memory_safety),
        TEST_CASE(test_mutated_decoder_memory_safety),
        TEST_CASE(test_header_names_are_case_insensitive),
        TEST_CASE(test_http10_disables_keep_alive),
        TEST_CASE(test_close_token_overrides_repeated_connection_header),
        TEST_CASE(test_rejects_invalid_header_syntax),
        TEST_CASE(test_body_size_boundary),
        TEST_CASE(test_header_conflicts_take_priority_over_body_size),
        TEST_CASE(test_http10_rejects_transfer_encoding),
        TEST_CASE(test_path_decoder_removes_query),
        TEST_CASE(test_path_decoder_decodes_only_once),
        TEST_CASE(test_path_decoder_rejects_invalid_targets),
    };
    return run_tests(argc, argv, tests, sizeof(tests) / sizeof(tests[0]));
}
