#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* 응답 길이와 송신 오프셋을 준비하고 쓰기 단계로 전환합니다. */
void respond(struct connection *client, int status, off_t size, const char *type)
{
    int length = http_response(client->context->header, sizeof(client->context->header),
                               status, size, type, client->keep_alive);

    if (length == -1) {
        client->reason = "response_error";
        return;
    }

    client->header_length = (size_t)length;
    client->header_sent = 0;
    client->sent = 0;

    if (client->context->request.suppress_body)
        client->length = 0;

    client->phase = WRITE_RESPONSE;
    client->progress = now_ms();
}

/* 크기 초과 등 오류도 먼저 HTTP 응답을 보내고 공통 종료 절차를 따릅니다. */
void respond_error(struct connection *client, int status)
{
    client->keep_alive = false;
    int length = snprintf((char *)client->context->data, sizeof(client->context->data),
                          "%d %s\n", status, http_reason(status));

    client->length = (size_t)length;
    respond(client, status, length, "text/plain; charset=utf-8");
}

static void prepare_get(struct server *server, struct connection *client)
{
    int status;

    client->file_fd = file_open(server->root_fd, client->context->request.target,
                                &client->file_left, &status);

    if (client->file_fd == -1) {
        respond_error(client, status);
        return;
    }

    client->length = 0;
    respond(client, 200, client->file_left, file_type(client->context->request.target));
}

/* 전체 POST 본문을 받은 뒤 CGI 실행 또는 기존 에코 응답을 준비합니다. */
static void body_received(struct server *server, struct connection *client)
{
    if (cgi_target(client->context->request.target))
        cgi_start(server, client);
    else
        respond(client, 200, (off_t)client->length, "application/octet-stream");
}

/* 현재 요청만 소비하고 미리 받은 다음 요청은 입력 버퍼에 남깁니다. */
static void consume_input(struct connection *client, size_t size)
{
    client->input_length -= size;
    memmove(client->context->input, client->context->input + size, client->input_length);
}

static void prepare_body(struct server *server, struct connection *client, size_t end)
{
    client->length = client->input_length - end;
    if (client->length > client->context->request.content_length)
        client->length = client->context->request.content_length;
    memcpy(client->context->data, client->context->input + end, client->length);
    consume_input(client, end + client->length);
    client->phase = READ_BODY;
    if (client->length == client->context->request.content_length)
        body_received(server, client);
}

/* 크기 초과여도 선언된 길이를 보존해, 이미 읽은 부분을 뺀 남은 업로드만 폐기합니다. */
static void reject_request(struct connection *client, int status, size_t end)
{
    size_t buffered = client->input_length - end;
    size_t declared = client->context->request.content_length;

    if (status == 413 && declared > BODY_LIMIT && declared > buffered)
        client->drain_remaining = declared - buffered;

    respond_error(client, status);
}

/* GET은 헤더만 소비하고 POST는 선언된 본문까지 분리해 후속 요청을 보존합니다. */
static void route_request(struct server *server, struct connection *client, size_t end)
{
    if (client->context->request.method == METHOD_GET) {
        consume_input(client, end);
        if (cgi_target(client->context->request.target))
            cgi_start(server, client);
        else
            prepare_get(server, client);
        return;
    }

    if (strcmp(client->context->request.target, "/echo") && !cgi_target(client->context->request.target)) {
        respond_error(client, 404);
        return;
    }

    prepare_body(server, client, end);
}

static void prepare_request(struct server *server, struct connection *client, size_t end)
{
    int status = http_parse(client->context->input, end, &client->context->request);
    if (status) {
        reject_request(client, status, end);
        return;
    }
    client->requests++;
    client->keep_alive = client->context->request.http11 && !client->context->request.close_connection &&
                         client->requests < KEEP_ALIVE_MAX_REQUESTS;
    route_request(server, client, end);
}

/* 소켓에 새 바이트가 없어도 완성된 선행 수신 요청은 바로 처리할 수 있습니다. */
bool connection_buffered(const struct connection *client)
{
    return client->context && client->phase == READ_HEADER &&
           (client->input_length == HEADER_LIMIT ||
            http_header_end(client->context->input, client->input_length) != 0);
}

bool connection_idle(const struct connection *client)
{
    return client->phase == READ_HEADER && client->requests > 0 && !client->input_length;
}

static void header_received(struct server *server, struct connection *client)
{
    size_t end = http_header_end(client->context->input, client->input_length);

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

static bool receive_step(struct server *server, struct connection *client, size_t *received)
{
    bool header = client->phase == READ_HEADER;
    size_t size = header ? HEADER_LIMIT - client->input_length :
                          client->context->request.content_length - client->length;

    void *out = header ? (void *)(client->context->input + client->input_length) :
                         (void *)(client->context->data + client->length);
                         
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
        if (client->length == client->context->request.content_length)
            body_received(server, client);
    }

    return true;
}

/* 실제 바이트가 있을 때만 확보합니다. 풀 고갈 시 대기 정책은 3단계에서 추가합니다. */
static bool receive_context(struct connection *client)
{
    unsigned char first_byte;
    ssize_t ready = recv(client->fd, &first_byte, 1, MSG_PEEK);
    if (!io_result(client, ready, false) || ready <= 0)
        return false;
    if (context_acquire(client))
        return true;
    client->reason = "request_context_limit";
    return false;
}

/* 이미 완성된 헤더는 recv를 기다리지 않고 파싱하고, 일부 헤더는 이어서 받습니다. */
static bool receive_request(struct server *server, struct connection *client, size_t *received)
{
    if (!client->context && !receive_context(client))
        return false;
    if (connection_buffered(client)) {
        header_received(server, client);
        return true;
    }
    return receive_step(server, client, received);
}

