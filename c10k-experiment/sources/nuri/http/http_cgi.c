#define _POSIX_C_SOURCE 200809L
#include "http_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { CGI_OUTPUT_LIMIT = 65536, CGI_HEADER_LIMIT = 8192, CGI_SECONDS = 5 };
static volatile sig_atomic_t interrupted;

struct cgi_process {
    pid_t pid;
    int input;
    int output;
    int exec_error;
    bool exited;
    bool success;
};

/* 종료 시그널 수신 시 CGI 처리 중단 플래그를 설정한다. */
static void cancel_cgi(int signo)
{
    (void)signo;
    interrupted = 1;
}

/* CGI 실행 기한 계산에 사용할 단조 시계 시간을 반환한다. */
static double now_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
        return -1;
    return now.tv_sec + now.tv_nsec / 1e9;
}

/* 유효한 파일 디스크립터를 닫고 값을 -1로 초기화한다. */
static void close_fd(int *fd)
{
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

/* exec 시 자동으로 닫히는 파이프를 생성하고 표준 입출력과 디스크립터 번호가 겹치지 않게 한다. */
static int make_pipe(int fds[2])
{
    if (pipe(fds) == -1)
        return -1;
    for (int i = 0; i < 2; ++i) {
        /* Keep originals separate from the standard descriptors used by dup2. */
        if (fds[i] <= STDERR_FILENO) {
            int copy = fcntl(fds[i], F_DUPFD_CLOEXEC, 3);
            close(fds[i]);
            fds[i] = copy;
        }
        if (fds[i] < 0 || fcntl(fds[i], F_SETFD, FD_CLOEXEC) == -1) {
            close_fd(&fds[0]);
            close_fd(&fds[1]);
            return -1;
        }
    }
    return 0;
}

/* 파일 디스크립터를 non-blocking 모드로 설정한다. */
static int nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Separate error pipe distinguishes exec/setup failure from the CGI's exit code. */
/* 파이프와 자식 프로세스를 생성하고 표준 입출력을 연결하여 build/adder를 실행한다. */
static int start_cgi(struct cgi_process *process, int client, char *const envp[])
{
    int input[2] = {-1, -1}, output[2] = {-1, -1}, error[2] = {-1, -1};
    if (make_pipe(input) == -1 || make_pipe(output) == -1 || make_pipe(error) == -1)
        goto failure;
    pid_t pid = fork();
    if (pid == -1)
        goto failure;
    if (pid == 0) {
        close(error[0]);
        struct sigaction action = {.sa_handler = SIG_DFL};
        sigemptyset(&action.sa_mask);
        if (setpgid(0, 0) == -1 || sigaction(SIGTERM, &action, NULL) == -1 ||
            sigaction(SIGINT, &action, NULL) == -1 || sigaction(SIGPIPE, &action, NULL) == -1 ||
            dup2(input[0], STDIN_FILENO) == -1 || dup2(output[1], STDOUT_FILENO) == -1)
            goto child_failure;
        close(input[0]);
        close(input[1]);
        close(output[0]);
        close(output[1]);
        close(client);
        char *const argv[] = {"adder", NULL};
        execve("build/adder", argv, envp);
child_failure:;
        char failed = 1;
        while (write(error[1], &failed, 1) == -1 && errno == EINTR) {}
        _exit(127);
    }
    process->pid = pid;
    close(input[0]);
    close(output[1]);
    close(error[1]);
    process->input = input[1];
    process->output = output[0];
    process->exec_error = error[0];
    /* Also set the group here so cleanup cannot race the child's setpgid. */
    if (setpgid(pid, pid) == -1 && errno != EACCES && errno != ESRCH)
        return -1;
    return nonblocking(process->input) == -1 || nonblocking(process->output) == -1 ? -1 : 0;
failure:
    for (int i = 0; i < 2; ++i) {
        close_fd(&input[i]);
        close_fd(&output[i]);
        close_fd(&error[i]);
    }
    return -1;
}

/* CGI 프로세스를 회수하지 않고 종료 여부와 정상 종료 여부를 확인한다. */
static int check_cgi_exit(struct cgi_process *process)
{
    if (process->exited)
        return 0;
    /* Retain the leader PID until group cleanup, preventing PID reuse before kill. */
    siginfo_t info = {0};
    int result = waitid(P_PID, (id_t)process->pid, &info, WEXITED | WNOHANG | WNOWAIT);
    if (result == 0 && info.si_pid == process->pid) {
        process->exited = true;
        process->success = info.si_code == CLD_EXITED && info.si_status == 0;
    }
    return result < 0 && errno != EINTR ? -1 : 0;
}

/* A bounded grace period also handles programs that ignore SIGTERM. */
/* 파이프와 CGI 프로세스 그룹을 정리하고 자식을 회수한다. 중단 시 종료 유예 후 강제 종료한다. */
static void finish_cgi(struct cgi_process *process, bool aborting)
{
    close_fd(&process->input);
    close_fd(&process->output);
    close_fd(&process->exec_error);
    if (process->pid <= 0)
        return;
    if (aborting) {
        kill(-process->pid, SIGTERM);
        if (!process->exited)
            kill(process->pid, SIGTERM);
        struct timespec pause = {.tv_nsec = 100000000};
        nanosleep(&pause, NULL);
    }
    /* Do not leave descendants holding pipe ends after the CGI leader exits. */
    kill(-process->pid, SIGKILL);
    if (aborting && !process->exited)
        kill(process->pid, SIGKILL);
    while (waitpid(process->pid, NULL, 0) == -1 && errno == EINTR) {}
}

/* Input and output must progress together: either pipe can fill before the other. */
/* 본문 전달과 CGI 출력 수신을 함께 진행하고 실행 시간·출력 크기·클라이언트 연결·종료 상태를 검사한다. */
static int exchange_cgi(struct cgi_process *process, int client, const char *body,
                        size_t length, char *output, size_t *used, double deadline)
{
    size_t written = 0;
    if (!length)
        close_fd(&process->input);
    for (;;) {
        if (interrupted)
            return RECEIVE_ERROR;
        double now = now_seconds();
        if (now < 0)
            return 500;
        if (now >= deadline)
            return 504;
        if (check_cgi_exit(process) == -1)
            return 500;
        if (process->exited && process->output < 0) {
            char failed;
            ssize_t n;
            do { n = read(process->exec_error, &failed, 1); } while (n < 0 && errno == EINTR);
            if (n != 0)
                return 500;
            return process->success && written == length ? 0 : 502;
        }
        struct pollfd fds[] = {
            {.fd = process->input, .events = POLLOUT},
            {.fd = process->output, .events = POLLIN},
            {.fd = client, .events = 0},
        };
        int timeout = (int)((deadline - now) * 1000) + 1;
        if (timeout > 20)
            timeout = 20;
        int ready = poll(fds, 3, timeout);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return 500;
        }
        /* FIN alone may be a legitimate HTTP write-half close; reset/hangup is fatal. */
        if (fds[2].revents & (POLLERR | POLLHUP | POLLNVAL))
            return RECEIVE_ERROR;
        if (fds[1].revents) {
            ssize_t n = read(process->output, output + *used, CGI_OUTPUT_LIMIT + 1 - *used);
            if (n > 0) {
                *used += (size_t)n;
                if (*used > CGI_OUTPUT_LIMIT)
                    return 502;
            } else if (n == 0) {
                close_fd(&process->output);
            } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                return 502;
            }
        }
        if (fds[0].revents) {
            ssize_t n = write(process->input, body + written, length - written);
            if (n > 0) {
                written += (size_t)n;
                if (written == length)
                    close_fd(&process->input);
            } else if (n < 0 && errno == EPIPE) {
                /* Exec failure can close stdin first; classify after exit/error-pipe checks. */
                close_fd(&process->input);
            } else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                return 502;
            }
        }
    }
}

