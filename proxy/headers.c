#include "proxy.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

enum { MAX_FIELDS = 512 };
struct field { char *name, *value; };
struct headers {
    char storage[HEADER_LIMIT + 1];
    char *line;
    struct field fields[MAX_FIELDS];
    size_t count;
};

static bool token(const char *s)
{
    if (!*s) return false;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && !strchr("!#$%&'*+-.^_`|~", *s))
            return false;
    return true;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
    return s;
}

static bool parse(struct headers *h, const char *input, size_t length)
{
    memset(h, 0, sizeof(*h));
    if (length > HEADER_LIMIT || http_header_end(input, length) != length) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)input[i];
        if ((c < 32 && c != '\r' && c != '\n' && c != '\t') || c == 127) return false;
        if (c == '\r' && (i + 1 == length || input[i + 1] != '\n')) return false;
        if (c == '\n' && (!i || input[i - 1] != '\r')) return false;
    }
    memcpy(h->storage, input, length);
    h->line = h->storage;
    char *end = strstr(h->line, "\r\n");
    if (!end) return false;
    *end = 0;
    for (char *line = end + 2; *line != '\r'; line = end + 2) {
        if (h->count == MAX_FIELDS || !(end = strstr(line, "\r\n"))) return false;
        *end = 0;
        char *colon = strchr(line, ':');
        if (!colon) return false;
        *colon = 0;
        if (!token(line)) return false;
        h->fields[h->count++] = (struct field){line, trim(colon + 1)};
    }
    return true;
}

static bool connection_options(struct headers *h)
{
    for (size_t i = 0; i < h->count; i++) {
        if (strcasecmp(h->fields[i].name, "Connection")) continue;
        const char *p = h->fields[i].value;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            const char *end = strchr(p, ',');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
            char option[HEADER_LIMIT + 1];
            memcpy(option, p, n); option[n] = 0;
            if (!token(option)) return false;
            if (!end) break;
            p = end + 1;
            if (!*p) return false;
        }
    }
    return true;
}

/* Connection can nominate arbitrary hop-by-hop field names. Inspect all copies. */
static bool hop(struct headers *h, const char *name)
{
    const char *fixed[] = {"Connection", "Proxy-Connection", "Keep-Alive", "TE",
        "Trailer", "Transfer-Encoding", "Upgrade", "Proxy-Authorization", "Proxy-Authenticate"};
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
        if (!strcasecmp(name, fixed[i])) return true;
    for (size_t i = 0; i < h->count; i++) {
        if (strcasecmp(h->fields[i].name, "Connection")) continue;
        const char *p = h->fields[i].value;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            const char *end = strchr(p, ',');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
            if (strlen(name) == n && !strncasecmp(name, p, n)) return true;
            if (!end) break;
            p = end + 1;
        }
    }
    return false;
}

static bool append(char *out, size_t capacity, size_t *used, const char *name, const char *value)
{
    int n = snprintf(out + *used, capacity - *used, "%s: %s\r\n", name, value);
    if (n < 0 || (size_t)n >= capacity - *used) return false;
    *used += (size_t)n;
    return true;
}

static int forward(struct headers *h, char *out, size_t capacity, const char *host)
{
    int n = snprintf(out, capacity, "%s\r\n", h->line);
    if (n < 0 || (size_t)n >= capacity) return -1;
    size_t used = (size_t)n;
    for (size_t i = 0; i < h->count; i++) {
        struct field *f = &h->fields[i];
        if (hop(h, f->name) || (host && !strcasecmp(f->name, "Host"))) continue;
        if (!append(out, capacity, &used, f->name, f->value)) return -1;
    }
    if (host && !append(out, capacity, &used, "Host", host)) return -1;
    if (!append(out, capacity, &used, "Via", "1.1 jungle-proxy") ||
        !append(out, capacity, &used, "Connection", "close") || capacity - used < 3) return -1;
    memcpy(out + used, "\r\n", 3);
    return (int)(used + 2);
}

int proxy_request(const char *header, size_t length, const char *host,
                  struct request *request, char *out, size_t capacity)
{
    int error = http_parse(header, length, request);
    if (error) return -error;
    struct headers h;
    if (!parse(&h, header, length) || !connection_options(&h)) return -400;
    /* Framing fields cannot be hidden as hop-by-hop options. */
    if (hop(&h, "Content-Length") || hop(&h, "Host")) return -400;
    int n = forward(&h, out, capacity, host);
    return n < 0 ? -431 : n;
}

int proxy_response(const char *header, size_t length, uint64_t *body,
                   char *out, size_t capacity)
{
    struct headers h;
    if (!parse(&h, header, length) || !connection_options(&h)) return -502;
    if (strlen(h.line) < 12 || (strncmp(h.line, "HTTP/1.1 ", 9) &&
        strncmp(h.line, "HTTP/1.0 ", 9)) || h.line[9] < '2' || h.line[9] > '5' ||
        !isdigit((unsigned char)h.line[10]) || !isdigit((unsigned char)h.line[11]) ||
        (h.line[12] && h.line[12] != ' ')) return -502;
    int status = (h.line[9] - '0') * 100 + (h.line[10] - '0') * 10 + h.line[11] - '0';
    bool has_length = false;
    uint64_t size = 0;
    for (size_t i = 0; i < h.count; i++) {
        struct field *f = &h.fields[i];
        if (!strcasecmp(f->name, "Transfer-Encoding")) return -502;
        if (strcasecmp(f->name, "Content-Length")) continue;
        if (has_length || !*f->value || hop(&h, "Content-Length")) return -502;
        has_length = true;
        for (const char *p = f->value; *p; p++) {
            if (*p < '0' || *p > '9' || size > (UINT64_MAX - (unsigned)(*p - '0')) / 10) return -502;
            size = size * 10 + (unsigned)(*p - '0');
        }
    }
    if (status == 204 && has_length) return -502;
    if (status != 204 && status != 304 && !has_length) return -502;
    *body = status == 204 || status == 304 ? 0 : size;
    return forward(&h, out, capacity, NULL);
}
