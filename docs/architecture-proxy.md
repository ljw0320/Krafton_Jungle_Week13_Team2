범위: 웹 서버 과제의 Reverse Proxy를 구현한다. 요구사항의 "동적 처리(백엔드로 전달)"와 "Proxy"를 함께 충족한다. TCP 서버와 이벤트 루프는 [architecture-tcp.md](architecture-tcp.md), 요청 파싱과 정적 파일은 [architecture-http.md](architecture-http.md)의 설계를 쓰고, 이 문서는 그 위에서 바뀌는 것을 적는다. 이 문서와 두 문서가 다르면 프록시 단계에서는 이 문서를 따른다.

하지 않는 것: Forward Proxy, CGI, 캐싱, 죽은 백엔드 건너뛰기, 시그널 셧다운

개요: [overview.md](overview.md), 요구사항: [requirements.md](requirements.md)

이 문서에서 **A**는 클라이언트와 우리 서버의 연결, **B**는 우리 서버와 백엔드의 연결이다.

---
# 결정 사항 (2026-11-24 확정)

| # | 항목 | 결정 | 이유 | 틀렸을 때의 비용 |
|---|---|---|---|---|
| 1 | 연결 구조 | `struct upstream`을 `struct conn` 안에 값으로 넣는다. `data.ptr`는 항상 `struct watch *`다 | B를 따로 할당하지 않아 double free의 대상이 없다. A 하나의 B는 동시에 최대 하나다. 정적 요청이 더 쓰는 메모리는 연결당 약 100바이트다 | `event_loop.c`, `conn_close`, B를 열고 닫는 함수 |
| 2 | 배치의 use-after-free | `loop_unwatch`가 지금 배치 배열에 남은 그 대상의 이벤트를 지운다. `free(c)`는 바로 한다 | `closed` 표시, 해제 목록, 배치 뒤 해제가 필요 없다. `closed` 표시는 결정 1과 같이 쓸 수 없다(같은 자리에 새 B를 열면 표시가 지워진다) | `event_loop.c`의 반복문 |
| 3 | B를 기다리는 동안 A의 감시 | `EPOLLERR`만 | 커널에는 감시 0과 같지만 값이 0이 아니라 `events == 0`은 미등록만 뜻한다. FIN만으로는 `close`와 `shutdown(SHUT_WR)`을 구분할 수 없어 `EPOLLRDHUP`으로 닫으면 정상 클라이언트의 응답을 버린다 | 감시 값 하나와 이벤트 분기 하나 |
| 4 | 설정 | CLI 인자 `<접두사>=<IPv4>:<포트>`. 적은 순서대로 처음 맞는 규칙 | 파일을 읽는 코드가 없다. 규칙이 없으면 지금 명령 그대로다 | `main.c`의 인자 처리 |
| 5 | 경로 비교와 전달 | 원래 대상의 앞부분을 접두사와 비교하고 원래 대상을 그대로 보낸다. 디코딩했을 때 `..` 세그먼트가 있으면 400 | 비교한 값과 보내는 값이 같다. 백엔드가 받는 대상은 항상 접두사로 시작한다 | 라우팅 함수 하나와 단위 테스트 |
| 6 | 접두사 | 떼지 않는다 | 떼면 응답의 `Location`, `Set-Cookie`의 `Path`도 고쳐야 한다 | 대상의 시작 위치 |
| 7 | 메서드 | GET, POST만 전달. 그 밖은 405 + `Allow: GET, POST` | 요구사항의 지원 메서드가 GET, POST다 | 메서드 검사 함수 |
| 8 | `Host` | 원래 값을 유지한다. 문법의 글자가 아니면 400, 1.0에서 없으면 백엔드 주소 | 백엔드의 절대 URL과 `Origin` 비교가 맞는다 | 요청을 만드는 함수의 한 줄 |
| 9 | 요청 바이트 검사 위치 | 전달 직전에만 한다 | 위험은 다른 서버가 바이트를 다시 해석할 때 생긴다. 정적 요청의 동작과 테스트가 그대로다 | 검사를 부르는 위치 |
| 10 | B에 보낼 버전 | 클라이언트와 같은 버전 + `Connection: close` | 1.1이면 길이를 모르는 응답이 chunked로 와서 잘린 것을 클라이언트가 안다. 1.0 클라이언트에는 chunked가 가지 않는다 | 요청 라인의 버전 한 글자 |
| 11 | `1xx` | 버리고 다음 응답 헤더를 기다린다. `101`은 502 | `Expect`와 `Upgrade`를 빼고 보낸다 | 반복문 하나 |
| 12 | 프록시용 헤더 | `X-Forwarded-*`, `Forwarded`, `X-Real-IP`를 지운다. 새로 넣지 않는다 | 클라이언트가 지어낸 주소가 백엔드에 가지 않는다 | 헤더 한 줄과 `getpeername` |
| 13 | 응답의 끝과 A | 길이를 알고 클라이언트가 원하면 A를 유지, 아니면 닫는다. chunked는 해석하지 않고 중계한다 | 바이트를 세는 것은 항상 닫아도 필요하다 | 유지 조건을 0으로 |
| 14 | 응답 헤더 검사 | 요청 파서와 같은 엄격한 규칙. 어기면 502 | A를 유지하므로 경계를 잘못 읽으면 다음 응답까지 어긋난다 | 해당 검사 한 줄 |
| 15 | 버퍼 | 중계 버퍼 16KB, 응답 헤더 한도 8192바이트. `c->wbuf` 하나를 요청 전송과 응답 중계에 다시 쓴다 | 동시 요청이 많아도 메모리가 작다. B 전용 버퍼 필드가 없다 | 상수 |
| 16 | 타임아웃 | 연결 3초, 응답 10초(바이트가 오지 않은 시간). 시각 `since` 하나로 잰다 | `IDLE_TIMEOUT_SEC`, `REQ_TIMEOUT_SEC`와 같다. `SENDING`도 잰다 | 상수 |
| 17 | 연결 타임아웃의 코드 | 504 | 시간 초과는 504, 나머지 실패는 502로 기준이 하나다 | 숫자 하나와 테스트 기대값 |
| 18 | 500 / 502 / 504 뒤의 A | 닫는다 | 실패 경로가 응답 → 닫기 한 방향이다. sweep 안에서 다음 요청이 시작되지 않는다 | 실패 함수와 부른 쪽의 반환값 처리 |
| 19 | Round-Robin | 마지막 구현 단계. 같은 접두사를 여러 번 적는다 | 앞 단계와 독립이라 빼도 된다 | `struct route`와 규칙 파싱 |
| 20 | 헤더 줄 수 (2026-11-24 추가) | 프록시로 보내는 요청과 백엔드 응답의 헤더를 100줄까지만 받는다. 요청은 431, 응답은 502 | 줄마다 `Connection` 값을 다시 훑어, 1800줄을 채운 8KB 요청 하나가 약 55ms 동안 루프를 멈췄다. 정적 요청은 그대로다 | 상수와 검사 두 곳 |

메인 세션이 정한 세부 판단 (틀렸을 때의 비용은 각각 한 함수 안의 수정이다)
- 요청과 응답을 다시 만드는 함수는 `http.c`에 둔다. `tchar`, `ieq`, `path_decode`, 헤더 끝 찾기를 같이 쓰고, 소켓 없이 단위 테스트한다.
- accept 재개를 `loop_unwatch`에서 `loop_accept_resume`으로 나눈다. backpressure의 DEL은 fd를 비우지 않는다.
- 408도 `conn_error`를 쓴다. 동작은 그대로다.
- 연결 유지 판단의 닫는 코드에 502, 504를 더한다. `connect`가 바로 실패하면 원래 `req`로 502를 만들어서 `minor`가 0이 아니다.
- B에서 응답을 받아 `WRITING`으로 갈 때 `last_active = now`로 적는다. 적지 않으면 백엔드를 9초 기다린 뒤 A의 `send`가 `EAGAIN`일 때 다음 sweep이 바로 닫는다.
- `204`와 `304`는 본문이 없는 것으로 보고 `Transfer-Encoding`을 뺀다. `204`는 `Content-Length`도 뺀다.
- B의 `epoll_ctl` 실패는 응답을 시작하기 전이면 500, 뒤면 A를 닫는다. A의 `epoll_ctl` 실패는 지금처럼 `conn_close`다.

