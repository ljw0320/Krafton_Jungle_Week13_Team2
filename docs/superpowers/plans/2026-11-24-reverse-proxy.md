# Reverse Proxy 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `./server <port> <root> [<prefix>=<ip>:<port> ...]`가 접두사에 맞는 GET/POST를 백엔드로 전달하고 응답을 스트리밍으로 중계한다. 실패하면 설계대로 500/502/504로 답하거나 연결을 닫는다.

**Architecture:** `struct upstream`을 `struct conn` 안에 값으로 넣고 epoll의 `data.ptr`는 항상 `struct watch *`다. `conn_process`가 `route_find` → `proxy_start`로 나누고, `proxy.c`가 B의 상태(연결, 전송, 수신)를, `conn_send`가 A로의 전송과 backpressure를 맡는다. 요청과 응답을 다시 만드는 함수는 소켓을 모르고 `http.c`에 있다.

**Tech Stack:** C (gnu11), gcc 13.3, POSIX 소켓, `epoll` LT, Make, bash + curl/nc/python3 표준 라이브러리. WSL2 Ubuntu 24.04.

**Spec:** `docs/architecture-proxy.md`. 함수 책임, 검사 표, 상태 전이, 상태 코드는 설계 표를 따르고 여기서 다시 적지 않는다. 절 이름은 큰따옴표로 가리킨다.

## 실행 방법

- `docs/lessons.md`의 실행 규칙을 따른다. 명령은 `MSYS_NO_PATHCONV=1 wsl.exe -e bash -lc 'cd /mnt/d/kraftonjungle/kraftonjungle_week13/Krafton_Jungle_Week13_Team2 && <명령>'`로 감싼다.
- **공통 확인** (모든 코드 Task의 끝, 셋 다 종료 코드 0)
  1. `make >/tmp/make.log 2>&1 && ! grep -q warning /tmp/make.log`
  2. `make clean && make test`
  3. `make clean && make test CFLAGS='-std=gnu11 -Wall -Wextra -g -fsanitize=address,undefined'` (끝나면 `make clean && make`로 되돌린다)
- 포트: 서버 8080, 백엔드 8001 / 8002, `accept` 안 하는 백엔드 8003, 꺼진 백엔드 8009. 다른 서버를 끄고 돌린다. 두 확인을 동시에 돌리지 않는다.
- 실험 파일은 레포 밖에 둔다.

## Global Constraints

- `gcc -std=gnu11 -Wall -Wextra -g` 경고 0개. 표준 C, POSIX API만. Linux 전용, `#ifdef` 분기 없음.
- 상수 값과 위치는 "파일 구조"의 상수 표 그대로다.
- 핸들러 반환값: -1이면 `c`가 해제됐다. `proxy.c`에서 `conn_on_writable`이나 `conn_error`를 부른 뒤에는 0이어도 `u`를 쓰지 않고 그 값을 그대로 반환한다.
- 기존 `tests/test_http.c`, `tests/http_test.sh`는 고치지 않는다. 고쳐야 통과하면 멈추고 보고한다.
- 하지 않는 것: Forward Proxy, CGI, 캐싱, 죽은 백엔드 건너뛰기, 시그널 셧다운. "넣지 않은 것"의 항목도 넣지 않는다.
- 설계와 다르게 해야 하거나 설계에 없는 판단이 필요하면 멈추고 보고한다.

## 설계 점검에서 찾은 것 (2026-11-24 확정)

설계를 지금 코드와 대조해 찾았고 사용자가 "처리"를 확정했다. 해당 Task에서 `architecture-proxy.md`에 같이 적는다.

