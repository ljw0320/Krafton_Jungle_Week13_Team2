#ifndef HTTP_INTERNAL_H
#define HTTP_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Private contracts between HTTP modules; server.c uses only http.h. */
enum {
    URI_LIMIT = 8192,
    HEADER_LIMIT = 32768,
    BODY_LIMIT = 1048576,
    CGI_BODY_LIMIT = 4096,
};

enum receive_result {
    RECEIVE_OK = 0,
    RECEIVE_ERROR = -1,
    RECEIVE_CLOSED = -2,
    RECEIVE_TIMEOUT = -3,
};

struct request {
    /* Borrowed from the mutable header buffer until the next request. */
    const char *method;
    const char *target;
    const char *content_type;
    size_t content_length;
    bool http11;
    bool close;
    bool expect_continue;
};

/* Resets request, mutates the complete NUL-terminated header, and borrows its strings.
 * Returns 0 on success or a positive HTTP error status. */
int parse_request(char *header, struct request *request);

/* Buffers include NUL space. Returns OK, CLOSED for idle/EOF, ERROR for I/O,
 * or a positive HTTP error. deadline starts when the first byte is received. */
int receive_header(int fd, char *header, size_t capacity, int idle_seconds, double *deadline);
/* Discards the body; returns OK, ERROR, or a positive HTTP error. */
int receive_body(int fd, size_t content_length, double deadline);
/* Stores exactly length bytes in a caller-owned buffer of at least length bytes. */
int receive_body_into(int fd, char *buffer, size_t length, double deadline);
/* Send functions borrow fd and return 0 on success or -1 on failure. */
int send_all(int fd, const void *data, size_t length);
int send_headers(int fd, const struct request *request, int status,
                 const char *type, uintmax_t length);
int send_error(int fd, const struct request *request, int status);

/* capacity includes NUL space; returns 0 or a positive HTTP error. */
int decode_path(const char *target, char *path, size_t capacity);
/* Mutates the decoded path, borrows root_fd, and closes any files it opens.
 * Returns 0 when a success/error response is sent, or -1 on I/O failure. */
int serve_static(int fd, int root_fd, const struct request *request, char *path);
/* Runs only build/adder; borrows body and sends a complete response. */
int serve_cgi(int fd, const struct request *request, const char *body);

#endif