확인한 커널 동작 (2026-11-24, WSL2, 저장소 밖의 작은 프로그램)

| 동작 | 결과 |
|---|---|
| `EPOLLERR`만 감시하는 소켓에 데이터와 FIN이 옴 | 이벤트 없음. `EPOLLIN`으로 바꾸면 바로 온다 |
| 같은 소켓에 RST가 옴 | `ERR\|HUP` |
| 배치를 받은 뒤 fd를 DEL | 이미 받은 배열에는 그대로 남는다 |
| 한 배치에 A와 B의 이벤트. A를 먼저 닫고 바로 `free` | 배열에서 지우면 ASan 오류 없음. 지우지 않으면 heap-use-after-free |
| 한 배치에 B의 이벤트가 남았는데 B를 닫고 같은 자리에 새 B를 엶 | 배열에서 지우면 이전 이벤트를 처리하지 않는다 |
| 닫힌 포트에 non-blocking `connect` | `EINPROGRESS` → `OUT\|ERR\|HUP` → `SO_ERROR`가 `ECONNREFUSED` |
| B의 감시를 끈 동안 데이터와 RST가 온 뒤 다시 등록 | `IN\|ERR\|HUP`, 남은 데이터를 `recv`할 수 있다 |
| `listen(0)`만 하고 `accept`하지 않는 백엔드 | 첫 연결은 성공, 두 번째부터 이벤트가 오지 않는다 |

---
# 출력 형식

| 항목 | 형식 |
|---|---|
| 사용법 메시지 | stderr에 `usage: ./server <port> <root> [<prefix>=<ip>:<port> ...]`, `exit(1)`. `argc < 3`이거나 포트 검사에 실패하면 |
| 규칙 오류 | stderr에 `route: <인자>: <이유>`, `exit(1)`. `http_init` 다음, `server_listen`보다 먼저 검사한다 |
| 502, 504 응답 | 다른 오류 응답과 같은 형식. `Connection: close`, 본문 `<h1>502 Bad Gateway</h1>\n`, `<h1>504 Gateway Timeout</h1>\n` |
| 백엔드 경로의 405 | `Allow: GET, POST` |

로그는 모두 stderr다. 기존 로그는 그대로이고 아래를 더한다. 앞의 fd가 A, 뒤의 fd가 B다.

```
fd=5 backend fd=6 connect 127.0.0.1:8001
fd=5 backend fd=6 send 120 bytes
fd=5 backend fd=6 recv 900 bytes
fd=5 backend fd=6 off                     # backpressure로 B의 감시를 끔
fd=5 backend fd=6 on
fd=5 backend fd=6 close
fd=5 proxy error 502: Connection refused  # 이유는 strerror 또는 짧은 영어 문구
```

- 본문 중계 중의 실패는 상태 코드가 없으므로 `fd=5 proxy error: <이유>`다. 길이를 모르는 응답의 정상 끝에는 찍지 않는다.
- `EPOLLOUT off`는 `EPOLLOUT`이 등록돼 있었을 때만 찍는다. 지금 조건(`EPOLLIN`이 아니면)은 `EPOLLERR`에서 돌아올 때도 찍는다.

---
# 파일 구조

```
src/
  main.c              # argc >= 3, argv[3..]을 route_add
  route.c / .h        # 새로 만듦. 규칙 파싱, 접두사 찾기, 백엔드 고르기
  proxy.c / .h        # 새로 만듦. B의 소켓과 상태, 실패 처리
  http.c / .h         # 요청과 응답 다시 만들기 추가
  conn.c / .h         # struct watch, struct upstream, PROXYING, conn_error
  event_loop.c / .h   # struct watch, 배치에서 지우기, loop_accept_resume
tests/
  test_proxy.c        # 새로 만듦. http.c와 route.c를 #include하는 단위 테스트
  backend.py          # 새로 만듦. 테스트용 백엔드(python3 표준 라이브러리)
  proxy_test.sh       # 새로 만듦. 기능 테스트
```

- `server.c`는 바뀌지 않는다.
- `Makefile`: `make test`가 `test_http`, `test_proxy`, `http_test.sh`, `proxy_test.sh`를 차례로 실행한다. 하나라도 실패하면 종료 코드가 0이 아니다. `make clean`과 `.gitignore`에 `tests/test_proxy`를 더한다.
- 테스트 출력 형식은 [architecture-http.md](architecture-http.md)와 같다.

| 상수 | 값 | 위치 |
|---|---|---|
| `MAX_ROUTES` | 16 | `route.h` |
| `MAX_BACKENDS` (Round-Robin 단계) | 8 | `route.h` |
| `HTTP_HEAD_MAX` (응답 헤더 한도) | 8192 | `http.h` |
| `HTTP_HDR_MAX` (프록시 헤더 줄 수) | 100 | `http.h` |
| `PROXY_BUF_SIZE` | 16384 | `proxy.h` |
| `CONNECT_TIMEOUT_SEC` | 3 | `proxy.h` |
| `RESPONSE_TIMEOUT_SEC` | 10 | `proxy.h` |

---
# 구조체

```C
// event_loop.h
enum watch_kind { W_CLIENT, W_BACKEND };
struct watch { enum watch_kind kind; int fd; uint32_t events; };   // events가 0이면 미등록

// route.h
struct backend { struct sockaddr_in addr; const char *name; };     // name은 "127.0.0.1:8001" (argv 안)
struct route   { const char *prefix; size_t prefix_len; struct backend b; };
// Round-Robin 단계: struct backend b[MAX_BACKENDS]; int n, next;

// http.h
struct http_req    { /* 기존 필드 */ int backend; };   // 백엔드 경로면 1. 405의 Allow를 고른다
struct http_upresp {
    int    status;                // 1xx면 out을 쓰지 않았다
    int    has_len;               // 응답의 길이를 아는가
    size_t body_len;              // has_len이면 본문 길이
    int    keep_alive;            // A를 유지하나
    size_t out_len;               // out에 쓴 헤더 길이
};

// proxy.h
enum up_state { UP_CONNECTING, UP_SENDING, UP_RECEIVING };
struct upstream {
    struct watch w;               // 첫 필드. w.fd가 -1이면 B 없음
    struct conn *c;
    enum up_state state;
    time_t since;                 // 타이머 시작 시각
    int    minor;                 // 보낸 요청의 HTTP/1.x
    int    head_done;             // 응답 헤더 검사를 통과했나 (A로 보내기 시작)
    int    has_len;
    size_t remain;                // has_len이면 B에서 더 받을 본문
};

// conn.h
enum conn_state { CONN_READING, CONN_PROXYING, CONN_WRITING };
struct conn {
    struct watch w;               // 첫 필드. 기존 fd, events를 대신한다
    /* 나머지 기존 필드 */
    struct upstream up;
};
```