| # | 찾은 것 | 처리 | Task |
|---|---|---|---|
| 1 | 응답을 받아 `WRITING`으로 갈 때는 `last_active = now`를 적는데, `conn_error`(502, 504)로 갈 때는 적지 않는다. 10초를 기다린 뒤 504의 `send`가 `EAGAIN`이면(앞의 파이프라인 응답이 송신 버퍼에 차 있는 느린 클라이언트) `last_active`가 이미 10초 전이라 다음 sweep이 504를 보내지 못하고 닫는다 | `proxy_fail`이 `conn_error`를 부르기 전에 `last_active = time(NULL)`을 적는다. 한 줄이다. 408의 동작은 바뀌지 않는다 | 5 |
| 2 | 타이머의 "3~4초", "10~11초"는 짧다. sweep은 `epoll_wait(1000)` 뒤에 돌아서 초가 바뀐 뒤 최대 1초 늦다. 실제로는 최대 약 5초, 12초다. 기존 408 테스트도 `10 <= t < 13`을 쓴다 | 구현은 그대로. 테스트 범위를 연결 `3 <= t < 6`, 응답 `10 <= t < 13`, 멈춘 클라이언트 `10 <= t < 14`(기존 `stuck`과 같음), fd 회수는 13초 안으로 하고 설계의 숫자를 고친다 | 6, 7 |
| 3 | 7단계의 "검사에 걸린 요청은 세지 않음"은 `proxy_start` 안의 순서라서 `http.c`, `route.c`만 포함하는 단위 테스트로 확인할 수 없다 | 기능 테스트로 옮긴다: 정상 요청 사이에 400 요청을 넣어도 백엔드 순서가 그대로다 | 8 |
| 4 | "알려진 한계"의 RST(커널 수신 버퍼에 다음 요청이 남은 채 닫음)는 502, 504만 적었다. chunked와 길이 없는 응답도 정상으로 끝난 뒤 A를 닫으므로 같은 일이 생긴다. 이때는 응답 본문의 끝이 사라질 수 있다 | 구현은 그대로(lingering close는 범위 밖). 한계 문장만 고친다 | 5 |

설계에 없는 작은 세부 판단(틀렸을 때의 비용은 각각 한 줄이다)
- 이유가 없는 상태 라인(`HTTP/1.1 200\r\n`)은 `HTTP/1.1 200 \r\n`으로 쓴다(RFC 9112는 SP를 요구한다).
- `backend fd=N close` 로그는 `proxy_close`, `on`은 `proxy_resume`, `off`는 `conn_send`가 찍는다. `proxy_resume`이 A의 `EPOLLOUT`을 끄면 `EPOLLOUT off`도 찍어 `on`과 짝을 맞춘다. `proxy error`는 500, 502, 504에만 찍는다.
- `route_add`의 포트는 전부 숫자인지 본 뒤 `strtol`로 읽어 1~65535를 검사한다. 같은 주소는 문자열이 아니라 `sockaddr_in`의 주소와 포트로 비교한다(Task 8).
- `http_proxy_response`가 0을 반환하면 미리 할당한 새 버퍼는 `free`한다. 다음 이벤트에서 다시 할당한다.

## Review Focus

설계의 확인 목록에 없는 입력이다. 각각 적힌 Task의 테스트에 넣는다.

1. 파이프라이닝한 `/api/cl` 요청 20개를 한 번에 보냄 → 같은 `up` 자리에 B를 20번 열고 닫는다. 응답 20개, ASan 오류 없음 → Task 5
2. 본문 길이가 버퍼 경계에 걸림(0, 1, 16383, 16384, 16385, 32768바이트) → 받은 바이트가 같고 같은 A의 다음 요청이 정상 → Task 5
3. 응답 헤더가 정확히 8192바이트 / 8193바이트 → 200 / 502 → Task 5
4. 헤더와 본문을 합쳐 8192바이트인 POST가 여러 조각으로 옴 → 백엔드가 받은 본문이 같다 → Task 5
5. 길이를 모르는 8MB 응답 + 느린 클라이언트 → sha256이 같고 `off`, `on`을 반복한 뒤 A가 닫힌다 → Task 5

## 병렬과 순서

```
Task 1 (event_loop, conn) ─────────────┐
Task 2 (route, main, Makefile, 테스트) ─┴─ Task 3 (http.c, test_proxy.c) ─┬─ Task 5 → 6 → 7 → 8(선택) → 9 → 10
                                          Task 4 (backend.py, proxy_test) ─┘
```

- Task 1과 2는 소스 파일이 겹치지 않는다. Task 2는 계획을 커밋한 뒤 `git worktree add <레포 밖 경로> -b proxy-task2 shw`로 만든 worktree에서 한다. `isolation: worktree`는 기준이 `origin/main`(README만 있음)일 수 있어 쓰지 않는다. `git log -1`이 `shw`의 HEAD와 같은지 본 뒤 시작한다.
- Task 1과 2의 문서 수정은 둘 다 `architecture-tcp.md`를 고치므로 Task 2를 merge한 뒤 한 번에 한다. merge 뒤 공통 확인을 한 번 더 돌린다.
- Task 4는 `make`를 돌리지 않고 `backend.py`만 실행하므로 Task 3과 같은 체크아웃에서 동시에 돌린다. `proxy_test.sh`는 Task 5 전까지 실패하는 것이 맞고, 그래서 `make test`에는 Task 5에서 더한다.

