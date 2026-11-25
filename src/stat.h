#ifndef STAT_H
#define STAT_H

#include <stddef.h>
#include <stdint.h>

#define STAT_JSON_CAP 16384

struct stat_counts {
    long conns, tunnels, pending;
    unsigned long long up_n, up_bytes, down_n, down_bytes;
};

extern struct stat_counts stat_c;
uint64_t stat_now_us(void);
void stat_event(uint32_t trace, const char *stage, size_t bytes);
void stat_event_at(uint32_t trace, const char *stage, size_t bytes, uint64_t t_us);
int stat_json(char *out, size_t cap);

#endif
