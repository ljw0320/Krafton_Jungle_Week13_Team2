#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../http/http_internal.h"

/* Bypass the HTTP body cap to force backpressure in both CGI pipes. */
int main(int argc, char **argv)
{
    assert(argc == 2);
    int fd = atoi(argv[1]);
    int flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    size_t length = 1024 * 1024;
    char *body = malloc(length);
    assert(body);
    memset(body, 'x', length);
    struct request request = {
        .method = "POST", .target = "/cgi-bin/adder", .http11 = true,
        .close = true, .content_length = length,
    };
    int result = serve_cgi(fd, &request, body);
    free(body);
    close(fd);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
