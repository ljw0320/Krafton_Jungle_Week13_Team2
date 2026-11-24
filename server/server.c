#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/time.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "server.h"
#include "../http/http.h"
#include "../proxy/proxy.h"
#include "../log/diagnostics.h"

static volatile sig_atomic_t stopping;

struct child_channel {
    int fd;
    int backend;                /* -1 when no request holds a backend slot. */
};

/* Only the parent updates this table, while SIGCHLD is blocked. */
struct child_processes {
    pid_t *pids;                 /* Entries [0, active) are live or unreaped. */
    int capacity;
    int active;
    struct child_channel *channels;
    struct pollfd *pollfds;
    int backend_limit;
};

static void close_channel(struct child_channel *channel)
{
    if (channel->fd >= 0)
        close(channel->fd);
    *channel = (struct child_channel){.fd = -1, .backend = -1};
}

/* Parent alone owns leases. Releasing a dead worker cannot race another acquire. */
static void service_channel(struct child_processes *children, int index)
{
    struct child_channel *channel = &children->channels[index];
    int backend;
    ssize_t n = recv(channel->fd, &backend, sizeof(backend), MSG_TRUNC);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        return;
    if (n != sizeof(backend) || backend < -1 || backend >= PROXY_BACKEND_LIMIT ||
        (backend >= 0 && channel->backend >= 0)) {
        close_channel(channel);
        return;
    }
    int status = 0;
    if (backend < 0)
        channel->backend = -1;
    else {
        int used = 0;
        for (int i = 0; i < children->active; ++i)
            used += children->channels[i].backend == backend;
        if (used >= children->backend_limit)
            status = 503;
        else
            channel->backend = backend;
    }
    if (send(channel->fd, &status, sizeof(status), 0) != sizeof(status))
        close_channel(channel);
}

/* ppoll retains atomic signal-mask switching without FD_SETSIZE's 1024-fd ceiling. */
static int poll_children(struct child_processes *children, int listener,
                         const sigset_t *mask, const struct timespec *timeout)
{
    children->pollfds[0] = (struct pollfd){
        .fd = children->active < children->capacity ? listener : -1,
        .events = POLLIN,
    };
    for (int i = 0; i < children->active; ++i)
        children->pollfds[i + 1] = (struct pollfd){
            .fd = children->channels[i].fd, .events = POLLIN,
        };
    if (ppoll(children->pollfds, (nfds_t)children->active + 1, timeout, mask) < 0)
        return errno == EINTR ? 0 : -1;
    for (int i = 0; i < children->active; ++i)
        if (children->pollfds[i + 1].revents)
            service_channel(children, i);
    return (children->pollfds[0].revents & POLLIN) != 0;
}

/* SIGINT·SIGTERM 수신 시 종료를 요청하며, SIGCHLD는 부모의 대기만 중단한다. */
static void handle_signal(int signo)
{
    if (signo == SIGINT || signo == SIGTERM)
        stopping = 1;
}

/* 시스템 호출 실패 시 오류를 출력하고 호출 결과를 그대로 반환한다. */
static int check_call(int result, const char *operation)
{
    if (result == -1)
        diagnostic_error(operation, errno);
    return result;
}

/* 종료된 자식을 대기 없이 회수하고 PID 배열과 자식 수를 갱신한다. */
static int reap_children(struct child_processes *children)
{
    for (;;) {
        pid_t pid = waitpid(-1, NULL, WNOHANG);
        if (pid == 0 || (pid == -1 && errno == ECHILD))
            return 0;
        if (pid == -1) {
            if (errno == EINTR)
                continue;
            diagnostic_error("waitpid reap", errno);
            return -1;
        }
        for (int i = 0; i < children->active; ++i) {
            if (children->pids[i] == pid) {
                if (children->channels) {
                    close_channel(&children->channels[i]);
                    children->channels[i] = children->channels[children->active - 1];
                }
                --children->active;
                children->pids[i] = children->pids[children->active];
                break;
            }
        }
    }
}

/* sigaction으로 지정한 시그널의 처리 방식을 등록한다. */
static int set_signal_handler(int signo, void (*handler)(int))
{
    struct sigaction action = {.sa_handler = handler};
    sigemptyset(&action.sa_mask);
    if (sigaction(signo, &action, NULL) == 0)
        return 0;
    int error = errno;
    char operation[64];
    snprintf(operation, sizeof(operation), "sigaction signal=%d", signo);
    diagnostic_error(operation, error);
    return -1;
}

/* 부모의 시그널 처리와 차단을 설정하고 ppoll 대기용 기존 마스크를 보관한다.
 * SIGPIPE는 무시한다. */
static int setup_parent_signals(sigset_t *wait_mask)
{
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    if (set_signal_handler(SIGINT, handle_signal) == -1 ||
        set_signal_handler(SIGTERM, handle_signal) == -1 ||
        set_signal_handler(SIGCHLD, handle_signal) == -1 ||
        set_signal_handler(SIGPIPE, SIG_IGN) == -1)
        return -1;
    return check_call(sigprocmask(SIG_BLOCK, &blocked, wait_mask), "sigprocmask parent");
}

