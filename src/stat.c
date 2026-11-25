#include <inttypes.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "stat.h"

#define EVENTS 64

struct stat_counts stat_c;
static struct {
    uint32_t trace;
    const char *stage;
    size_t bytes;
    uint64_t t_us;
} events[EVENTS];
static uint64_t seq;

uint64_t stat_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static unsigned long rss_kb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long total, resident = 0;
    if (fscanf(f, "%lu %lu", &total, &resident) != 2) resident = 0;
    fclose(f);
    return resident * (unsigned long)(sysconf(_SC_PAGESIZE) / 1024);
}

void stat_event(uint32_t trace, const char *stage, size_t bytes) {
    if (trace) stat_event_at(trace, stage, bytes, stat_now_us());
}

void stat_event_at(uint32_t trace, const char *stage, size_t bytes, uint64_t t_us) {
    if (!trace) return;
    size_t i = seq++ % EVENTS;
    events[i].trace = trace;
    events[i].stage = stage;
    events[i].bytes = bytes;
    events[i].t_us = t_us;
}

int stat_json(char *out, size_t cap) {
    int len = snprintf(out, cap, "{\"now_us\":%" PRIu64 ",\"conns\":%ld,\"tunnels\":%ld,"
                       "\"up\":{\"n\":%llu,\"bytes\":%llu},\"down\":{\"n\":%llu,\"bytes\":%llu},"
                       "\"pending\":%ld,\"rss_kb\":%lu,\"events\":[",
                       stat_now_us(), stat_c.conns, stat_c.tunnels, stat_c.up_n, stat_c.up_bytes,
                       stat_c.down_n, stat_c.down_bytes, stat_c.pending, rss_kb());
    if (len < 0 || (size_t)len >= cap) return -1;
    uint64_t start = seq > EVENTS ? seq - EVENTS : 0;
    for (uint64_t n = start; n < seq; n++) {
        size_t i = n % EVENTS;
        int written = snprintf(out + len, cap - (size_t)len, "%s{\"seq\":%" PRIu64 ",\"trace\":%" PRIu32
                               ",\"stage\":\"%s\",\"bytes\":%zu,\"t_us\":%" PRIu64 "}",
                               n == start ? "" : ",", n + 1, events[i].trace, events[i].stage,
                               events[i].bytes, events[i].t_us);
        if (written < 0 || (size_t)written >= cap - (size_t)len) return -1;
        len += written;
    }
    if (cap - (size_t)len < sizeof "]}") return -1;
    snprintf(out + len, cap - (size_t)len, "]}");
    return len + 2;
}
