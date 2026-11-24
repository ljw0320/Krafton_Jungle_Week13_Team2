범위: 웹 서버 과제 중 ③ HTTP 요청 파싱, ④ HTTP 응답 생성, ⑤ 정적 파일 서빙을 구현한다. TCP 서버와 `epoll` 이벤트 루프는 [architecture-tcp.md](architecture-tcp.md)의 설계를 그대로 쓰고, `http.c` stub을 실제 구현으로 바꾼다. CGI, 시그널 셧다운은 이번 범위에서 빠진다. Reverse Proxy는 [architecture-proxy.md](architecture-proxy.md)에 있고, 프록시 단계를 구현하면 바뀌는 곳을 **프록시**로 표시했다.

개요: [overview.md](overview.md), 요구사항: [requirements.md](requirements.md)

---
# 결정 사항 (2026-11-22 확정)

| # | 항목 | 결정 | 이유 |
|---|---|---|---|
| 1 | 인터페이스 | `http_parse`가 `struct http_req`를 채우고, `http_handle`이 그 `req`로 응답을 만든다. `req`는 `conn_process`의 지역 변수다 | 파싱을 한 곳에서 한 번만 한다. 길이를 정한 파서와 응답을 만드는 파서가 같아서 경계가 어긋날 수 없다. 오류 코드와 keep-alive를 `http_handle` 한 곳에서 정한다 |
| 2 | 파싱 엄격도 | 엄격. 줄 끝은 `\r\n`만, SP 하나, 이름과 콜론 사이 공백·obs-fold는 400, CL은 숫자만 한 번 | 입력 하나에 결과 하나다. 관대한 파서는 프록시를 붙일 때 smuggling 경로가 된다 |
| 3 | chunked 요청 | `Transfer-Encoding`이 있으면 값과 관계없이 501 + 닫기 | 이번 단계에 본문을 쓰는 곳이 없다. RFC 9112 6.1의 "이해하지 못하는 transfer coding은 501"과 맞는다 |
| 4 | 한도 | 요청 전체(헤더 + 본문)를 `rbuf[8192]` 안에서 판단한다. 본문 버퍼를 따로 두지 않는다 | 본문은 경계만 계산하고 버린다. 한도가 하나라 경계값 테스트가 정확하다 |
| 5 | 메서드 | RFC 9110 8개(GET HEAD POST PUT DELETE CONNECT OPTIONS TRACE)를 안다. 허용은 GET만, `Allow: GET` | 모르는 메서드(501)와 이 자원에 허용 안 함(405)을 구분한다. POST는 처리할 곳이 없어 405다. `CONNECT`도 대상이 `/`로 시작하면 405지만, 규격의 authority-form(`CONNECT host:443`)은 대상이 `/`로 시작하지 않아 400이다. **프록시**(4단계에서 연결): 백엔드 경로는 GET, POST를 전달하고 `Allow: GET, POST` |
| 6 | 검사 순서 | 메시지 단계(`http_parse`)를 모두 끝낸 뒤 의미 단계(`http_handle`). 411은 405보다 앞. 닫기는 상태 코드로만 정한다 | 413을 501/405보다 먼저 봐야 큰 본문이 다음 요청으로 해석되지 않는다. 411을 앞에 둬야 이번 단계에서 411이 나간다 |
| 7 | 응답 헤더 | 모든 응답에 `Date`, `Content-Type`, `Content-Length`, `Connection`. 405에만 `Allow`. 오류 본문은 `<h1>코드 이유</h1>\n` | RFC 9110의 `Date` MUST를 지킨다. 헤더 집합이 항상 같아 테스트가 단순하다 |
| 8 | 정적 루트 | `./server <port> <root>` 필수 인자. `http_init`이 시작할 때 `realpath`를 한 번 구해 `http.c`의 `static` 변수에 둔다 | 요구사항의 "루트를 CLI 인자로"와 같다. epoll fd를 `event_loop.c`의 `static`으로 둔 기존 구조와 같다 |
| 9 | 경로 안전 | 쿼리 제거 → 퍼센트 디코딩 한 번 → 세그먼트 정규화 → `realpath` → 접두사 비교(`real[n]`이 `/` 또는 `\0`) | 정규화로 `..` 탈출을 파일 시스템 접근 전에 거르고, `realpath`로 심볼릭 링크 탈출까지 막는다 |
| 10 | 파일 열기 | `open(O_RDONLY \| O_NONBLOCK)` + `fstat`. 일반 파일만 보낸다. 디렉토리는 `/`로 끝날 때만 `index.html`, 아니면 404. 301은 하지 않는다 | FIFO `open`이 이벤트 루프를 멈추지 않는다. 연 fd에 `fstat`해서 검사 대상과 읽을 대상이 같다 |
| 11 | MIME | 요구사항 5종(`.html .css .js .png .jpg/.jpeg`) + 기본값 `application/octet-stream`. `strcasecmp`, 실제로 연 경로의 마지막 세그먼트에서 확장자를 찾는다 | `GET /`도 `index.html`로 판단된다. `/v1.2/readme`를 `.2/readme`로 보지 않는다 |
| 12 | 큰 파일 전송 | `sendfile`. `struct conn`에 `file_fd`, `file_off`, `file_len`. WRITING은 헤더(`wbuf`) → 본문(`sendfile`) 순서다 | 연결당 메모리가 파일 크기와 무관하다. 사용자 공간 복사가 없다 |
| 13 | 요청 타이머 | `req_start`를 두고 `REQ_TIMEOUT_SEC` = 10초를 넘으면 408 + 닫기 | 유휴 타이머만으로는 조금씩 보내는 연결(Slowloris)을 닫지 못한다 |
| 14 | lingering close | 하지 않는다 | 과제에서 필수가 아니다. 테스트를 수신 버퍼에 바이트가 남지 않는 방식으로 만든다 |
| 15 | 테스트 | `make test` = 파서 단위 테스트(`tests/test_http.c`) + 기능 테스트(`tests/http_test.sh`) | 요구사항 품질 체크리스트의 "단위 테스트: HTTP 파서, MIME"를 채운다. 종료 코드로 판정한다 |

