#ifndef CONN_H
#define CONN_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define RBUF_SIZE 8192

enum conn_state { CONN_READING, CONN_WRITING };

struct conn {
    int    fd;
    enum conn_state state;
    char   rbuf[RBUF_SIZE];   // 받았지만 아직 처리 안 한 바이트
    size_t rlen;
    char  *wbuf;              // http_handle이 만든 응답, 다 보내면 free
    size_t wlen, woff;        // 응답 길이, 보낸 위치
    int    keep_alive;        // 응답 후 READING으로 돌아갈지
    uint32_t events;          // 지금 epoll에 등록된 이벤트 (0이면 미등록)
    time_t last_active;       // recv/send 성공 시각
    struct conn *prev, *next; // 유휴 검사용 리스트
};

// 핸들러는 연결이 살아 있으면 0, conn_close로 닫았으면 -1을 반환한다.
// -1을 받은 쪽은 c를 다시 건드리지 않는다.
struct conn *conn_new(int fd);
void conn_close(struct conn *c);
int conn_on_readable(struct conn *c);
int conn_on_writable(struct conn *c);

#endif