/* 응답을 모두 넘겼는지 확인합니다. 상대의 수신 완료를 뜻하지는 않습니다. */
static bool response_sent(const struct connection *client)
{
    return client->header_sent == client->header_length &&
           client->sent == client->length && client->file_left == 0;
}

/* 송신 종료는 한 번만 수행합니다. 업로드가 남았다면 폐기 후 호출합니다. */
static bool shutdown_write(struct connection *client)
{
    if (shutdown(client->fd, SHUT_WR) == -1) {
        if (errno == EINTR)
            return true;

        client->reason = "shutdown_error";
        return false;
    }

    client->write_shutdown = true;
    return true;
}

/* 413은 이미 보냈습니다. WSL 전달 호환을 위해 남은 업로드를 먼저 폐기합니다. */
static bool begin_drain(struct connection *client)
{
    context_release(client);
    client->phase = DRAIN_INPUT;
    client->drain_deadline = now_ms() + DRAIN_TIMEOUT_MS;

    return client->drain_remaining ? true : shutdown_write(client);
}

/* 소켓·요청 횟수·선행 수신 데이터는 보존하고 요청별 자원과 상태만 정리합니다. */
static bool finish_response(struct connection *client)
{
    if (!client->keep_alive)
        return begin_drain(client);
    cgi_close(client);
    if (client->file_fd >= 0)
        close(client->file_fd);
    client->file_fd = -1;
    client->file_left = 0;
    memset(&client->context->request, 0, sizeof(client->context->request));
    client->cgi.length = 0;
    client->header_length = client->header_sent = 0;
    client->length = client->sent = 0;
    client->drain_remaining = 0;
    client->drain_deadline = 0;
    client->write_shutdown = client->keep_alive = false;
    client->phase = READ_HEADER;
    client->progress = now_ms();
    if (!client->input_length)
        context_release(client);
    return true;
}

/* 본문을 버린 양만 기록합니다. 상대가 먼저 끝내면 남은 길이에 관계없이 송신도 종료합니다. */
static bool discard_received(struct connection *client, ssize_t result, size_t *received)
{
    if (result > 0) {
        *received += (size_t)result;
        if (client->drain_remaining) {
            client->drain_remaining -= (size_t)result;
            if (!client->drain_remaining)
                return shutdown_write(client);
        }
        return true;
    }

    if (result == -1 && errno == EINTR)
        return true;

    if (result == 0 && !client->write_shutdown) {
        client->drain_remaining = 0;
        return shutdown_write(client);
    }
    if (result == 0)
        client->reason = "response_complete";
    else if (!would_block())
        client->reason = "drain_error";

    return false;
}

/* 기한은 연장하지 않습니다. EAGAIN이면 poll에 양보하고, 폐기가 끝나면 반쪽 종료합니다. */
static bool drain_step(struct connection *client, size_t *received)
{
    unsigned char discard[DRAIN_BUFFER_SIZE];

    if (now_ms() >= client->drain_deadline) {
        client->reason = "drain_timeout";
        return false;
    }
    if (!client->write_shutdown && !client->drain_remaining)
        return shutdown_write(client);

    size_t size = IO_BUDGET - *received;
    if (size > sizeof(discard))
        size = sizeof(discard);
    if (client->drain_remaining && size > client->drain_remaining)
        size = client->drain_remaining;

    ssize_t result = recv(client->fd, discard, size, 0);
    return discard_received(client, result, received);
}

/* 최대 16KiB씩 채우고, 응답 완료 시 연결 유지 또는 종료를 선택합니다. */
static bool refill_file(struct connection *client)
{
    if (client->file_left == 0)
        return finish_response(client);

    size_t size = client->file_left > 16384 ? 16384 : (size_t)client->file_left;
    ssize_t result = read(client->file_fd, client->context->data, size);

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

/* 부분 송신 위치를 보존하며 마지막 바이트를 넘기면 다음 요청 또는 종료로 전환합니다. */
static bool send_step(struct connection *client, size_t *transmitted)
{
    bool header = client->header_sent < client->header_length;
    if (!header && client->sent == client->length)
        return refill_file(client);

    size_t *offset = header ? &client->header_sent : &client->sent;
    size_t size = (header ? client->header_length : client->length) - *offset;
    const void *data = header ? (void *)(client->context->header + *offset) :
                               (void *)(client->context->data + *offset);
    if (size > IO_BUDGET - *transmitted)
        size = IO_BUDGET - *transmitted;

    ssize_t result = send(client->fd, data, size, 0);
    if (!io_result(client, result, true))
        return false;

    if (result > 0) {
        *offset += (size_t)result;
        *transmitted += (size_t)result;
    }

    return response_sent(client) ? finish_response(client) : true;
}

/* 정리 수신도 일반 수신과 같은 64KiB/64단계 예산 안에서 처리합니다. */
void process_turn(struct server *server, struct connection *client)
{
    size_t received = 0, transmitted = 0;

    for (int calls = 0; calls < CALL_BUDGET && !client->reason; calls++) {
        if (client->phase == CGI_OUTPUT) {
            cgi_turn(client);
            break;
        } else if (client->phase == WRITE_RESPONSE) {
            if (transmitted == IO_BUDGET || !send_step(client, &transmitted))
                break;
        } else if (client->phase == DRAIN_INPUT) {
            if (received == IO_BUDGET || !drain_step(client, &received))
                break;
        } else {
            if (received == IO_BUDGET || !receive_request(server, client, &received))
                break;
        }
    }
}