메인 세션이 정한 세부 판단 (틀렸을 때의 비용은 각각 한 함수 안의 수정이다)
- `http.c` 한 파일에 파싱, 응답 생성, 경로 해석, MIME을 둔다. 단위 테스트가 `http.c`를 `#include`하기 때문이다. 파일을 나누면 테스트도 나눠야 한다.
- 요청 대상의 원래 바이트는 `0x21`~`0x7E`만 받고 `#`은 400이다. 인코딩하지 않은 UTF-8, 제어 문자를 거절한다.
- 경로가 `PATH_MAX`를 넘으면 404다. `realpath`의 `ENAMETOOLONG`과 같게 처리한다.
- index.html이 없는 디렉토리는 403이 아니라 404다. 403은 디렉토리가 있다는 정보를 준다.
- 루트의 `realpath`가 `/`이면 시작을 거부한다. `root_len = 1`이면 접두사 비교가 성립하지 않고, 파일 시스템 전체를 서빙할 이유가 없다.
- `conn_process`의 "rbuf가 찼는데 요청 미완성이면 닫기" 줄은 남긴다. `http_parse`는 `len == cap`에서 0을 반환하지 않지만, 파서 버그가 FIN 오판(크기 0 `recv`)으로 이어지지 않게 하는 방어다.

---
# 출력 형식

| 항목 | 형식 |
|---|---|
| 사용법 메시지 | stderr에 `usage: ./server <port> <root> [<prefix>=<ip>:<port> ...]`, `exit(1)`. `argc < 3`이거나 포트 검사(지금과 같음)에 실패하면 |
| 루트 오류 | `realpath: <경로>: <strerror>` / `root: <경로>: Not a directory` / `root: /: not allowed`, 모두 stderr, `exit(1)`. `server_listen`보다 먼저 검사한다 |
| 로그 | [architecture-tcp.md](architecture-tcp.md)의 로그 그대로. 요청 로그는 추가하지 않는다 |
| 상태 라인 | 요청 버전과 관계없이 `HTTP/1.1 <코드> <이유>` |
| 헤더 순서 | `Date` → `Content-Type` → `Content-Length` → `Connection` → (`Allow`) |
| `Date` | `gmtime_r` + `strftime("%a, %d %b %Y %H:%M:%S GMT")`. `setlocale`을 부르지 않으므로 C 로케일 영어다 |
| `Connection` | 항상 `keep-alive` 또는 `close` |
| 200 `Content-Type` | MIME 표 |
| 오류 응답 | `Content-Type: text/html; charset=utf-8`, 본문 `<h1>404 Not Found</h1>\n`. `Content-Length`는 `snprintf` 반환값으로 계산한다 |

```
HTTP/1.1 405 Method Not Allowed\r\n
Date: Sun, 22 Nov 2026 05:00:00 GMT\r\n
Content-Type: text/html; charset=utf-8\r\n
Content-Length: 32\r\n
Connection: keep-alive\r\n
Allow: GET\r\n
\r\n
<h1>405 Method Not Allowed</h1>\n
```

이유 문구 (RFC 9110)

| 코드 | 이유 | 코드 | 이유 |
|---|---|---|---|
| 200 | OK | 411 | Length Required |
| 400 | Bad Request | 413 | Content Too Large |
| 403 | Forbidden | 414 | URI Too Long |
| 404 | Not Found | 431 | Request Header Fields Too Large |
| 405 | Method Not Allowed | 500 | Internal Server Error |
| 408 | Request Timeout | 501 | Not Implemented |
| | | 502 | Bad Gateway (프록시 3단계) |
| | | 504 | Gateway Timeout (프록시 3단계) |
| | | 505 | HTTP Version Not Supported |

MIME

| 확장자 (`strcasecmp`) | `Content-Type` |
|---|---|
| `.html` | `text/html; charset=utf-8` |
| `.css` | `text/css` |
| `.js` | `text/javascript` |
| `.png` | `image/png` |
| `.jpg`, `.jpeg` | `image/jpeg` |
| 그 밖, 확장자 없음 | `application/octet-stream` |

---
# 파일 구조 (바뀌는 것)

```
Makefile              # test 타깃 추가
.gitignore            # tests/test_http 추가
src/
  main.c              # argc 3, http_init(argv[2]) 후 server_listen
  conn.c / .h         # struct conn 필드, conn_respond, conn_send 2단계, 요청 타이머
  http.c / .h         # 파싱, 응답 생성, 경로 해석, MIME (stub 제거)
tests/
  test_http.c         # #include "../src/http.c" 단위 테스트
  http_test.sh        # 서버를 띄워 curl/nc로 확인하는 기능 테스트
```

- `event_loop.c`, `server.c`는 바뀌지 않는다.
- `make test`: `test: server` 의존 → `gcc $(CFLAGS) -o tests/test_http tests/test_http.c && ./tests/test_http && bash tests/http_test.sh`. 단위 테스트는 `src/http.c`를 `#include`하므로 `src/*.c`와 함께 링크하지 않는다. (2단계에서는 `http_test.sh`가 없어 단위 테스트만 돈다.) `make clean`은 `tests/test_http`도 지운다.
- 테스트 출력(`test_http.c`, `http_test.sh` 공통): 실패마다 stderr에 `FAIL <케이스 이름>: 기대 <값>, 실제 <값>`, 끝에 `N passed, M failed`, 실패가 있으면 종료 코드 1.
- 상수: `conn.h`에 `REQ_TIMEOUT_SEC 10`을 추가한다. `RBUF_SIZE`는 `http_parse`의 `cap` 인자로 넘긴다(`http.c`가 `conn.h`를 포함하지 않는다).

---
# HTTP 인터페이스

