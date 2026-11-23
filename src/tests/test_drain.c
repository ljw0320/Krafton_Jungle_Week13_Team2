#include "server.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct fixture {
    struct connection client;
    int peer;
};

static struct server server;

static void fixture_open(struct fixture *fixture)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(nonblocking(pair[0]) == 0 && nonblocking(pair[1]) == 0);

    memset(fixture, 0, sizeof(*fixture));
    fixture->client.fd = pair[0];
    fixture->client.file_fd = -1;
    fixture->client.state = RUNNING;
    fixture->client.progress = now_ms();
    fixture->peer = pair[1];
}

static void fixture_close(struct fixture *fixture)
{
    close(fixture->client.fd);
    close(fixture->peer);
}

/* 송신 종료를 기다리지 않고, Content-Length에 해당하는 응답을 먼저 읽을 수 있는지 봅니다. */
static void check_response(struct fixture *fixture, int status, const char *body)
{
    char response[1024] = {0};
    char status_line[64];
    size_t length = 0;
    ssize_t received;

    while ((received = recv(fixture->peer, response + length,
                            sizeof(response) - 1 - length, 0)) > 0)
        length += (size_t)received;

    assert(received == 0 || (received == -1 && would_block()));
    snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d ", status);
    assert(strncmp(response, status_line, strlen(status_line)) == 0);

    size_t end = http_header_end(response, length);
    assert(end > 0 && length - end == strlen(body));
    assert(memcmp(response + end, body, strlen(body)) == 0);
}

/* 남은 업로드를 폐기하는 동안 정리 기한이 연장되지 않는지 확인합니다. */
static void discard_payload(struct fixture *fixture, size_t bytes)
{
    unsigned char chunk[IO_BUDGET] = {0};
    int64_t deadline = fixture->client.drain_deadline;

    while (bytes) {
        size_t size = bytes < sizeof(chunk) ? bytes : sizeof(chunk);
        assert(send(fixture->peer, chunk, size, 0) == (ssize_t)size);
        process_turn(&server, &fixture->client);

        assert(fixture->client.phase == DRAIN_INPUT);
        assert(fixture->client.reason == NULL);
        assert(fixture->client.drain_deadline == deadline);
        bytes -= size;
    }
}

static void finish_peer(struct fixture *fixture)
{
    assert(shutdown(fixture->peer, SHUT_WR) == 0);
    process_turn(&server, &fixture->client);

    assert(fixture->client.reason);
    assert(strcmp(fixture->client.reason, "response_complete") == 0);
}

/* 실제 요청 파싱부터 413·본문 22바이트·반쪽 종료·남은 본문 폐기까지 검사합니다. */
static void test_oversized(size_t bytes)
{
    struct fixture fixture;
    char header[128];
    fixture_open(&fixture);

    int length = snprintf(header, sizeof(header),
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: %zu\r\n\r\n", bytes);
    assert(send(fixture.peer, header, (size_t)length, 0) == length);
    process_turn(&server, &fixture.client);

    assert(fixture.client.phase == DRAIN_INPUT && !fixture.client.reason);
    assert(!fixture.client.write_shutdown && fixture.client.drain_remaining == bytes);
    check_response(&fixture, 413, "413 Content Too Large\n");
    discard_payload(&fixture, bytes);
    assert(fixture.client.write_shutdown && fixture.client.drain_remaining == 0);
    finish_peer(&fixture);
    fixture_close(&fixture);

    printf("PASS oversized %zu bytes / 413 / drain / peer EOF\n", bytes);
}

static void test_normal_post(void)
{
    struct fixture fixture;
    const char request[] = "POST /echo HTTP/1.0\r\nContent-Length: 5\r\n\r\nhello";
    fixture_open(&fixture);

    assert(send(fixture.peer, request, sizeof(request) - 1, 0) == sizeof(request) - 1);
    process_turn(&server, &fixture.client);

    assert(fixture.client.phase == DRAIN_INPUT && !fixture.client.reason);
    check_response(&fixture, 200, "hello");
    finish_peer(&fixture);
    fixture_close(&fixture);
    puts("PASS normal POST / half-close / peer EOF");
}

/* 브라우저가 413을 받고 업로드를 중단해도 전체 선언 길이를 기다리지는 않습니다. */
static void test_stopped_upload(void)
{
    struct fixture fixture;
    const char request[] = "POST /echo HTTP/1.0\r\nContent-Length: 1048576\r\n\r\n";
    fixture_open(&fixture);

    assert(send(fixture.peer, request, sizeof(request) - 1, 0) == sizeof(request) - 1);
    process_turn(&server, &fixture.client);
    check_response(&fixture, 413, "413 Content Too Large\n");
    assert(!fixture.client.write_shutdown && fixture.client.drain_remaining > 0);

    finish_peer(&fixture);
    assert(fixture.client.write_shutdown);
    fixture_close(&fixture);
    puts("PASS client stops upload after 413 / bounded close");
}

/* 시간을 실제로 기다리지 않고, 기한이 지나면 읽을 데이터가 있어도 종료하는지 검사합니다. */
static void test_expired_deadline(void)
{
    struct fixture fixture;
    fixture_open(&fixture);
    respond_error(&fixture.client, 413);
    process_turn(&server, &fixture.client);
    assert(fixture.client.phase == DRAIN_INPUT && !fixture.client.reason);

    check_response(&fixture, 413, "413 Content Too Large\n");
    discard_payload(&fixture, DRAIN_BUFFER_SIZE);
    fixture.client.drain_deadline = now_ms() - 1;
    assert(send(fixture.peer, "x", 1, 0) == 1);
    process_turn(&server, &fixture.client);

    assert(fixture.client.reason);
    assert(strcmp(fixture.client.reason, "drain_timeout") == 0);
    fixture_close(&fixture);
    puts("PASS fixed drain deadline / no extension / timeout");
}

int main(void)
{
    test_oversized(512 * 1024);
    test_oversized(1024 * 1024);
    test_normal_post();
    test_stopped_upload();
    test_expired_deadline();
    puts("All 5 drain unit groups passed.");
    return 0;
}
