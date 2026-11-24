#include "proxy.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum phase { UNUSED, REQUEST, CONNECTING, UPLOAD, RESPONSE, RELAY, ERROR_REPLY, DRAIN };
struct exchange {
    int client, upstream;
    enum phase phase;
    int64_t first, client_progress, upstream_progress, connect_started, drain_until;
    bool committed;
    char request[HEADER_LIMIT + BODY_LIMIT];
    size_t request_used, request_header, request_body;
    char outgoing[FORWARD_HEADER_SIZE];
    size_t outgoing_length, outgoing_sent, body_sent;
    char response[HEADER_LIMIT + 1];
    size_t response_used;
    unsigned char relay[RELAY_SIZE];
    size_t relay_used, relay_sent;
    uint64_t remaining;
};
static struct exchange clients[PROXY_CLIENTS];
static struct sockaddr_in backend;
static char backend_host[64];
static volatile sig_atomic_t stopping;

static int64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) == -1) abort();
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void stop(int signal_number) { (void)signal_number; stopping = 1; }
static bool retry(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }

static void close_upstream(struct exchange *c)
{
    if (c->upstream >= 0) close(c->upstream);
    c->upstream = -1;
}

static void dispose(struct exchange *c)
{
    close_upstream(c);
    if (c->client >= 0) close(c->client);
    c->client = -1;
    c->phase = UNUSED;
}

/* Half-close before draining unread input so an error response is not lost to RST. */
static void finish(struct exchange *c)
{
    close_upstream(c);
    shutdown(c->client, SHUT_WR);
    c->phase = DRAIN;
    c->drain_until = now_ms() + 100;
    if (c->first && c->drain_until > c->first + TOTAL_MS)
        c->drain_until = c->first + TOTAL_MS;
}

