#define _POSIX_C_SOURCE 200809L
#include "http_internal.h"
#include <stdio.h>
#include <string.h>

/* 상태 코드에 대응하는 설명 문자열을 반환한다. */
static const char *reason_phrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "Internal Server Error";
    }
}

/* 상태 라인과 본문 형식·길이·연결 정책을 포함한 응답 헤더를 생성하여 전송한다. */
int send_headers(int fd, const struct request *request, int status,
                        const char *type, uintmax_t length)
{
    char header[512];
    int size = snprintf(header, sizeof(header),
        "HTTP/1.%d %d %s\r\nContent-Type: %s\r\nContent-Length: %ju\r\n"
        "Connection: %s\r\n%s\r\n",
        request->http11 ? 1 : 0, status, reason_phrase(status), type, length,
        request->close ? "close" : "keep-alive",
        status == 405 ? "Allow: GET, POST\r\n" : "");
    if (size < 0 || (size_t)size >= sizeof(header))
        return -1;
    return send_all(fd, header, (size_t)size);
}

/* 상태 코드와 설명을 담은 텍스트 오류 응답을 전송하며 HEAD 요청에는 본문을 생략한다. */
int send_error(int fd, const struct request *request, int status)
{
    char body[80];
    int length = snprintf(body, sizeof(body), "%d %s\n", status, reason_phrase(status));
    if (length < 0 || (size_t)length >= sizeof(body) ||
        send_headers(fd, request, status, "text/plain; charset=utf-8", (size_t)length) == -1)
        return -1;
    if (request->method && strcmp(request->method, "HEAD") == 0)
        return 0;
    return send_all(fd, body, (size_t)length);
}