- `w`가 첫 필드라 `kind`를 확인한 뒤 `(struct conn *)w`, `(struct upstream *)w`로 바꾼다.
- `conn_new`에서 `w = {W_CLIENT, fd, 0}`, `up.w = {W_BACKEND, -1, 0}`, `up.c = c`로 둔다. `calloc`의 0은 stdin이라 `up.w.fd`를 -1로 두지 않으면 fd 0을 닫는다.
- `conn.h`가 `proxy.h`를, `proxy.h`가 `event_loop.h`를 포함한다. `proxy.h`는 `struct conn`을 선언만 한다.

---
# 함수

```C
// event_loop.h
int  loop_watch(struct watch *w, uint32_t events);   // events는 0이 아니어야 한다
void loop_unwatch(struct watch *w);                  // DEL + 배치에서 지우기
void loop_accept_resume(void);                       // fd를 닫은 뒤 부른다. 멈춰 있지 않으면 아무것도 안 한다

// route.h
int route_add(const char *arg);                                   // 0, 틀리면 메시지를 찍고 -1
struct route *route_find(const char *target, size_t len);         // 맞는 규칙, 없으면 NULL(정적)
const struct backend *route_pick(struct route *r);                // 지금 순서의 백엔드. Round-Robin 단계 전에는 항상 &r->b
void route_advance(struct route *r);                              // Round-Robin 단계에서 추가. 다음 순서로 넘긴다

// http.h (추가)
int http_keep_alive(const struct http_req *req);                  // 클라이언트가 유지를 원하나
/* buf 안의 req를 백엔드에 보낼 요청(헤더 + 본문)으로 out에 다시 만든다. host는 Host가 없을 때 쓴다.
   0 또는 400/405/411/431/500/501 */
int http_proxy_request(const struct http_req *req, const char *buf, const char *host,
                       char *out, size_t cap, size_t *out_len);
/* 백엔드 응답 헤더를 검사하고 A로 보낼 헤더를 out에 만든다. minor는 보낸 요청의 버전, keep은 클라이언트의 희망.
   >0: 원본 헤더 길이, 0: 덜 왔다, -1: 502 (len이 HTTP_HEAD_MAX인데 헤더 끝이 없는 것도 포함) */
ssize_t http_proxy_response(const char *buf, size_t len, int minor, int keep,
                            char *out, size_t cap, struct http_upresp *r);

// proxy.h
int  proxy_start(struct conn *c, const struct http_req *req, struct route *r);
                                       // 0: 시작함, >0: 보낼 오류 코드(B와 wbuf는 정리됨), -1: c를 닫음
int  proxy_event(struct upstream *u, uint32_t events);   // 0 / -1
int  proxy_resume(struct conn *c);     // A의 버퍼를 비웠으니 B를 다시 읽는다. 0 / -1
int  proxy_timeout(struct conn *c, time_t now);          // sweep이 PROXYING에 부른다. 0 / -1
void proxy_close(struct conn *c);      // B만 닫는다. B가 없으면 아무것도 안 한다

// conn.h (추가)
int  conn_error(struct conn *c, int status);   // B를 닫고 wbuf를 버린 뒤 오류 응답을 보낸다. 0 / -1
```

핸들러 반환값

| 값 | 뜻 |
|---|---|
| -1 | **`c`가 닫히고 해제됐다.** `c`와 그 안의 `up`을 다시 건드리지 않는다 |
| 0 | `c`가 살아 있다. B는 닫혔을 수도, 다음 요청의 새 B일 수도 있다 |

- B만 닫힌 것은 -1이 아니다.
- `proxy.c`에서 `conn_on_writable`이나 `conn_error`를 부른 뒤에는 반환값이 0이어도 `u`를 쓰지 않고 그 값을 그대로 반환한다. 파이프라이닝된 다음 요청이 같은 자리에 새 B를 열었을 수 있다.
- `conn_error`의 응답은 항상 닫는 코드라 `conn_send`가 1을 반환하지 않는다.

이벤트 루프

```C
static struct epoll_event events[MAX_EVENTS];   // loop_unwatch가 보므로 static
static int ev_i, ev_n;

void loop_unwatch(struct watch *w) {
    if (w->events != 0) {
        if (epoll_ctl(epfd, EPOLL_CTL_DEL, w->fd, NULL) < 0) perror("epoll_ctl");
        w->events = 0;
    }
    for (int j = ev_i + 1; j < ev_n; j++)       // 이번 배치에 남은 이벤트
        if (events[j].data.ptr == w) events[j].events = 0;
}

for (ev_i = 0; ev_i < ev_n; ev_i++) {
    uint32_t e = events[ev_i].events;
    if (e == 0) continue;                        // 이번 배치에서 닫힌 대상. w를 읽기 전에 본다
    struct watch *w = events[ev_i].data.ptr;
    if (w == NULL) { accept_all(listen_fd); continue; }
    if (w->kind == W_BACKEND) { proxy_event((struct upstream *)w, e); continue; }
    /* W_CLIENT: 지금 코드 그대로. ERR|HUP이면 conn_close */
}
ev_n = 0;                                        // sweep에서 부른 loop_unwatch가 지난 배치를 훑지 않게
```

`conn_process`

```C
struct route *r = n > 0 ? route_find(req.target, req.target_len) : NULL;
int st = r ? proxy_start(c, &req, r) : 0;
if (st < 0) return -1;
if (st > 0) { req.status = st; req.backend = 1; }        // 오류 응답은 기존 경로로
if ((!r || st > 0) && conn_respond(c, &req) < 0) return -1;
/* 기존 memmove: 응답이나 전달할 요청을 만든 뒤에 지운다 */
if (r && st == 0) return 0;                              // PROXYING. 다음 요청은 응답이 끝난 뒤
```

`conn_send`에 더하는 것
- 남았을 때: `if (c->up.w.events) loop_unwatch(&c->up.w)` (B를 끔) 후 A를 `EPOLLOUT`으로
- 다 보냈을 때, `free(wbuf)`보다 먼저: `if (c->up.w.fd >= 0) return proxy_resume(c);`

`conn_sweep_idle`
- `CONN_PROXYING`이면 `proxy_timeout(c, now)`만 부른다. 요청 10초와 유휴 10초를 검사하지 않는다.
- 408은 `conn_error(c, 408)`을 부른다.

---
# 상태 전이

## A

| 상태 | 뜻 | A의 감시 | B의 감시 | 타이머 |
|---|---|---|---|---|
| `READING` | 요청을 기다림 | `EPOLLIN` | B 없음 | 요청 10초(408) / 유휴 10초 |
| `PROXYING` | B를 기다림. A로 보낼 것 없음 | `EPOLLERR` | `EPOLLOUT` 또는 `EPOLLIN` | B의 `since`: 3초 / 10초 |
| `WRITING` | A로 보낼 것이 있음 | 못 보낸 것이 있으면 `EPOLLOUT` | B 없음, 또는 못 보낸 것이 있으면 꺼짐(DEL) | 유휴 10초 + `SIOCOUTQ` (지금 코드) |

```
READING ── 정적, 오류 (conn_respond) ──────────────▶ WRITING
READING ── 규칙에 맞음, proxy_start가 0 ───────────▶ PROXYING
PROXYING ── B에서 응답을 받음 (last_active = now) ─▶ WRITING
PROXYING ── 응답 전의 실패 (last_active = now, conn_error) ▶ WRITING
PROXYING ── 본문 중 recv(B)가 0이나 오류, 시간 초과 ▶ conn_close
WRITING ── 다 보냄, B가 열려 있음 (proxy_resume) ──▶ PROXYING
WRITING ── 다 보냄, B가 없음, keep_alive ──────────▶ READING → rbuf에 남은 요청을 처리
WRITING ── 다 보냄, B가 없음, keep_alive 아님 ─────▶ conn_close
모든 상태 ── A의 ERR|HUP, send/recv 오류, 유휴 ────▶ conn_close (B도 닫힘)
```

