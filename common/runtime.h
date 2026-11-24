#ifndef RUNTIME_H
#define RUNTIME_H
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>

static inline int runtime_limit(const char *name, int fallback, int maximum)
{
    const char *value = getenv(name);
    if (!value) return fallback;
    char *end;
    errno = 0;
    long n = strtol(value, &end, 10);
    if (errno || !*value || *end || *value < '0' || *value > '9' || n < 1 || n > maximum) {
        fprintf(stderr, "%s must be an integer in 1..%d\n", name, maximum);
        exit(EXIT_FAILURE);
    }
    return (int)n;
}

static inline void *runtime_alloc(size_t count, size_t size)
{
    void *memory = calloc(count, size);
    if (!memory) { perror("capacity allocation"); exit(EXIT_FAILURE); }
    return memory;
}

static inline void runtime_fd_check(unsigned needed)
{
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit)) { perror("getrlimit"); exit(EXIT_FAILURE); }
    if (limit.rlim_cur < needed) {
        fprintf(stderr, "FD limit too low: need >= %u, soft=%llu, hard=%llu; raise ulimit -n in this shell\n",
            needed, (unsigned long long)limit.rlim_cur, (unsigned long long)limit.rlim_max);
        exit(EXIT_FAILURE);
    }
    fprintf(stderr, "[capacity] fd_soft=%llu fd_budget=%u\n", (unsigned long long)limit.rlim_cur, needed);
}
#endif
