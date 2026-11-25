#include "proxy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static char out[FORWARD_HEADER_SIZE];

static int response(const char *s, uint64_t *length)
{
    return proxy_response(s, strlen(s), length, out, sizeof(out));
}

int main(void)
{
    struct request request;
    const char *input = "POST /echo?q=%26 HTTP/1.1\r\nHost: old\r\nContent-Length: 3\r\n"
        "Content-Type: application/octet-stream\r\nConnection: keep-alive, X-Private\r\n"
        "X-Private: hidden\r\nConnection: X-Other\r\nX-Other: secret\r\nX-End: keep\r\n\r\n";
    CHECK(proxy_request(input, strlen(input), "127.0.0.1:8081", &request, out, sizeof(out)) > 0);
    CHECK(request.content_length == 3 && !strcmp(request.target, "/echo?q=%26"));
    CHECK(strstr(out, "Host: 127.0.0.1:8081\r\n"));
    CHECK(strstr(out, "Connection: close\r\n") && strstr(out, "Via: 1.1 jungle-proxy\r\n"));
    CHECK(!strstr(out, "X-Private") && !strstr(out, "X-Other") && strstr(out, "X-End: keep"));
    input = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 65537\r\n\r\n";
    CHECK(proxy_request(input, strlen(input), "x", &request, out, sizeof(out)) == -413);
    input = "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
    CHECK(proxy_request(input, strlen(input), "x", &request, out, sizeof(out)) == -400);
    input = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n";
    CHECK(proxy_request(input, strlen(input), "x", &request, out, sizeof(out)) == -400);
    uint64_t size;
    CHECK(response("HTTP/1.1 200 OK\r\nContent-Length: 10737418240\r\n"
        "Connection: X-Internal\r\nX-Internal: no\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\n\r\n", &size) > 0);
    CHECK(size == UINT64_C(10737418240) && !strstr(out, "X-Internal"));
    CHECK(strstr(out, "Set-Cookie: a=1") && strstr(out, "Set-Cookie: b=2"));
    CHECK(response("HTTP/1.1 204 No Content\r\n\r\n", &size) > 0 && size == 0);
    CHECK(response("HTTP/1.1 304 Not Modified\r\nContent-Length: 100\r\n\r\n", &size) > 0 && size == 0);
    const char *invalid[] = {
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551616\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: Content-Length\r\n\r\n",
        "HTTP/1.1 204 No Content\r\nContent-Length: 1\r\n\r\n",
        "HTTP/1.1 100 Continue\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n bad: field\r\n\r\n"
    };
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) CHECK(response(invalid[i], &size) < 0);
    puts("proxy headers: forwarding, framing, hop-by-hop, bounds passed");
    return 0;
}