```C
enum http_method { M_GET, M_POST, M_KNOWN /* 알지만 구현 안 함 */, M_UNKNOWN };

struct http_req {
    int status;                       // 0: 형식 정상. 아니면 400/408/413/414/431/501/505
    enum http_method method;
    const char *target; size_t target_len;   // rbuf 안을 가리킨다. memmove 전까지만 유효
    int minor;                        // HTTP/1.x의 x
    int conn_close, conn_keep_alive;  // Connection 헤더 토큰
    int has_cl;
    size_t content_length, head_len;
    int backend;                      // 백엔드 경로면 1. 405의 Allow를 고른다 (프록시 3단계)
};

struct http_resp {
    char  *head;  size_t head_len;    // malloc. 상태 라인 + 헤더 (+ 오류 응답이면 본문). conn이 free
    int    file_fd;                   // 보낼 파일, 없으면 -1. conn이 close
    off_t  file_len;
    int    keep_alive;
};

/* 시작할 때 한 번. 루트의 realpath를 static 변수에 저장한다. 실패하면 메시지를 찍고 -1 */
int http_init(const char *root);

/* buf 앞의 요청 하나를 파싱한다.
   >0: 요청 길이(헤더 + 본문), 0: 덜 왔다, -1: 형식 오류(req->status에 코드) */
ssize_t http_parse(const char *buf, size_t len, size_t cap, struct http_req *req);

/* req로 응답을 만든다. req->status가 0이 아니면 그 오류 응답을 만든다.
   -1이면 응답을 만들지 못한 것이고, 열었던 file_fd와 head는 이미 정리했다 */
int http_handle(const struct http_req *req, struct http_resp *resp);
```

`conn.c`의 사용 흐름

```C
// conn_process와 conn_sweep_idle(408)이 같이 쓴다. 0: WRITING 준비됨, -1: 닫음
static int conn_respond(struct conn *c, const struct http_req *req) {
    struct http_resp r;
    if (http_handle(req, &r) < 0) { conn_close(c); return -1; }   // 응답을 만들지 못함
    c->wbuf = r.head; c->wlen = r.head_len; c->woff = 0;
    c->file_fd = r.file_fd; c->file_len = r.file_len; c->file_off = 0;
    c->keep_alive = r.keep_alive;
    c->outq = 0;                  // 앞 응답의 값이 남지 않게
    c->state = CONN_WRITING;
    return 0;
}

// conn_process 루프 한 번
struct http_req req;
ssize_t n = http_parse(c->rbuf, c->rlen, RBUF_SIZE, &req);
if (n == 0) break;
if (conn_respond(c, &req) < 0) return -1;
// req가 rbuf를 가리키므로 응답을 만든 뒤에 지운다. conn_send가 c를 닫을 수 있으므로 그 전에 지운다
if (n > 0) { c->rlen -= (size_t)n; memmove(c->rbuf, c->rbuf + n, c->rlen); }
else c->rlen = 0;        // 형식 오류면 닫는 응답이므로 남은 바이트는 쓰지 않는다
int r = conn_send(c);
if (r <= 0) return r;
```

- 순서는 `http_parse` → `http_handle` → `memmove` → `conn_send`다.
- 기존 `http_request_len`, `/big` stub, `hello\n` 응답은 지운다.
- 프록시 3단계에서 `http_keep_alive`, `http_proxy_request`, `http_proxy_response`([architecture-proxy.md](architecture-proxy.md) "함수")와 `http_req.backend`를 더했다. `http_parse`의 헤더 끝 찾기(`head_end`), 헤더 한 줄 파싱(`hdr_next`), CL 파싱(`parse_cl`)을 `static` 함수로 꺼내 응답 파서와 같이 쓴다(동작은 그대로). 501/411/405 판단은 `method_check`로 꺼내 백엔드 경로와 같이 쓴다.
- **프록시**: `conn_process`는 `http_handle` 전에 라우팅한다. 408은 `conn_error`를 쓴다.

---
# `struct conn` 변경

```C
struct conn {
    ...                       // 기존 필드 그대로
    int    file_fd;           // 보낼 파일, 없으면 -1 (conn_new에서 -1로 둔다)
    off_t  file_off, file_len;// sendfile이 file_off를 보낸 만큼 늘린다
    time_t req_start;         // rbuf 맨 앞 요청의 첫 바이트 시각
    int    outq;              // 지난 sweep의 SIOCOUTQ(WRITING). 줄었으면 클라이언트가 읽는 중이다
};
```

- `calloc`은 `file_fd`를 0(stdin)으로 채운다. `conn_new`에서 `-1`로 두지 않으면 파일 없는 응답이 끝날 때마다 `close(0)`이 실행된다.
- `req_start` 기록 시점

| 언제 | 값 |
|---|---|
| `recv` 직전 `rlen == 0`이었고 바이트가 들어옴 | `now` |
| 응답을 다 보내고 READING으로 돌아왔는데 `rlen > 0` | `now` |
| 요청 중에 바이트가 더 들어옴 | 그대로 |

- 남은 바이트의 시각을 `memmove` 때 기록하면, 큰 파일을 30초 보낸 뒤 READING으로 돌아오는 순간 다음 요청이 바로 408이 된다. 그래서 READING으로 돌아올 때 기록한다.

## 연결 상태 전이

```
accept4 ──▶ READING (EPOLLIN 감시)
              │ recv → rbuf에 이어 붙임 → http_parse
              │   0  → 대기
              │   >0 → http_handle (정상 / 405 / 404 …)
              │   -1 → http_handle (req.status의 오류 응답, keep_alive = 0)
              ▼
            WRITING (EPOLLIN 끔)
              │ ① woff < wlen      → send(wbuf) 한 번
              │ ② file_off < len   → sendfile(file_fd) 한 번
              │ 남음(EAGAIN 또는 일부) → EPOLLOUT 감시, 다음 쓰기 이벤트 대기
              │ sendfile 0 반환(파일이 줄어듦) → conn_close
              │ 다 보냄 → close(file_fd), file_fd = -1, free(wbuf)
              ▼
            keep_alive? ── 예 ──▶ READING (EPOLLIN 감시로 되돌림)
              │                    rlen > 0이면 req_start = now, 바로 다시 처리
              └─ 아니오 ──▶ conn_close

sweep (1초마다)
  READING && rlen > 0 && now - req_start > REQ_TIMEOUT_SEC → req.status = 408 → conn_respond
  WRITING이면 ioctl(SIOCOUTQ)로 outq를 읽어 지난 sweep보다 줄었으면 last_active = now (outq 갱신)
  그 밖에 now - last_active > IDLE_TIMEOUT_SEC            → 응답 없이 conn_close

모든 상태 ──▶ conn_close
  recv 0, ECONNRESET/EPIPE 등 오류, EPOLLHUP/EPOLLERR,
  유휴 타임아웃, http_handle 실패(응답 버퍼 malloc 실패 등)
```

