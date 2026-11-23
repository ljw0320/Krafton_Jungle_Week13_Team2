#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <sys/types.h>

/* 정적 루트를 realpath로 확인해 저장한다. 실패하면 메시지를 stderr에 찍고 -1 */
int http_init(const char *root);

enum http_method { M_GET, M_POST, M_KNOWN /* 알지만 구현 안 함 */, M_UNKNOWN };

struct http_req {
    int status;                       /* 0: 형식 정상. 아니면 400/413/414/431/501/505 */
    enum http_method method;
    const char *target; size_t target_len;   /* buf 안을 가리킨다. memmove 전까지만 유효 */
    int minor;                        /* HTTP/1.x의 x */
    int conn_close, conn_keep_alive;  /* Connection 헤더 토큰 */
    int has_cl;
    size_t content_length, head_len;
};

/* buf 앞의 요청 하나를 파싱한다.
   >0: 요청 길이(헤더 + 본문), 0: 덜 왔다, -1: 형식 오류(req->status에 코드) */
ssize_t http_parse(const char *buf, size_t len, size_t cap, struct http_req *req);

/* buf 앞에서 완성된 요청 하나의 길이를 반환한다. 덜 왔으면 0, 형식 오류면 -1 */
ssize_t http_request_len(const char *buf, size_t len);

/* 요청 하나로 응답을 만든다. *out은 malloc한 버퍼로 conn이 free한다.
   *keep_alive는 응답 후 연결 유지 여부다. 실패하면 -1 */
int http_handle(const char *req, size_t req_len,
                char **out, size_t *out_len, int *keep_alive);

#endif