- **B가 열려 있으면 A로 보낼 것이 더 있다.** 본문을 다 받으면 B를 먼저 닫고 A로 보낸다. 그래서 `conn_send`는 "B가 없다 = 응답 끝"으로 지금처럼 마무리한다.
- `recv(B)` 뒤 바로 다 보내면 `epoll_ctl`을 부르지 않고 `PROXYING`으로 돌아온다.
- 이벤트 하나에 `recv(B)`와 `send(A)`를 한 번씩만 한다.

## B

| 상태 | 감시 | 이벤트가 오면 (`ERR\|HUP` 포함) | 실패 |
|---|---|---|---|
| `CONNECTING` | `EPOLLOUT` | `SO_ERROR`를 한 번 읽는다. 0이면 `SENDING`, 바로 `send` | 502 |
| `SENDING` | `EPOLLOUT` | `send` 한 번. 다 보내면 `wlen = woff = 0`, `RECEIVING`, `EPOLLIN` | 502 |
| `RECEIVING`, `head_done` 아님 | `EPOLLIN` | `recv` 한 번(최대 `HTTP_HEAD_MAX - wlen`) → 헤더 검사 | 502 |
| `RECEIVING`, `head_done` | `EPOLLIN` 또는 끔 | `recv` 한 번(최대 `PROXY_BUF_SIZE`, `has_len`이면 `remain`까지) → A로 `send` | A를 닫음 |

- `EPOLLERR|HUP`만 보고 닫지 않는다. `SENDING`은 `send`, `RECEIVING`은 `recv`의 반환값으로 판단한다. `HUP`과 함께 응답이 남아 있을 수 있다.
- `recv`, `send`의 `EAGAIN`, `EINTR`은 0을 반환하고 다음 이벤트를 기다린다.
- `SO_ERROR`는 이벤트가 온 뒤에 한 번만 읽는다. 진행 중에도 0이고, 읽으면 지워진다. `getsockopt`이 실패하면 `errno`를 오류로 본다.

## 타이머 (`since`)

| 상태 | `since = now`로 적는 때 | 한도 | 초과하면 |
|---|---|---|---|
| `CONNECTING` | `connect`를 부를 때 | 3초 | 504 |
| `SENDING` | 연결이 됐을 때, `send`가 1바이트 이상 보냈을 때 | 10초 | 504 |
| `RECEIVING` | 요청을 다 보냈을 때, `recv`가 1바이트 이상 받았을 때, `proxy_resume` | 10초 | `head_done`이 아니면 504, 맞으면 A를 닫음 |

- 비교는 `now - since > 한도`다. `time()`이 초 단위라 실제로는 3~4초, 10~11초다.
- B를 끈 동안(A가 `WRITING`)은 재지 않는다. 늦은 쪽이 클라이언트라 A의 유휴 검사가 맡는다.

## `proxy_start`

1. `wbuf = malloc(PROXY_BUF_SIZE)`. 실패하면 500
2. `route_pick`, `http_proxy_request`. 0이 아니면 `free(wbuf)`, `wbuf = NULL` 후 그 코드. Round-Robin 단계에서는 이 검사를 통과한 뒤에만 순서를 넘긴다(아래 "Round-Robin")
3. `socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)`. 실패하면 500
4. `connect`. `EINPROGRESS`가 아닌 실패면 502. 0이면 `EINPROGRESS`와 똑같이 이벤트를 기다린다
5. `loop_watch(&up.w, EPOLLOUT)`. 실패하면 500. `connect` 뒤에 등록해야 한다(먼저 등록하면 `OUT|HUP`이 온다)
6. `up.state = UP_CONNECTING`, `since = now`, `minor = req->minor`, `head_done = 0`, `c->keep_alive = http_keep_alive(req)`, `c->state = CONN_PROXYING`
7. `loop_watch(&c->w, EPOLLERR)`. 실패하면 `conn_close(c)`, -1

3~5에서 실패하면 `proxy_close(c)`와 `free(wbuf)`를 하고 코드를 반환한다.

## 응답 헤더를 받았을 때

1. `http_proxy_response`가 0이면 다음 이벤트를 기다린다. -1이면 502
2. `1xx`면 그 헤더만큼 `memmove`로 지우고 1부터 다시 한다
3. 새 16KB 버퍼에 재작성한 헤더와 이미 받은 본문을 담고 이전 `wbuf`를 `free`한다. 버퍼는 `http_proxy_response`를 부르기 전에 할당하고, 실패하면 500
4. 이미 받은 본문이 `body_len`보다 길면 `body_len`까지만 담는다
5. `head_done = 1`, `has_len`, `remain = body_len - 담은 본문`, `c->keep_alive = r.keep_alive`, `outq = 0`, `last_active = now`, `c->state = CONN_WRITING`
6. `has_len`이고 `remain`이 0이면 `proxy_close(c)`
7. `return conn_on_writable(c)`

본문을 받았을 때도 `remain`을 줄인 뒤 5의 `last_active`, `state`와 6, 7을 똑같이 한다.

---
# 닫는 순서

```C
void proxy_close(struct conn *c) {
    if (c->up.w.fd < 0) return;
    loop_unwatch(&c->up.w);      // close 전에
    close(c->up.w.fd);
    c->up.w.fd = -1;
    loop_accept_resume();
}
```

`conn_close`
1. `proxy_close(c)`
2. `loop_unwatch(&c->w)` (`close` 전에)
3. `close(c->w.fd)`
4. `loop_accept_resume()`
5. `free(c->wbuf)`
6. `if (c->file_fd >= 0) close(c->file_fd)`
7. 리스트에서 제거
8. `free(c)` (마지막에)

| 막는 것 | 방법 |
|---|---|
| double free, 이중 close | B를 따로 할당하지 않는다. 닫으면 `fd = -1`이다. 닫는 방향은 A → B 하나이고 `proxy_close`는 A를 닫지 않는다 |
| use-after-free (같은 배치) | `loop_unwatch`가 배치에 남은 이벤트를 지우고, 루프는 `w`를 읽기 전에 건너뛴다. A가 닫히면 B도 `loop_unwatch`를 거친다 |
| use-after-free (함수 안) | 핸들러 반환값 규칙 |
| use-after-free (sweep) | 지금처럼 `next`를 먼저 저장한다. `c` 하나를 처리하면서 다른 `c`는 닫지 않는다 |
| A 없이 남는 B | B는 `struct conn` 안에 있고 `conn_close`가 항상 `proxy_close`를 부른다 |

---
# 라우팅

규칙은 `argv[3]`부터 하나씩 `route_add`로 더한다.

| 검사 (어기면 시작하지 않음) | 규칙 |
|---|---|
| 형식 | 첫 `=`로 나눈다. 앞이 접두사, 뒤가 `<IPv4>:<포트>` |
| 접두사 | `/`로 시작하고 `/`로 끝남. `/` 하나는 허용 |
| 접두사의 글자 | `0x21`~`0x7E`. `#`, `?`, `%`, `=`, `//`, `.`이나 `..` 세그먼트는 안 됨 |
| 주소 | `inet_pton(AF_INET)`. 이름(`localhost`)은 안 됨 |
| 포트 | 숫자만, 1~65535 |
| 중복 | 같은 접두사가 이미 있으면 안 됨 (Round-Robin 단계에서 바뀜) |
| 개수 | `MAX_ROUTES`까지 |

```C
// 원래 대상의 앞부분을 비교한다. 접두사가 '/'로 끝나므로 /apix는 맞지 않는다
if (len >= r->prefix_len && memcmp(target, r->prefix, r->prefix_len) == 0) return r;
```