## 담당과 강도

| Task | builder | verifier | 이유 |
|---|---|---|---|
| 1 | sonnet / medium | opus / high | 배치에서 지우기가 틀리면 use-after-free다. 구현은 설계 코드 그대로다 |
| 2 | sonnet / medium | sonnet / medium | 독립된 파싱 |
| 3 | opus / high | opus / high | request smuggling, 응답 경계 |
| 4 | sonnet / medium | Task 5와 함께 | 테스트 코드 |
| 5 | opus / high | opus / high | 상태 전이, 해제 순서, backpressure |
| 6 | sonnet / medium | opus / high | sweep 안의 해제. 구현은 약 30줄이다 |
| 7 | sonnet / medium | opus / high | 자원 회수와 해제의 마지막 확인 |
| 8 | sonnet / medium | sonnet / medium | 작은 배열과 인덱스 |
| 9 | - | sonnet / medium | 명령 실행 |
| 10 | 지적에 따라 | `final-reviewer` (낮추지 않음) | - |

- 고의 결함 검사는 `verifier`만 한다. 결함은 레포 밖 사본에 넣는다.
- Critical과 Important는 그 Task 안에서 고치고, Minor는 아래 "Minor 모음"에 적는다.

## 사용자 확인 지점

| 시점 | 내용 |
|---|---|
| 각 Task가 끝날 때 | 커밋, push 뒤 채팅 설명과 `week13\구현\Reverse Proxy\구현 노트.md` 추가. 설명하고 멈춘다 |
| Task 8 전 | Round-Robin을 넣을지 |
| Task 9 | 브라우저 확인은 사용자가 한다 |
| Task 10 뒤 | PR |

- 노트는 요구사항과 관련된 핵심 로직이 있는 Task만 쓴다. 테스트 코드만 만드는 Task 4는 노트 없이 Task 5와 같이 설명한다.

## 문서 수정 배정

| 줄 | Task |
|---|---|
| architecture-tcp.md의 **프록시** 표시: `struct watch`, `loop_watch` / `loop_unwatch`, `struct conn`의 `w`, `conn_close`의 `loop_accept_resume` | 1 |
| architecture-tcp.md의 설정·사용법·인자 검사, architecture-http.md의 사용법, CLAUDE.md "실행"의 "프록시 2단계부터" | 2 |
| architecture-http.md의 **프록시** 표시: 상태 코드 502/504, 함수 세 개와 `http_req.backend`, `Allow`, 닫는 코드. CLAUDE.md "HTTP는 … 세 함수로만" | 3 |
| architecture-tcp.md의 PROXYING, `proxy_close`, 배치 해제 한계. architecture-http.md의 `CONN_PROXYING`, `conn_error`, 라우팅 순서, `proxy_close`. CLAUDE.md "`conn_close` 순서"의 "프록시 단계부터". architecture-proxy.md에 점검 1, 4 | 5 |
| architecture-proxy.md에 점검 2 | 6 |
| architecture-proxy.md에 점검 3, "Round-Robin 단계 전에는" 같은 문장 정리. CLAUDE.md "명령어"의 테스트 설명 | 8 (빼면 9) |
| requirements.md 품질 체크리스트, 남은 **프록시** 표시 0개 확인 | 9 |

"프록시 단계에서 바뀜"을 뜻하던 표시는 지우고 지금 동작으로 고쳐 쓴다.

---

### Task 1: `struct watch`와 이벤트 루프 (설계 1단계, 동작 그대로)

**Files:**
- Modify: `src/event_loop.h`, `src/event_loop.c`, `src/conn.h`, `src/conn.c`, 문서(배정 표)

**Interfaces:**
- Produces: `enum watch_kind { W_CLIENT, W_BACKEND }`, `struct watch { enum watch_kind kind; int fd; uint32_t events; }`, `int loop_watch(struct watch *w, uint32_t events)`, `void loop_unwatch(struct watch *w)`, `void loop_accept_resume(void)`. `struct conn`의 첫 필드가 `struct watch w`이고 `fd`, `events` 필드는 없다. `conn.h`가 `event_loop.h`를 포함한다.