- WRITING 동안 `EPOLLIN`을 끄는 규칙, 핸들러 반환 규칙(0 / -1), `SIGPIPE` 무시는 그대로다. `enum conn_state`도 늘지 않는다(**프록시**: `CONN_PROXYING`이 추가된다). 헤더 단계와 본문 단계는 오프셋으로 구분한다.
- 이벤트 하나에 `send`/`sendfile`을 한 번만 한다. 큰 파일 하나가 루프를 독차지하지 않는다.
- 유휴 활동 정의 변경(Task 9, 사용자 승인): `last_active`는 `recv`/`send` 성공에 더해, WRITING 연결의 `SIOCOUTQ`(클라이언트가 아직 확인하지 않은 송신 바이트 수)가 지난 sweep보다 줄어들 때도 갱신한다. 이유: 송신 버퍼가 약 1/3 빌 때까지 `EPOLLOUT`이 오지 않아, 계속 읽는 느린 클라이언트(8MB를 50KB/s로)가 유휴 타이머에 잘렸다. 결과: 50KB/s는 완료, 10KB/s는 100초 동안 유지, 4KB/s는 수신 창 갱신이 10초보다 드물어 여전히 닫힌다. 완전히 멈춘 연결은 응답 직후 `SIOCOUTQ`가 한 번 줄 수 있어 10~13초에 닫힌다.
- 변경(Task 9, 사용자 결정): `conn_new`에서 `TCP_NODELAY`를 켠다. 실패하면 느려질 뿐이라 `perror`만 하고 계속한다. 이유: 헤더와 본문이 따로 나가면 Nagle + 지연 ACK로 keep-alive의 두 번째 파일 응답부터 약 44ms 늦었다. 결과: 헤더와 본문 사이, 마지막 조각의 Nagle 대기가 없어진다. 헤더 `send`의 `MSG_MORE`만으로는 150KB 이상 파일에서 약 44ms가 드물게 남았다.

## `conn_close` 순서

1. `loop_unwatch(c)` (지금과 같음)
2. `close(c->fd)`
3. `free(c->wbuf)`
4. **`if (c->file_fd >= 0) close(c->file_fd)`** (추가)
5. 리스트에서 제거
6. `free(c)`

- 프록시 1단계에서 2 다음에 `loop_accept_resume()`이 들어갔다. **프록시**: 맨 앞에 `proxy_close(c)`가 추가된다.
- TCP 설계의 순서를 바꾸지 않고 한 줄을 더한다. `file_fd`는 epoll에 등록하지 않으므로 `loop_unwatch`보다 앞일 필요가 없다.

---
# 검사 순서

앞에서 걸리면 뒤는 보지 않는다. 응답은 하나만 보낸다.

| 순서 | 단계 | 검사 | 코드 |
|---|---|---|---|
| 1 | parse: 헤더 끝을 찾으며 바이트를 훑음 | `\n` 앞이 `\r`이 아님, `\r` 뒤가 `\n`이 아님, `\0` | 400 |
| 2 | parse: 헤더 끝 없음 + `len == cap` | 첫 `\r\n` 없음 / 있음 | 414 / 431 |
| | parse: 헤더 끝 없음 + `len < cap` | | 0 반환 (대기) |
| 3 | parse: 요청 라인 | `메서드 SP 대상 SP HTTP/d.d` 형식, 메서드 tchar, 대상이 `/`로 시작, 앞에 빈 줄 없음 | 400 |
| 4 | | 버전이 1.0/1.1이 아님 (`HTTP/2.0`, `HTTP/1.2`) | 505 |
| 5 | parse: 헤더 | 이름 tchar, 콜론 앞 공백, obs-fold, 콜론 없음, Host 개수, CL 형식·중복, TE + CL | 400 |
| 6 | | TE 있음 | 501 |
| 7 | | `content_length > cap - head_len` | 413 (본문을 기다리지 않음) |
| | | 본문이 덜 옴 | 0 반환 (대기) |
| 8 | handle: 메서드 | `M_UNKNOWN` | 501 |
| 9 | | POST인데 CL 없음 | 411 |
| 10 | | GET이 아님 | 405 + `Allow: GET` |
| 11 | handle: 경로 | 대상 바이트가 `0x21`~`0x7E` 밖이거나 `#`, 퍼센트 인코딩 오류, `%00` | 400 |
| 12 | | 정규화 중 루트 위로 `..`, 접두사 불일치, `EACCES`/`ELOOP`, 일반 파일 아님 | 403 |
| 13 | | `ENOENT`/`ENOTDIR`/`ENAMETOOLONG`, `/`로 끝나지 않는 디렉토리, index.html 없음 | 404 |
| 어디서든 | handle | 경로 버퍼 `malloc` 실패, `open`·`realpath`의 그 밖 오류, `fstat` 실패 | 500 |
| 어디서든 | handle | 응답 버퍼 `malloc` 실패, 응답 헤더 `snprintf` 잘림 | 응답 없이 닫음 |
| 어디서든 | sweep | 요청이 `REQ_TIMEOUT_SEC` 안에 완성되지 않음 | 408 |

- **프록시**: 7과 8 사이에서 라우팅한다. 백엔드 경로는 8~13 대신 [architecture-proxy.md](architecture-proxy.md) "요청 재작성"의 검사를 한다.
- 원칙은 개념 노트와 같다: 메시지를 믿을 수 있는지 먼저(1~7), 의미는 나중(8~13).
- 노트의 순서와 다른 점
	- 413이 501/405보다 앞이다. `FOO /` + `CL: 1000000`에 501 + 유지로 답하면 읽지 않은 본문이 다음 요청이 된다.
	- 411이 405보다 앞이다. 그래야 POST에 CL이 없을 때 411이 나간다.
	- 505가 헤더 검사(5)보다 앞이다. Host 규칙이 버전에 따라 다르기 때문이다.
