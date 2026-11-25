#ifndef PROXY_PROFILE_H
#define PROXY_PROFILE_H
#include <inttypes.h>
struct profile_stamp { int64_t wall, cpu; };
struct profile_metric { uint64_t calls, wall, cpu; };
enum { P_PREPARE, P_GENERATIONS, P_POLL, P_SCAN, P_ACCEPT, P_UPDATE, P_TIMERS, P_COUNT };
static bool profile_enabled;
static struct profile_metric metrics[P_COUNT];
static uint64_t profile_loops, profile_slots, profile_ready;
static int64_t profile_clock(clockid_t clock)
{
    struct timespec t; clock_gettime(clock, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static struct profile_stamp profile_begin(void)
{
    if (!profile_enabled) return (struct profile_stamp){0,0};
    return (struct profile_stamp){profile_clock(CLOCK_MONOTONIC), profile_clock(CLOCK_THREAD_CPUTIME_ID)};
}
static void profile_end(int stage, struct profile_stamp start)
{
    if (!profile_enabled) return;
    metrics[stage].calls++;
    metrics[stage].cpu += (uint64_t)(profile_clock(CLOCK_THREAD_CPUTIME_ID)-start.cpu);
    metrics[stage].wall += (uint64_t)(profile_clock(CLOCK_MONOTONIC)-start.wall);
}
static void profile_report(void)
{
    if (!profile_enabled) return;
    const char *names[] = {"prepare", "generations", "epoll_wait", "ready_scan_dispatch", "accept", "update", "timers"};
    fprintf(stderr, "[proxy_profile] {\"loops\":%" PRIu64 ",\"slots_checked\":%" PRIu64 ",\"ready_events\":%" PRIu64 ",\"stages\":{", profile_loops, profile_slots, profile_ready);
    for (int i = 0; i < P_COUNT; i++)
        fprintf(stderr, "%s\"%s\":{\"calls\":%" PRIu64 ",\"wall_seconds\":%.6f,\"cpu_seconds\":%.6f}",
            i ? "," : "", names[i], metrics[i].calls, metrics[i].wall/1e9, metrics[i].cpu/1e9);
    fprintf(stderr, "}}\n");
}
#endif
