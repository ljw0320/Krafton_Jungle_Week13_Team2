#include "server.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct metric { uint64_t calls, wall, cpu, units, max_wall, max_cpu; };
static bool enabled; /* immutable after start, before pthread_create */
static _Thread_local struct metric local[SP_COUNT];
static _Thread_local int role;
static struct metric total[2][SP_COUNT];
static pthread_mutex_t merge_mutex = PTHREAD_MUTEX_INITIALIZER;

static int64_t profile_clock(clockid_t clock)
{
    struct timespec t;
    if (clock_gettime(clock, &t)) abort();
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
void server_profile_start(void)
{
    enabled = getenv("JUNGLE_PROFILE") && !strcmp(getenv("JUNGLE_PROFILE"), "1");
    memset(total, 0, sizeof(total)); memset(local, 0, sizeof(local)); role = 0;
}
void server_profile_worker(void) { role = 1; }
struct server_stamp server_profile_begin(void)
{
    if (!enabled) return (struct server_stamp){0,0};
    int error = errno;
    struct server_stamp stamp = {profile_clock(CLOCK_MONOTONIC), profile_clock(CLOCK_THREAD_CPUTIME_ID)};
    errno = error;
    return stamp;
}
void server_profile_end(enum server_stage stage, struct server_stamp stamp, uint64_t units)
{
    if (!enabled) return;
    int error = errno;
    local[stage].calls++;
    uint64_t cpu = (uint64_t)(profile_clock(CLOCK_THREAD_CPUTIME_ID)-stamp.cpu);
    uint64_t wall = (uint64_t)(profile_clock(CLOCK_MONOTONIC)-stamp.wall);
    local[stage].cpu += cpu;
    local[stage].wall += wall;
    if (wall > local[stage].max_wall) local[stage].max_wall = wall;
    if (cpu > local[stage].max_cpu) local[stage].max_cpu = cpu;
    local[stage].units += units;
    errno = error;
}
int64_t server_profile_wall(void)
{
    if (!enabled) return 0;
    int error = errno;
    int64_t now = profile_clock(CLOCK_MONOTONIC);
    errno = error;
    return now;
}
static void record_wait(enum server_stage stage, uint64_t wall)
{
    local[stage].calls++;
    local[stage].wall += wall;
    local[stage].units++;
    if (wall > local[stage].max_wall) local[stage].max_wall = wall;
}
static void residence(enum server_stage stage, int64_t queued)
{
    if (!enabled || !queued) return;
    record_wait(stage, (uint64_t)(server_profile_wall()-queued));
}
void server_profile_active_waits(int64_t pending, int64_t queued, int64_t taken)
{
    if (!enabled || !pending || !queued || !taken) return;
    record_wait(SP_ACTIVE_PENDING, (uint64_t)(queued-pending));
    record_wait(SP_ACTIVE_QUEUE, (uint64_t)(taken-queued));
}
void server_profile_residence(int64_t queued) { residence(SP_QUEUE_RESIDENCE, queued); }
void server_profile_pending(int64_t queued) { residence(SP_PENDING_RESIDENCE, queued); }
int server_profile_lock(pthread_mutex_t *mutex)
{
    struct server_stamp stamp = server_profile_begin();
    int result = pthread_mutex_lock(mutex);
    server_profile_end(SP_MUTEX_WAIT, stamp, 0);
    return result;
}
/* No shared atomic increments on hot paths. Workers merge only when exiting. */
void server_profile_flush(void)
{
    if (!enabled) return;
    check_thread(pthread_mutex_lock(&merge_mutex));
    for (int i = 0; i < SP_COUNT; i++) {
        total[role][i].calls += local[i].calls;
        total[role][i].wall += local[i].wall;
        total[role][i].cpu += local[i].cpu;
        total[role][i].units += local[i].units;
        if (local[i].max_wall > total[role][i].max_wall) total[role][i].max_wall = local[i].max_wall;
        if (local[i].max_cpu > total[role][i].max_cpu) total[role][i].max_cpu = local[i].max_cpu;
    }
    memset(local, 0, sizeof(local));
    check_thread(pthread_mutex_unlock(&merge_mutex));
}
void server_profile_report(void)
{
    if (!enabled) return;
    server_profile_flush(); /* called by main only after all workers joined */
    const char *names[] = {"prepare_changed", "epoll_wait", "ready_dispatch", "accept", "notification_drain",
        "timer_cancel", "timer_update", "timer_expire", "server_mutex_wait", "queue_take",
        "queue_enqueue", "worker_idle_condwait", "request_turn", "publish_result", "notify_main", "queue_residence_wall_only", "pending_residence_wall_only", "nonterminal_pending_wall_only", "nonterminal_queue_wall_only", "nonterminal_turn"};
    fprintf(stderr, "[server_profile] {");
    for (int group = 0; group < 2; group++) {
        fprintf(stderr, "%s\"%s\":{", group ? "," : "", group ? "workers" : "main");
        for (int i = 0; i < SP_COUNT; i++) {
            struct metric *m = &total[group][i];
            fprintf(stderr, "%s\"%s\":{\"calls\":%" PRIu64 ",\"wall_seconds\":%.6f,\"cpu_seconds\":%.6f,\"units\":%" PRIu64 ",\"max_wall_seconds\":%.9f,\"max_cpu_seconds\":%.9f}",
                i ? "," : "", names[i], m->calls, m->wall/1e9, m->cpu/1e9, m->units, m->max_wall/1e9, m->max_cpu/1e9);
        }
        fprintf(stderr, "}");
    }
    fprintf(stderr, "}\n");
}