- 5번의 Host 규칙: 1.1은 정확히 1개, 1.0은 0~1개. 값은 검사하지 않는다(가상 호스트 없음).
- CL 파싱은 개념 노트의 숫자 세기 루프 그대로다(`strtoul`은 `-1`을 큰 양수로, `strtol`은 `+`와 앞 공백을 받는다). 값 앞뒤의 SP/HTAB은 헤더 값 규칙으로 먼저 잘린다.
- `cap - head_len`으로 비교한다. `head_len + content_length`는 CL이 `SIZE_MAX` 근처일 때 오버플로가 난다.

## 연결 유지 판단

```
1. 닫는 코드인가? 400, 408, 411, 413, 414, 431, 500, 501, 502, 504, 505 → 닫기
2. 아니면 (200, 403, 404, 405) 클라이언트 요청대로
     HTTP/1.1: Connection 토큰에 close가 있으면 닫기, 아니면 유지
     HTTP/1.0: Connection 토큰에 keep-alive가 있으면 유지, 아니면 닫기
```

- 노트의 닫기 목록(400, 408, 413, 414, 431, 500)에 셋을 더했다.
	- 411: CL 없는 POST는 본문을 실제로 보내고 있을 가능성이 크다.
	- 501: TE 때문이면 경계를 모른다. 모르는 메서드 때문이면 유지할 수도 있지만, 표를 하나로 두려고 둘 다 닫는다.
	- 505: 1.x가 아닌 버전은 메시지 형식을 믿을 수 없다.
- `Connection` 값은 쉼표로 나눈 토큰을 앞뒤 공백을 자르고 `strncasecmp` + 길이로 비교한다. 여러 줄이면 모두 본다.

---
# 경로 처리

| 순서 | 동작 | 실패하면 |
|---|---|---|
| 1 | 대상 바이트가 `0x21`~`0x7E`이고 `#`이 아님 | 400 |
| 2 | 첫 `?`에서 자른다 | |
| 3 | 퍼센트 디코딩을 한 번, 별도 버퍼에 한다. 디코딩 결과가 `/`로 끝나는지 기록한다 | `%` 뒤에 16진수 2자리가 없으면 400, `%00`이면 400 |
| 4 | 디코딩 결과를 `/`로 나눠 정규화한다. 빈 칸과 `.`은 무시, `..`이면 하나 꺼낸다 | 비어 있는데 `..` → 403 |
| 5 | `root_real + "/" + 정규화 결과` | 경로를 만드는 `snprintf`가 잘리거나(`PATH_MAX` 초과) 음수를 반환하면 → 404 (응답 헤더의 `snprintf` 잘림은 응답 없이 닫음) |
| 6 | `realpath(full, real)` | `ENOENT`, `ENOTDIR`, `ENAMETOOLONG` → 404 / `EACCES`, `ELOOP` → 403 / 그 밖 → 500 |
| 7 | `strncmp(real, root_real, n) == 0 && (real[n] == '/' \|\| real[n] == '\0')` | 403 |
| 8 | `open(real, O_RDONLY \| O_NONBLOCK)` | `ENOENT` → 404, `EACCES`, `ENXIO`(유닉스 소켓. 10번의 소켓 → 403과 맞춘다) → 403, 그 밖 → 500 |
| 9 | `fstat(fd)` | 500 |
| 10 | `S_ISREG` → 200, `Content-Length = st_size` | |
| | `S_ISDIR` + 3번 기록이 `/`로 끝남 → `real + "/index.html"`로 6~9를 **한 번 더**. 또 디렉토리면 403 | index.html 없음 → 404 |
| | `S_ISDIR` + `/`로 끝나지 않음 | 404 |
| | 그 밖 (FIFO, 장치, 소켓) | 403 |
| 11 | MIME: 최종 `real`의 마지막 `/` 뒤에서 `strrchr(base, '.')` | 없으면 기본값 |

- 검사는 디코딩한 **뒤에** 하고, 디코딩은 한 번만 한다. 검사한 문자열을 그대로 쓴다. `%2f`는 디코딩되면 구분자로 쓰이고, 4번에서 함께 정규화된다.
- index.html도 루트 밖 심볼릭 링크일 수 있어서 6~7번을 다시 거친다.
- 오류 응답을 정했으면 그 전에 연 fd를 `close`한다. 정상일 때 fd 소유권은 `struct http_resp`를 거쳐 `conn`으로 넘어간다.
- 0바이트 파일은 `Content-Length: 0`이고 `sendfile`을 부르지 않는다.

---
# 엣지 케이스

