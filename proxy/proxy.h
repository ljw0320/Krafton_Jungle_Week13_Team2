#ifndef PROXY_H
#define PROXY_H

#include "routes.h"
#include "headers.h"

/* Child-only control channel to the parent; -1 disables admission for unit callers. */
void proxy_set_control(int fd);

/* Borrows request/header/body. On failure closes downstream reuse via request.close.
 * Owns and closes its upstream socket; returns 0 if a response was sent, -1 otherwise. */
int serve_proxy(int client, const struct proxy_route *route, struct request *request,
                const char *header, const char *body);

#endif
