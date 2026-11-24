#ifndef PROXY_H
#define PROXY_H

#include "http.h"
#include <stdint.h>

enum {
    PROXY_CLIENTS = 8,
    RELAY_SIZE = 16384,
    FORWARD_HEADER_SIZE = HEADER_LIMIT + 1024,
    CONNECT_MS = 3000,
    IDLE_MS = 5000,
    TOTAL_MS = 10000
};

/* The request parser is shared; forwarding policy belongs to the proxy. */
int proxy_request(const char *header, size_t length, const char *host,
                  struct request *request, char *out, size_t capacity);
int proxy_response(const char *header, size_t length, uint64_t *body,
                   char *out, size_t capacity);

#endif
