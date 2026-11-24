#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int cgi_spawn(int executable, struct connection *client);

/* CGI 이름 공간은 정적 파일 라우팅보다 먼저 선택합니다. */
bool cgi_target(const char *target)
{
    return !strncmp(target, "/cgi-bin", 8) &&
           (!target[8] || target[8] == '/' || target[8] == '?');
}

/* CGI_ROOT 바로 아래의 명시적인 ASCII 프로그램 이름만 실행합니다. */
static int program_name(const char *target)
{
    if (target[8] != '/' || !target[9] || target[9] == '?')
        return 404;
    size_t length = strcspn(target + 9, "?");
    if ((length == 1 && target[9] == '.') ||
        (length == 2 && !strncmp(target + 9, "..", 2)))
        return 403;
    for (size_t i = 9; i < 9 + length; i++) {
        unsigned char c = (unsigned char)target[i];
        if (c == '/' || c == '%' || c == '\\')
            return 403;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 400;
    }
    return 0;
}

/* 셸 명령 문자열을 만들지 않고 검증된 파일 fd를 posix_spawn에 전달합니다. */
void cgi_start(struct server *server, struct connection *client)
{
    int status = program_name(client->request.target);
    off_t size;
    if (!status && server->cgi_root_fd < 0)
        status = 404;
    if (status) {
        respond_error(client, status);
        return;
    }
    int fd = file_open(server->cgi_root_fd, client->request.target + 8, &size, &status);
    if (fd >= 0) {
        struct stat info;
        status = fstat(fd, &info) == -1 ? 500 : !(info.st_mode & 0111) ? 403 : 0;
        if (!status)
            status = cgi_spawn(fd, client);
        close(fd);
    }
    if (status)
        respond_error(client, status);
}

/* 기한 초과·서버 종료에서도 CGI 프로세스 그룹을 종료하고 직접 자식을 회수합니다. */
void cgi_close(struct connection *client)
{
    if (client->cgi.pid <= 0)
        return;
    kill(-client->cgi.pid, SIGKILL);
    while (waitpid(client->cgi.pid, NULL, 0) == -1 && errno == EINTR) {}
    if (client->cgi.fd >= 0)
        close(client->cgi.fd);
    client->cgi.pid = 0;
    client->cgi.fd = -1;
}

static void cgi_failed(struct connection *client, int status)
{
    cgi_close(client);
    respond_error(client, status);
}

/* EOF 뒤에도 자식 종료를 비차단 확인합니다. stdout만 닫고 멈추는 CGI도 기한을 적용합니다. */
static void cgi_finished(struct connection *client)
{
    int result;
    pid_t pid = waitpid(client->cgi.pid, &result, WNOHANG);
    if (pid == 0 || (pid == -1 && errno == EINTR))
        return;
    if (pid == -1 || !WIFEXITED(result) || WEXITSTATUS(result) != 0) {
        cgi_failed(client, 502);
        return;
    }
    cgi_close(client);
    int error = cgi_response(client);
    if (error)
        respond_error(client, error);
}

static bool read_output(struct connection *client, size_t *received)
{
    size_t space = sizeof(client->cgi.output) - 1 - client->cgi.length;
    size_t size = space < IO_BUDGET - *received ? space : IO_BUDGET - *received;
    char overflow;
    char *out = size ? client->cgi.output + client->cgi.length : &overflow;
    ssize_t n = read(client->cgi.fd, out, size ? size : 1);
    if ((n > 0 && !size) || (n < 0 && errno != EINTR && !would_block())) {
        cgi_failed(client, 502);
        return false;
    }
    if (n > 0) {
        client->cgi.length += (size_t)n;
        *received += (size_t)n;
    } else if (n == 0) {
        close(client->cgi.fd);
        client->cgi.fd = -1;
    }
    return n >= 0 || errno == EINTR;
}

/* stdout 수집은 차례당 64KiB/64회 이내입니다. EAGAIN은 메인의 poll에 양보합니다. */
void cgi_turn(struct connection *client)
{
    size_t received = 0;
    client->cgi.next_check = now_ms() + CGI_POLL_MS;
    for (int calls = 0; calls < CALL_BUDGET && received < IO_BUDGET; calls++) {
        if (now_ms() >= client->cgi.deadline) {
            cgi_failed(client, 504);
            return;
        }
        if (client->cgi.fd < 0) {
            cgi_finished(client);
            return;
        }
        if (!read_output(client, &received))
            return;
    }
}