| 상황 | 어디서 | 처리 |
|---|---|---|
| 요청이 여러 `recv`로 나뉨 | `http_parse` | 헤더 끝과 본문이 다 올 때까지 0 |
| bare LF (`GET / HTTP/1.1\nHost: x\n\n`) | `http_parse` | 헤더 끝을 찾기 전이라도 400 + 닫기. 이 검사가 없으면 `\r\n\r\n`을 영원히 못 찾아 408/431까지 기다린다 |
| bare CR, 헤더 안의 `\0` | `http_parse` | 400 |
| `GET  / HTTP/1.1` (SP 2개), 버전 없음, `HTTX/1.1` | `http_parse` | 400 |
| `HTTP/2.0`, `HTTP/1.2` | `http_parse` | 505 + 닫기 |
| 대상이 `http://x/`, `*` | `http_parse` | 400 |
| 요청 라인 앞의 빈 줄 | `http_parse` | 400 |
| `Host : x`, 공백으로 시작하는 줄, 콜론 없는 줄 | `http_parse` | 400 |
| 1.1에 Host 없음 / 2개 | `http_parse` | 400 |
| 1.0에 Host 없음 | `http_parse` | 정상 |
| CL `+5`, `-1`, `0x5`, `5 5`, 빈 값, 20자리 수 | `http_parse` | 400 |
| CL이 두 번 (값이 같아도) | `http_parse` | 400 |
| TE + CL | `http_parse` | 400 + 닫기 |
| TE만 (`chunked`, `gzip, chunked`) | `http_parse` | 501 + 닫기 |
| 헤더 + 본문 = 8192바이트 | `http_parse` | 정상 처리 |
| 헤더 + 본문 = 8193바이트 | `http_parse` | 413 + 닫기 |
| `CL: 1000000`, 본문 미전송 | `http_parse` | 헤더만 보고 바로 413 |
| CL이 `SIZE_MAX` 근처 | `http_parse` | 숫자 루프가 오버플로로 400. 통과한 값도 `cap - head_len`과 비교 |
| 8192바이트 안에 첫 `\r\n` 없음 | `http_parse` | 414 + 닫기 |
| 첫 `\r\n`은 있는데 헤더 끝 없음 | `http_parse` | 431 + 닫기 |
| `BREW /`, `get /` | `http_handle` | 501 + 닫기 |
| `DELETE /`, `HEAD /`, `OPTIONS /` | `http_handle` | 405 + `Allow: GET`, 유지 |
| POST, CL 없음 | `http_handle` | 411 + 닫기 |
| `POST /index.html` + `CL: 5` + `hello` | `http_handle` | 405, 유지. 본문은 요청 길이에 포함되어 함께 지워지고, 다음 요청이 정상 처리된다 |
| `DELETE /../../etc/passwd` | `http_handle` | 405. 경로는 보지 않는다 |
| 한 번의 recv에 GET 두 개 | `conn_process` | 응답 두 개를 순서대로 |
| 1.1 + `Connection: close` / 1.0 + 헤더 없음 | `http_handle` | 응답 후 닫기, `Connection: close` |
| 1.0 + `Connection: Keep-Alive, Upgrade` | `http_handle` | 유지, `Connection: keep-alive` |
| `/a.png?v=3` | 경로 | `a.png` |
| `/my%20file.txt` | 경로 | `my file.txt` |
| `%zz`, 끝의 `%`, `%0`, `%00` | 경로 | 400 |
| 인코딩하지 않은 `0x80` 이상 바이트, `#` | 경로 | 400 |
| `/../etc/passwd`, `/%2e%2e/%2e%2e/etc/passwd`, `/img/../../x` | 경로 | 403 |
| `/%252e%252e/etc/passwd` | 경로 | 한 번만 디코딩되어 `%2e%2e`라는 이름의 디렉토리가 된다 → 404 |
| `/img/../index.html`, `/notes..txt`, `//index.html`, `/./index.html` | 경로 | 200 |
| `/index.html/` (일반 파일 + 끝 `/`) | 경로 | 200 |
| `/docs/.` | 경로 | 404 (`.`이 무시되어 끝 `/`가 아니므로 디렉토리 + `/` 없음) |
| 권한 111 디렉토리의 `/dir/` | 경로 | 403 (디렉토리를 `O_RDONLY`로 여는 것이 `EACCES`다. `/dir/index.html`은 200) |
| 루트 밖을 가리키는 심볼릭 링크, 형제 디렉토리(`/srv/www2`)로 가는 링크 | 경로 | 403 |
| 루트 안을 가리키는 심볼릭 링크 | 경로 | 200 |
| FIFO | 경로 | `O_NONBLOCK`이라 `open`이 바로 돌아오고 403. 다른 연결이 멈추지 않는다 |
| `chmod 000` 파일, `x` 권한 없는 디렉토리 아래 파일 | 경로 | 403 |
| 없는 파일 | 경로 | 404, 유지 |
| `/docs/` / `/docs` | 경로 | `docs/index.html` / 404 |
| index.html 없는 디렉토리 / index.html이 디렉토리 / index.html이 바깥 링크 | 경로 | 404 / 403 / 403 |
| `realpath`와 `open` 사이에 파일이 지워짐 | 경로 | `open`의 `ENOENT` → 404 |
| 0바이트 파일 | 경로 | 200, `Content-Length: 0` |
| `.PNG` / 확장자 없음 / `/v1.2/readme` | MIME | `image/png` / 기본값 / 기본값 |
| 8MB 파일, 느린 클라이언트 | `conn_send` | `sendfile`이 일부씩 보내고 `EPOLLOUT` on → off. 받은 바이트 수가 파일 크기와 같다 |
| 전송 중 클라이언트가 끊음 | `conn_send` | `EPIPE`/`ECONNRESET` → `conn_close`가 `file_fd`도 닫는다 |
| 전송 중 파일이 줄어듦 | `conn_send` | `sendfile`이 0 반환 → `conn_close` (클라이언트는 잘린 응답을 안다) |
| 파일 없는 응답 | `conn_new`, `conn_close` | `file_fd = -1`이라 `close(0)`을 하지 않는다 |
| 파일을 연 뒤 응답 생성 실패 | `http_handle` | 열린 fd를 닫고 -1 |
| 응답 버퍼 `malloc` 실패 | `http_handle`, `conn_respond` | 응답 없이 `conn_close` |
| 1바이트씩 4초 간격으로 보냄 (Slowloris) | sweep | 요청 시작 후 10~11초에 408 + 닫기 |
| 아무것도 안 보내는 연결, 응답 후 다음 요청 없는 keep-alive | sweep | 유휴 10~11초에 응답 없이 닫기 (지금과 같음) |
| 느리게 계속 읽는 클라이언트 (8MB를 50KB/s로) | sweep | `SIOCOUTQ`가 줄어들면 `last_active = now`라 끝까지 전송 |
| 응답 중 완전히 멈춘 클라이언트 | sweep | `SIOCOUTQ`가 그대로라 닫기. 응답 직후 한 번 줄 수 있어 10~13초 |
| 큰 파일을 오래 보낸 뒤 파이프라인으로 남은 요청 | `conn_send` | READING으로 돌아올 때 `req_start = now`라 408이 아니다 |
| sweep에서 408을 보내다가 연결이 닫힘 | sweep | `next`를 먼저 저장하므로 순회가 안전하다 |
| 닫는 응답 뒤 수신 버퍼에 남은 본문 | `conn_close` | RST가 나갈 수 있다(알려진 한계). 테스트는 헤더만 보낸다 |

---
# 구현 순서

모든 명령은 WSL의 `/mnt/d/kraftonjungle/kraftonjungle_week13/Krafton_Jungle_Week13_Team2`에서 실행한다. 각 단계는 확인(종료 코드 0)을 통과한 뒤 커밋한다. 기능 테스트 스크립트는 3단계에서 만들고 단계마다 케이스를 더한다.