static void fail_request(struct exchange *c, int status)
{
    if (c->committed) { dispose(c); return; }
    close_upstream(c);
    const char *reason = status == 408 ? "Request Timeout" : http_reason(status);
    int n = snprintf(c->outgoing, sizeof(c->outgoing),
        "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status, reason);
    c->outgoing_length = (size_t)n;
    c->outgoing_sent = 0;
    c->phase = ERROR_REPLY;
    c->client_progress = now_ms();
}

static void connect_backend(struct exchange *c)
{
    c->upstream = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (c->upstream < 0) { fail_request(c, 502); return; }
    c->connect_started = c->upstream_progress = now_ms();
    int result = connect(c->upstream, (struct sockaddr *)&backend, sizeof(backend));
    if (!result) c->phase = UPLOAD;
    else if (errno == EINPROGRESS) c->phase = CONNECTING;
    else fail_request(c, 502);
}

static void read_request(struct exchange *c)
{
    size_t limit = c->request_header ? c->request_header + c->request_body : HEADER_LIMIT;
    ssize_t n = recv(c->client, c->request + c->request_used, limit - c->request_used, 0);
    if (n <= 0) {
        if (!n || !retry()) dispose(c);
        return;
    }
    int64_t now = now_ms();
    if (!c->first) c->first = now;
    c->client_progress = now;
    c->request_used += (size_t)n;
    if (!c->request_header) {
        size_t end = http_header_end(c->request, c->request_used);
        if (!end) {
            if (c->request_used == HEADER_LIMIT) fail_request(c, 431);
            return;
        }
        struct request request;
        int result = proxy_request(c->request, end, backend_host, &request,
                                   c->outgoing, sizeof(c->outgoing));
        if (result < 0) { fail_request(c, -result); return; }
        c->request_header = end;
        c->request_body = request.content_length;
        c->outgoing_length = (size_t)result;
    }
    if (c->request_used >= c->request_header + c->request_body) connect_backend(c);
}

static void complete_connect(struct exchange *c)
{
    int error = 0;
    socklen_t size = sizeof(error);
    if (getsockopt(c->upstream, SOL_SOCKET, SO_ERROR, &error, &size) || error)
        fail_request(c, 502);
    else {
        c->phase = UPLOAD;
        c->upstream_progress = now_ms();
    }
}

static void upload(struct exchange *c)
{
    bool header = c->outgoing_sent < c->outgoing_length;
    size_t *sent = header ? &c->outgoing_sent : &c->body_sent;
    size_t length = header ? c->outgoing_length : c->request_body;
    const char *data = header ? c->outgoing : c->request + c->request_header;
    if (*sent < length) {
        ssize_t n = send(c->upstream, data + *sent, length - *sent, MSG_NOSIGNAL);
        if (n < 0 && retry()) return;
        if (n <= 0) { fail_request(c, 502); return; }
        *sent += (size_t)n;
        c->upstream_progress = now_ms();
    }
    if (c->outgoing_sent == c->outgoing_length && c->body_sent == c->request_body) {
        c->phase = RESPONSE;
        c->outgoing_length = c->outgoing_sent = 0;
    }
}

static void read_response(struct exchange *c)
{
    ssize_t n = recv(c->upstream, c->response + c->response_used,
                     HEADER_LIMIT - c->response_used, 0);
    if (n < 0 && retry()) return;
    if (n <= 0) { fail_request(c, 502); return; }
    c->upstream_progress = now_ms();
    c->response_used += (size_t)n;
    size_t end = http_header_end(c->response, c->response_used);
    if (!end) {
        if (c->response_used == HEADER_LIMIT) fail_request(c, 502);
        return;
    }
    int result = proxy_response(c->response, end, &c->remaining,
                                c->outgoing, sizeof(c->outgoing));
    size_t buffered = c->response_used - end;
    if (result < 0 || buffered > c->remaining) { fail_request(c, 502); return; }
    c->outgoing_length = (size_t)result;
    memcpy(c->relay, c->response + end, buffered);
    c->relay_used = buffered;
    c->remaining -= buffered;
    c->phase = RELAY;
    c->client_progress = now_ms();
    if (!c->remaining) close_upstream(c);
}

static void read_body(struct exchange *c)
{
    size_t size = c->remaining < RELAY_SIZE ? (size_t)c->remaining : RELAY_SIZE;
    ssize_t n = recv(c->upstream, c->relay, size, 0);
    if (n < 0 && retry()) return;
    if (n <= 0) { fail_request(c, 502); return; }
    c->upstream_progress = now_ms();
    c->relay_used = (size_t)n;
    c->relay_sent = 0;
    c->remaining -= (size_t)n;
    c->client_progress = now_ms();
    if (!c->remaining) close_upstream(c);
}

static void send_response(struct exchange *c)
{
    bool header = c->outgoing_sent < c->outgoing_length;
    size_t *sent = header ? &c->outgoing_sent : &c->relay_sent;
    size_t length = header ? c->outgoing_length : c->relay_used;
    const void *data = header ? (void *)c->outgoing : (void *)c->relay;
    if (*sent < length) {
        ssize_t n = send(c->client, (const char *)data + *sent, length - *sent, MSG_NOSIGNAL);
        if (n < 0 && retry()) return;
        if (n <= 0) { dispose(c); return; }
        c->committed = true;
        *sent += (size_t)n;
        c->client_progress = now_ms();
    }
    if (c->outgoing_sent < c->outgoing_length) return;
    if (c->phase == ERROR_REPLY) { finish(c); return; }
    if (c->relay_sent == c->relay_used) {
        c->relay_sent = c->relay_used = 0;
        c->upstream_progress = now_ms(); /* resume upstream's idle clock after backpressure */
        if (!c->remaining) finish(c);
    }
}

static void drain(struct exchange *c)
{
    char discard[4096];
    ssize_t n = recv(c->client, discard, sizeof(discard), 0);
    if (!n || (n < 0 && !retry())) dispose(c);
}

static bool has_output(const struct exchange *c)
{
    return c->outgoing_sent < c->outgoing_length || c->relay_sent < c->relay_used;
}

static int64_t deadline(const struct exchange *c)
{
    int64_t limit = c->first ? c->first + TOTAL_MS : c->client_progress + IDLE_MS;
    int64_t idle = limit;
    if (c->phase == REQUEST || c->phase == ERROR_REPLY)
        idle = c->client_progress + IDLE_MS;
    else if (c->phase == CONNECTING) idle = c->connect_started + CONNECT_MS;
    else if (c->phase == UPLOAD || c->phase == RESPONSE)
        idle = c->upstream_progress + IDLE_MS;
    else if (c->phase == RELAY)
        idle = has_output(c) ? c->client_progress + IDLE_MS : c->upstream_progress + IDLE_MS;
    else if (c->phase == DRAIN) idle = c->drain_until;
    return idle < limit ? idle : limit;
}

static void expire(struct exchange *c)
{
    if (c->phase == DRAIN || c->phase == ERROR_REPLY || c->committed) { dispose(c); return; }
    fail_request(c, c->phase == REQUEST ? 408 : 504);
    /* At the absolute deadline attempt only an immediate error write; never extend it. */
    if (c->first && now_ms() >= c->first + TOTAL_MS) {
        send_response(c);
        dispose(c);
    }
}

static nfds_t prepare(struct pollfd *fds, int listener, int *timeout)
{
    fds[0] = (struct pollfd){.fd = listener, .events = POLLIN};
    int64_t now = now_ms();
    *timeout = 1000; /* also bounds signal-driven shutdown latency */
    for (int i = 0; i < PROXY_CLIENTS; i++) {
        struct exchange *c = &clients[i];
        if (c->phase != UNUSED && deadline(c) <= now) expire(c);
        short client_events = 0, upstream_events = 0;
        if (c->phase == REQUEST || c->phase == DRAIN) client_events = POLLIN;
        if (c->phase == ERROR_REPLY || (c->phase == RELAY && has_output(c))) client_events = POLLOUT;
        if (c->phase == CONNECTING || c->phase == UPLOAD) upstream_events = POLLOUT;
        if (c->phase == RESPONSE || (c->phase == RELAY && !has_output(c) && c->remaining))
            upstream_events = POLLIN;
        fds[1 + 2*i] = (struct pollfd){.fd = c->phase == UNUSED ? -1 : c->client, .events = client_events};
        /* Suppress even HUP while paused; otherwise an EOF upstream can spin poll. */
        fds[2 + 2*i] = (struct pollfd){.fd = upstream_events ? c->upstream : -1, .events = upstream_events};
        if (c->phase != UNUSED) {
            int64_t left = deadline(c) - now;
            if (left < *timeout) *timeout = left > 0 ? (int)left : 0;
        }
    }
    return 1 + 2 * PROXY_CLIENTS;
}

static void dispatch(struct exchange *c, short client_events, short upstream_events)
{
    if (c->phase == UNUSED) return;
    if (deadline(c) <= now_ms()) { expire(c); return; }
    if (client_events & (POLLERR | POLLNVAL)) { dispose(c); return; }
    if (client_events & POLLIN) {
        if (c->phase == REQUEST) read_request(c);
        else if (c->phase == DRAIN) drain(c);
    }
    if (c->phase == UNUSED) return;
    if (client_events & POLLOUT) {
        if (c->phase == RELAY || c->phase == ERROR_REPLY) send_response(c);
    }
    if (c->phase == UNUSED || c->phase == DRAIN || c->phase == ERROR_REPLY) return;
    if (client_events & POLLHUP) { dispose(c); return; }
    if (upstream_events & POLLNVAL) { fail_request(c, 502); return; }
    if (upstream_events & (POLLIN | POLLOUT | POLLHUP | POLLERR)) {
        if (c->phase == CONNECTING) complete_connect(c);
        else if (c->phase == UPLOAD) upload(c);
        else if (c->phase == RESPONSE) read_response(c);
        else if (c->phase == RELAY && !has_output(c) && c->remaining) read_body(c);
    }
}

/* Rejected connections only drain for 100ms; they never reserve an upstream. */
enum { REJECT_SLOTS = 8 };
struct rejection { int fd; int64_t until; size_t sent; };
static struct rejection rejected[REJECT_SLOTS];
static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

static void accept_clients(int listener)
{
    for (int budget = 0; budget < 16; budget++) {
        int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        struct exchange *c = NULL;
        for (int i = 0; i < PROXY_CLIENTS; i++)
            if (clients[i].phase == UNUSED) { c = &clients[i]; break; }
        if (!c) {
            bool saved = false;
            for (int i = 0; i < REJECT_SLOTS; i++) {
                if (rejected[i].fd >= 0) continue;
                rejected[i] = (struct rejection){fd, now_ms() + 100, 0};
                saved = true;
                break;
            }
            if (!saved) close(fd);
            continue;
        }
        memset(c, 0, sizeof(*c));
        c->client = fd; c->upstream = -1;
        c->phase = REQUEST;
        c->client_progress = now_ms();
    }
}

static void prepare_rejections(struct pollfd *fds, int *timeout)
{
    int64_t now = now_ms();
    for (int i = 0; i < REJECT_SLOTS; i++) {
        struct rejection *r = &rejected[i];
        if (r->fd >= 0 && r->until <= now) { close(r->fd); r->fd = -1; }
        fds[i] = (struct pollfd){.fd = r->fd, .events = r->sent == sizeof(busy)-1 ? POLLIN : POLLOUT};
        if (r->fd >= 0 && r->until - now < *timeout) *timeout = (int)(r->until - now);
    }
}

static void dispatch_rejections(struct pollfd *fds)
{
    for (int i = 0; i < REJECT_SLOTS; i++) {
        struct rejection *r = &rejected[i];
        if (r->fd < 0) continue;
        if (fds[i].revents & POLLOUT) {
            ssize_t n = send(r->fd, busy + r->sent, sizeof(busy)-1-r->sent, MSG_NOSIGNAL);
            if (n > 0) r->sent += (size_t)n;
            if (r->sent == sizeof(busy)-1) shutdown(r->fd, SHUT_WR);
        }
        if (fds[i].revents & POLLIN) {
            char discard[4096];
            ssize_t n = recv(r->fd, discard, sizeof(discard), 0);
            if (!n) { close(r->fd); r->fd = -1; }
        }
        if (r->fd >= 0 && (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL))) {
            close(r->fd); r->fd = -1;
        }
    }
}

