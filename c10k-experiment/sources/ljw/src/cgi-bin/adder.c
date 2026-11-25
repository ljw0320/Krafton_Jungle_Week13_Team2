#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 예제 입력은 URL 디코딩 후 a와 b 두 정수 필드만 허용합니다. */
static bool decode(char *text)
{
    char *out = text;
    for (char *p = text; *p; p++) {
        if (*p == '+')
            *out++ = ' ';
        else if (*p == '%') {
            unsigned value;
            if (!p[1] || !p[2] || !strchr("0123456789abcdefABCDEF", p[1]) ||
                !strchr("0123456789abcdefABCDEF", p[2]) || sscanf(p + 1, "%2x", &value) != 1 || !value)
                return false;
            *out++ = (char)value;
            p += 2;
        } else
            *out++ = *p;
    }
    *out = '\0';
    return true;
}

static bool number(const char *text, long long *value)
{
    char *end;
    if (!*text || !strchr("+-0123456789", *text))
        return false;
    errno = 0;
    *value = strtoll(text, &end, 10);
    return !errno && end != text && !*end;
}

static bool operands(char *text, long long *a, long long *b)
{
    bool has_a = false, has_b = false;
    char *save;
    for (char *part = strtok_r(text, "&", &save); part; part = strtok_r(NULL, "&", &save)) {
        if (!decode(part))
            return false;
        if (!strncmp(part, "a=", 2) && !has_a)
            has_a = number(part + 2, a);
        else if (!strncmp(part, "b=", 2) && !has_b)
            has_b = number(part + 2, b);
        else
            return false;
        if ((!has_a && part[0] == 'a') || (!has_b && part[0] == 'b'))
            return false;
    }
    return has_a && has_b && !((*b > 0 && *a > LLONG_MAX - *b) ||
                               (*b < 0 && *a < LLONG_MIN - *b));
}

static bool read_parameters(char *text, size_t capacity)
{
    const char *method = getenv("REQUEST_METHOD");
    if (method && !strcmp(method, "POST")) {
        const char *length = getenv("CONTENT_LENGTH");
        long long bytes;
        const char *type = getenv("CONTENT_TYPE");
        if (!type || strcmp(type, "application/x-www-form-urlencoded") ||
            !length || !number(length, &bytes) || bytes < 0 || (unsigned long long)bytes >= capacity)
            return false;
        if (fread(text, 1, (size_t)bytes, stdin) != (size_t)bytes || memchr(text, 0, (size_t)bytes))
            return false;
        text[bytes] = '\0';
    } else {
        const char *query = getenv("QUERY_STRING");
        if (!query || strlen(query) >= capacity)
            return false;
        strcpy(text, query);
    }
    return true;
}

/* CGI는 HTTP 상태 줄 대신 CGI 헤더와 계산 결과를 stdout에 씁니다. */
int main(void)
{
    char parameters[256];
    long long a = 0, b = 0;
    if (!read_parameters(parameters, sizeof(parameters)) || !operands(parameters, &a, &b)) {
        printf("Status: 400 Bad Request\r\nContent-Type: application/json\r\n\r\n"
               "{\"error\":\"provide integer a and b without sum overflow\"}\n");
        return 0;
    }
    printf("Content-Type: application/json\r\n\r\n{\"a\":%lld,\"b\":%lld,\"sum\":%lld}\n", a, b, a + b);
    return 0;
}
