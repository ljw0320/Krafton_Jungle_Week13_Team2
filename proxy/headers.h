#ifndef PROXY_HEADERS_H
#define PROXY_HEADERS_H

#include "../http/http_internal.h"

struct proxy_field { char *name; char *value; };
struct proxy_headers {
    char data[HEADER_LIMIT + 1];
    char *first;
    struct proxy_field fields[HEADER_LIMIT / 4];
    size_t count;
};

/* Parse a complete raw header without borrowing the source. */
int proxy_parse_headers(struct proxy_headers *headers, const char *raw);
int proxy_request_header(const char *raw, const struct request *request, char **out);
/* Produces a downstream header and body length (0 for 304).
 * reason is a static diagnostic string on failure, NULL on success. */
int proxy_response_header(const char *raw, const struct request *request,
                          char **out, uintmax_t *length, const char **reason);

#endif