/* This endpoint accepts a document response: Content-Type plus optional Status. */
/* CGI 출력의 Content-Type과 선택적 Status를 검증하여 HTTP 응답으로 전송하고 잘못된 출력에는 502를 응답한다. */
static int send_cgi_output(int fd, const struct request *request, char *output, size_t length)
{
    size_t offset = 0;
    char *type = NULL;
    bool has_status = false;
    int status = 200;
    while (offset < length && offset < CGI_HEADER_LIMIT) {
        char *line = output + offset;
        char *end = memchr(line, '\n', length - offset);
        if (!end || (size_t)(end - output) >= CGI_HEADER_LIMIT)
            break;
        offset = (size_t)(end - output) + 1;
        if (end > line && end[-1] == '\r')
            --end;
        for (char *p = line; p < end; ++p)
            if ((unsigned char)*p < 32 || (unsigned char)*p >= 127)
                return send_error(fd, request, 502);
        *end = '\0';
        if (!*line) {
            if (!type)
                break;
            if (send_headers(fd, request, status, type, length - offset) == -1)
                return -1;
            return send_all(fd, output + offset, length - offset);
        }
        if (strncasecmp(line, "Content-Type: ", 14) == 0) {
            if (type || !line[14] || strlen(line + 14) > 127)
                break;
            type = line + 14;
        } else if (strncasecmp(line, "Status: ", 8) == 0) {
            const char *value = line + 8;
            if (has_status || strlen(value) < 5 || value[0] < '2' || value[0] > '5' ||
                value[1] < '0' || value[1] > '9' || value[2] < '0' || value[2] > '9' || value[3] != ' ')
                break;
            status = (value[0] - '0') * 100 + (value[1] - '0') * 10 + value[2] - '0';
            if (status != 200 && status != 400 && status != 403 && status != 404 &&
                status != 405 && status != 413 && status != 415 && status != 500)
                break;
            has_status = true;
        } else {
            break;
        }
    }
    return send_error(fd, request, 502);
}

