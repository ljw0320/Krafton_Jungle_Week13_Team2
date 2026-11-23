#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../http/http_internal.h"

static pid_t caller;
static int child_first;
static int saw_epipe;

ssize_t __real_write(int fd, const void *buffer, size_t length);

/* Force the real CGI to exit before the first POST write, without reaping it. */
ssize_t __wrap_write(int fd, const void *buffer, size_t length)
{
    if (child_first && getpid() == caller && !saw_epipe) {
        siginfo_t info = {0};
        int result;
        do { result = waitid(P_ALL, 0, &info, WEXITED | WNOWAIT); }
        while (result == -1 && errno == EINTR);
        assert(result == 0 && info.si_pid > 0);
        ssize_t n = __real_write(fd, buffer, length);
        assert(n == -1 && errno == EPIPE);
        saw_epipe = 1;
        return n;
    }
    return __real_write(fd, buffer, length);
}

/* Bypass the HTTP body cap to force backpressure in both CGI pipes. */
int main(int argc, char **argv)
{
    assert(argc == 2 || (argc == 3 && strcmp(argv[2], "child-first") == 0));
    child_first = argc == 3;
    caller = getpid();
    int fd = atoi(argv[1]);
    int flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    size_t length = child_first ? 7 : 1024 * 1024;
    char *body = malloc(length);
    assert(body);
    memset(body, 'x', length);
    struct request request = {
        .method = "POST", .target = "/cgi-bin/adder", .http11 = true,
        .close = true, .content_length = length,
    };
    int result = serve_cgi(fd, &request, body);
    assert(!child_first || saw_epipe);
    free(body);
    close(fd);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