- [x] **Step 1: 구현** — "이벤트 루프"의 코드 그대로. `data.ptr`는 listen이면 NULL, 아니면 `struct watch *`. `W_BACKEND` 분기는 Task 5에서 넣는다. `conn_close`는 `loop_unwatch` → `close` → `loop_accept_resume` → 나머지 순서. `loop_unwatch`는 accept를 재개하지 않는다. `conn_send`의 `EPOLLOUT off` 로그는 `EPOLLOUT`이 등록돼 있었을 때만 찍는다.
- [x] **Step 2: 확인**
  - 공통 확인. 테스트 파일의 diff가 없다(`git diff --stat tests/`가 비어 있음).
  - EMFILE: 서버를 `(ulimit -n 12; exec ./server 8080 /tmp/www)`로 띄우고 연결 20개를 열었다 닫는다 → 로그에 `accept paused` 다음 `accept resumed`, 이후 `curl -s -o /dev/null -w '%{http_code}' localhost:8080/`이 `200`
- [x] **Step 3: verifier** — "닫는 순서"와 대조. 고의 결함: `loop_unwatch`의 배치 지우기 반복문을 빼면 무엇이 잡는지 본다. 이 단계에서는 연결 하나가 다른 연결을 닫지 않아 잡히지 않는 것이 맞다. 그렇게 보고하고 Task 7에서 다시 한다. `conn_close`의 `loop_accept_resume()`을 빼면 EMFILE 확인이 실패한다.
- [x] **Step 4: 커밋, push** — `refactor: struct watch와 배치에서 지우기`

### Task 2: `route.c`와 인자 (설계 2단계) — worktree `proxy-task2`

**Files:**
- Create: `src/route.h`, `src/route.c`, `tests/test_proxy.c`
- Modify: `src/main.c`, `Makefile`, `.gitignore`, 문서(배정 표)

**Interfaces:**
- Produces: `MAX_ROUTES 16`, `struct backend`, `struct route`(`struct backend b` 하나), `int route_add(const char *arg)`, `struct route *route_find(const char *target, size_t len)`, `const struct backend *route_pick(struct route *r)`. 규칙 배열은 `route.c`의 `static`이다.
- `tests/test_proxy.c`는 `../src/http.c`, `../src/route.c`를 `#include`한다. `route.c`의 `static` 이름이 `http.c`와 겹치지 않게 한다.

- [x] **Step 1: 실패하는 테스트** — 출력 형식은 `test_http.c`와 같다. 규칙 배열은 프로세스에 하나라서 거부 케이스를 먼저, 개수 케이스를 마지막에 둔다.
  - `route_add`가 -1: `/api=127.0.0.1:8001`, `api/=127.0.0.1:8001`, `=127.0.0.1:8001`, `/a/../b/=127.0.0.1:8001`, `/a/./b/=…`, `//=…`, `/a//b/=…`, `/a%20/=…`, `/a?/=…`, `/a#/=…`, 공백·`0x7F`·`0x80`이 든 접두사, `/api/=localhost:8001`, `/api/=127.0.0.1`, `/api/=127.0.0.1:`, `…:0`, `…:65536`, `…:8001x`, `…:-1`, `…:+1`, `…: 80`, `…:99999999999999999999`, `/api/=:8001`, `/api/=127.0.0.1.1:8001`, `/api/`, `=`, 빈 문자열
  - `route_add`가 0: `/api/=127.0.0.1:8001`(`addr`의 포트·주소, `name`이 `127.0.0.1:8001`), `/=127.0.0.1:65535`, `…:1`
  - 같은 접두사 두 번 -1, 16번째 0, 17번째 -1
  - `route_find`: `/api/`, `/api/users?x=1`, `/api/?x`은 맞음. `/api`, `/apix`, `/api?x=/`, `/%61pi/users`, `//api/users`, `/API/users`, `/`는 NULL(규칙 `/`가 없을 때). `/api/` 다음 `/api/v2/`를 적으면 `/api/v2/x`는 `/api/`. `len`보다 뒤의 글자를 보지 않는다(`"/api/users"`에 `len` 4 → NULL).
- [x] **Step 2: 실패 확인** — `make test` 종료 코드가 0이 아니다.
- [x] **Step 3: 구현** — `make test`는 `test_http` → `test_proxy` → `http_test.sh`를 `&&`로 잇는다. `main`: `argc < 3`이거나 포트가 틀리면 `usage: ./server <port> <root> [<prefix>=<ip>:<port> ...]`. `http_init` 다음, `server_listen` 전에 `argv[3..]`을 `route_add`하고 실패하면 `exit(1)`.
- [x] **Step 4: 확인** — 공통 확인(worktree에서는 1, 2의 단위 테스트까지. 8080을 쓰는 확인은 Task 1과 겹치지 않게 merge 뒤에 돌린다). `./server 8080 /tmp/www /api=127.0.0.1:8001; echo $?` → `route: /api=127.0.0.1:8001: <이유>`, `1`. `./server 8080 /tmp/www /api/=127.0.0.1:8001`이 뜨고 `curl localhost:8080/`이 200. 규칙 오류일 때 `ss -tln | grep -c ':8080 '`이 `0`.
- [x] **Step 5: verifier** — "라우팅" 표와 대조. 고의 결함: `len >= prefix_len` 검사를 빼면 단위 테스트(또는 ASan)가 실패한다.
- [x] **Step 6: 커밋, merge, push** — `feat: 라우팅 규칙과 인자`

