#ifndef PROXY_ROUTES_H
#define PROXY_ROUTES_H

#include <netinet/in.h>
#include <stdbool.h>

struct proxy_route {
    char *path;
    struct sockaddr_in address;
    int backend_id;
};

enum { PROXY_BACKEND_LIMIT = 128 };

/* Loaded once before server_run/fork; immutable until routes_free. */
int routes_load(const char *path);
bool routes_enabled(void);
void routes_free(void);
const struct proxy_route *routes_match(const char *path);

#endif
