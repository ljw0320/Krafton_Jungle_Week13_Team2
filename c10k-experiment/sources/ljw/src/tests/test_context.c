#include "context_fixture.h"

static void test_idle_connections_do_not_borrow(void)
{
    fixture_start();
    struct test_client clients[MAX_CONNECTIONS];
    /* 실행: 연결 16개를 열고 데이터 없이 처리 차례를 진행한다. */
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        clients[i] = open_client();
        process_turn(&fixture_server, clients[i].connection);
    }
    /* 검증: 연결 수가 풀 크기보다 많아도 유휴 연결은 점유하지 않는다. */
    for (int i = 0; i < MAX_CONNECTIONS; i++) CHECK(clients[i].connection->context == NULL);
    expect_count("idle connection owners", used_contexts(&fixture_server.contexts), 0);
    for (int i = 0; i < MAX_CONNECTIONS; i++) close_client(&clients[i]);
    fixture_finish();
}

static void test_partial_input_keeps_ownership(void)
{
    fixture_start();
    struct test_client client = open_client();
    /* 준비/실행: 선언된 5바이트 중 he만 보낸다. */
    send_text(&client, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhe");
    process_turn(&fixture_server, client.connection);
    struct request_context *owned = client.connection->context;
    CHECK(owned != NULL && client.connection->phase == READ_BODY);
    expect_count("partial body bytes", client.connection->length, 2);
    process_turn(&fixture_server, client.connection);
    /* 검증: 다음 차례에서도 같은 공간과 미완성 본문을 유지한다. */
    CHECK(client.connection->context == owned);
    CHECK(memcmp(owned->data, "he", 2) == 0);
    expect_count("partial request owners", used_contexts(&fixture_server.contexts), 1);
    send_text(&client, "llo");
    process_turn(&fixture_server, client.connection);
    struct observed_response response = {0};
    collect_response(&client, &response);
    expect_body(&response, "hello", 5);
    CHECK(client.connection->context == NULL && client.connection->requests == 1);
    close_client(&client);
    fixture_finish();
}

static void test_next_request_reacquires_clean_space(void)
{
    fixture_start();
    struct test_client client = open_client();
    send_text(&client, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\none");
    process_turn(&fixture_server, client.connection);
    struct observed_response first = {0};
    collect_response(&client, &first);
    expect_body(&first, "one", 3);
    CHECK(client.connection->context == NULL);
    /* 실행: 같은 소켓의 다음 요청은 반환된 풀 공간을 다시 빌린다. */
    send_text(&client, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\ntwo");
    process_turn(&fixture_server, client.connection);
    struct observed_response second = {0};
    collect_response(&client, &second);
    expect_body(&second, "two", 3);
    expect_count("requests on same connection", client.connection->requests, 2);
    CHECK(client.connection->phase == READ_HEADER && client.connection->context == NULL);
    close_client(&client);
    fixture_finish();
}

static void test_pending_request_delays_release(void)
{
    fixture_start();
    struct test_client client = open_client();
    bool acquired = context_acquire(client.connection);
    CHECK(acquired);
    struct request_context *owned = client.connection->context;
    const char pending[] = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nnext";
    memcpy(owned->input, pending, sizeof(pending) - 1);
    client.connection->input_length = sizeof(pending) - 1;
    client.connection->requests = 1;
    client.connection->keep_alive = true;
    /* 실행: 앞선 응답을 마쳐도 이미 받은 다음 요청은 버리지 않는다. */
    bool finished = finish_response(client.connection);
    CHECK(finished && client.connection->context == owned);
    CHECK(memcmp(owned->input, pending, sizeof(pending) - 1) == 0);
    expect_count("owners with pending request", used_contexts(&fixture_server.contexts), 1);
    process_turn(&fixture_server, client.connection);
    struct observed_response response = {0};
    collect_response(&client, &response);
    expect_body(&response, "next", 4);
    CHECK(client.connection->context == NULL && client.connection->requests == 2);
    close_client(&client);
    fixture_finish();
}

static void test_timeout_releases_context_and_fds(void)
{
    fixture_start();
    struct test_client client = open_client();
    send_text(&client, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhe");
    process_turn(&fixture_server, client.connection);
    int socket_fd = client.connection->fd;
    int file_fd = open("/dev/null", O_RDONLY);
    CHECK(file_fd >= 0);
    client.connection->file_fd = file_fd;
    client.connection->progress = now_ms() - RECV_TIMEOUT_MS - 1;
    struct pollfd descriptor;
    int timeout = -1;
    /* 실행: 실제 메인 루프의 기한 검사 함수를 호출한다. */
    bool retained = poll_client(client.connection, &descriptor, &timeout, now_ms());
    CHECK(!retained && client.connection->state == FREE);
    CHECK(client.connection->context == NULL);
    CHECK(fcntl(socket_fd, F_GETFD) == -1 && errno == EBADF);
    CHECK(fcntl(file_fd, F_GETFD) == -1 && errno == EBADF);
    close_client(&client);
    fixture_finish();
}

static void test_close_response_releases_before_drain(void)
{
    fixture_start();
    struct test_client client = open_client();
    send_text(&client, "POST /echo HTTP/1.0\r\nContent-Length: 3\r\n\r\nbye");
    process_turn(&fixture_server, client.connection);
    /* 검증: 종료 정리 중에는 이미 전송한 응답 버퍼를 점유하지 않는다. */
    CHECK(client.connection->phase == DRAIN_INPUT);
    CHECK(client.connection->context == NULL);
    struct observed_response response = {0};
    collect_response(&client, &response);
    expect_body(&response, "bye", 3);
    close_client(&client);
    fixture_finish();
}

static void test_partial_send_keeps_context_until_complete(void)
{
    fixture_start();
    struct test_client client = open_client();
    int capacity = 1024;
    int status = setsockopt(client.connection->fd, SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity));
    CHECK(status == 0 && context_acquire(client.connection));
    struct request_context *owned = client.connection->context;
    memset(owned->data, 0x42, BODY_LIMIT);
    client.connection->length = BODY_LIMIT;
    client.connection->keep_alive = true;
    respond(client.connection, 200, BODY_LIMIT, "application/octet-stream");
    process_turn(&fixture_server, client.connection);
    CHECK(client.connection->phase == WRITE_RESPONSE && client.connection->sent < BODY_LIMIT);
    CHECK(client.connection->context == owned);
    struct observed_response response = {0};
    /* 실행: 수신 공간을 비워 주면서 부분 송신을 반복한다. */
    for (int turns = 0; turns < 128; turns++) {
        collect_response(&client, &response);
        if (client.connection->phase == READ_HEADER) break;
        CHECK(client.connection->context == owned);
        process_turn(&fixture_server, client.connection);
    }
    size_t end = http_header_end(response.bytes, response.length);
    CHECK(end > 0 && response.length - end == BODY_LIMIT);
    for (size_t i = end; i < response.length; i++) CHECK(response.bytes[i] == 0x42);
    CHECK(client.connection->context == NULL);
    close_client(&client);
    fixture_finish();
}

static void test_exhaustion_closes_only_the_new_request(void)
{
    fixture_start();
    struct test_client holders[CONTEXT_CAPACITY];
    /* 준비: 미완성 헤더로 풀을 모두 점유한다. */
    for (int i = 0; i < CONTEXT_CAPACITY; i++) {
        holders[i] = open_client();
        send_text(&holders[i], "GET /");
        process_turn(&fixture_server, holders[i].connection);
    }
    struct test_client extra = open_client();
    send_text(&extra, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx");
    process_turn(&fixture_server, extra.connection);
    /* 검증: 고갈 실패는 새 연결만 종료하고 기존 데이터는 보존한다. */
    CHECK(extra.connection->context == NULL);
    CHECK(strcmp(extra.connection->reason, "request_context_limit") == 0);
    expect_count("owners after rejected request", used_contexts(&fixture_server.contexts), CONTEXT_CAPACITY);
    for (int i = 0; i < CONTEXT_CAPACITY; i++) {
        CHECK(memcmp(holders[i].connection->context->input, "GET /", 5) == 0);
        close_client(&holders[i]);
    }
    close_client(&extra);
    fixture_finish();
}

int main(void)
{
    test_idle_connections_do_not_borrow();
    test_partial_input_keeps_ownership();
    test_next_request_reacquires_clean_space();
    test_pending_request_delays_release();
    test_timeout_releases_context_and_fds();
    test_close_response_releases_before_drain();
    test_partial_send_keeps_context_until_complete();
    test_exhaustion_closes_only_the_new_request();
    printf("sizeof(connection)=%zu, sizeof(request_context)=%zu, sizeof(server)=%zu\n",
           sizeof(struct connection), sizeof(struct request_context), sizeof(struct server));
    puts("All 8 context lifecycle groups passed.");
    return 0;
}
