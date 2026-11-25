#ifndef SERVER_H
#define SERVER_H

#include "http.h"
#include "server_profile.h"
#include "../common/runtime.h"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdatomic.h>

enum {
    WORKERS = 4,
    MAX_CONNECTIONS = 16,
    QUEUE_CAPACITY = 4,
    CONTEXT_CAPACITY = 8,
    CONTEXT_WAIT_TIMEOUT_MS = 2000,

    IO_BUDGET = 65536,
    CALL_BUDGET = 64,
    ACCEPT_BUDGET = 16,

    RECV_TIMEOUT_MS = 5000,
    SEND_TIMEOUT_MS = 3000,
    DRAIN_TIMEOUT_MS = 2000,
    DRAIN_BUFFER_SIZE = 4096,
    KEEP_ALIVE_TIMEOUT_MS = 5000,
    KEEP_ALIVE_MAX_REQUESTS = 100,
    CGI_CAPACITY = 4,
    FILE_CAPACITY = 4,
    CGI_TIMEOUT_MS = 2000,
    CGI_POLL_MS = 50
};

enum state {
    FREE,
    WAITING,
    QUEUED,
    RUNNING,
    FINISHED
};

enum phase {
    READ_HEADER,
    READ_BODY,
    CGI_OUTPUT,
    WRITE_RESPONSE,
    DRAIN_INPUT
};

struct cgi_state {
    pid_t pid;
    int fd;
    int64_t deadline;
    int64_t next_check;
    size_t length;
};

/* 요청을 처리하는 동안 풀에서 빌려 소유하는 요청 정보·처리 버퍼입니다. */
struct request_context {
    struct request request;
    char input[HEADER_LIMIT + 1];
    char header[1024];
    unsigned char data[BODY_LIMIT];
    char cgi_output[HEADER_LIMIT + BODY_LIMIT + 1];
};

/* 작업자가 바뀌어도 상태·진행 위치·context 소유권은 연결에 남습니다. */
struct connection {
    struct server *server;
    struct server *resource_server;
    bool cgi_reserved;
    bool file_reserved;
    struct connection *free_next;
    size_t timer_position; /* 1-based heap position; zero means absent. */
    int64_t expires;
    bool pending, control_queued;
    struct connection *pending_prev, *pending_next;
    struct connection *control_prev, *control_next;
    int watch_fd;
    uint32_t watch_ticket;
    bool watch_registered, watch_armed;
    enum state state;
    enum phase phase;

    int fd;
    int file_fd;
    off_t file_left;

    struct request_context *context;
    struct context_pool *context_pool;
    struct cgi_state cgi;
    size_t input_length;

    size_t header_length;
    size_t header_sent;

    size_t length;
    size_t sent;

    int64_t profile_pending_at;
    int64_t profile_queued_at;
    int64_t profile_taken_at;
    int64_t context_wait_deadline;
    int64_t progress;
    int64_t drain_deadline;
    size_t drain_remaining;
    bool write_shutdown;
    bool keep_alive;
    unsigned requests;
    const char *reason;
};

struct context_pool {
    struct request_context *entries;
    int capacity, wait_capacity;
    struct connection **owners;
    struct connection **waiters;
    size_t wait_count;
    pthread_mutex_t mutex;
};

struct server {
    struct connection *clients;
    int capacity;
    struct context_pool contexts;
    struct connection *free_clients;
    struct connection **timer_heap;
    size_t timer_count;
    struct connection *control_head, *control_tail;
    struct connection *pending_head, *pending_tail;
    bool slots_initialized;

    struct connection *jobs[QUEUE_CAPACITY];
    size_t head, tail, count;

    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_t workers[WORKERS];
    bool stopping;
    atomic_uint active_cgi;
    atomic_uint active_files;

    int epoll_fd;
    bool epoll_active, io_failed;
    uint32_t next_ticket;
    int wake_fd;
    int root_fd;
    int cgi_root_fd;
};

extern volatile sig_atomic_t stop_requested;

void context_pool_init(struct context_pool *pool);
void context_pool_destroy(struct context_pool *pool);
bool context_acquire(struct connection *client);
void context_release(struct connection *client);

void fail(const char *operation);
void check_thread(int error);
int64_t now_ms(void);
bool would_block(void);

int nonblocking(int fd);
int listener_open(int port);

void notify_main(int fd);
void drain_notifications(int fd);

void signals_start(int wake_fd);
void signals_unblock(void);
void signals_detach(void);

bool resource_acquire(atomic_uint *count, unsigned limit);
void file_release(struct connection *client);

void timer_cancel(struct server *server, struct connection *client);
void timer_update(struct server *server, struct connection *client);
int timer_expire(struct server *server);
void slots_init(struct server *server);

void pool_start(struct server *server);
void pool_stop(struct server *server);
void server_changed(struct server *server, struct connection *client);
void server_forget(struct server *server, struct connection *client);
void pending_remove(struct server *server, struct connection *client);
void dispatch_pending(struct server *server);
void server_io_suspend(struct connection *client);
void enqueue_turn(struct server *server, struct connection *client);

void process_turn(struct server *server, struct connection *client);
bool connection_buffered(const struct connection *client);
bool connection_idle(const struct connection *client);
void respond(struct connection *client, int status, off_t size,
             const char *type);
void respond_error(struct connection *client, int status);
bool cgi_target(const char *target);
void cgi_start(struct server *server, struct connection *client);
void cgi_turn(struct connection *client);
void cgi_close(struct connection *client);
int cgi_response(struct connection *client);

void server_run(struct server *server, int listener);
void connection_close(struct connection *client);

#endif
