#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/time.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "server.h"
#include "../http/http.h"

static volatile sig_atomic_t stopping;

/* Only the parent updates this table, while SIGCHLD is blocked. */
struct child_processes {
    pid_t *pids;                 /* Entries [0, active) are live or unreaped. */
    int capacity;
    int active;
};

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
        perror(operation);
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
            perror("waitpid reap");
            return -1;
        }
        for (int i = 0; i < children->active; ++i) {
            if (children->pids[i] == pid) {
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
    fprintf(stderr, "sigaction signal=%d: %s\n", signo, strerror(errno));
    return -1;
}

/* 부모의 시그널 처리와 차단을 설정하고 pselect 대기용 기존 마스크를 보관한다.
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
        perror("fcntl socket mode");
        return -1;
    }
    return 0;
}

/* 접속 수신 소켓을 생성하고 주소 재사용, bind, listen, non-blocking 모드를 설정한다. */
static int open_listener(int port)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        perror("socket");
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
        perror("listener setup");
        close(listener);
        return -1;
    }
    if (listener >= FD_SETSIZE) {
        fprintf(stderr, "Listening descriptor exceeds pselect capacity\n");
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
                      struct child_processes *children, const sigset_t *wait_mask)
{
    /* The child owns client_fd; it does not accept or manage other children. */
    close(listener);
    free(children->pids);
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
    close(root_fd);
    close(client_fd);
    _exit(status);
}

/* 자식 수 제한 내에서 접속을 수락하고 처리할 자식을 생성하며, 종료된 자식을 회수한다. */
static int run_parent(int listener, int root_fd, struct child_processes *children,
                      int idle_seconds, const sigset_t *wait_mask)
{
    while (!stopping) {
        if (reap_children(children) == -1)
            return EXIT_FAILURE;
        fd_set ready;
        FD_ZERO(&ready);
        if (children->active < children->capacity)
            FD_SET(listener, &ready);
        /* Atomically unblock signals while waiting, avoiding a check/wait race. */
        int count = pselect(listener + 1, &ready, NULL, NULL, NULL, wait_mask);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            perror("pselect");
            return EXIT_FAILURE;
        }
        if (!FD_ISSET(listener, &ready))
            continue;
        int fd = accept(listener, NULL, NULL);
        if (fd == -1) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED)
                continue;
            perror("accept");
            return EXIT_FAILURE;
        }
        if (set_socket_blocking(fd, false) == -1) {
            close(fd);
            continue;
        }
        pid_t pid = check_call(fork(), "fork");
        if (pid == 0)
            run_child(listener, fd, root_fd, idle_seconds, children, wait_mask);
        close(fd);  /* Only the child retains the connection after fork. */
        if (pid == -1)
            return EXIT_FAILURE;
        children->pids[children->active] = pid;
        ++children->active;
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
            perror("kill child");
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
        perror("waitpid shutdown");
        return EXIT_FAILURE;
    }
}

/* 서버 자원을 준비하고 부모 루프를 실행한 뒤 자식과 자원을 정리한다. */
int server_run(int port, const char *root, int limit, int idle, int grace)
{
    int root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd == -1) {
        perror("open static root");
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
    };
    if (!children.pids) {
        perror("calloc");
        close(root_fd);
        close(listener);
        return EXIT_FAILURE;
    }
    printf("READY port=%d max_children=%d idle=%d grace=%d\n", port, limit, idle, grace);
    fflush(stdout);

    int result = run_parent(listener, root_fd, &children, idle, &wait_mask);
    close(listener);
    close(root_fd);
    if (shutdown_children(&children, grace) != EXIT_SUCCESS)
        result = EXIT_FAILURE;
    free(children.pids);
    return result;
}