- 쿼리를 따로 떼지 않는다. `?`는 접두사에 올 수 없어서 접두사가 경로 부분하고만 맞는다.
- 요청 값으로 `connect` 주소를 만들지 않는다. 요청은 등록된 규칙을 고르기만 한다(SSRF).
- `/api`(끝에 `/` 없음), `/%61pi/users`, `//api/users`, `/API/users`는 규칙 `/api/`에 맞지 않아 정적 파일로 처리한다.

## Round-Robin (마지막 단계)

| 항목 | 동작 |
|---|---|
| 같은 접두사 | 그 규칙의 백엔드로 더한다. 최대 `MAX_BACKENDS` |
| 같은 접두사 + 같은 주소 | 시작하지 않는다 |
| 순서 | 적은 순서. `i = next; next = (next + 1) % n`으로 넘치지 않는다 |
| 세는 단위 | 요청 검사를 통과해 `connect`를 시도하는 요청. 400, 405, 411, 501로 끝난 요청은 세지 않는다. `connect`가 실패해도 다음 순서로 넘어간다 |

`http_proxy_request`가 백엔드 이름을 쓰므로 `route_pick`을 먼저 부르되 `next`는 검사를 통과한 뒤에 넘겨야 한다. `route_pick`은 고르기만 하고 `void route_advance(struct route *r)`을 3번 전에 부른다.

---
# 요청 재작성

검사 순서. 앞의 것은 [architecture-http.md](architecture-http.md)의 검사 순서 1~7(`http_parse`)이고, 통과한 요청만 라우팅한다.

| 순서 | 검사 (`http_proxy_request`) | 코드 |
|---|---|---|
| 1 | 모르는 메서드 | 501 |
| 2 | POST인데 `Content-Length` 없음 | 411 |
| 3 | GET, POST가 아님 | 405 + `Allow: GET, POST` |
| 4 | `path_decode` 실패(바이트가 `0x21`~`0x7E` 밖, `#`, 퍼센트 인코딩 오류, `%00`) | 400 |
| 5 | 디코딩한 경로에 `..` 세그먼트 | 400 |
| 6 | 헤더 값에 `0x01`~`0x1F`(탭 제외)나 `0x7F`. `0x80` 이상은 허용 | 400 |
| 7 | `Host` 값이 비었거나 영문자, 숫자, `-._~%!$&'()*+,;=:[]` 밖의 글자 | 400 |
| 8 | 헤더가 `HTTP_HDR_MAX`줄을 넘음 | 431 |
| 9 | 경로를 디코딩할 버퍼의 `malloc` 실패, 다시 만든 요청이 `cap`을 넘음 | 500 |

- 1~3은 `http_handle`과 같은 함수를 쓴다. 허용 메서드에 POST가 있는지만 다르다.
- 6~8은 헤더를 한 번 훑으며 줄 순서대로 본다. 8은 101번째 줄에서 걸리므로 앞의 100줄에 6, 7이 있으면 400이다.
- `path_decode`는 쿼리도 바이트를 검사하고, 디코딩과 `..` 검사는 쿼리 앞까지만 한다.
- `.` 세그먼트와 빈 세그먼트는 막지 않는다.
- 헤더는 파서를 통과했으므로 이름, 콜론, 줄 끝은 다시 검사하지 않는다.

만드는 순서는 요청 라인 → `Host` → `Connection: close` → (`Content-Length`) → 나머지 헤더 → 빈 줄 → 본문이다.

| 처리 | 헤더 |
|---|---|
| 새로 만듦 | 요청 라인(`<메서드> <원래 대상> HTTP/1.<minor>`), `Host`(원래 값, 1.0에서 없으면 백엔드 주소), `Connection: close`, `Content-Length`(요청에 있었을 때만, 파싱한 값) |
| 뺌 | `Connection`, `Keep-Alive`, `Proxy-Connection`, `TE`, `Upgrade`, `Expect`, `Trailer`, `Connection` 값에 적힌 이름, `X-Forwarded-`로 시작하는 이름, `Forwarded`, `X-Real-IP` |
| 그대로 | 나머지. 이름의 글자와 순서를 유지하고 값 앞뒤의 공백은 정리해 `이름: 값`으로 쓴다 |

- 이름 비교는 대소문자를 구분하지 않는다. `Connection`이 여러 줄이면 모두 본다.
- `Connection: Content-Length, Host`라고 적어도 둘은 항상 새로 만들므로 빠지지 않는다(request smuggling).
- `Transfer-Encoding`이 있는 요청은 파서가 501로 거절했다.
- 본문은 `buf + head_len`부터 `content_length`바이트다. 파서가 본문까지 다 온 요청만 돌려준다.

```
받은 것 (A)                                  보낼 것 (B)
POST /api/users?x=1 HTTP/1.1                 POST /api/users?x=1 HTTP/1.1
Host: shop.com                               Host: shop.com
Connection: keep-alive, X-Debug              Connection: close
X-Debug: 1                                   Content-Length: 5
X-Forwarded-For: 127.0.0.1                   Accept: */*
Accept: */*
Content-Length: 5                            hello

hello
```

크기: 원본은 8192바이트 이하다. 늘어나는 것은 줄마다 콜론 뒤 공백 1바이트(100줄까지)와 새로 만드는 헤더 약 100바이트라 8.4KB 이하이고 16KB에 들어간다. 그래도 쓸 때마다 남은 크기를 검사한다.

---
# 응답 중계

## 헤더 검사 (어기면 502)

헤더를 끝까지 받아 검사한 뒤 A로 보내기 시작한다.

| 검사 | |
|---|---|
| 헤더 끝 | NUL, 홀로 있는 `\r`이나 `\n`, 앞의 빈 줄이 없음. 8192바이트 안에 `\r\n\r\n` |
| 상태 라인 | `HTTP/1.0` 또는 `HTTP/1.1`, SP, 숫자 3자리(100~599), 그 뒤는 줄 끝이거나 SP와 이유. 이유에 탭을 뺀 제어 문자와 `0x7F`가 없음 |
| 헤더 | 이름 tchar, 콜론 앞 공백, 콜론 없음, obs-fold, 값의 제어 문자(요청 검사 6과 같음) |
| `Content-Length` | 숫자만, 넘치지 않음, 한 번만(값이 같아도 두 번이면 502) |
| `Transfer-Encoding` | `Content-Length`와 함께 있으면 안 됨. 보낸 요청이나 응답이 1.0이면 안 됨 |
| `101` | 안 됨 |
| 헤더 줄 수 | `HTTP_HDR_MAX`줄까지 |

`http_parse`의 "헤더 끝을 찾으며 NUL과 홀로 있는 CR/LF를 거르는 부분"을 `static` 함수로 꺼내 같이 쓴다. 동작이 같은 리팩터링이고, 파서 단위 테스트가 그대로 통과해야 한다. `Content-Length` 숫자 파싱도 같이 쓴다.

## 헤더 재작성

| 처리 | 헤더 |
|---|---|
| 새로 만듦 | `HTTP/1.1 <코드> <이유>`, `Connection: keep-alive` 또는 `close`, `Content-Length`(응답에 있었을 때만. `204`면 뺌) |
| 뺌 | `Connection`, `Keep-Alive`, `Proxy-Connection`, `Upgrade`, `Connection` 값에 적힌 이름. `204`, `304`면 `Transfer-Encoding`도 |
| 남김 | `Transfer-Encoding`, `Trailer`. 본문을 해석하지 않고 보내므로 있어야 한다. `Connection`에 적혀 있어도 남긴다 |
| 그대로 | 나머지(`Date`, `Server`, `Content-Type`, `Set-Cookie`, `Location` 등). 여러 줄이면 여러 줄 그대로 |

## 응답의 끝

위에서부터 처음 맞는 것을 쓴다.

