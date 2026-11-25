#define _POSIX_C_SOURCE 200809L
#include "http_internal.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>

struct header_fields {
    bool has_host;
    bool has_content_length;
    bool has_transfer_encoding;
    bool has_expect;
};

/* HTTP 토큰에 사용할 수 있는 문자인지 검사한다. */
static bool token_character(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c));
}

/* 헤더 값 앞뒤의 공백과 탭을 제거한다. */
static char *trim_value(char *value)
{
    while (*value == ' ' || *value == '\t')
        ++value;
    char *end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return value;
}

/* Connection 값을 쉼표 단위로 검사하고 close가 있으면 연결 종료를 설정한다. */
static int parse_connection(char *value, bool *close)
{
    for (;;) {
        char *comma = strchr(value, ',');
        if (comma)
            *comma = '\0';
        char *token = trim_value(value);
        if (!*token)
            return 400;
        for (char *p = token; *p; ++p)
            if (!token_character((unsigned char)*p))
                return 400;
        if (strcasecmp(token, "close") == 0)
            *close = true;
        if (!comma)
            return 0;
        value = comma + 1;
    }
}

/* 본문 길이를 정수로 변환하고 숫자 형식과 오버플로를 검사한다. */
static int parse_content_length(const char *value, size_t *length)
{
    if (!*value)
        return 400;
    size_t n = 0;
    for (const char *p = value; *p; ++p) {
        if (*p < '0' || *p > '9' || n > (SIZE_MAX - (size_t)(*p - '0')) / 10)
            return 400;
        n = n * 10 + (size_t)(*p - '0');
    }
    *length = n;
    return 0;
}

/* Splits the line in place; request strings borrow its storage. */
/* 요청 라인을 메서드·요청 대상·HTTP 버전으로 분리하고 형식과 길이를 검사한다. */
static int parse_request_line(char *line, struct request *request)
{
    char *target = strchr(line, ' ');
    if (!target)
        return 400;
    *target++ = '\0';
    char *version = strchr(target, ' ');
    if (!version)
        return 400;
    *version++ = '\0';
    if (!*line || !*target || !*version || strchr(version, ' '))
        return 400;
    for (char *p = line; *p; ++p)
        if (!token_character((unsigned char)*p))
            return 400;
    for (char *p = target; *p; ++p)
        if ((unsigned char)*p <= 32 || (unsigned char)*p == 127)
            return 400;
    if (strlen(target) > URI_LIMIT)
        return 414;
    if (strcmp(version, "HTTP/1.1") == 0)
        request->http11 = true;
    else if (strcmp(version, "HTTP/1.0") == 0)
        request->http11 = false;
    else
        return strlen(version) == 8 && strncmp(version, "HTTP/", 5) == 0 &&
            isdigit((unsigned char)version[5]) && version[6] == '.' &&
            isdigit((unsigned char)version[7]) ? 505 : 400;
    request->method = line;
    request->target = target;
    request->close = !request->http11;
    return 0;
}

/* 헤더 형식을 검사하고 Host, Content-Length, Content-Type 등 주요 값을 저장한다. */
static int parse_headers(char *headers, struct request *request, struct header_fields *fields)
{
    char *line_end;
    for (char *line = headers; *line; line = line_end + 2) {
        line_end = strstr(line, "\r\n");
        if (!line_end)
            return 400;
        *line_end = '\0';
        if (!*line)
            break;
        char *colon = strchr(line, ':');
        if (!colon || colon == line)
            return 400;
        *colon = '\0';
        for (char *p = line; *p; ++p)
            if (!token_character((unsigned char)*p))
                return 400;
        char *value = trim_value(colon + 1);
        for (char *p = value; *p; ++p)
            if (((unsigned char)*p < 32 && *p != '\t') || (unsigned char)*p == 127)
                return 400;
        if (strcasecmp(line, "Host") == 0) {
            if (fields->has_host || !*value || strpbrk(value, " /\\,#?@\t"))
                return 400;
            fields->has_host = true;
        } else if (strcasecmp(line, "Content-Length") == 0) {
            if (fields->has_content_length || parse_content_length(value, &request->content_length))
                return 400;
            fields->has_content_length = true;
        } else if (strcasecmp(line, "Content-Type") == 0) {
            if (request->content_type || !*value)
                return 400;
            request->content_type = value;
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            if (fields->has_transfer_encoding || !*value)
                return 400;
            fields->has_transfer_encoding = true;
        } else if (strcasecmp(line, "Connection") == 0) {
            if (parse_connection(value, &request->close))
                return 400;
        } else if (strcasecmp(line, "Expect") == 0) {
            if (fields->has_expect || !request->http11 || strcasecmp(value, "100-continue") != 0)
                return 417;
            fields->has_expect = true;
            request->expect_continue = true;
        }
    }
    return 0;
}

/* Host 누락, 길이 관련 헤더 충돌, 미지원 전송 방식과 본문 크기 초과를 검사한다. */
static int validate_request_headers(const struct request *request, const struct header_fields *fields)
{
    if ((request->http11 && !fields->has_host) ||
        (fields->has_transfer_encoding && fields->has_content_length))
        return 400;
    if (fields->has_transfer_encoding)
        return request->http11 ? 501 : 400;
    return request->content_length > BODY_LIMIT ? 413 : 0;
}

/* Resets request and mutates the complete NUL-terminated header it borrows from. */
/* 요청 구조체를 초기화하고 요청 라인·헤더 파싱과 최종 검증을 수행한다. */
int parse_request(char *header, struct request *request)
{
    *request = (struct request){.http11 = true, .close = true};
    char *line_end = strstr(header, "\r\n");
    if (!line_end)
        return 400;
    *line_end = '\0';
    int status = parse_request_line(header, request);
    if (status)
        return status;
    struct header_fields fields = {0};
    status = parse_headers(line_end + 2, request, &fields);
    if (status)
        return status;
    return validate_request_headers(request, &fields);
}

/* 16진수 문자 하나를 숫자로 변환하고 잘못된 문자이면 -1을 반환한다. */
static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 쿼리를 제외한 URL 경로를 디코딩하고 잘못된 인코딩·금지 문자·길이 초과를 검사한다. */
int decode_path(const char *target, char *path, size_t capacity)
{
    if (*target != '/' || strchr(target, '#'))
        return 400;
    size_t n = 0;
    for (size_t i = 0; target[i] && target[i] != '?'; ++i) {
        unsigned char c = (unsigned char)target[i];
        if (c == '%') {
            if (!target[i + 1] || !target[i + 2])
                return 400;
            int high = hex_digit((unsigned char)target[i + 1]);
            int low = hex_digit((unsigned char)target[i + 2]);
            if (high < 0 || low < 0)
                return 400;
            c = (unsigned char)(high * 16 + low);
            i += 2;
        }
        if (c == 0 || c < 32 || c == 127 || c == '\\')
            return 400;
        if (n + 1 >= capacity)
            return 414;
        path[n++] = (char)c;
    }
    if (n >= capacity)
        return 414;
    path[n] = '\0';
    return 0;
}
