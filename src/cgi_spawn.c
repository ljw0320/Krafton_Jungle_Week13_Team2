#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct environment {
    char method[32], query[TARGET_LIMIT + 32], script[TARGET_LIMIT + 32];
    char length[64], type[288];
    char *values[12];
};

/* 프로세스 전역 environ을 변경하지 않고 요청별 독립 환경을 만듭니다. */
static void make_environment(struct environment *env, const struct request *request)
{
    const char *query = strchr(request->target, '?');
    size_t length = query ? (size_t)(query - request->target) : strlen(request->target);
    snprintf(env->method, sizeof(env->method), "REQUEST_METHOD=%s",
             request->method == METHOD_GET ? "GET" : "POST");
    snprintf(env->query, sizeof(env->query), "QUERY_STRING=%s", query ? query + 1 : "");
    snprintf(env->script, sizeof(env->script), "SCRIPT_NAME=%.*s", (int)length, request->target);
    snprintf(env->length, sizeof(env->length), "CONTENT_LENGTH=%zu", request->content_length);
    snprintf(env->type, sizeof(env->type), "CONTENT_TYPE=%s", request->content_type);
    char *values[] = {env->method, env->query, env->script, env->length, env->type,
        "GATEWAY_INTERFACE=CGI/1.1", "SERVER_SOFTWARE=JungleWebServer",
        request->http11 ? "SERVER_PROTOCOL=HTTP/1.1" : "SERVER_PROTOCOL=HTTP/1.0",
        "PATH=/usr/bin:/bin", "LANG=C", NULL};
    memcpy(env->values, values, sizeof(values));
}

/* 64KiB 이하 본문을 익명 메모리 파일로 전달해 stdin/stdout 파이프 교착을 피합니다. */
static int input_open(const struct connection *client)
{
    int fd = memfd_create("cgi-input", MFD_CLOEXEC);
    if (fd == -1)
        return -1;
    size_t offset = 0;
    while (offset < client->length) {
        ssize_t n = write(fd, client->context->data + offset, client->length - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            return -1;
        }
        offset += (size_t)n;
    }
    if (lseek(fd, 0, SEEK_SET) == -1) {
        close(fd);
        return -1;
    }
    return fd;
}

/* 검증한 실행 fd만 CLOEXEC을 해제하며 stderr는 /dev/null로 보냅니다. */
static int spawn_actions(posix_spawn_file_actions_t *actions, int executable, int input, int output)
{
    int error = posix_spawn_file_actions_adddup2(actions, input, STDIN_FILENO);
    if (!error)
        error = posix_spawn_file_actions_adddup2(actions, output, STDOUT_FILENO);
    if (!error)
        error = posix_spawn_file_actions_addopen(actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (!error)
        error = posix_spawn_file_actions_adddup2(actions, executable, 3);
    return error;
}

/* 작업자의 신호 마스크를 상속하지 않고 별도 프로세스 그룹을 생성합니다. */
static int spawn_attributes(posix_spawnattr_t *attributes)
{
    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    int error = posix_spawnattr_setsigmask(attributes, &empty);
    if (!error)
        error = posix_spawnattr_setsigdefault(attributes, &defaults);
    if (!error)
        error = posix_spawnattr_setpgroup(attributes, 0);
    if (!error)
        error = posix_spawnattr_setflags(attributes,
            POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
    return error;
}

static int launch(int executable, int input, int output, struct connection *client)
{
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    struct environment env;
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/3");
    char *argv[] = {client->context->request.target, NULL};
    make_environment(&env, &client->context->request);
    int error = posix_spawn_file_actions_init(&actions);
    if (error)
        return error;
    error = posix_spawnattr_init(&attributes);
    if (!error) {
        error = spawn_actions(&actions, executable, input, output);
        if (!error)
            error = spawn_attributes(&attributes);
        if (!error)
            error = posix_spawn(&client->cgi.pid, path, &actions, &attributes, argv, env.values);
        posix_spawnattr_destroy(&attributes);
    }
    posix_spawn_file_actions_destroy(&actions);
    return error;
}

/* 모든 새 fd는 생성 시 CLOEXEC을 적용해 동시 spawn으로의 fd 누출을 방지합니다. */
int cgi_spawn(int executable, struct connection *client)
{
    int pipes[2];
    int input = input_open(client);
    if (input < 0)
        return 500;
    if (pipe2(pipes, O_CLOEXEC) == -1) {
        close(input);
        return 500;
    }
    /* 동일 fd dup2의 CLOEXEC 처리에 의존하지 않습니다.
     * 자식 fd 3과 다른 원본을 예약하고, dup2로 명시적으로 상속합니다. */
    int inherited = fcntl(executable, F_DUPFD_CLOEXEC, 4);
    int error = inherited == -1 ? errno :
                nonblocking(pipes[0]) == -1 ? errno : launch(inherited, input, pipes[1], client);
    if (inherited >= 0)
        close(inherited);
    close(input);
    close(pipes[1]);
    if (error) {
        client->cgi.pid = 0;
        close(pipes[0]);
        return error == EACCES ? 403 : error == ENOEXEC || error == ENOENT ? 502 : 500;
    }
    client->cgi.fd = pipes[0];
    client->cgi.length = 0;
    client->cgi.deadline = now_ms() + CGI_TIMEOUT_MS;
    client->cgi.next_check = now_ms() + CGI_POLL_MS;
    client->length = 0;
    client->phase = CGI_OUTPUT;
    return 0;
}