### Task 3: 요청과 응답 다시 만들기 (설계 3단계)

**Files:**
- Modify: `src/http.h`, `src/http.c`, `tests/test_proxy.c`, 문서(배정 표)

**Interfaces:**
- Produces: `HTTP_HEAD_MAX 8192`, `http_req.backend`, `struct http_upresp`, `http_keep_alive`, `http_proxy_request`, `http_proxy_response` (시그니처는 "함수"의 코드 그대로). `http.c`는 여전히 `conn.h`, `route.h`, `proxy.h`를 모른다.

- [x] **Step 1: 리팩터링** — 헤더 끝 찾기, `Content-Length` 파싱, 메서드 검사(허용 메서드에 POST가 있는지를 인자로), 연결 유지 판단을 함수로 꺼낸다. `reason()`과 닫는 코드에 502, 504. 405의 `Allow`는 `req->backend`로 고른다. 이 시점에 `test_http.c`가 그대로 통과한다.
- [x] **Step 2: 실패하는 테스트** (`tests/test_proxy.c`)
  - 응답: `{.status = 502, .minor = 1}`, `504`가 `Connection: close`. `{.status = 405, .minor = 1, .backend = 1}`이 `Allow: GET, POST`, `backend = 0`이면 `Allow: GET`
  - 요청 재작성: 설계의 예시(입력과 출력 전체를 바이트로 비교), "요청 재작성" 처리 표의 모든 헤더 이름(대소문자 섞어서), `Connection` 여러 줄, `Connection: Content-Length, Host, Transfer-Encoding`, `Connection: x-a ,, X-B`, 1.0에서 `Host` 없음 / 있음, 본문 있는 GET, `X-Forwarded`(접두사보다 짧아 남음), 값이 빈 헤더, `\t값\t`, 값에 `0x80`
  - 요청 검사: 순서 1~8의 모든 줄과 엣지 케이스 표의 `요청 검사` 행. 순서 확인: `BREW` + `..` → 501, CL 없는 POST + `..` → 411, `HEAD` + `..` → 405, `%zz` + 나쁜 `Host` → 400. `cap`을 1씩 줄여 정확한 길이에서는 0, 1바이트 모자라면 500(버퍼 밖 쓰기는 ASan이 본다)
  - 가장 많이 늘어나는 요청: `a:\r\n` 반복으로 8192바이트 → `cap` 16384에 들어간다
  - 응답 검사: "헤더 검사" 표의 모든 줄. `HTTP/1.1 99`, `600`, `HTTP/2.0`, 이유에 `0x01` / `0x7F` / 탭, 이유 없음, CL `1, 1`, CL `+5`, CL 2^64, TE + CL, `minor` 0 + TE, 1.0 응답 + TE, 101, 8192바이트에 헤더 끝 없음 -1, 8191바이트에 없음 0
  - 응답 재작성: "헤더 재작성" 표의 모든 줄, "응답의 끝" 표의 모든 줄의 `has_len`·`body_len`·`keep_alive`(`keep` 0/1), `1xx`는 `out`을 쓰지 않고 원본 헤더 길이를 반환, `Set-Cookie` 두 줄 유지, `Connection: Transfer-Encoding`이어도 TE 유지
  - 덜 온 입력: 정상 응답 헤더를 한 바이트씩 늘리면 끝나기 전에는 모두 0. 뒤에 본문이 붙어도 반환값은 헤더 길이. `cap`을 작게 주면 -1
  - `http_keep_alive`: 1.1 기본 / `close`, 1.0 기본 / `keep-alive` / 둘 다
