#ifndef ROUTE_H
#define ROUTE_H

#include <netinet/in.h>
#include <stddef.h>

#define MAX_ROUTES 16
#define MAX_BACKENDS 8

struct backend { struct sockaddr_in addr; const char *name; };   // name은 "127.0.0.1:8001" (argv 안)
struct route {
    const char *prefix;
    size_t prefix_len;
    struct backend b[MAX_BACKENDS];
    int n, next;
};

int route_add(const char *arg);                                   // 0, 틀리면 메시지를 찍고 -1
struct route *route_find(const char *target, size_t len);         // 맞는 규칙, 없으면 NULL(정적)
const struct backend *route_pick(struct route *r);                // 지금 순서의 백엔드
void route_advance(struct route *r);

#endif
