#include "http.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int parse(const char *text, struct request *request)
{
    return http_parse(text, strlen(text), request);
}

static void test_get(void)
{
    struct request request;
    const char *text = "GET /style.css?x=1 HTTP/1.1\r\nHost: localhost\r\n\r\n";

    assert(http_header_end(text, strlen(text) - 1) == 0);
    assert(http_header_end(text, strlen(text)) == strlen(text));

    assert(parse(text, &request) == 0);
    assert(request.method == METHOD_GET);
    assert(!strcmp(request.target, "/style.css?x=1"));

    assert(parse("GET / HTTP/1.0\r\n\r\n", &request) == 0);

    puts("PASS GET / header boundary");
}

static void test_post(void)
{
    struct request request;

    assert(parse("POST /echo HTTP/1.1\r\nHost: x\r\n"
                 "content-length: 65536\r\n\r\n", &request) == 0);
    assert(request.method == METHOD_POST && request.content_length == BODY_LIMIT);

    assert(parse("POST /echo HTTP/1.0\r\nContent-Length: 0\r\n\r\n", &request) == 0);

    assert(parse("POST /echo HTTP/1.0\r\n\r\n", &request) == 411);
    assert(parse("POST /echo HTTP/1.0\r\nContent-Length: 65537\r\n\r\n", &request) == 413);
    assert(request.content_length == 65537);
    assert(parse("POST /echo HTTP/1.0\r\nContent-Length: 999999999999999999999\r\n\r\n", &request) == 413);
    assert(request.content_length == 0);
    assert(parse("POST /cgi-bin/adder HTTP/1.0\r\nContent-Length: 0\r\n"
                 "Content-Type: application/x-www-form-urlencoded\r\n\r\n", &request) == 0);
    assert(!strcmp(request.content_type, "application/x-www-form-urlencoded"));

    puts("PASS POST / body length boundary");
}

static void test_errors(void)
{
    struct request request;

    assert(parse("GET / HTTP/1.1\r\n\r\n", &request) == 400);

    assert(parse("HEAD / HTTP/1.0\r\n\r\n", &request) == 405);
    assert(request.suppress_body);
    assert(parse("DELETE / HTTP/1.0\r\n\r\n", &request) == 405);

    assert(parse("GET / HTTP/1.0\r\nContent-Length: 1\r\n\r\n", &request) == 400);
    assert(parse("POST /echo HTTP/1.0\r\nContent-Length: 0\r\n"
                 "Content-Length: 0\r\n\r\n", &request) == 400);

    assert(parse("GET / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n", &request) == 400);
    assert(parse("GET / HTTP/1.0\r\nX: a\nb\r\n\r\n", &request) == 400);
    assert(parse("GET / HTTP/1.0\r\nBad : x\r\n\r\n", &request) == 400);
    assert(parse("POST /cgi-bin/adder HTTP/1.0\r\nContent-Length: 0\r\n"
                 "Content-Type: a\r\nContent-Type: b\r\n\r\n", &request) == 400);

    puts("PASS invalid requests / unsupported methods");
}

static void test_response(void)
{
    char out[512];

    assert(http_response(out, sizeof(out), 405, 4, "text/plain", false) > 0);
    assert(strstr(out, "Content-Length: 4\r\n"));
    assert(strstr(out, "Connection: close\r\n"));
    assert(strstr(out, "Allow: GET, POST\r\n"));
    assert(http_header_end(out, strlen(out)) == strlen(out));

    assert(http_response(out, 8, 200, 0, "text/plain", true) == -1);
    assert(http_response(out, sizeof(out), 204, 0, "text/plain", true) > 0);
    assert(!strstr(out, "Content-Length:"));
    assert(strstr(out, "Connection: keep-alive\r\n"));

    puts("PASS response headers / output bound");
}

static void test_connection(void)
{
    struct request request;
    assert(parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n", &request) == 0);
    assert(request.http11 && !request.close_connection);
    assert(parse("GET / HTTP/1.1\r\nHost: x\r\n"
                 "Connection: Keep-Alive, cLoSe\r\nConnection: upgrade\r\n\r\n", &request) == 0);
    assert(request.close_connection);
    assert(parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", &request) == 0);
    assert(!request.http11 && !request.close_connection);
    assert(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: bad token\r\n\r\n", &request) == 400);
    puts("PASS Connection tokens / close precedence / HTTP version");
}

static void test_files(const char *path)
{
    int root = open(path, O_RDONLY | O_DIRECTORY);
    assert(root >= 0);

    int status;
    off_t size;
    int fd = file_open(root, "/?v=1", &size, &status);
    assert(fd >= 0 && status == 200 && size > 0);
    close(fd);

    assert(file_open(root, "/missing-file", &size, &status) == -1 && status == 404);
    assert(file_open(root, "/%2e%2e/README.md", &size, &status) == -1 && status == 403);
    assert(file_open(root, "/%00", &size, &status) == -1 && status == 400);
    assert(!strcmp(file_type("/style.css?q=1"), "text/css; charset=utf-8"));

    close(root);

    puts("PASS static file / traversal / MIME");
}

int main(int argc, char **argv)
{
    assert(argc == 2);

    test_get();
    test_post();
    test_errors();
    test_response();
    test_connection();
    test_files(argv[1]);

    puts("All 6 unit groups passed.");

    return 0;
}