- [x] **Step 3: 실패 확인** — `make test` 종료 코드가 0이 아니다.
- [x] **Step 4: 구현** — 쓸 때마다 남은 크기를 검사한다. `Connection` 토큰 비교는 헤더 전체를 다시 훑는다("알려진 한계").
- [x] **Step 5: 확인** — 공통 확인, `git diff --stat tests/test_http.c`가 비어 있다.
- [x] **Step 6: verifier** — 검사 표와 한 줄씩 대조. 고의 결함 5개가 각각 실패로 잡힌다: `Host` 글자 검사 제거, `Connection` 토큰 제거 안 함, 응답 CL 중복 허용, 204에서 TE 남김, `..` 검사를 원래 대상에 함. 무작위 요청·응답 1,000건을 ASan으로 돌린다.
- [x] **Step 7: 커밋, push** — `feat: 프록시 요청과 응답 재작성`

### Task 4: 테스트 백엔드와 기능 테스트 골격 (설계 4단계의 테스트) — Task 3과 병렬

**Files:**
- Create: `tests/backend.py`, `tests/proxy_test.sh`

**Interfaces:**
- `python3 tests/backend.py <포트>`: 연결마다 스레드. `/<접두사>/<이름>[/<인자>][?쿼리]`의 이름으로 동작을 고른다. 이름은 설계 "테스트" 절의 목록에 더해 `port`(자기 포트를 본문으로), `size/<N>`(본문 N바이트), `head/<N>`(헤더가 정확히 N바이트인 200), `split`(응답을 1바이트씩), `bignolen`(길이 없이 8MB). 모르는 이름은 200 + 요청 대상을 본문으로(`Content-Length` 있음). 본문은 정해진 규칙으로 만들어 sha256을 예상할 수 있다.
- `python3 tests/backend.py <포트> noaccept`: `listen(0)`만 한다.
- `proxy_test.sh`: `http_test.sh`의 `check`, `cleanup`, 포트 대기, `SERVER_WRAP`, 로그의 `AddressSanitizer` / `runtime error` 검사, 마지막 줄 형식을 같게 쓴다. 서버 규칙은 `/api/=127.0.0.1:8001 /dead/=127.0.0.1:8009 /noaccept/=127.0.0.1:8003`이다.

- [x] **Step 1: `backend.py`** — 확인: 이름마다 `curl -s --path-as-is -i localhost:8001/api/<이름>`(깨진 응답은 `nc`)이 예상한 바이트를 준다.
- [x] **Step 2: `proxy_test.sh`** — 설계 4단계 확인 목록과 엣지 케이스 표의 `요청 검사`, `요청`, `헤더`, `헤더 검사`, `본문`, `conn_send`, `conn_process` 행, Review Focus 1~5를 케이스로 넣는다. 시간이 걸리는 5, 6단계 케이스는 넣지 않는다. 깨진 응답 502: `bad`, `lf`, `dupcl`, `tecl`, `bighead`, `101`, `close`, `rst`.
- [x] **Step 3: 실패 확인** — `bash -n tests/proxy_test.sh` 종료 코드 0. 실제 실행은 Task 5의 첫 단계다.
- [x] **Step 4: 커밋, push** — `test: 프록시 테스트 백엔드와 기능 테스트`

### Task 5: 프록시 연결 (설계 4단계)

**Files:**
- Create: `src/proxy.h`, `src/proxy.c`
- Modify: `src/conn.h`, `src/conn.c`, `src/event_loop.c`(`W_BACKEND` 분기), `Makefile`(`proxy_test.sh`), 문서(배정 표)

**Interfaces:**
- Consumes: Task 1의 `loop_*`, Task 2의 `route_*`, Task 3의 `http_*`
- Produces: "함수"의 `proxy.h`, `conn_error`. `proxy_timeout`은 Task 6에서 만든다.

- [x] **Step 1: 실패 확인** — `make && bash tests/proxy_test.sh` 종료 코드가 0이 아니다. 실패한 케이스 이름을 보고한다(프록시 케이스만 실패해야 한다).
- [x] **Step 2: 구현** — "`conn_process`", "`conn_send`에 더하는 것", "상태 전이", "`proxy_start`", "응답 헤더를 받았을 때", "닫는 순서", "실패 처리"의 코드와 표 그대로. `since`는 기록하되 sweep은 `CONN_PROXYING`을 건너뛰기만 한다. 408은 `conn_error(c, 408)`. 점검 1의 `last_active`.
- [x] **Step 3: 확인** — 공통 확인(`make test`에 `proxy_test.sh`가 들어 있다).
- [x] **Step 4: verifier** — 상태 전이 표, 실패 표, 닫는 순서와 한 줄씩 대조. 고의 결함이 각각 잡힌다: `conn_new`의 `up.w.fd = -1` 제거(fd 0), `conn_send`의 `loop_unwatch(&up.w)` 제거(backpressure), N바이트에서 자르기 제거(`over`), `conn_close`의 `proxy_close` 제거(fd 수). `conn_on_writable`·`conn_error` 뒤에 `u`를 읽는 코드가 없는지 본다. 점검 1의 `last_active`는 코드로 확인한다(`EAGAIN`을 정해진 시점에 만들기 어려워 테스트가 없다).
- [x] **Step 5: 커밋, push** — `feat: 리버스 프록시 연결과 응답 중계`