/* CGI 환경변수 구성과 실행·입출력·정리를 관리하고 결과 또는 오류 응답을 전송한다. */
int serve_cgi(int fd, const struct request *request, const char *body)
{
    char query[URI_LIMIT + 32], method[32], length[64];
    const char *question = strchr(request->target, '?');
    snprintf(query, sizeof(query), "QUERY_STRING=%s", question ? question + 1 : "");
    snprintf(method, sizeof(method), "REQUEST_METHOD=%s", request->method);
    bool post = strcmp(request->method, "POST") == 0;
    snprintf(length, sizeof(length), "CONTENT_LENGTH=%zu", post ? request->content_length : 0);
    char *const envp[] = {method, query, length,
        "CONTENT_TYPE=application/x-www-form-urlencoded", "GATEWAY_INTERFACE=CGI/1.1",
        "SCRIPT_NAME=/cgi-bin/adder",
        request->http11 ? "SERVER_PROTOCOL=HTTP/1.1" : "SERVER_PROTOCOL=HTTP/1.0", NULL};
    char *output = malloc(CGI_OUTPUT_LIMIT + 1);
    if (!output)
        return send_error(fd, request, 500);
    struct sigaction action = {.sa_handler = cancel_cgi}, old_term, old_int;
    sigemptyset(&action.sa_mask);
    interrupted = 0;
    if (sigaction(SIGTERM, &action, &old_term) == -1) {
        free(output);
        return send_error(fd, request, 500);
    }
    if (sigaction(SIGINT, &action, &old_int) == -1) {
        sigaction(SIGTERM, &old_term, NULL);
        free(output);
        return send_error(fd, request, 500);
    }
    struct cgi_process process = {.input = -1, .output = -1, .exec_error = -1};
    double start = now_seconds();
    size_t used = 0;
    int status = 500;
    if (start >= 0 && start_cgi(&process, fd, envp) == 0)
        status = exchange_cgi(&process, fd, body, post ? request->content_length : 0,
                              output, &used, start + CGI_SECONDS);
    finish_cgi(&process, status != 0);
    sigaction(SIGTERM, &old_term, NULL);
    sigaction(SIGINT, &old_int, NULL);
    int result = interrupted || status < 0 ? -1 : status ? send_error(fd, request, status) :
        send_cgi_output(fd, request, output, used);
    free(output);
    return result;
}
