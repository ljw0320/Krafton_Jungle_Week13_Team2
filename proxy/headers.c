#define _POSIX_C_SOURCE 200809L
#include "headers.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool token(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c));
}

static char *trim(char *p)
{
    while (*p == ' ' || *p == '\t')
        ++p;
    char *end = p + strlen(p);
    while (end > p && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return p;
}

int proxy_parse_headers(struct proxy_headers *h, const char *raw)
{
    size_t size = strlen(raw);
    if (size > HEADER_LIMIT || size < 4 || strcmp(raw + size - 4, "\r\n\r\n") != 0)
        return -1;
    memcpy(h->data, raw, size + 1);
    h->count = 0;
    h->first = h->data;
    char *line = strstr(h->data, "\r\n");
    if (!line)
        return -1;
    *line = '\0';
    for (const unsigned char *p = (unsigned char *)h->first; *p; ++p)
        if (*p < 32 || *p == 127)
            return -1;
    line += 2;
    while (*line) {
        char *end = strstr(line, "\r\n");
        if (!end)
            return -1;
        if (end == line)
            return end[2] == '\0' ? 0 : -1;
        *end = '\0';
        char *colon = strchr(line, ':');
        if (!colon || colon == line || h->count == HEADER_LIMIT / 4)
            return -1;
        *colon = '\0';
        for (unsigned char *p = (unsigned char *)line; *p; ++p)
            if (!token(*p))
                return -1;
        for (unsigned char *p = (unsigned char *)(colon + 1); *p; ++p)
            if ((*p < 32 && *p != '\t') || *p == 127)
                return -1;
        h->fields[h->count++] = (struct proxy_field){line, trim(colon + 1)};
        line = end + 2;
    }
    return -1;
}

static bool equal(const char *a, const char *b)
{
    return strcasecmp(a, b) == 0;
}

/* Check every Connection token before filtering any fields. Framing cannot be hidden. */
static int connection_fields(const struct proxy_headers *h, const char *name)
{
    int found = 0;
    for (size_t i = 0; i < h->count; ++i) {
        if (!equal(h->fields[i].name, "Connection"))
            continue;
        const char *p = h->fields[i].value;
        for (;;) {
            while (*p == ' ' || *p == '\t')
                ++p;
            const char *start = p;
            while (token((unsigned char)*p))
                ++p;
            size_t n = (size_t)(p - start);
            if (!n)
                return -1;
            const char *critical[] = {"Host", "Content-Length", "Transfer-Encoding"};
            for (size_t j = 0; j < sizeof(critical) / sizeof(*critical); ++j)
                if (strlen(critical[j]) == n && strncasecmp(start, critical[j], n) == 0)
                    return -1;
            if (strlen(name) == n && strncasecmp(start, name, n) == 0)
                found = 1;
            while (*p == ' ' || *p == '\t')
                ++p;
            if (!*p)
                break;
            if (*p++ != ',')
                return -1;
        }
    }
    return found;
}

static bool hop_header(const char *name)
{
    const char *names[] = {"Connection", "Proxy-Connection", "Keep-Alive", "TE",
        "Trailer", "Upgrade", "Proxy-Authenticate", "Proxy-Authorization"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i)
        if (equal(name, names[i]))
            return true;
    return false;
}

static bool valid_port(const char *p)
{
    unsigned port = 0;
    if (!*p)
        return false;
    for (; *p; ++p) {
        if (*p < '0' || *p > '9' || port > 6553)
            return false;
        port = port * 10 + (unsigned)(*p - '0');
    }
    return port > 0 && port <= 65535;
}

static bool valid_host(const char *value)
{
    if (!*value)
        return false;
    if (*value == '[') {
        const char *end = strchr(value, ']');
        if (!end || end - value > INET6_ADDRSTRLEN)
            return false;
        char ip[INET6_ADDRSTRLEN];
        size_t n = (size_t)(end - value - 1);
        memcpy(ip, value + 1, n);
        ip[n] = '\0';
        struct in6_addr address;
        return inet_pton(AF_INET6, ip, &address) == 1 &&
            (!end[1] || (end[1] == ':' && valid_port(end + 2)));
    }
    const char *colon = strchr(value, ':');
    size_t n = colon ? (size_t)(colon - value) : strlen(value);
    if (!n || n > 253 || (colon && !valid_port(colon + 1)))
        return false;
    if (value[n - 1] == '.')
        --n;
    size_t label = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c == '.') {
            if (!label || value[i - 1] == '-')
                return false;
            label = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || (c == '-' && label)))
                return false;
            if (++label > 63)
                return false;
        }
    }
    return label && value[n - 1] != '-';
}