### Task 6: 타이머와 504 (설계 5단계)

**Files:**
- Modify: `src/proxy.h`, `src/proxy.c`, `src/conn.c`(`conn_sweep_idle`), `tests/proxy_test.sh`, 문서(배정 표)

**Interfaces:**
- Produces: `int proxy_timeout(struct conn *c, time_t now)` → 0 / -1

- [x] **Step 1: 실패하는 테스트** — 백그라운드로 동시에 돌리고 PID로 `wait`한다. `time.time()`으로 잰다. 범위는 점검 2.
  - `hang` → 504, 10~13초. `stall` → A가 10~13초에 닫힘, `curl` 종료 코드 18. `slow` → 200. `drip`(5초마다, 총 15초) → 끝까지 받음
  - `noaccept`: 먼저 연결 하나로 큐를 채운 뒤 → 504, 3~6초
  - `nolen`처럼 길이가 없는데 닫지 않는 응답 → 10~13초에 A가 닫힘
  - 504의 `Connection: close`와 본문 `<h1>504 Gateway Timeout</h1>`
- [x] **Step 2: 실패 확인** → **Step 3: 구현** ("타이머"의 표. 비교는 `now - since > 한도`) → **Step 4: 공통 확인**
- [x] **Step 5: verifier** — 고의 결함: `proxy_resume`의 `since = now` 제거, sweep에서 `PROXYING`도 유휴 검사를 하게 함(`slow`가 실패), 첫 본문 뒤 `last_active` 제거. 잡히지 않는 것은 보고한다.
- [x] **Step 6: 커밋, push** — `feat: 프록시 타이머와 504`

### Task 7: 자원 회수 (설계 6단계)

**Files:**
- Modify: `tests/proxy_test.sh`. 결함이 나오면 `src/`를 Task 5의 강도로 고친다.

- [x] **Step 1: 테스트** — 설계 6단계의 여섯 항목. fd 수는 `/proc/$SP/fd`, 연결 수는 로그 마지막의 `conn_count=`. RST 클라이언트는 1초(valgrind면 5초), 나머지는 13초 안. 멈춘 클라이언트는 10~14초. `ulimit` 케이스는 별도 서버를 `ulimit -n`을 낮춰 띄운다.
- [x] **Step 2: 공통 확인** — RST 300건 케이스가 ASan에서 통과한다.
- [x] **Step 3: verifier** — 고의 결함: `loop_unwatch`의 배치 지우기 반복문을 빼고 ASan으로 RST 300건을 돌린다. 잡히지 않으면 한 배치에 A의 RST와 B의 `EPOLLIN`이 같이 오는 상황을 만들어 확인한다(서버를 `SIGSTOP` → A의 RST와 B의 응답을 보냄 → `SIGCONT`). 이 방법으로 잡히면 케이스로 넣는다.
- [x] **Step 4: 커밋, push** — `test: 프록시 자원 회수`

### Task 8: Round-Robin (설계 7단계)

**Files:**
- Modify: `src/route.h`, `src/route.c`, `src/proxy.c`(`proxy_start`), `tests/test_proxy.c`, `tests/proxy_test.sh`, 문서(배정 표)

**Interfaces:**
- Produces: `MAX_BACKENDS 8`, `struct route`의 `struct backend b[MAX_BACKENDS]; int n, next;`, `void route_advance(struct route *r)`