static int port(const char *value)
{
    char *end;
    errno = 0;
    long n = strtol(value, &end, 10);
    if (errno || !*value || *end || n < 1 || n > 65535) return -1;
    return (int)n;
}

int main(int argc, char **argv)
{
    int listen_port = argc == 4 ? port(argv[1]) : -1;
    int backend_port = argc == 4 ? port(argv[3]) : -1;
    backend.sin_family = AF_INET;
    if (listen_port < 0 || backend_port < 0 || inet_pton(AF_INET, argv[2], &backend.sin_addr) != 1 ||
        backend.sin_addr.s_addr == htonl(INADDR_ANY) ||
        (listen_port == backend_port && (ntohl(backend.sin_addr.s_addr) >> 24) == 127)) {
        fprintf(stderr, "usage: %s LISTEN_PORT BACKEND_IPV4 BACKEND_PORT (distinct endpoints)\n", argv[0]);
        return 1;
    }
    backend.sin_port = htons((uint16_t)backend_port);
    snprintf(backend_host, sizeof(backend_host), "%s:%d", argv[2], backend_port);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)listen_port),
                                  .sin_addr = {.s_addr = htonl(INADDR_ANY)}};
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int yes = 1;
    if (listener < 0 || setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 128)) {
        perror("proxy listener"); return 1;
    }
    struct sigaction action = {.sa_handler = stop};
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL); sigaction(SIGTERM, &action, NULL);
    for (int i = 0; i < PROXY_CLIENTS; i++) clients[i].client = clients[i].upstream = -1;
    for (int i = 0; i < REJECT_SLOTS; i++) rejected[i].fd = -1;
    while (!stopping) {
        struct pollfd fds[1 + PROXY_CLIENTS*2 + REJECT_SLOTS];
        int timeout;
        nfds_t count = prepare(fds, listener, &timeout);
        prepare_rejections(fds + count, &timeout);
        if (poll(fds, count + REJECT_SLOTS, timeout) < 0) {
            if (errno == EINTR) continue;
            perror("proxy poll"); break;
        }
        for (int i = 0; i < PROXY_CLIENTS; i++)
            dispatch(&clients[i], fds[1+2*i].revents, fds[2+2*i].revents);
        dispatch_rejections(fds + count);
        if (fds[0].revents & POLLIN) accept_clients(listener);
    }
    for (int i = 0; i < PROXY_CLIENTS; i++) dispose(&clients[i]);
    for (int i = 0; i < REJECT_SLOTS; i++) if (rejected[i].fd >= 0) close(rejected[i].fd);
    close(listener);
    return 0;
}