/* O_NONBLOCK 플래그를 변경하여 소켓의 blocking/non-blocking 모드를 설정한다. */
static int set_socket_blocking(int fd, bool blocking)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1 || fcntl(fd, F_SETFL,
            blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK) == -1) {
        diagnostic_error("fcntl socket mode", errno);
        return -1;
    }
    return 0;
}

/* 접속 수신 소켓을 생성하고 주소 재사용, bind, listen, non-blocking 모드를 설정한다. */
static int open_listener(int port)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        diagnostic_error("socket", errno);
        return -1;
    }
    int reuse = 1;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)port);
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) == -1 ||
        listen(listener, 128) == -1) {
        diagnostic_error("listener setup", errno);
        close(listener);
        return -1;
    }
    if (set_socket_blocking(listener, false) == -1) {
        close(listener);
        return -1;
    }
    return listener;
}

/* 불필요한 자원을 정리하고 시그널 설정을 복원한 뒤 HTTP 연결을 처리하고 종료한다. */
static void run_child(int listener, int client_fd, int root_fd, int idle_seconds,
                      struct child_processes *children, const sigset_t *wait_mask,
                      int control)
{
    /* The child owns client_fd; it does not accept or manage other children. */
    close(listener);
    for (int i = 0; i < children->active; ++i)
        if (children->channels[i].fd >= 0)
            close(children->channels[i].fd);
    free(children->pids);
    free(children->channels);
    free(children->pollfds);
    proxy_set_control(control);
    const int signals[] = {SIGINT, SIGTERM, SIGCHLD};
    for (int i = 0; i < 3; ++i) {
        if (set_signal_handler(signals[i], SIG_DFL) == -1) {
            close(client_fd);
            _exit(EXIT_FAILURE);
        }
    }
    if (check_call(sigprocmask(SIG_SETMASK, wait_mask, NULL), "child sigprocmask") == -1) {
        close(client_fd);
        _exit(EXIT_FAILURE);
    }
    int status = serve_http(client_fd, root_fd, idle_seconds);
    if (control >= 0)
        close(control);
    close(root_fd);
    close(client_fd);
    _exit(status);
}

static int defer_accept(const char *operation, int error, struct timespec *until,
                         bool *reported)
{
    bool temporary = error == EMFILE || error == ENFILE || error == ENOMEM ||
                     error == ENOBUFS || error == EAGAIN;
    if (!temporary || !*reported) {
        errno = error;
        diagnostic_error(operation, error);
        *reported = true;
    }
    if (!temporary)
        return -1;
    if (clock_gettime(CLOCK_MONOTONIC, until) < 0) {
        diagnostic_error("clock_gettime accept retry", errno);
        return -1;
    }
    until->tv_nsec += 100000000;
    if (until->tv_nsec >= 1000000000) {
        ++until->tv_sec;
        until->tv_nsec -= 1000000000;
    }
    return 0;
}

/* 자식 수 제한 내에서 접속을 수락하고 처리할 자식을 생성하며, 종료된 자식을 회수한다. */
static int run_parent(int listener, int root_fd, struct child_processes *children,
                      int idle_seconds, const sigset_t *wait_mask)
{
    struct timespec retry_until = {0};
    bool pausing = false, reported = false;
    while (!stopping) {
        if (reap_children(children) == -1)
            return EXIT_FAILURE;
        struct timespec delay;
        const struct timespec *timeout = NULL;
        if (pausing) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
                diagnostic_error("clock_gettime accept retry", errno);
                return EXIT_FAILURE;
            }
            delay.tv_sec = retry_until.tv_sec - now.tv_sec;
            delay.tv_nsec = retry_until.tv_nsec - now.tv_nsec;
            if (delay.tv_nsec < 0) {
                --delay.tv_sec;
                delay.tv_nsec += 1000000000;
            }
            pausing = delay.tv_sec >= 0 && (delay.tv_sec || delay.tv_nsec);
            if (pausing)
                timeout = &delay;
        }
        /* Pause only admission: existing workers still need IPC and signal handling. */
        /* Atomically unblock signals while waiting, avoiding a check/wait race. */
        int count = poll_children(children, pausing ? -1 : listener, wait_mask, timeout);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            diagnostic_error("ppoll", errno);
            return EXIT_FAILURE;
        }
        if (!count || stopping)
            continue;
        int fd = accept(listener, NULL, NULL);
        if (fd == -1) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED)
                continue;
            if (defer_accept("accept", errno, &retry_until, &reported) < 0)
                return EXIT_FAILURE;
            pausing = true;
            continue;
        }
        if (set_socket_blocking(fd, false) == -1) {
            close(fd);
            continue;
        }
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
            close(fd);
            continue;
        }
        int no_delay = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) == -1) {
            diagnostic_error("TCP_NODELAY", errno);
            close(fd);
            continue;
        }
        int control[2] = {-1, -1};
        if (routes_enabled() &&
            socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK,
                       0, control) < 0) {
            int error = errno;
            close(fd);
            if (defer_accept("socketpair control", error, &retry_until, &reported) < 0)
                return EXIT_FAILURE;
            pausing = true;
            continue;
        }
        pid_t pid = fork();
        int fork_error = pid < 0 ? errno : 0;
        if (pid == 0) {
            if (control[0] >= 0)
                close(control[0]);
            run_child(listener, fd, root_fd, idle_seconds, children, wait_mask, control[1]);
        }
        if (control[1] >= 0)
            close(control[1]);
        close(fd);  /* Only the child retains the connection after fork. */
        if (pid == -1) {
            if (control[0] >= 0)
                close(control[0]);
            if (defer_accept("fork", fork_error, &retry_until, &reported) < 0)
                return EXIT_FAILURE;
            pausing = true;
            continue;
        }
        children->pids[children->active] = pid;
        children->channels[children->active] =
            (struct child_channel){.fd = control[0], .backend = -1};
        ++children->active;
        reported = false;
    }
    return EXIT_SUCCESS;
}

