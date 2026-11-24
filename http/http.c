#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include "http_internal.h"
#include "../proxy/proxy.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum request_route {
    ROUTE_STATIC,
    ROUTE_CGI,
    ROUTE_CGI_NOT_FOUND,
    ROUTE_PROXY,
};

struct request_storage {
    char header[HEADER_LIMIT + 1];
    char raw_header[HEADER_LIMIT + 1];
    char path[URI_LIMIT + 1];
    char body[CGI_BODY_LIMIT];
    enum request_route route;
    const struct proxy_route *upstream;
    char *proxy_header;
    char *proxy_body;
};

enum connection_action {
    CONNECTION_ERROR = -1,
    CONNECTION_DONE = 0,
    CONNECTION_KEEP = 1,
};

/* 요청 경로를 정적 파일, 지원하는 CGI, 존재하지 않는 CGI 경로로 분류한다. */
static enum request_route select_route(const char *path)
{
    if (strcmp(path, "/cgi-bin/adder") == 0)
        return ROUTE_CGI;
    while (*path == '/')
        ++path;
    if (strncmp(path, "cgi-bin", 7) == 0 && (path[7] == '/' || path[7] == '\0'))
        return ROUTE_CGI_NOT_FOUND;
    return ROUTE_STATIC;
}

/* 헤더와 경로를 해석하고 CGI POST의 본문 크기·형식을 검사하며, 본문을 저장하거나 읽어 버린다. */
static int read_request(int fd, int idle_seconds, struct request_storage *storage,
                        struct request *request)
{
    double deadline;
    int status = receive_header(fd, storage->header, sizeof(storage->header), idle_seconds, &deadline);
    if (status)
        return status;
    strcpy(storage->raw_header, storage->header);
    status = parse_request(storage->header, request);
    if (status)
        return status;
    if (strcmp(request->method, "GET") != 0 && strcmp(request->method, "POST") != 0)
        return 405;
    status = decode_path(request->target, storage->path, sizeof(storage->path));
    if (status)
        return status;
    storage->route = select_route(storage->path);
    if (storage->route == ROUTE_STATIC) {
        storage->upstream = routes_match(storage->path);
        if (storage->upstream) {
            storage->route = ROUTE_PROXY;
            status = proxy_request_header(storage->raw_header, request, &storage->proxy_header);
            if (status)
                return status;
            if (request->content_length) {
                storage->proxy_body = malloc(request->content_length);
                if (!storage->proxy_body)
                    return 500;
            }
        }
    }
    bool cgi_post = storage->route == ROUTE_CGI &&
        strcmp(request->method, "POST") == 0;
    if (cgi_post) {
        if (request->content_length > CGI_BODY_LIMIT)
            return 413;
        if (!request->content_type ||
            strcasecmp(request->content_type, "application/x-www-form-urlencoded") != 0)
            return 415;
    }
    if (request->expect_continue && request->content_length) {
        static const char response[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (send_all(fd, response, sizeof(response) - 1) == -1)
            return RECEIVE_ERROR;
    }
    if (storage->route == ROUTE_PROXY)
        return receive_body_into(fd, storage->proxy_body, request->content_length, deadline);
    return cgi_post ? receive_body_into(fd, storage->body, request->content_length, deadline) :
        receive_body(fd, request->content_length, deadline);
}

/* 요청을 정적 파일 또는 CGI 처리로 연결하고 오류 응답과 연결 유지·종료를 결정한다. */
static enum connection_action serve_request(int fd, int root_fd, int idle_seconds,
                                            struct request_storage *storage)
{
    struct request request = {.http11 = true, .close = true};
    storage->proxy_body = NULL;
    storage->proxy_header = NULL;
    int status = read_request(fd, idle_seconds, storage, &request);
    if (status) {
        free(storage->proxy_body);
        free(storage->proxy_header);
    }
    if (status == RECEIVE_CLOSED)
        return CONNECTION_DONE;
    if (status < 0)
        return CONNECTION_ERROR;
    if (status) {
        request.close = true;
        if (send_error(fd, &request, status) == -1)
            return CONNECTION_ERROR;
        return CONNECTION_DONE;
    }
    int sent;
    if (storage->route == ROUTE_PROXY)
        sent = serve_proxy(fd, storage->upstream, &request, storage->proxy_header, storage->proxy_body);
    else if (storage->route == ROUTE_CGI)
        sent = serve_cgi(fd, &request, storage->body);
    else if (storage->route == ROUTE_CGI_NOT_FOUND)
        sent = send_error(fd, &request, 404);
    else if (strcmp(request.method, "POST") == 0)
        sent = send_error(fd, &request, 501);
    else
        sent = serve_static(fd, root_fd, &request, storage->path);
    free(storage->proxy_body);
    free(storage->proxy_header);
    if (sent == -1)
        return CONNECTION_ERROR;
    return request.close ? CONNECTION_DONE : CONNECTION_KEEP;
}

/* 요청 저장 공간을 할당하고 연결이 유지되는 동안 요청을 반복 처리한 뒤 해제한다. */
int serve_http(int fd, int root_fd, int idle_seconds)
{
    struct request_storage *storage = malloc(sizeof(*storage));
    if (!storage)
        return EXIT_FAILURE;
    enum connection_action action;
    do {
        action = serve_request(fd, root_fd, idle_seconds, storage);
    } while (action == CONNECTION_KEEP);
    free(storage);
    return action == CONNECTION_ERROR ? EXIT_FAILURE : EXIT_SUCCESS;
}
