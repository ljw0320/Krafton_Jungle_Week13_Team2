#include "route.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct route routes[MAX_ROUTES];
static int nroutes;

static int route_fail(const char *arg, const char *why) {
    fprintf(stderr, "route: %s: %s\n", arg, why);
    return -1;
}

// prefix는 NUL로 끝나지 않는다(argv 안을 가리킨다). 접두사 검사: '/'로 시작해 '/'로 끝나고, 0x21~0x7E, #?%= 와 '//', '.', '..' 세그먼트 금지
static const char *prefix_error(const char *p, size_t n) {
    if (n == 0 || p[0] != '/' || p[n - 1] != '/') return "prefix must start and end with /";
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)p[i];
        if (ch < 0x21 || ch > 0x7E || strchr("#?%=", ch)) return "invalid character in prefix";
        if (i > 0 && ch == '/' && p[i - 1] == '/') return "empty segment in prefix";
        if (ch == '.' && p[i - 1] == '/' &&
            (p[i + 1] == '/' || (p[i + 1] == '.' && p[i + 2] == '/'))) return "dot segment in prefix";
    }
    return NULL;
}

int route_add(const char *arg) {
    const char *eq = strchr(arg, '=');
    if (!eq) return route_fail(arg, "missing =");
    size_t n = (size_t)(eq - arg);
    const char *why = prefix_error(arg, n);
    if (why) return route_fail(arg, why);

    const char *addr = eq + 1;
    const char *colon = strchr(addr, ':');
    if (!colon || colon - addr >= INET_ADDRSTRLEN) return route_fail(arg, "backend must be <ip>:<port>");
    char ip[INET_ADDRSTRLEN];
    memcpy(ip, addr, (size_t)(colon - addr));
    ip[colon - addr] = '\0';
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) return route_fail(arg, "invalid IPv4 address");

    const char *ps = colon + 1;
    if (!*ps || strspn(ps, "0123456789") != strlen(ps)) return route_fail(arg, "invalid port");
    long port = strtol(ps, NULL, 10);
    if (port < 1 || port > 65535) return route_fail(arg, "invalid port");
    sa.sin_port = htons((uint16_t)port);

    struct backend backend = {sa, addr};
    for (int i = 0; i < nroutes; i++) {
        struct route *r = &routes[i];
        if (r->prefix_len != n || memcmp(r->prefix, arg, n) != 0) continue;
        for (int j = 0; j < r->n; j++) {
            const struct sockaddr_in *existing = &r->b[j].addr;
            if (existing->sin_addr.s_addr == sa.sin_addr.s_addr && existing->sin_port == sa.sin_port)
                return route_fail(arg, "duplicate backend");
        }
        if (r->n >= MAX_BACKENDS) return route_fail(arg, "too many backends");
        r->b[r->n++] = backend;
        return 0;
    }
    if (nroutes >= MAX_ROUTES) return route_fail(arg, "too many routes");

    routes[nroutes++] = (struct route){.prefix = arg, .prefix_len = n, .b = {backend}, .n = 1};
    return 0;
}

struct route *route_find(const char *target, size_t len) {
    for (int i = 0; i < nroutes; i++)
        if (len >= routes[i].prefix_len && memcmp(target, routes[i].prefix, routes[i].prefix_len) == 0)
            return &routes[i];
    return NULL;
}

const struct backend *route_pick(struct route *r) { return &r->b[r->next]; }

void route_advance(struct route *r) { r->next = (r->next + 1) % r->n; }