/* 지정한 시간 동안 자식들을 회수하면서 자연 종료를 기다린다. */
static int wait_for_children(struct child_processes *children, int seconds)
{
    if (!children->active)
        return 0;
    struct timespec start, current;
    if (check_call(clock_gettime(CLOCK_MONOTONIC, &start), "clock_gettime") == -1)
        return -1;
    while (children->active) {
        if (check_call(clock_gettime(CLOCK_MONOTONIC, &current), "clock_gettime") == -1)
            return -1;
        double elapsed = current.tv_sec - start.tv_sec + (current.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= seconds)
            break;
        if (reap_children(children) == -1)
            return -1;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 20000000};
        if (children->channels) {
            if (poll_children(children, -1, NULL, &pause) < 0)
                return -1;
        } else
            nanosleep(&pause, NULL);
    }
    return 0;
}

/* 관리 중인 모든 자식 프로세스에 지정한 시그널을 전송한다. */
static int signal_children(const struct child_processes *children, int signo)
{
    int result = 0;
    for (int i = 0; i < children->active; ++i) {
        if (kill(children->pids[i], signo) == -1 && errno != ESRCH) {
            diagnostic_error("kill child", errno);
            result = -1;
        }
    }
    return result;
}

/* 유예 시간 후 남은 자식에 SIGTERM, SIGKILL을 차례로 전송하고 최종 회수한다. */
static int shutdown_children(struct child_processes *children, int grace_seconds)
{
    int result = EXIT_SUCCESS;
    if (wait_for_children(children, grace_seconds) == -1)
        result = EXIT_FAILURE;
    const int signals[] = {SIGTERM, SIGKILL};
    for (int i = 0; i < 2 && children->active; ++i) {
        if (signal_children(children, signals[i]) == -1)
            result = EXIT_FAILURE;
        if (wait_for_children(children, 1) == -1)
            result = EXIT_FAILURE;
    }
    for (;;) {
        pid_t pid = waitpid(-1, NULL, 0);
        if (pid > 0 || (pid == -1 && errno == EINTR))
            continue;
        if (errno == ECHILD)
            return result;
        diagnostic_error("waitpid shutdown", errno);
        return EXIT_FAILURE;
    }
}

/* 서버 자원을 준비하고 부모 루프를 실행한 뒤 자식과 자원을 정리한다. */
int server_run(int port, const char *root, int limit, int idle, int grace, int backend_limit)
{
    int root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd == -1) {
        diagnostic_error("open static root", errno);
        return EXIT_FAILURE;
    }
    sigset_t wait_mask;
    if (setup_parent_signals(&wait_mask) == -1) {
        close(root_fd);
        return EXIT_FAILURE;
    }
    int listener = open_listener(port);
    if (listener == -1) {
        close(root_fd);
        return EXIT_FAILURE;
    }
    struct child_processes children = {
        .pids = calloc((size_t)limit, sizeof(pid_t)),
        .capacity = limit,
        .active = 0,
        .channels = calloc((size_t)limit, sizeof(struct child_channel)),
        .pollfds = calloc((size_t)limit + 1, sizeof(struct pollfd)),
        .backend_limit = backend_limit,
    };
    if (!children.pids || !children.channels || !children.pollfds) {
        diagnostic_error("calloc", errno);
        free(children.pids);
        free(children.channels);
        free(children.pollfds);
        close(root_fd);
        close(listener);
        return EXIT_FAILURE;
    }
    printf("READY port=%d max_children=%d idle=%d grace=%d backend_limit=%d\n",
           port, limit, idle, grace, backend_limit);
    fflush(stdout);

    int result = run_parent(listener, root_fd, &children, idle, &wait_mask);
    close(listener);
    close(root_fd);
    if (shutdown_children(&children, grace) != EXIT_SUCCESS)
        result = EXIT_FAILURE;
    for (int i = 0; i < children.active; ++i)
        close_channel(&children.channels[i]);
    free(children.pids);
    free(children.channels);
    free(children.pollfds);
    return result;
}
