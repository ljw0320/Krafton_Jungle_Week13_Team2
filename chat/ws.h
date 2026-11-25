#ifndef WS_H
#define WS_H

#include <stddef.h>
#include <sys/types.h>

struct ws_frame {
    int fin, op;
    size_t head, len;
    unsigned char mask[4];
    int close;
};

int ws_accept(const char *key, size_t n, char out[29]);
// 입력을 보존한다. 전체 길이, 미완성은 0, 오류는 -1과 f->close를 반환한다.
ssize_t ws_parse(const unsigned char *buf, size_t n, size_t max, struct ws_frame *f);
void ws_unmask(unsigned char *p, size_t n, const unsigned char mask[4]);
size_t ws_head(unsigned char out[10], int fin, int op, size_t len);
int ws_utf8(const unsigned char *p, size_t n);

#endif
