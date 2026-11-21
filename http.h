#ifndef HTTP_H
#define HTTP_H

/* Borrows both fds. fd must be nonblocking and SIGPIPE ignored by the caller.
 * Owns request storage until return; returns EXIT_SUCCESS or EXIT_FAILURE. */
int serve_http(int fd, int root_fd, int idle_seconds);

#endif