| 응답 | 끝 | `has_len` |
|---|---|---|
| `204`, `304` | 헤더 끝 | 1 (`body_len` 0) |
| `Transfer-Encoding` 있음 | `recv(B)`가 0 | 0 |
| `Content-Length: N` | 본문 N바이트 | 1 |
| 둘 다 없음 | `recv(B)`가 0 | 0 |

A를 유지하는 조건은 `has_len`이고 클라이언트가 유지를 원하는 것(`http_keep_alive`)이다.

| 상황 | 처리 |
|---|---|
| N바이트를 다 받음 | B의 FIN을 기다리지 않고 B를 닫은 뒤 A로 보낸다 |
| N바이트보다 더 옴 | N바이트까지만 보낸다. 나머지를 A로 보내면 다음 응답으로 읽힌다. A는 조건대로 유지한다 |
| N바이트보다 적게 오고 `recv(B)`가 0이나 오류 | `conn_close` |
| `has_len`이 0이고 `recv(B)`가 0이나 오류 | `conn_close` |

B는 A의 버퍼가 비었을 때만 읽으므로 `recv(B)`가 0일 때 A로 보낼 것이 남아 있지 않다. 그래서 정상 끝과 잘린 끝의 처리가 같다.

## 버퍼와 backpressure

| 시점 | `wbuf` | 보내는 곳 |
|---|---|---|
| `CONNECTING`, `SENDING` | 다시 만든 요청 | B |
| `RECEIVING`, 헤더 전 | 응답을 최대 8192바이트까지 모음 | - |
| 헤더 검사 통과 | 새 버퍼에 재작성한 헤더 + 이미 받은 본문 | A |
| 본문 | 비면 다시 `recv(B)`로 채움 | A |

```
recv(B) → send(A) ─ 다 보냄 → PROXYING (epoll_ctl 없음)
                  └ 남음    → A는 EPOLLOUT, B는 DEL, WRITING
A에 EPOLLOUT → send → 비면 proxy_resume: A는 EPOLLERR, B는 EPOLLIN, since = now, PROXYING
```

- B를 감시 `EPOLLERR`로 낮추지 않고 DEL한다. 낮추기만 하면 B의 RST가 `ERR|HUP`을 LT로 계속 알린다.
- B를 읽지 않으면 B의 커널 수신 버퍼가 차고, TCP가 수신 창 0을 알려 백엔드의 `send`가 멈춘다. 연결 하나의 사용자 메모리는 16KB로 고정된다.
- `proxy_resume`은 등록돼 있지 않을 때만 `loop_watch`를 부른다.

---
# 실패 처리

기준은 `head_done`이다. A로 보내기 전이면 상태 코드를 보낼 수 있고, 보낸 뒤면 A를 닫는다. 어느 경우든 B는 닫는다. 실패한 요청을 다시 보내지 않는다.

| 단계 | 상황 | 아는 방법 | 결과 |
|---|---|---|---|
| 준비 | `malloc`, `socket`(`EMFILE` 포함), B의 `epoll_ctl` 실패, 요청 버퍼 초과 | 반환값 | 500 |
| 연결 | `connect`가 바로 실패(`EINPROGRESS` 제외) | 반환값 | 502 |
| 연결 | `ECONNREFUSED`, `EHOSTUNREACH`, 커널의 `ETIMEDOUT` 등 | `SO_ERROR` | 502 |
| 연결 | 3초 초과 | sweep | 504 |
| 요청 전송 | `EPIPE`, `ECONNRESET` 등 | `send` | 502 |
| 요청 전송 | 10초 초과 | sweep | 504 |
| 전송 → 수신 전환 | B의 `epoll_ctl` 실패 | 반환값 | 500 |
| 헤더 기다림 | 헤더가 끝나기 전에 닫힘, 오류 | `recv` 0, 음수 | 502 |
| 헤더 기다림 | 10초 초과 | sweep | 504 |
| 헤더 검사 | 검사에 걸림, 한도 초과, 재작성 버퍼를 넘음 | 반환값 | 502 |
| 헤더 검사 | 재작성 버퍼 `malloc` 실패 | 반환값 | 500 |
| 본문 중계 | 끊김, 길이 모자람, 10초 초과, B의 `epoll_ctl` 실패 | `recv`, sweep, 반환값 | A를 닫음 |
| 언제든 | A의 RST, `send` 오류, 유휴 타임아웃, A의 `epoll_ctl` 실패 | 이벤트, 반환값, sweep | A와 B를 닫음 |

```C
// proxy.c. why는 로그에 쓴다
static int proxy_fail(struct conn *c, int status, const char *why) {
    if (c->up.head_done) { /* 로그 */ conn_close(c); return -1; }
    /* 로그 */
    c->last_active = time(NULL);       // B를 기다린 시간 때문에 다음 sweep이 응답을 보내기 전에 닫지 않게
    return conn_error(c, status);      // proxy_close(c) → free(wbuf), wbuf = NULL → conn_respond → conn_send
}
```

- `reason()`에 `502 Bad Gateway`, `504 Gateway Timeout`을 더하고, `http_handle`의 닫는 코드에 502, 504를 더한다.

---
# 엣지 케이스

규칙은 `/api/` → 8001이라고 본다.

