#ifndef SERVER_PROFILE_H
#define SERVER_PROFILE_H
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct connection;
struct server_stamp { int64_t wall, cpu; };
enum server_stage {
    SP_PREPARE, SP_POLL, SP_DISPATCH, SP_ACCEPT, SP_DRAIN,
    SP_TIMER_CANCEL, SP_TIMER_UPDATE, SP_TIMER_EXPIRE,
    SP_MUTEX_WAIT, SP_QUEUE_TAKE, SP_QUEUE_ENQUEUE, SP_WORKER_IDLE,
    SP_REQUEST_TURN, SP_PUBLISH, SP_NOTIFY, SP_QUEUE_RESIDENCE, SP_PENDING_RESIDENCE, SP_ACTIVE_PENDING, SP_ACTIVE_QUEUE, SP_ACTIVE_TURN,
    SP_CONTEXT_WAIT_START, SP_CONTEXT_WAIT, SP_CONTEXT_CANCEL, SP_CONTEXT_MUTEX, SP_COUNT
};
void server_profile_start(void);
void server_profile_worker(void);
struct server_stamp server_profile_begin(void);
void server_profile_end(enum server_stage stage, struct server_stamp stamp, uint64_t units);
int64_t server_profile_wall(void);
void server_profile_residence(int64_t queued);
void server_profile_pending(int64_t queued);
void server_profile_active_waits(int64_t pending, int64_t queued, int64_t taken);
int server_profile_lock(pthread_mutex_t *mutex);
int server_profile_context_lock(pthread_mutex_t *mutex);
void server_profile_context_start(struct connection *client, size_t waiting);
void server_profile_context_finish(struct connection *client, bool acquired);
void server_profile_flush(void);
void server_profile_report(void);
#endif