### 1단계: 인자와 `http_init`
- `main`: `argc == 3`, 포트 검사, `http_init(argv[2])`가 -1이면 `exit(1)`. 그다음 `server_listen`.
- `http_init`: `realpath` → `stat`이 `S_ISDIR` → 결과가 `/`이면 거부. `root_real`, `root_len`을 `static`에 저장한다.
- HTTP 처리는 아직 stub 그대로다.
- 확인
	- `make`가 경고 0개다.
	- `./server 8080`, `./server 8080 /nonexistent`, `./server 8080 Makefile`, `./server 8080 /`가 모두 출력 형식 표의 메시지를 찍고 `echo $?`가 1이다.
	- `mkdir -p /tmp/www && ./server 8080 /tmp/www`가 뜨고 `curl localhost:8080/`이 stub 응답을 받는다.

### 2단계: `http_parse` + 단위 테스트
- `struct http_req`, `enum http_method`, `http_parse`. 검사 순서 1~7번과 파싱 규칙.
- `tests/test_http.c`: 입력 바이트 → (반환값, `status`, 필드)를 표로 두고 실패 개수를 센다. `return fails ? 1 : 0`.
- Makefile `test` 타깃(이 단계는 단위 테스트만), `.gitignore`에 `tests/test_http`.
- 연결은 아직 stub `http_request_len`을 쓴다.
- 확인
	- `make test`의 종료 코드가 0이다. 엣지 케이스 표의 `http_parse` 행이 모두 케이스로 들어 있다.
	- 일부러 한 케이스의 기대값을 틀리게 바꾸면 `make test`가 0이 아닌 값으로 끝나는 것을 한 번 확인하고 되돌린다.
	- `make test`를 `-fsanitize=address,undefined`로 빌드해 돌려도 오류가 0개다.

### 3단계: 응답 생성과 연결 교체
- `struct http_resp`, 새 `http_handle`, 응답 생성(헤더 순서, `Date`, 오류 본문), 연결 유지 판단, 검사 순서 8~10번.
- 이 단계의 GET은 경로를 보지 않고 모두 404다(정적 파일은 4단계).
- `conn.c`: `conn_respond`, `conn_process`가 `http_parse`를 쓴다. `http_request_len`과 `/big` stub을 지운다. `file_fd`는 이 단계에서 항상 -1이다.
- `tests/http_test.sh`: `mktemp -d`로 루트를 만들고, 한 번의 실행 안에서 서버를 띄움 → 케이스 → `kill $SP`. 실패가 하나라도 있으면 `exit 1`. `make test`가 이 스크립트도 실행한다.
- 확인 (`make test`의 케이스로 넣는다)
	- `printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n' | nc -N 127.0.0.1 8080`: `HTTP/1.1 404 Not Found`, 헤더 순서, `Date`가 `^Date: [A-Z][a-z]{2}, [0-9]{2} [A-Z][a-z]{2} [0-9]{4} [0-9:]{8} GMT$`에 맞음
	- 400(`Host : x`), 505(`HTTP/2.0`), 501(`BREW`), 501(TE), 411(POST, CL 없음): 응답 뒤 서버가 닫아서 `nc -N`이 바로 끝난다. `Connection: close`
	- 413: 헤더만 `Content-Length: 1000000`로 보내면 413
	- 414: `GET /aaa…`를 `\r\n` 없이 **정확히 8192바이트** / 431: 짧은 요청 라인 + 헤더 줄들을 빈 줄 없이 정확히 8192바이트. 더 보내면 서버가 읽지 않은 바이트 때문에 RST가 나가 응답을 못 볼 수 있다(lingering close 없음)
	- 유지되는 응답을 확인할 때는 마지막 요청에 `Connection: close`를 붙인다. 그렇지 않으면 `nc -N`이 유휴 타임아웃(10초)까지 기다린다.
	- 405 + `Allow: GET`, 같은 연결의 다음 요청에도 응답한다(요청 두 개를 이어 보내 응답 두 개)
	- `POST / ... Content-Length: 5\r\n\r\nhelloGET / ...`: 405와 404 두 응답
	- 1.0 + 헤더 없음 → `Connection: close`, 1.0 + `Keep-Alive` → `keep-alive`
	- `curl -v localhost:8080/ localhost:8080/`에서 `Re-using existing connection`이 보인다.

### 4단계: 정적 파일
- 경로 처리 표 1~11번, MIME, `struct conn`의 `file_fd`/`file_off`/`file_len`, `conn_send`의 두 단계, `conn_close`의 `close(file_fd)`.
- 단위 테스트에 디코딩, 정규화, MIME 케이스를 더한다(`static` 함수를 `#include`로 바로 부른다).
- 기능 테스트 픽스처는 스크립트가 임시 루트에 만든다: `index.html`, `style.css`, `app.js`, `img.png`, `photo.PNG`, `notes..txt`, `my file.txt`, `docs/index.html`, `empty/`, `out` → 루트 밖 디렉토리 링크, `in` → 루트 안 링크, `pipe`(FIFO), `secret`(`chmod 000`), `big.bin`(8388608바이트), `zero`(0바이트). Windows 체크아웃에서 심볼릭 링크와 FIFO가 깨지므로 저장소에 넣지 않는다.
- 확인 (`make test`의 케이스로 넣는다)
	- `curl -s -o /dev/null -w '%{http_code} %{content_type}'`로 각 파일의 200과 MIME, `cmp`로 본문이 원본과 같다.
	- 엣지 케이스 표의 경로 행(403/404/400) 전부. 경로는 `curl --path-as-is`로 보낸다(curl이 `..`을 미리 정리하지 않게).
	- FIFO 요청 중에도 다른 `curl`이 바로 응답받는다.
	- `curl -s --limit-rate 200k localhost:8080/big.bin -o /dev/null &` 중에 `curl localhost:8080/`이 바로 응답받는다. 로그에 `EPOLLOUT on` → `off`, `curl -s localhost:8080/big.bin | wc -c`가 8388608이다.
	- `curl --max-time 0.1 localhost:8080/big.bin`으로 끊은 뒤 `ls /proc/$(pidof server)/fd | wc -l`이 처음 값으로 돌아온다(`file_fd` 누수 없음).