| 상황 | 어디서 | 처리 |
|---|---|---|
| 규칙 없이 시작 | `main` | 지금과 똑같이 동작한다 |
| `/api=...`, `api/=...`, `/a/../b/=...`, `/api/=localhost:8001`, 포트 0, 65536, 빈 포트, `8001x`, `=` 없음, 같은 접두사 두 번, 17번째 규칙 | `route_add` | `route:` 메시지, `exit(1)` |
| `/api/`와 `/api/v2/` 순서 | `route_find` | 먼저 적은 것이 이긴다 |
| `/api/../admin`, `/api/%2e%2e/admin`, `/api/a%2f..%2f..%2fadmin` | 요청 검사 | 400 |
| `/api/a..b`, `/api/x?y=../z` | 요청 검사 | 전달. 세그먼트가 `..`이 아니고 쿼리는 보지 않는다 |
| `/api/%zz`, `/api/x#y`, 대상에 `0x01`, `0x80` | 요청 검사 | 400 |
| `/api/a%2Fb?x=%20` | 요청 | 원래 글자 그대로 전달 |
| `HEAD /api/x` / `POST /api/x` CL 없음 / `BREW /api/x` | 요청 검사 | 405 + `Allow: GET, POST` / 411 / 501 |
| `POST /index.html` | `http_handle` | 405 + `Allow: GET` (그대로) |
| 본문 있는 GET | 요청 | 본문과 `Content-Length`를 함께 전달 |
| `Host: a b/c`, 빈 `Host`, 헤더 값에 `0x01`, `0x7F` | 요청 검사 | 백엔드 경로는 400, 정적 경로는 지금처럼 통과 |
| 1.0 요청에 `Host` 없음 | 요청 | 백엔드 주소를 넣고 `HTTP/1.0`으로 보낸다 |
| `Connection: Content-Length`, `Connection: Host` | 요청 | 둘 다 새로 만들어 남는다 |
| `Connection: close` 요청 | 응답 | A를 닫는다. B에는 항상 `Connection: close` |
| 백엔드가 꺼짐 | `CONNECTING` | 502 |
| 백엔드가 `accept`를 안 부르고 큐에 자리가 있음 | `RECEIVING` | 연결과 `send`는 성공. 10~11초에 504 |
| accept 큐가 가득 참 | `CONNECTING` | 3~4초에 504 |
| 응답 없이 닫음, 헤더 중간에 닫음, RST | 헤더 전 | 502 |
| 상태 라인이 깨짐, LF만 쓰는 응답, CL 두 개, TE + CL, 8192바이트 넘는 헤더 | 헤더 검사 | 502 |
| `100` 뒤 `200`, `103` 여러 개 뒤 `200`이 한 번에 옴 | 헤더 | `1xx`를 버리고 `200`만 보낸다 |
| `101` | 헤더 검사 | 502 |
| `204` + `Content-Length: 0`, `304` + `Content-Length: 5` | 헤더 | 본문 없음. `204`는 CL을 빼고 `304`는 남긴다. A를 유지한다 |
| `Content-Length: 0`인 200 | 헤더 | B를 닫고 헤더만 보낸다 |
| chunked 응답 | 본문 | 그대로 중계. `Connection: close`, B가 닫히면 A를 닫는다 |
| 1.0 요청에 chunked로 답함 | 헤더 검사 | 502 |
| CL보다 적게 보내고 닫음 | 본문 | A를 닫는다. 클라이언트는 잘린 것을 안다 |
| CL보다 더 보냄 (헤더와 한 번에 / 나눠서) | 본문 | CL까지만 보낸다. 같은 A의 다음 요청이 정상이다 |
| 응답을 다 보냈는데 B를 안 닫는 백엔드 | 본문 | 길이를 알면 기다리지 않는다. 모르면 10~11초에 A를 닫는다 |
| 본문 중 10초 멈춤 | sweep | A를 닫는다 |
| 8MB 응답, 느린 클라이언트 | `conn_send` | B off → on을 반복한다. 받은 바이트가 같다. 다른 연결은 바로 응답받는다 |
| B가 꺼진 동안 백엔드가 다 보내고 닫음 / RST | `proxy_resume` | 다시 등록하면 이벤트가 온다. 남은 데이터를 `recv`한 뒤 0이나 오류로 끝난다 |
| 느린 클라이언트가 완전히 멈춤 | sweep | A의 유휴 검사가 A와 B를 닫는다 |
| 기다리는 중 클라이언트가 강제 종료(RST) | 이벤트 루프 | `ERR\|HUP` → A와 B를 닫는다 |
| 기다리는 중 클라이언트가 FIN | - | 모른다. 응답을 보내면 RST가 오거나 응답 타이머로 정리된다 |
| `nc -N`처럼 요청 뒤 쓰기만 닫음 | - | 응답을 정상으로 받는다 |
| 중계 중 클라이언트가 끊음 | `conn_send` | `EPIPE`/`ECONNRESET` → A와 B를 닫는다 |
| 한 배치에 A의 RST와 B의 `EPOLLIN` | `loop_unwatch` | B의 이벤트를 지운다 |
| `/api/a` + `/index.html` + `/api/b` 파이프라이닝 | `conn_process` | 응답이 순서대로 나간다. 백엔드를 기다리는 동안 다음 요청을 처리하지 않는다 |
| 백엔드를 10초 가까이 기다린 뒤 응답 | sweep | A가 유휴나 408로 닫히지 않는다 |
| 프록시 요청 중 fd가 한도에 닿음 | `proxy_start` | 500 + 닫기. 서버는 계속 동작한다 |
| accept가 멈춘 뒤 B가 닫힘 | `proxy_close` | accept를 재개한다 |
| backpressure로 B를 끔 | `loop_unwatch` | accept를 재개하지 않는다 |

---
# 구현 순서

모든 명령은 WSL의 레포 루트에서 실행한다. 단계마다 `make`가 경고 0개, `make test`와 `make clean && make test CFLAGS='-std=gnu11 -Wall -Wextra -g -fsanitize=address,undefined'`의 종료 코드가 0인 것을 확인한 뒤 커밋한다. `http_test.sh`가 정적 파일 동작이 깨지지 않았는지 확인한다.

### 1단계: `struct watch`와 이벤트 루프 (동작 그대로)
- `struct conn`의 `fd`, `events`를 `w`로 옮긴다. `loop_watch`, `loop_unwatch`가 `struct watch *`를 받는다.
- 배치 배열을 `static`으로 옮기고 배치에서 지우기, `e == 0` 건너뛰기, `loop_accept_resume`을 넣는다.
- 확인
	- 공통 확인(위의 두 `make test`). 기존 테스트를 고치지 않고 통과한다.
	- EMFILE: `(ulimit -n 12; ./server 8080 /tmp/www)`에 연결 20개를 열었다 닫으면 로그에 `accept paused` → `accept resumed`가 있고 이후 `curl`이 200이다.

### 2단계: `route.c`와 인자
- `route_add`, `route_find`, `route_pick`. `main`의 `argc >= 3`, 규칙 파싱. 아직 요청 처리에 연결하지 않는다.
- `tests/test_proxy.c`를 만들고 `make test`에 더한다.
- 확인
	- 단위 테스트: "라우팅"의 검사 표와 엣지 케이스 표의 `route_add`, `route_find` 행이 모두 있다.
	- `./server 8080 /tmp/www /api=127.0.0.1:8001; echo $?`가 `route:` 메시지와 1을 출력한다.
	- `./server 8080 /tmp/www /api/=127.0.0.1:8001`이 뜨고 `curl localhost:8080/`이 200이다.

### 3단계: 요청과 응답 다시 만들기
- 헤더 끝 찾기와 `Content-Length` 파싱을 함수로 꺼낸다. `http_keep_alive`, 메서드 검사 함수, `http_proxy_request`, `http_proxy_response`. `reason()`과 닫는 코드에 502, 504. `http_req.backend`와 `Allow`.
- 확인
	- 단위 테스트(`test_proxy.c`): 요청 재작성 표, 검사 순서 1~9, 응답 검사 표, 재작성 표, 응답의 끝 표의 모든 행. 입력을 한 바이트씩 늘려 가며 준 응답 헤더가 끝나기 전에는 0. `cap`을 작게 준 500과 -1. 가장 많이 늘어나는 요청(100줄, 8192바이트)이 16384에 들어가고 101줄은 431이다.
	- 단위 테스트: `{.status = 502, .minor = 1}`의 응답이 `Connection: close`다. `backend = 1`인 405가 `Allow: GET, POST`다.
	- `test_http.c`를 고치지 않고 통과한다.

### 4단계: 프록시 연결
- `proxy.c`, `struct upstream`, `CONN_PROXYING`, `conn_process`, `conn_send`, `conn_close`, `conn_error`. 연결, 요청 전송, 응답 중계, backpressure, 500과 502. sweep은 이 단계에서 `PROXYING`을 건너뛰기만 한다.
- `tests/backend.py`와 `tests/proxy_test.sh`를 만들고 `make test`에 더한다.
- 확인 (`proxy_test.sh`의 케이스로 넣는다)
	- `curl -s localhost:8080/api/echo?x=1`의 본문(백엔드가 받은 요청 그대로)이 요청 재작성 표와 같다.
	- `curl -s -o /dev/null -w '%{http_code}'`: 백엔드 꺼짐 502, 응답 없이 닫음 502, 깨진 응답 502, RST 502.
	- 한 `curl` 명령에 `/api/cl`, `/index.html`, `/api/cl`을 주면 A를 다시 쓴다(`-v`의 `Re-using`). chunked와 길이 없는 응답은 본문이 같고 A가 닫힌다.
	- CL보다 더 보내는 응답 뒤에 같은 A의 다음 요청이 정상이고, CL보다 적게 보내는 응답은 `curl`의 종료 코드가 18이다.
	- 8MB 응답의 sha256이 같다. `--limit-rate 200k` 중에 다른 `curl`이 1초 안에 응답받고 로그에 `off`, `on`이 있다.
	- 파이프라이닝 3개(`/api/a`, `/index.html`, `/api/b`)의 응답 순서. `nc -N`으로 보낸 요청이 응답을 받는다.
	- 정적 경로의 405는 `Allow: GET`, 백엔드 경로의 405는 `Allow: GET, POST`.