- [x] **Step 1: 실패하는 테스트** — 단위: 같은 접두사에 백엔드 추가, 고르기와 순서 이동, 마지막에서 처음으로 돌아가기, 백엔드 한 개일 때 순서 유지, 같은 주소 거부(`:8001`과 `:08001` 포함), 8개 한도와 거부 뒤 규칙 보존, 규칙 16개를 모두 쓴 뒤 기존 접두사에 백엔드 추가. 기능: `/rr/port`가 8001, 8002, 8001, 8002 순으로 응답한다. 사이의 400, 405, 411 응답은 순서를 소비하지 않는다. 8002를 끄면 200과 502가 교대로 나온다.
- [x] **Step 2: 실패 확인** → **Step 3: 구현** (`route_advance`는 `http_proxy_request`가 0을 반환한 뒤, `socket` 전) → **Step 4: 공통 확인**
- [x] **Step 5: 고의 결함 검증** — `/tmp` 사본에서 `% n` 제거, `route_advance`를 검사 전과 연결 완료 뒤로 이동, 주소 중복 검사 제거, 한도를 9로 변경했다. 각 사본에서 관련 단위 또는 기능 테스트가 실패했다.
- [x] **Step 6: 커밋, push** — `feat: Round-Robin`, `docs: 프록시 7단계 반영`

### Task 9: 종합 확인 (설계 8단계)

담당: `verifier` (결함 수정은 `builder`)

- [x] **Step 1** — 공통 확인 3개 종료 코드 0. 일반 테스트와 ASan·UBSan 테스트는 각각 526 + 1794 + 168 + 119건을 통과했다.
- [x] **Step 2: valgrind** — `rm -f /tmp/vg.*.log` 뒤 `make clean && make && SERVER_WRAP='valgrind --leak-check=full --log-file=/tmp/vg.%p.log' bash tests/proxy_test.sh` 실행. 종료 코드 0, 117건 통과. 로그 2개 모두 `definitely lost: 0 bytes`, `Invalid read`와 `Invalid write` 0건이었다.
- [x] **Step 3: 브라우저 (사용자)** — 2026-11-25 Chrome에서 사용자가 확인했다. `/index.html`에 `home`이 표시됐고, `/api/echo`에는 백엔드가 받은 `GET /api/echo HTTP/1.1` 요청과 헤더가 표시됐다. `/rr/port`의 본문은 새로 고침 전 `8001`, 새로 고침 후 `8002`였다.
- [x] **Step 4: 문서** — 품질 체크리스트에 단위 테스트, curl·브라우저 기능 테스트, 동시 다중 클라이언트 접속, 메모리, 엣지 케이스를 표시했다. `grep -n '프록시' docs/architecture-tcp.md docs/architecture-http.md CLAUDE.md` 결과에 프록시 단계에서 바뀔 예정임을 나타내는 표시는 없었다.
- [x] **Step 5: 커밋, push** — 자동 확인 결과를 `docs: 프록시 종합 확인`으로 반영했다.

### Task 10: 최종 리뷰와 Minor 정리

- [ ] **Step 1** — `final-reviewer`에게 `git diff 5cc6594..HEAD`
- [ ] **Step 2** — 메인 세션이 지적마다 수정, 문서화, 범위 밖 중 하나로 정한다. Critical/Important와 "Minor 모음"을 `builder`에게 한 번에 맡긴다.
- [ ] **Step 3** — 공통 확인과 Task 9 Step 2. `verifier`가 고친 범위만 다시 본다.
- [ ] **Step 4: 커밋, push** — `fix: 프록시 최종 리뷰 정리`. PR은 사용자에게 묻는다.

---

## Minor 모음

- Task 1: `event_loop.c:12` 주석의 `loop_unwatch가 재개할`을 `loop_accept_resume`으로, `event_loop.h:8`의 쓰지 않는 `struct conn;` 삭제 (Task 5에서 고침)
- Task 5: `up_send`의 부분 전송 검사를 빼도 기능 테스트가 잡지 못한다. 요청이 8.4KB 이하라 보통 한 번에 나간다(사본에서 `send`를 잘라 확인함). `start_fail`의 `free` 누락은 valgrind(Task 9)로만 잡힌다
- Task 6: 테스트가 잡지 못하는 결함(비용이 커서 문서화만): `proxy_resume`·`up_send`·연결 완료의 `since` 제거, `relay`와 `proxy_fail`의 `last_active` 제거, 한도 비교의 `>`를 `>=`로(3회 중 1회만 실패). 원본은 코드 대조와 사본 실험으로 확인했다(SENDING 타이머 포함)
- Task 7: `event_loop.c`의 `ev_n = 0`은 없어도 동작이 같다(`for`가 끝나면 `ev_i == ev_n`). 설계 코드라 남겼고, 지울지는 Task 10에서 정한다. valgrind에서는 fd를 3개로 맞출 수 없어 `resume`, `idle`을 건너뛰고, fd 한도 케이스의 curl 56을 허용한다(valgrind의 `accept4`가 한도를 넘은 연결을 닫는 것을 strace로 확인)
