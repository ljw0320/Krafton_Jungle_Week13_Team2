#ifndef PROXY_H
#define PROXY_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "event_loop.h"

#define PROXY_BUF_SIZE 16384

struct conn;
struct http_req;
struct route;

enum up_state { UP_CONNECTING, UP_SENDING, UP_RECEIVING };

// 백엔드 연결(B). struct conn 안에 값으로 들어 있다
struct upstream {
    struct watch w;           // 첫 필드. w.fd가 -1이면 B 없음
    struct conn *c;
    enum up_state state;
    time_t since;             // 타이머 시작 시각
    int    minor;             // 보낸 요청의 HTTP/1.x
    int    head_done;         // 응답 헤더 검사를 통과했나 (A로 보내기 시작)
    int    has_len;
    size_t remain;            // has_len이면 B에서 더 받을 본문
};

// -1이면 c가 닫히고 해제됐다. c와 그 안의 up을 다시 건드리지 않는다.
// 0이면 c는 살아 있지만 B는 닫혔을 수도, 다음 요청의 새 B일 수도 있다.

// 0: 시작함, >0: 보낼 오류 코드(B와 wbuf는 정리됨), -1: c를 닫음
int proxy_start(struct conn *c, const struct http_req *req, struct route *r);
int proxy_event(struct upstream *u, uint32_t events);
int proxy_resume(struct conn *c);   // A의 버퍼를 비웠으니 B를 다시 읽는다
void proxy_close(struct conn *c);   // B만 닫는다. B가 없으면 아무것도 안 한다

#endif
