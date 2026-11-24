#include "server.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* CGI 헤더의 제어 문자와 내장 NUL을 거부합니다. 본문은 바이너리를 허용합니다. */
static bool valid_header(const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c < 32 && c != '\r' && c != '\n' && c != '\t') || c == 127)
            return false;
        if (c == '\r' && (i + 1 == length || text[i + 1] != '\n'))
            return false;
        if (c == '\n' && (!i || text[i - 1] != '\r'))
            return false;
    }
    return true;
}

static int status_value(const char *value)
{
    if (strlen(value) < 3 || value[0] < '2' || value[0] > '5' ||
        !isdigit((unsigned char)value[1]) || !isdigit((unsigned char)value[2]) ||
        (value[3] && value[3] != ' '))
        return 0;
    return (value[0] - '0') * 100 + (value[1] - '0') * 10 + value[2] - '0';
}

/* 응답 헤더는 Content-Type과 선택적 Status만 지원합니다. 길이는 서버가 계산합니다. */
static bool parse_headers(char *text, char *type, int *status)
{
    bool has_status = false;
    for (char *line = text; *line; ) {
        char *end = strstr(line, "\r\n"), *value = strchr(line, ':');
        if (!end || !value || value > end)
            return false;
        *end = '\0';
        *value++ = '\0';
        while (*value == ' ' || *value == '\t')
            value++;
        if (!strcasecmp(line, "Content-Type")) {
            if (*type || !*value || strlen(value) >= 256)
                return false;
            strcpy(type, value);
        } else if (!strcasecmp(line, "Status")) {
            if (has_status || !(*status = status_value(value)))
                return false;
            has_status = true;
        } else
            return false;
        line = end + 2;
    }
    return *type != '\0';
}

/* CGI 헤더를 클라이언트에 그대로 보내지 않고 공통 HTTP 응답 생성기를 사용합니다. */
int cgi_response(struct connection *client)
{
    char type[256] = {0};
    int status = 200;
    size_t end = http_header_end(client->context->cgi_output, client->cgi.length);
    if (!end || end > HEADER_LIMIT || client->cgi.length - end > BODY_LIMIT ||
        !valid_header(client->context->cgi_output, end))
        return 502;
    client->context->cgi_output[end - 2] = '\0';
    if (!parse_headers(client->context->cgi_output, type, &status))
        return 502;
    client->length = client->cgi.length - end;
    if ((status == 204 || status == 205 || status == 304) && client->length)
        return 502;
    memcpy(client->context->data, client->context->cgi_output + end, client->length);
    respond(client, status, (off_t)client->length, type);
    return 0;
}
