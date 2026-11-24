#define _POSIX_C_SOURCE 200809L
#include "routes.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ROUTE_LIMIT = 128, LINE_LIMIT = 16384 };
static struct proxy_route routes[ROUTE_LIMIT];
static size_t count;

void routes_free(void)
{
    while (count)
        free(routes[--count].path);
}

static int valid_path(const char *path)
{
    size_t length = strlen(path);
    if (*path != '/' || length > 8192 || (length > 1 && path[length - 1] == '/') ||
        (strncmp(path, "/cgi-bin", 8) == 0 && (path[8] == '/' || path[8] == '\0')))
        return 0;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || strchr("/-._~", *p)))
            return 0;
    const char *part = path + 1;
    while (*part) {
        size_t n = strcspn(part, "/");
        if (!n || (n == 1 && *part == '.') || (n == 2 && strncmp(part, "..", 2) == 0))
            return 0;
        part += n;
        if (*part)
            ++part;
    }
    return 1;
}

/* Count bytes, not strlen: an embedded NUL must not hide trailing fields. */
static int read_line(FILE *file, char *line)
{
    size_t used = 0;
    int c;
    while ((c = fgetc(file)) != EOF) {
        if (c == 0 || used == LINE_LIMIT)
            return -1;
        line[used++] = (char)c;
        if (c == '\n')
            break;
    }
    line[used] = '\0';
    return used ? 1 : 0;
}

int routes_load(const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "routes %s: %s\n", path, strerror(errno));
        return -1;
    }
    char line[LINE_LIMIT + 1];
    size_t number = 0;
    const char *error = NULL;
    int line_status;
    while ((line_status = read_line(file, line)) > 0) {
        ++number;
        char *save;
        char *prefix = strtok_r(line, " \t\r\n", &save);
        if (!prefix || *prefix == '#')
            continue;
        char *host = strtok_r(NULL, " \t\r\n", &save);
        char *port = strtok_r(NULL, " \t\r\n", &save);
        if (!host || !port || strtok_r(NULL, " \t\r\n", &save) || !valid_path(prefix)) {
            error = "expected safe path, numeric IPv4 and port";
            break;
        }
        struct sockaddr_in address = {.sin_family = AF_INET};
        unsigned n = 0;
        for (const char *p = port; *p; ++p) {
            if (*p < '0' || *p > '9' || n > 6553) {
                n = 65536;
                break;
            }
            n = n * 10 + (unsigned)(*p - '0');
        }
        if (!n || n > 65535 || inet_pton(AF_INET, host, &address.sin_addr) != 1) {
            error = "invalid IPv4 or port";
            break;
        }
        for (size_t i = 0; i < count; ++i)
            if (strcmp(prefix, routes[i].path) == 0)
                error = "duplicate path";
        if (count == ROUTE_LIMIT)
            error = "too many routes (maximum 128)";
        if (error)
            break;
        char *copy = strdup(prefix);
        if (!copy) {
            error = "out of memory";
            break;
        }
        address.sin_port = htons((unsigned short)n);
        routes[count++] = (struct proxy_route){.path = copy, .address = address};
    }
    if (line_status < 0) {
        ++number;
        error = "line too long or contains NUL";
    }
    if (ferror(file))
        error = "read error";
    fclose(file);
    if (error) {
        fprintf(stderr, "routes %s line %zu: %s\n", path, number, error);
        routes_free();
        return -1;
    }
    return 0;
}

const struct proxy_route *routes_match(const char *path)
{
    const struct proxy_route *best = NULL;
    size_t longest = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t n = strlen(routes[i].path);
        if (n > longest && strncmp(path, routes[i].path, n) == 0 &&
            (n == 1 || path[n] == '\0' || path[n] == '/')) {
            best = &routes[i];
            longest = n;
        }
    }
    return best;
}