static int length_value(const char *p, uintmax_t *out)
{
    if (!*p)
        return -1;
    uintmax_t value = 0;
    for (; *p; ++p) {
        if (*p < '0' || *p > '9' || value > (UINTMAX_MAX - (unsigned)(*p - '0')) / 10)
            return -1;
        value = value * 10 + (unsigned)(*p - '0');
    }
    *out = value;
    return 0;
}

static int append_field(char *output, size_t *used, const char *name, const char *value)
{
    const size_t capacity = HEADER_LIMIT + 512;
    int n = snprintf(output + *used, capacity - *used, "%s:%s\r\n", name, value);
    if (n < 0 || (size_t)n >= capacity - *used)
        return -1;
    *used += (size_t)n;
    return 0;
}

/* The output may grow slightly when adding a canonical length and connection field. */
static int build_fields(const struct proxy_headers *h, char *out, size_t *used,
                         bool request)
{
    if (connection_fields(h, "") < 0)
        return -1;
    for (size_t i = 0; i < h->count; ++i) {
        const char *name = h->fields[i].name;
        if (hop_header(name) || equal(name, "Content-Length") ||
            connection_fields(h, name) == 1)
            continue;
        if (request && (equal(name, "Expect") || equal(name, "Forwarded") ||
                        strncasecmp(name, "X-Forwarded-", 12) == 0))
            continue;
        if (append_field(out, used, name, h->fields[i].value))
            return -1;
    }
    return 0;
}

int proxy_request_header(const char *raw, const struct request *request, char **out)
{
    int status = 400;
    struct proxy_headers *h = malloc(sizeof(*h));
    char *output = malloc(HEADER_LIMIT + 512);
    if (!h || !output) {
        status = 500;
        goto done;
    }
    if (proxy_parse_headers(h, raw))
        goto done;
    size_t hosts = 0;
    for (size_t i = 0; i < h->count; ++i) {
        if (equal(h->fields[i].name, "Host")) {
            if (!valid_host(h->fields[i].value))
                goto done;
            ++hosts;
        }
    }
    if (hosts != 1)
        goto done;
    int n = snprintf(output, HEADER_LIMIT + 512, "%s %s HTTP/1.1\r\n",
                     request->method, request->target);
    if (n < 0 || n >= HEADER_LIMIT + 512)
        goto done;
    size_t used = (size_t)n;
    if (build_fields(h, output, &used, true))
        goto done;
    char length[32];
    snprintf(length, sizeof(length), "%zu", request->content_length);
    if (append_field(output, &used, "Content-Length", length) ||
        append_field(output, &used, "Connection", "close"))
        goto done;
    memcpy(output + used, "\r\n", 3);
    *out = output;
    output = NULL;
    status = 0;
done:
    free(h);
    free(output);
    return status;
}

int proxy_response_header(const char *raw, const struct request *request,
                          char **out, uintmax_t *length)
{
    int result = 502;
    struct proxy_headers *h = malloc(sizeof(*h));
    char *output = malloc(HEADER_LIMIT + 512);
    if (!h || !output) {
        result = 500;
        goto done;
    }
    if (proxy_parse_headers(h, raw))
        goto done;
    const char *line = h->first;
    if (strlen(line) < 13 || (strncmp(line, "HTTP/1.0 ", 9) && strncmp(line, "HTTP/1.1 ", 9)) ||
        line[9] < '2' || line[9] > '5' || line[10] < '0' || line[10] > '9' ||
        line[11] < '0' || line[11] > '9' || line[12] != ' ')
        goto done;
    int code = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
    bool has_length = false;
    uintmax_t declared = 0;
    for (size_t i = 0; i < h->count; ++i) {
        const char *name = h->fields[i].name;
        if (equal(name, "Transfer-Encoding"))
            goto done;
        if (equal(name, "Content-Length")) {
            if (has_length || length_value(h->fields[i].value, &declared))
                goto done;
            has_length = true;
        }
    }
    if ((code == 204 && has_length) || (code != 204 && code != 304 && !has_length))
        goto done;
    int n = snprintf(output, HEADER_LIMIT + 512, "HTTP/1.%d %s\r\n",
                     request->http11 ? 1 : 0, line + 9);
    if (n < 0 || n >= HEADER_LIMIT + 512)
        goto done;
    size_t used = (size_t)n;
    if (build_fields(h, output, &used, false))
        goto done;
    if (has_length) {
        char value[32];
        snprintf(value, sizeof(value), "%ju", declared);
        if (append_field(output, &used, "Content-Length", value))
            goto done;
    }
    if (append_field(output, &used, "Connection", request->close ? "close" : "keep-alive"))
        goto done;
    memcpy(output + used, "\r\n", 3);
    *length = (code == 204 || code == 304) ? 0 : declared;
    *out = output;
    output = NULL;
    result = 0;
done:
    free(output);
    free(h);
    return result;
}