### 5단계: 타이머와 504
- `since`, `proxy_timeout`, sweep의 `PROXYING` 분기.
- 확인 (`proxy_test.sh`)
	- 응답하지 않는 백엔드: 10~11초에 504. `accept`하지 않는 백엔드(`listen(0)`에 연결 하나를 채움): 3~4초에 504. 시간은 벽시계(`time.time()`)로 잰다(lessons.md).
	- 본문 중 멈춘 백엔드: 10~11초에 A가 닫히고 `curl` 종료 코드가 18이다.
	- 9초 뒤에 답하는 백엔드: 200이다(A의 유휴, 408이 먼저 닫지 않는다).
	- 5초마다 나눠 보내는 응답(총 15초): 끝까지 받는다.

### 6단계: 자원 회수
- 확인 (`proxy_test.sh`). 코드가 바뀌지 않을 수도 있다.
	- 기다리는 중 `SO_LINGER {1, 0}`으로 닫은(RST) 클라이언트: 1초 안에 서버의 fd 수가 처음 값으로 돌아온다.
	- 중계 중 `curl --max-time 0.1`로 끊기, 기다리는 중 FIN으로 떠나기, 백엔드 강제 종료(`kill -9`) 뒤에도 12초 안에 fd 수와 `conn_count`가 처음 값으로 돌아온다.
	- 느린 클라이언트가 완전히 멈추면 10~13초에 A와 B가 닫힌다.
	- 동시 요청: `seq 200 | xargs -P 100 -I{} curl -s -o /dev/null -w '%{http_code}\n' localhost:8080/api/cl | sort | uniq -c`가 `200 200`
	- RST 클라이언트와 정상 요청을 섞은 동시 요청 300건 뒤 ASan 오류가 0개다(한 배치 안의 해제).
	- `ulimit -n`을 낮춰 프록시 요청을 동시에 보내면 500이 나오거나 기다렸다가 처리되고, 그 뒤 `curl`이 200이다.

### 7단계: Round-Robin (빼도 된다)
- `struct route`의 배열, 같은 접두사 더하기, `route_advance`.
- 확인
	- 단위 테스트: 순서, 되돌아감, 같은 주소 중복 거부, 9번째 거부, 검사에 걸린 요청은 세지 않음.
	- 백엔드 두 개에 요청 4번이 8001, 8002, 8001, 8002로 간다. 하나를 끄면 200과 502가 번갈아 나온다.

### 8단계: 종합 확인
- 공통 확인 두 가지.
- `valgrind --leak-check=full ./server 8080 <루트> /api/=127.0.0.1:8001`로 `proxy_test.sh`의 시나리오를 돌린 뒤 `kill $SP`(lessons.md)로 끝내면 `definitely lost: 0`이고 `Invalid read/write`가 0개다.
- 브라우저로 `/index.html`과 `/api/` 경로를 열어 확인한다.
- `docs/requirements.md` 품질 체크리스트의 해당 항목을 표시한다.

## 테스트

| 종류 | 파일 | 대상 |
|---|---|---|
| 단위 | `tests/test_proxy.c` | `route.c`, `http_proxy_request`, `http_proxy_response`, 502/504 응답 |
| 단위 | `tests/test_http.c` | 기존 파서. 리팩터링 뒤에도 그대로 통과 |
| 기능 | `tests/proxy_test.sh` | 4~7단계의 확인. 시작할 때 서버와 백엔드를 띄우고 끝에 종료한다 |
| 기능 | `tests/http_test.sh` | 정적 파일. 그대로 통과 |
| 메모리 | ASan/UBSan, valgrind | 위 두 기능 테스트를 그대로 |

- `tests/backend.py <포트>`는 경로로 동작을 고른다: `echo`(받은 요청을 본문으로), `cl`, `chunked`, `nolen`, `short`, `over`, `empty`(204), `304`, `1xx`, `101`, `bad`(깨진 상태 라인), `lf`, `dupcl`, `tecl`, `bighead`, `close`, `rst`, `hang`, `stall`(본문 중 멈춤), `slow`(9초 뒤), `drip`, `big`(8MB). `backend.py <포트> noaccept`는 `listen(0)`만 하고 `accept`하지 않는다.
- `proxy_test.sh`는 서버 로그에서 `AddressSanitizer`와 `runtime error`를 검사한다(`http_test.sh`와 같음). `lessons.md`의 실행 규칙을 따른다.

---
# 알려진 한계 (나중 작업)

- 응답 타이머는 바이트가 오지 않은 시간이다. 조금씩 계속 보내는 백엔드는 끝나지 않는다.
- 클라이언트가 FIN으로 떠나도 백엔드는 그 요청을 끝까지 처리하고, A와 B의 fd가 길게는 응답 타이머까지 남는다.
- 백엔드 주소를 자기 자신으로 설정하면 요청이 돌면서 fd를 소진하고 500이나 504가 나온다.
- 백엔드 하나가 죽으면 그 백엔드로 가는 요청은 502다. Round-Robin이면 일부 요청만 실패한다.
- 백엔드가 요청을 다 받지 않고 오류 응답을 보낸 뒤 닫으면 그 응답 대신 502가 나갈 수 있다.
- 길이도 chunked도 없는 응답이 잘리면 클라이언트가 알 수 없다. RST로 끝나도 우리는 A를 FIN으로 닫는다.
- 요청은 헤더와 본문을 합쳐 8192바이트까지다(지금 파서의 한도). 응답 헤더는 8192바이트까지다.
- `%252e%252e`는 한 번만 디코딩해 통과한다. 백엔드가 두 번 디코딩하거나 `\`를 구분자로 보면 막지 못한다.
- `/%61pi/`와 `/api/`는 규격으로 같은 URI지만 앞의 것은 정적 파일로 처리한다.
- 위조한 `Host`가 백엔드까지 간다. 백엔드가 허용할 이름을 검사해야 한다.
- 백엔드는 클라이언트의 주소를 모른다. 우리 앞에 다른 프록시가 있으면 그 프록시가 적은 주소도 지워진다.
- `Via`를 붙이지 않고, `Date`가 없는 응답에 넣지 않고, `1xx`를 버린다. RFC 9110의 MUST와 다르다.
- HEAD를 전달하지 않는다(405).
- B를 재사용하지 않아 요청마다 TCP 연결을 새로 맺는다. 프록시 요청은 fd를 2개 쓴다. accept가 fd를 다 쓰면 프록시 요청은 500이다.
- 파이프라이닝한 다음 요청이 커널 수신 버퍼에 남은 채 A를 닫으면 RST가 나가 클라이언트가 응답을 못 볼 수 있다(lingering close 없음). 502, 504뿐 아니라 chunked와 길이 없는 응답이 정상으로 끝난 뒤에도 생긴다. 이때는 응답 본문의 끝이 사라질 수 있다.
- `Connection` 값에 적힌 이름을 헤더마다 다시 훑는다(O(헤더 수²)). 줄 수를 100으로 막아 요청 하나의 시간을 제한한다(결정 20).

## 넣지 않은 것 (제안)

요구사항에 없어서 넣지 않았다. 필요해지면 따로 정한다.
- `X-Forwarded-For`, `Via` 추가
- 설정 파일, 백엔드 호스트 이름, IPv6
- 접두사 떼기, `Location` 고치기
- B의 keep-alive 연결 재사용, 실패한 GET의 재시도, 헬스 체크
- chunked 해석(A를 항상 유지), 응답 전체 시간 한도
- `conn_send`의 `close(file_fd)` 뒤 `loop_accept_resume()` 호출. [architecture-http.md](architecture-http.md)의 알려진 한계 하나가 한 줄로 없어진다
