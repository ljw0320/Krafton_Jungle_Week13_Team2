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

static bool token_character(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c));
}

static char *trim_value(char *value)
{
    while (*value == ' ' || *value == '\t')
        ++value;
    char *end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return value;
}

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
