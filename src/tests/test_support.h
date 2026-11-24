#ifndef TEST_SUPPORT_H
#define TEST_SUPPORT_H

#include "server.h"
#include <stdio.h>
#include <stdlib.h>

/* NDEBUG와 무관하게 검사합니다. 실패 시 비교식과 소스 위치를 표시합니다. */
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
    exit(EXIT_FAILURE); } } while (0)

static inline size_t used_contexts(const struct context_pool *pool)
{
    size_t used = 0;
    for (int i = 0; i < CONTEXT_CAPACITY; i++)
        if (pool->owners[i]) used++;
    return used;
}

static inline void expect_count(const char *label, size_t actual, size_t expected)
{
    printf("  %s: expected=%zu, actual=%zu\n", label, expected, actual);
    CHECK(actual == expected);
}

static inline void expect_zero_context(const struct request_context *context)
{
    const unsigned char *bytes = (const unsigned char *)context;
    for (size_t i = 0; i < sizeof(*context); i++)
        CHECK(bytes[i] == 0);
}

#endif
