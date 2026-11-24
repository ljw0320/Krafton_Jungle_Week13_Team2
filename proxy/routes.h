#ifndef PROXY_ROUTES_H
#define PROXY_ROUTES_H

#include <netinet/in.h>

struct proxy_route {
    char *path;
    struct sockaddr_in address;
};

/* Loaded once before server_run/fork; immutable until routes_free. */
int routes_load(const char *path);
void routes_free(void);
const struct proxy_route *routes_match(const char *path);

#endif
