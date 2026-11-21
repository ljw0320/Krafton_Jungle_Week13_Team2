#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

enum {
    HEADER_LIMIT = 16384,
    BODY_LIMIT = 65536,
    TARGET_LIMIT = 2048
};

enum method {
    METHOD_GET,
    METHOD_POST,
    METHOD_OTHER
};

struct request {
    enum method method;
    char target[TARGET_LIMIT];

    size_t content_length;
    bool has_length;

    bool has_host;
    bool http11;
    bool suppress_body;
};

size_t http_header_end(const char *data, size_t length);
int http_parse(const char *data, size_t length, struct request *request);

const char *http_reason(int status);
int http_response(char *out, size_t capacity, int status, off_t length,
                  const char *type);

int file_open(int root, const char *target, off_t *size, int *status);
const char *file_type(const char *target);

#endif
