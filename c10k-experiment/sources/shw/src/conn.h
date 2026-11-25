#ifndef CONN_H
#define CONN_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "event_loop.h"
#include "proxy.h"

#define RBUF_SIZE 8192
#define IDLE_TIMEOUT_SEC 600
#define REQ_TIMEOUT_SEC 10

enum conn_state { CONN_READING, CONN_PROXYING, CONN_WRITING };

struct conn {
    struct watch w;           // 첫 필드. w.fd, w.events가 클라이언트 소켓과 등록 이벤트
    enum conn_state state;
    char   rbuf[RBUF_SIZE];   // 받았지만 아직 처리 안 한 바이트
    size_t rlen;
    char  *wbuf;              // A로 보낼 응답 또는 B로 보낼 요청, 다 보내면 free
    size_t wlen, woff;        // wbuf 길이, 보낸 위치
    int    file_fd;           // 보낼 파일, 없으면 -1
    off_t  file_off, file_len; // sendfile이 file_off를 보낸 만큼 늘린다
    int    keep_alive;        // 응답 후 READING으로 돌아갈지
    int    outq;              // 지난 sweep의 SIOCOUTQ 값 (아직 ACK 안 된 바이트)
    time_t last_active;       // recv/send 성공 시각, 또는 sweep이 outq가 준 것을 본 시각
    time_t req_start;         // rbuf 맨 앞 요청의 첫 바이트 시각
    struct conn *prev, *next; // 유휴 검사용 리스트
    struct upstream up;       // 백엔드 연결
};

// 핸들러는 연결이 살아 있으면 0, conn_close로 닫았으면 -1을 반환한다.
// -1을 받은 쪽은 c를 다시 건드리지 않는다.
struct conn *conn_new(int fd);
void conn_close(struct conn *c);
int conn_on_readable(struct conn *c);
int conn_on_writable(struct conn *c);
int conn_error(struct conn *c, int status);  // B를 닫고 wbuf를 버린 뒤 오류 응답을 보낸다
void conn_sweep_idle(time_t now);

#endif
