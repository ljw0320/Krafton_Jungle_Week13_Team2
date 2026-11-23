#include "http.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* 분할 수신 중에는 마지막 빈 줄이 도착할 때까지 파싱을 미룹니다. */
size_t http_header_end(const char *data, size_t length)
{
    for (size_t i = 3; i < length; i++)
        if (memcmp(data + i - 3, "\r\n\r\n", 4) == 0)
            return i + 1;

    return 0;
}

static bool token(const char *text)
{
    if (!*text)
        return false;

    for (; *text; text++)
        if (!isalnum((unsigned char)*text) &&
            !strchr("!#$%&'*+-.^_`|~", *text))
            return false;

    return true;
}

/* 요청 줄은 method SP target SP version 형태만 허용합니다. */
static int parse_line(char *line, struct request *request)
{
    char *target = strchr(line, ' ');
    if (!target)
        return 400;

    *target++ = '\0';
    char *version = strchr(target, ' ');
    if (!version)
        return 400;
    *version++ = '\0';
    if (!token(line) || target[0] != '/' || strchr(target, '#') ||
        strlen(target) >= TARGET_LIMIT)
        return 400;
    for (const char *p = target; *p; p++)
        if ((unsigned char)*p <= 32 || (unsigned char)*p == 127)
            return 400;
    if (strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1"))
        return 505;

    request->http11 = strcmp(version, "HTTP/1.1") == 0;
    request->suppress_body = strcmp(line, "HEAD") == 0;
    request->method = !strcmp(line, "GET") ? METHOD_GET :
                      !strcmp(line, "POST") ? METHOD_POST : METHOD_OTHER;
    strcpy(request->target, target);

    return 0;
}

static char *trim(char *value)
{
    while (*value == ' ' || *value == '\t')
        value++;

    char *end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';

    return value;
}

static int parse_length(const char *value, struct request *request)
{
    if (request->has_length || !*value)
        return 400;

    request->has_length = true;

    for (; *value; value++) {
        if (*value < '0' || *value > '9')
            return 400;

        unsigned digit = (unsigned)(*value - '0');
        if (request->content_length > (SIZE_MAX - digit) / 10) {
            request->content_length = 0;
            return 413;
        }
        request->content_length = request->content_length * 10 + digit;
    }

    return request->content_length > BODY_LIMIT ? 413 : 0;
}

static int parse_header(char *line, struct request *request)
{
    char *value = strchr(line, ':');
    if (!value)
        return 400;

    *value++ = '\0';
    if (!token(line))
        return 400;

    value = trim(value);
    if (!strcasecmp(line, "Host")) {
        if (request->has_host || !*value || strpbrk(value, " \t"))
            return 400;
        request->has_host = true;
    }

    if (!strcasecmp(line, "Content-Length"))
        return parse_length(value, request);
    if (!strcasecmp(line, "Transfer-Encoding"))
        return 400;
    if (!strcasecmp(line, "Expect"))
        return 417;

    return 0;
}

static int parse_lines(char *text, struct request *request)
{
    char *end = strstr(text, "\r\n");
    if (!end)
        return 400;

    *end = '\0';
    int status = parse_line(text, request);
    char *line = end + 2;

    while (!status && strncmp(line, "\r\n", 2)) {
        end = strstr(line, "\r\n");
        if (!end)
            return 400;

        *end = '\0';
        status = parse_header(line, request);
        line = end + 2;
    }

    return status;
}

static bool valid_bytes(const char *data, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)data[i];

        if ((c < 32 && c != '\r' && c != '\n' && c != '\t') || c == 127)
            return false;
        if (c == '\r' && (i + 1 == length || data[i + 1] != '\n'))
            return false;
        if (c == '\n' && (!i || data[i - 1] != '\r'))
            return false;
    }

    return true;
}

/* 헤더만 복사해 파싱하므로 뒤에 수신한 POST 본문은 훼손하지 않습니다. */
int http_parse(const char *data, size_t length, struct request *request)
{
    char text[HEADER_LIMIT + 1];
    memset(request, 0, sizeof(*request));
    if (length > HEADER_LIMIT)
        return 431;
    if (http_header_end(data, length) != length)
        return 400;
    if (!valid_bytes(data, length))
        return 400;

    memcpy(text, data, length);
    text[length] = '\0';
    int status = parse_lines(text, request);
    if (status)
        return status;

    if (request->http11 && !request->has_host)
        return 400;
    if (request->method == METHOD_OTHER)
        return 405;
    if (request->method == METHOD_POST && !request->has_length)
        return 411;
    if (request->method == METHOD_GET && request->content_length)
        return 400;

    return 0;
}

const char *http_reason(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 411:
        return "Length Required";
    case 413:
        return "Content Too Large";
    case 417:
        return "Expectation Failed";
    case 431:
        return "Request Header Fields Too Large";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "Internal Server Error";
    }
}

/* 본문 길이를 명시하고 모든 응답 뒤 연결을 닫습니다. */
int http_response(char *out, size_t capacity, int status, off_t length,
                  const char *type)
{
    int size = snprintf(out, capacity,
        "HTTP/1.1 %d %s\r\nContent-Length: %jd\r\n"
        "Content-Type: %s\r\nConnection: close\r\n%s\r\n",
        status, http_reason(status), (intmax_t)length, type,
        status == 405 ? "Allow: GET, POST\r\n" : "");

    return size < 0 || (size_t)size >= capacity ? -1 : size;
}
