#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <sys/types.h>

#define HTTP_HEAD_MAX 8192   /* 백엔드 응답 헤더 한도 */
#define HTTP_HDR_MAX 100     /* 프록시 헤더 줄 수 한도. Connection 비교가 줄 수의 제곱이라 막는다 */

/* 정적 루트를 realpath로 확인해 저장한다. 실패하면 메시지를 stderr에 찍고 -1 */
int http_init(const char *root);

enum http_method { M_GET, M_POST, M_KNOWN /* 알지만 구현 안 함 */, M_UNKNOWN };

struct http_req {
    int status;                       /* 0: 형식 정상. 아니면 400/408/413/414/431/501/505 */
    enum http_method method;
    const char *target; size_t target_len;   /* buf 안을 가리킨다. memmove 전까지만 유효 */
    int minor;                        /* HTTP/1.x의 x */
    int conn_close, conn_keep_alive;  /* Connection 헤더 토큰 */
    int has_cl;
    size_t content_length, head_len;
    int backend;                      /* 백엔드 경로면 1. 405의 Allow를 고른다 */
};

/* buf 앞의 요청 하나를 파싱한다. len <= cap이어야 한다(cap - 헤더 길이가 언더플로하지 않게).
   >0: 요청 길이(헤더 + 본문), 0: 덜 왔다, -1: 형식 오류(req->status에 코드) */
ssize_t http_parse(const char *buf, size_t len, size_t cap, struct http_req *req);

/* 클라이언트가 연결 유지를 원하나 */
int http_keep_alive(const struct http_req *req);

struct http_resp {
    char  *head;  size_t head_len;    /* malloc. 상태 라인 + 헤더 + 오류 본문. conn이 free */
    int    file_fd;                   /* 보낼 파일, 없으면 -1 */
    off_t  file_len;
    int    keep_alive;
};

/* req로 응답을 만든다. req->status가 0이 아니면 그 오류 응답을 만든다.
   -1이면 응답을 만들지 못한 것이다 */
int http_handle(const struct http_req *req, struct http_resp *resp);

/* buf 안의 req를 백엔드에 보낼 요청(헤더 + 본문)으로 out에 다시 만든다. host는 Host가 없을 때 쓴다.
   0 또는 400/405/411/431/500/501 */
int http_proxy_request(const struct http_req *req, const char *buf, const char *host,
                       char *out, size_t cap, size_t *out_len);

struct http_upresp {
    int    status;                    /* 1xx면 out을 쓰지 않았다 */
    int    has_len;                   /* 응답의 길이를 아는가 */
    size_t body_len;                  /* has_len이면 본문 길이 */
    int    keep_alive;                /* A를 유지하나 */
    size_t out_len;                   /* out에 쓴 헤더 길이 */
};

/* 백엔드 응답 헤더를 검사하고 A로 보낼 헤더를 out에 만든다. minor는 보낸 요청의 버전, keep은 클라이언트의 희망.
   >0: 원본 헤더 길이, 0: 덜 왔다, -1: 502 (len이 HTTP_HEAD_MAX인데 헤더 끝이 없는 것도 포함) */
ssize_t http_proxy_response(const char *buf, size_t len, int minor, int keep,
                            char *out, size_t cap, struct http_upresp *r);

#endif