### 5단계: 요청 타이머와 408
- `req_start` 기록 규칙, sweep의 408 분기, `REQ_TIMEOUT_SEC 10`.
- 확인
	- `(printf 'G'; sleep 4; printf 'E'; sleep 4; printf 'T'; sleep 4) | nc 127.0.0.1 8080`이 요청 시작 후 10~11초에 `408 Request Timeout`을 받고 끝난다.
	- `nc 127.0.0.1 8080`에 아무것도 보내지 않으면 10~11초 후 응답 없이 끝난다.
	- `python3`로 `GET /big.bin`과 `GET /index.html`의 앞부분을 한 번에 보내고, 15초에 걸쳐 천천히 읽은 뒤 두 번째 요청의 나머지를 나눠 보내면 두 번째 응답이 200이다(408이 아님). 완전한 두 번째 요청은 `req_start = now`를 지워도 통과하므로 이 결함을 잡지 못한다.

### 6단계: 종합 확인
- `make test` 종료 코드 0.
- `make asan` 빌드로 `tests/http_test.sh`를 다시 돌려 ASan/UBSan 오류가 0개다.
- `valgrind --leak-check=full ./server 8080 <루트>`로 기능 테스트를 돌린 뒤 `kill $SP`(lessons.md)로 끝내면 `definitely lost: 0`이다.
- 동시 접속: `seq 200 | xargs -P 100 -I{} curl -s -o /dev/null -w '%{http_code}\n' localhost:8080/ | sort | uniq -c` → `200 200`
- 브라우저로 `index.html`을 열면 CSS, JS, PNG가 모두 로드된다(개발자 도구의 응답 코드와 `Content-Type`).
- fd 수와 `conn_count`가 처음 값으로 돌아온다.

---
# 알려진 한계 (나중 작업)

- `realpath`와 `open` 사이에 루트 안의 링크를 바꿔치기하면 막지 못한다. 루트에 쓰기 권한이 있는 공격자만 가능하다. 막으려면 `openat2(RESOLVE_BENEATH)`로 바꾼다(경로 확인 함수 하나).
- chunked 요청을 받지 않는다. RFC 9112의 MUST를 지키지 않는다. 프록시 단계에서도 501 그대로다. 필요해지면 `http_parse`에 디코더를 넣는다.
- 요청 본문은 `8192 - head_len`바이트까지다. 더 큰 본문이 필요하면 본문 버퍼를 따로 둔다(`http_parse` 반환값과 `struct conn`이 바뀐다).
- `Expect: 100-continue`를 처리하지 않는다. curl은 본문이 크면 이 헤더를 붙이고 최대 1초 기다린다. 테스트에서는 `-H 'Expect:'`를 쓴다.
- lingering close가 없다. 닫는 응답 뒤 수신 버퍼에 바이트가 남으면 RST가 나가 클라이언트가 응답 대신 reset을 볼 수 있다. 자주 보이면 `CONN_LINGERING` 상태를 더한다.
- URI가 길고 헤더도 붙어서 8192를 넘으면 414가 아니라 431이 나간다. 둘 다 닫는 응답이라 경계 문제는 없다.
- 시작할 때 구한 루트 `realpath`는 실행 중 루트가 바뀌거나 지워져도 갱신되지 않는다.
- 일반 파일에는 `O_NONBLOCK`이 효과가 없다. 페이지 캐시에 없는 파일이면 `sendfile`이 디스크 읽기만큼 루프를 멈춘다.
- 느리게 읽는 클라이언트(slow read)는 `SIOCOUTQ`가 줄어드는 동안 유휴로 닫지 않는다. 그래서 극단적으로 천천히 읽어도(10KB/s까지 확인) 연결 하나를 오래 붙잡을 수 있다. 반대로 4KB/s처럼 수신 창 갱신이 10초보다 드물면 `SIOCOUTQ`가 줄지 않아 정상 클라이언트도 닫힌다.
- HEAD를 지원하지 않는다(RFC 9110은 범용 서버에 MUST). 405로 답한다.
- 모르는 메서드의 501도 연결을 닫는다. 클라이언트가 다시 연결하는 비용이 든다.
- 헤더와 본문이 별도 시스템 콜로 나가 패킷이 하나 더 생길 수 있다.
- fd 한도에 닿아 accept가 멈춘 뒤 `conn_send`가 파일을 다 보내고 `close(file_fd)`로 fd를 비워도 accept는 재개되지 않는다. 재개는 `conn_close`의 `loop_accept_resume`에만 있다. 열린 연결이 모두 계속 요청을 보낼 때만 오래가고, 유휴 연결이 하나라도 있으면 10~11초 안에 풀린다. 막으려면 `conn_send`의 `close(file_fd)` 뒤에도 `loop_accept_resume()`을 부른다.

## 넣지 않은 것 (제안)

요구사항에 없어서 넣지 않았다. 필요해지면 따로 정한다.
- HEAD, 디렉토리의 301 리다이렉트, 디렉토리 리스팅, Range, 캐시 헤더(`Last-Modified`, `ETag`)
- MIME 추가(`.txt`, `.svg`, `.ico`, `.json`, `.gif`)
- 요청마다 stderr에 `fd=N GET /a.html 404` 로그
- URI 길이 한도를 따로 두기(414/431 구분을 정확히)
- `TCP_CORK` / `MSG_MORE`, `openat2`, lingering close
- `O_CLOEXEC` (CGI로 `fork` + `exec`를 붙이면 열린 fd가 자식에 새지 않게)

## 구현하면서 함께 고칠 문서

지금 코드와 맞지 않게 되므로 해당 단계를 구현할 때 고친다.
- CLAUDE.md: 첫 문단의 범위, "실행"(`./server <port> <root>`), "테스트"(`make test`), 제약의 "HTTP 파싱, 정적 파일 … 범위가 아니다", 설계 결정의 "`http_request_len`, `http_handle` 두 함수"(→ `http_init`, `http_parse`, `http_handle`)
- architecture-tcp.md "출력 형식": 사용법 메시지, 인자 검사, `/big` 본문, stub 응답 헤더 줄
