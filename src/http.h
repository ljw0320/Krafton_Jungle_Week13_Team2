#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <sys/types.h>

/* buf 앞에서 완성된 요청 하나의 길이를 반환한다. 덜 왔으면 0, 형식 오류면 -1 */
ssize_t http_request_len(const char *buf, size_t len);

/* 요청 하나로 응답을 만든다. *out은 malloc한 버퍼로 conn이 free한다.
   *keep_alive는 응답 후 연결 유지 여부다. 실패하면 -1 */
int http_handle(const char *req, size_t req_len,
                char **out, size_t *out_len, int *keep_alive);

#endif
