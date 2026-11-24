#ifndef SERVER_H
#define SERVER_H

#include "http.h"

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>

enum {
    WORKERS = 4,
    MAX_CONNECTIONS = 8,
    QUEUE_CAPACITY = 8,

    IO_BUDGET = 65536,
    CALL_BUDGET = 64,
    ACCEPT_BUDGET = 16,

    RECV_TIMEOUT_MS = 5000,
    SEND_TIMEOUT_MS = 3000,
    DRAIN_TIMEOUT_MS = 2000,
    DRAIN_BUFFER_SIZE = 4096,
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
    char output[HEADER_LIMIT + BODY_LIMIT + 1];
};

/* 작업자가 바뀌어도 요청과 부분 송신 위치는 연결에 남습니다. */
struct connection {
    enum state state;
    enum phase phase;

    int fd;
    int file_fd;
    off_t file_left;

    struct request request;
    struct cgi_state cgi;
    char input[HEADER_LIMIT + 1];
    size_t input_length;

    char header[1024];
    size_t header_length;
    size_t header_sent;

    unsigned char data[BODY_LIMIT];
    size_t length;
    size_t sent;

    int64_t progress;
    int64_t drain_deadline;
    size_t drain_remaining;
    bool write_shutdown;
    const char *reason;
};

struct server {
    struct connection clients[MAX_CONNECTIONS];

    struct connection *jobs[QUEUE_CAPACITY];
    size_t head, tail, count;

    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_t workers[WORKERS];
    bool stopping;

    int wake_fd;
    int root_fd;
    int cgi_root_fd;
};

extern volatile sig_atomic_t stop_requested;

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

void pool_start(struct server *server);
void pool_stop(struct server *server);
void enqueue_turn(struct server *server, struct connection *client);

void process_turn(struct server *server, struct connection *client);
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
