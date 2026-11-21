#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void respond(struct connection *client, int status, off_t size, const char *type)
{
    int length = http_response(client->header, sizeof(client->header),
                               status, size, type);

    if (length == -1) {
        client->reason = "response_error";
        return;
    }

    client->header_length = (size_t)length;
    client->header_sent = 0;
    client->sent = 0;

    if (client->request.suppress_body)
        client->length = 0;

    client->phase = WRITE_RESPONSE;
    client->progress = now_ms();
}

void respond_error(struct connection *client, int status)
{
    int length = snprintf((char *)client->data, sizeof(client->data),
                          "%d %s\n", status, http_reason(status));

    client->length = (size_t)length;
    respond(client, status, length, "text/plain; charset=utf-8");
}

static void prepare_get(struct server *server, struct connection *client)
{
    int status;

    client->file_fd = file_open(server->root_fd, client->request.target,
                                &client->file_left, &status);

    if (client->file_fd == -1) {
        respond_error(client, status);
        return;
    }

    client->length = 0;
    respond(client, 200, client->file_left, file_type(client->request.target));
}

/* 헤더 뒤에 함께 도착한 POST 본문도 보존하고 지정된 길이만 처리합니다. */
static void prepare_request(struct server *server, struct connection *client,
                            size_t end)
{
    int status = http_parse(client->input, end, &client->request);
    if (status) {
        respond_error(client, status);
        return;
    }

    if (client->request.method == METHOD_GET) {
        prepare_get(server, client);
        return;
    }

    if (strcmp(client->request.target, "/echo")) {
        respond_error(client, 404);
        return;
    }

    client->length = client->input_length - end;
    if (client->length > client->request.content_length)
        client->length = client->request.content_length;

    memcpy(client->data, client->input + end, client->length);
    client->phase = READ_BODY;
    if (client->length == client->request.content_length)
        respond(client, 200, (off_t)client->length, "application/octet-stream");
}

static void header_received(struct server *server, struct connection *client)
{
    size_t end = http_header_end(client->input, client->input_length);

    if (end)
        prepare_request(server, client, end);
    else if (client->input_length == HEADER_LIMIT)
        respond_error(client, 431);
}

/* EINTR은 예산 안에서 재시도하고 EAGAIN은 다음 poll까지 양보합니다. */
static bool io_result(struct connection *client, ssize_t result, bool writing)
{
    if (result > 0) {
        client->progress = now_ms();
        return true;
    }

    if (result == -1 && errno == EINTR)
        return true;

    if (result == 0)
        client->reason = writing ? "send_no_progress" : "peer_closed";
    else if (!would_block())
        client->reason = writing ? "send_error" : "recv_error";

    return false;
}

static bool receive_step(struct server *server, struct connection *client,
                          size_t *received)
{
    bool header = client->phase == READ_HEADER;
    size_t size = header ? HEADER_LIMIT - client->input_length :
                          client->request.content_length - client->length;
    void *out = header ? (void *)(client->input + client->input_length) :
                         (void *)(client->data + client->length);
    if (size > IO_BUDGET - *received)
        size = IO_BUDGET - *received;

    ssize_t result = recv(client->fd, out, size, 0);
    if (!io_result(client, result, false))
        return false;
    if (result <= 0)
        return true;

    *received += (size_t)result;
    if (header) {
        client->input_length += (size_t)result;
        header_received(server, client);
    } else {
        client->length += (size_t)result;
        if (client->length == client->request.content_length)
            respond(client, 200, (off_t)client->length, "application/octet-stream");
    }

    return true;
}

/* 파일 전체를 올리지 않고 최대 16KiB씩 연결 버퍼를 다시 채웁니다. */
static bool refill_file(struct connection *client)
{
    if (client->file_left == 0) {
        client->reason = "response_complete";
        return false;
    }

    size_t size = client->file_left > 16384 ? 16384 : (size_t)client->file_left;
    ssize_t result = read(client->file_fd, client->data, size);

    if (result == -1 && errno == EINTR)
        return true;

    if (result <= 0) {
        client->reason = "file_read_error";
        return false;
    }

    client->file_left -= result;
    client->length = (size_t)result;
    client->sent = 0;

    return true;
}

static bool send_step(struct connection *client, size_t *transmitted)
{
    bool header = client->header_sent < client->header_length;
    if (!header && client->sent == client->length)
        return refill_file(client);

    size_t *offset = header ? &client->header_sent : &client->sent;
    size_t size = (header ? client->header_length : client->length) - *offset;
    const void *data = header ? (void *)(client->header + *offset) :
                               (void *)(client->data + *offset);
    if (size > IO_BUDGET - *transmitted)
        size = IO_BUDGET - *transmitted;

    ssize_t result = send(client->fd, data, size, 0);
    if (!io_result(client, result, true))
        return false;

    if (result > 0) {
        *offset += (size_t)result;
        *transmitted += (size_t)result;
    }

    return true;
}

/* RUNNING 연결을 독점하며 수신·송신 각각 64KiB/최대 64단계만 처리합니다. */
void process_turn(struct server *server, struct connection *client)
{
    size_t received = 0, transmitted = 0;

    for (int calls = 0; calls < CALL_BUDGET && !client->reason; calls++) {
        if (client->phase == WRITE_RESPONSE) {
            if (transmitted == IO_BUDGET || !send_step(client, &transmitted))
                break;
        } else {
            if (received == IO_BUDGET || !receive_step(server, client, &received))
                break;
        }
    }
}
