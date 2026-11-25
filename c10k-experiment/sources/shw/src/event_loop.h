#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <stdint.h>

#define MAX_EVENTS 64

enum watch_kind { W_CLIENT, W_BACKEND };
struct watch {
    enum watch_kind kind;
    int fd;
    uint32_t events;  // 0이면 미등록
};

int loop_run(int listen_fd);
int loop_watch(struct watch *w, uint32_t events);  // events는 0이 아니어야 한다
void loop_unwatch(struct watch *w);                // DEL + 이번 배치에서 지우기
void loop_accept_resume(void);                     // fd를 닫은 뒤 부른다. 멈춰 있지 않으면 아무것도 안 한다

#endif
