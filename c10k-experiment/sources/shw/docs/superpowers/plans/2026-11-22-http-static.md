# HTTP 파싱 + 응답 생성 + 정적 파일 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `./server <port> <root>`가 HTTP/1.x 요청을 엄격하게 파싱하고, 루트 아래 정적 파일을 `sendfile`로 보낸다. 형식 오류, 경로 탈출, 느린 요청에 설계의 상태 코드로 답한다.

**Architecture:** `main` → `http_init(root)` → `server_listen` → `loop_run`. `conn_process`가 `http_parse` → `http_handle` → `memmove` → `conn_send` 순서로 부른다. `conn_send`는 헤더(`wbuf`) → 본문(`sendfile`) 두 단계다. `event_loop.c`, `server.c`는 바뀌지 않는다.

**Tech Stack:** C (gnu11), gcc 13.3, POSIX 소켓, `epoll`, `sendfile`, Make, bash + curl/nc/python3 기능 테스트. WSL2 Ubuntu 24.04.

**Spec:** `docs/architecture-http.md` (결정 사항, 출력 형식, HTTP 인터페이스, `struct conn` 변경, 검사 순서, 경로 처리, 엣지 케이스, 구현 순서). 각 Task는 설계 "구현 순서"의 같은 번호 단계를 구현한다. 함수 책임, 상태 코드, 오류 처리는 설계 표를 따르고 여기서 다시 적지 않는다.

## 실행 방법

- Windows Git Bash에서는 `MSYS_NO_PATHCONV=1 wsl.exe -e bash -lc 'cd /mnt/d/kraftonjungle/kraftonjungle_week13/Krafton_Jungle_Week13_Team2 && <명령>'`로 감싼다 (lessons.md).
- 서버를 띄우는 확인은 한 번의 `bash -lc` 안에서 띄우기 → 확인 → `kill $SP`로 끝낸다. `pkill -f`를 쓰지 않는다 (lessons.md).
- 서버가 닫았는지, 몇 초 뒤 닫았는지는 python `recv`가 `b''`를 반환하는 시점으로 본다.
- 응답을 `grep`할 때는 `-a`를 붙인다.
- 경고 0개 확인: `make >/tmp/make.log 2>&1 && ! grep -q warning /tmp/make.log` (파이프로 `make`의 종료 코드를 가리지 않는다).
- 기능 테스트의 `secret`(`chmod 000`) 케이스는 root가 아닌 사용자로 돌린다. root는 권한과 관계없이 읽어서 403이 아니라 200이 나온다.

## Global Constraints

- 컴파일: `gcc -std=gnu11 -Wall -Wextra -g`. ASan은 `-fsanitize=address,undefined`를 더한다.
- 상수: `conn.h`에 `REQ_TIMEOUT_SEC 10`. `RBUF_SIZE`는 `http_parse`의 `cap` 인자로 넘긴다. `http.c`는 `conn.h`를 포함하지 않는다.
- `make test`: `test: server` 의존 → `gcc $(CFLAGS) -o tests/test_http tests/test_http.c && ./tests/test_http && bash tests/http_test.sh`. 단위 테스트는 `src/http.c`를 `#include`하므로 `src/*.c`와 함께 링크하지 않는다.
- ASan 기능 테스트는 `make asan && bash tests/http_test.sh`로 돌린다 (`make test`는 `server` 의존 때문에 ASan 없이 다시 빌드한다).
- 기능 테스트 포트는 8080 고정이다. 개발용 서버를 끄고 돌린다.
- 기능 테스트는 서버를 `$SERVER_WRAP ./server 8080 "$ROOT"`로 띄운다. `SERVER_WRAP`은 기본 빈 값이고, valgrind 확인(Task 8)에서만 쓴다. 서버가 뜰 때까지 고정 `sleep` 대신 포트가 열릴 때까지 기다린다(valgrind는 시작이 느리다).

## 출력 형식

- 서버의 사용법·루트 오류 메시지, 응답 헤더, 오류 본문, MIME은 `docs/architecture-http.md`의 "출력 형식" 절을 따른다 (2026-11-22 확정).
- 테스트 출력 (2026-11-22 확정, `tests/test_http.c`와 `tests/http_test.sh` 공통)
  - 실패마다 stderr에 `FAIL <케이스 이름>: 기대 <값>, 실제 <값>`
  - 끝에 `N passed, M failed`
  - 실패가 하나라도 있으면 종료 코드 1

## Review Focus

1. 경로 탈출: `/../etc/passwd`, `/%2e%2e/%2e%2e/etc/passwd`, 루트 밖 링크, 형제 디렉토리(`/srv/www2`)로 가는 링크 → 모두 403 → Task 5, 6
2. `calloc`이 `file_fd`를 0으로 채움 → 파일 없는 응답 뒤 `close(0)`이 일어나지 않는다 → Task 4
3. 전송 중에 클라이언트가 끊음 → `file_fd`가 새지 않는다(fd 수가 처음 값으로 돌아온다) → Task 4, 6
4. `CL: 1000000`인 모르는 메서드·POST → 413이 501/405보다 먼저 나가고, 본문이 다음 요청으로 해석되지 않는다 → Task 2, 3
5. `req.target`이 `rbuf`를 가리킴 → `http_handle`이 `memmove`보다 먼저 실행된다(ASan) → Task 3
6. 큰 파일을 오래 보낸 뒤 파이프라인으로 들어온 요청 → 408이 아니라 200 → Task 7

## 병렬과 순서

```
Task 1 → Task 2 → Task 3 ─┬─ Task 4 (conn.c/.h, http_test.sh) ─┬─ Task 6 → Task 7 → Task 8 → Task 9
                          └─ Task 5 (http.c, test_http.c)     ─┘
```

- Task 4와 Task 5만 파일이 겹치지 않는다. 둘 다 `isolation: worktree`로 동시에 돌린다. 같은 체크아웃에서 돌리면 한쪽의 `make`가 다른 쪽이 편집 중인 파일을 컴파일한다.
- 메인 세션은 각 worktree에서 verifier 검증 → 커밋 → `shw`에 merge한다. 둘 다 merge한 뒤 `shw`에서 `make test`를 한 번 돌린다.
- Task 4는 열 파일이 없어(파일을 여는 쪽이 Task 5) `sendfile` 경로를 Task 6에서 처음 실제로 지난다.
- 나머지는 `http.c` 또는 `conn.c`를 같이 바꾸므로 순서대로 한다. Task 7은 마지막 확인에 `big.bin` 픽스처가 필요해서 Task 6 뒤다.

## 강도

- 표시가 없으면 sonnet, 중간 강도다.
- `model: opus`, `effort: high`: Task 4(결정 12, `conn_close` 변경), Task 5(결정 9, 경로 안전), Task 7(결정 13, 요청 타이머). 그 Task의 builder와 verifier 호출에 모두 지정한다.
- 모든 구현 Task 뒤에 `verifier`가 검증한다. 고의 결함 검사는 verifier만 한다.

## 문서 수정 배정

| 줄 | Task |
|---|---|
| CLAUDE.md 첫 문단의 범위 (HTTP stub 문장 포함) | 1 |
| CLAUDE.md "실행" → `./server <port> <root>` | 1 |
| CLAUDE.md "그 밖의 검사"의 `valgrind ... ./server 8080` → 루트 인자 추가 (설계 목록에 없던 줄) | 1 |
| CLAUDE.md 제약 "HTTP 파싱, 정적 파일 … 범위가 아니다" → Proxy, CGI, 시그널 셧다운만 남김 | 1 |
| architecture-tcp.md "출력 형식"의 사용법 메시지, 인자 검사 | 1 |
| CLAUDE.md "테스트" → `make test` | 2 |
| architecture-http.md "파일 구조"의 `make test` 줄 → `test: server` 의존, 테스트 출력 형식 | 2 |
| CLAUDE.md 설계 결정 "`http_request_len`, `http_handle` 두 함수" → `http_init`, `http_parse`, `http_handle` | 3 |
| architecture-tcp.md "출력 형식"의 `/big` 본문, stub 응답 헤더 줄 | 3 |
| CLAUDE.md 설계 결정 "`conn_close` 순서" → `free(wbuf)` 뒤 `close(file_fd)` (설계 목록에 없던 줄) | 4 |

---

### Task 1: 인자와 `http_init` (설계 1단계)
담당: `builder` / 강도: 기본

**Files:**
- Modify: `src/main.c` (`argc == 3`, `http_init(argv[2])` 실패 시 `exit(1)`, 그다음 `server_listen`), `src/http.c`, `src/http.h` (`http_init`, `static` `root_real`/`root_len`), `CLAUDE.md`, `docs/architecture-tcp.md`

**Interfaces:**
- Produces: `int http_init(const char *root)` → 0 / -1 (메시지는 `http_init`이 찍는다)

- [x] **Step 1: 실패 확인** — `mkdir -p /tmp/www && ./server 8080 /tmp/www; echo $?` → 지금의 사용법 메시지, `1`
- [x] **Step 2: 구현** (설계 1단계. HTTP 처리는 stub 그대로)
- [x] **Step 3: 확인**
  - 경고 0개 명령 → 종료 코드 0
  - 아래 각각이 출력 형식 표의 메시지를 stderr에 찍고 `echo $?`가 `1`
    - `./server`, `./server 8080`, `./server 8080 /tmp/www extra`, `./server abc /tmp/www`, `./server 70000 /tmp/www` → `usage: ./server <port> <root>`
    - `./server 8080 /nonexistent` → `realpath: /nonexistent: No such file or directory`
    - `./server 8080 Makefile` → `root: Makefile: Not a directory`
    - `./server 8080 /`, `./server 8080 /tmp/..` → `root: /: not allowed` (`realpath` 결과로 판단)
  - `./server 8080 /tmp/www` 띄운 뒤 `curl -s localhost:8080/` → `hello`
  - `ln -sfn /tmp/www /tmp/wwwlink && ./server 8080 /tmp/wwwlink`, `./server 8080 /tmp/www/`가 뜬다
  - 루트 오류일 때 포트를 열지 않는다: `./server 8080 /nonexistent` 직후 `ss -tln | grep -c ':8080 '` → `0`
- [x] **Step 4: 문서** — 문서 수정 배정 표의 Task 1 줄
- [x] **Step 5: 커밋, push** — `feat: 정적 루트 인자와 http_init`

### Task 2: `http_parse` + 단위 테스트 (설계 2단계)
담당: `builder` (고의 결함 검사는 `verifier`) / 강도: 기본

**Files:**
- Create: `tests/test_http.c`
- Modify: `src/http.c`, `src/http.h` (`enum http_method`, `struct http_req`, `http_parse`), `Makefile` (`test` 타깃, 이 Task는 단위 테스트만), `.gitignore` (`tests/test_http`), `CLAUDE.md`, `docs/architecture-http.md`

**Interfaces:**
- Produces: `ssize_t http_parse(const char *buf, size_t len, size_t cap, struct http_req *req)` (설계 "HTTP 인터페이스")
- 연결은 아직 stub `http_request_len`을 쓴다

- [x] **Step 1: 실패 확인** — `tests/test_http.c`를 먼저 쓰고 `make test` → 종료 코드가 0이 아니다
- [x] **Step 2: 구현** (검사 순서 1~7번, 숫자 세기 루프로 CL 파싱, `cap - head_len` 비교)
- [x] **Step 3: 확인**
  - 경고 0개 명령 → 종료 코드 0
  - `make test` → 종료 코드 0, 마지막 줄 `N passed, 0 failed`
  - `make clean && make test CFLAGS='-std=gnu11 -Wall -Wextra -g -fsanitize=address,undefined'` → 종료 코드 0, 출력에 `AddressSanitizer`, `runtime error`가 없다
  - 케이스: 엣지 케이스 표의 `http_parse` 행 전부. 경계값으로 헤더 + 본문 8192 / 8193바이트, 정확히 8192바이트의 414 / 431, 정상 요청의 모든 앞부분(길이 1 ~ n-1)이 0을 반환
- [x] **Step 4: verifier 고의 결함 검사** — 기대값 하나를 틀리게 바꿈 → `make test`가 0이 아닌 값으로 끝남, bare LF 검사를 지움 → 해당 케이스가 `FAIL`. 둘 다 되돌린다
- [x] **Step 5: 문서** — 문서 수정 배정 표의 Task 2 줄
- [x] **Step 6: 커밋, push** — `feat: HTTP 요청 파서와 단위 테스트`

### Task 3: 응답 생성과 연결 교체 (설계 3단계)
담당: `builder` / 강도: 기본

**Files:**
- Create: `tests/http_test.sh`
- Modify: `src/http.c`, `src/http.h` (`struct http_resp`, 새 `http_handle`, `http_request_len`·`/big`·`hello` 제거), `src/conn.c` (`conn_respond`, `conn_process`가 `http_parse` 사용), `Makefile` (`test`가 `http_test.sh`도 실행), `CLAUDE.md`, `docs/architecture-tcp.md`

**Interfaces:**
- Produces: `int http_handle(const struct http_req *req, struct http_resp *resp)`, `static int conn_respond(struct conn *c, const struct http_req *req)`
- 이 Task의 GET은 경로를 보지 않고 모두 404다. `resp.file_fd`는 항상 -1이고, `struct conn`에 파일 필드가 아직 없어서 `conn_respond`는 설계 코드의 `file_fd`/`file_len`/`file_off` 줄을 빼고 쓴다(Task 4에서 넣는다)

- [x] **Step 1: 실패 확인** — `printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n' | nc -N 127.0.0.1 8080 | head -1` → `HTTP/1.1 200 OK` (stub)
- [x] **Step 2: 구현** (검사 순서 8~10번, 연결 유지 판단, 헤더 순서, `Date`, 오류 본문. `conn_process`의 순서는 `http_parse` → `http_handle` → `memmove` → `conn_send`. "rbuf가 찼는데 미완성이면 닫기" 줄은 남긴다)
- [x] **Step 3: `tests/http_test.sh`** — `mktemp -d` 루트, 서버 띄움(`$SERVER_WRAP`, 포트 대기) → 케이스 → `kill $SP`, 출력 형식은 위 "테스트 출력"
- [x] **Step 4: 확인** — `make test` 종료 코드 0. 설계 3단계 "확인"의 항목이 모두 케이스로 들어 있다
  - 404, 헤더 순서, `Date` 정규식
  - 닫는 응답 400(`Host : x`) / 505(`HTTP/2.0`) / 501(`BREW`) / 501(TE) / 411(POST, CL 없음): `Connection: close`, 서버가 닫음
  - 413(헤더만 `Content-Length: 1000000`), 414 / 431(정확히 8192바이트)
  - 405 + `Allow: GET` 뒤 같은 연결의 다음 요청에도 응답, `POST ... CL: 5 hello` + `GET` → 405와 404
  - 1.0 + 헤더 없음 → `close`, 1.0 + `Keep-Alive` → `keep-alive`
  - `curl -sv localhost:8080/ localhost:8080/ 2>&1 | grep -c 'Re-using existing connection'` → `1`
  - `make asan && bash tests/http_test.sh` → 종료 코드 0, 서버 로그에 `AddressSanitizer`, `runtime error`가 없다 (Review Focus 5)
- [x] **Step 5: 문서** — 문서 수정 배정 표의 Task 3 줄
- [x] **Step 6: 커밋, push** — `feat: HTTP 응답 생성과 연결 교체`

### Task 4: `sendfile` 전송 (설계 4단계 중 결정 12) — Task 5와 병렬
담당: `builder` / `model: opus`, `effort: high` (`conn_close` 순서, fd 소유권, 상태 전이) / `isolation: worktree`

**Files:**
- Modify: `src/conn.h` (`file_fd`, `file_off`, `file_len`), `src/conn.c` (`conn_new`에서 `file_fd = -1`, `conn_close` 4번 줄, `conn_respond`가 파일 필드를 옮겨 담음, `conn_send` 두 단계), `tests/http_test.sh` (fd 0 케이스), `CLAUDE.md`

**Interfaces:** 공개 함수 변경 없음

- [x] **Step 1: 실패 확인** — 없음. 파일을 여는 쪽이 Task 5라 이 Task만으로는 관찰할 동작이 없다. `sendfile` 경로는 Task 6에서 확인한다
- [x] **Step 2: 구현** (설계 "연결 상태 전이"의 WRITING ①②, 이벤트 하나에 `send`/`sendfile` 한 번, `sendfile` 0 반환 → `conn_close`, 다 보내면 `close(file_fd)` → `file_fd = -1` → `free(wbuf)`. 0바이트 파일은 `sendfile`을 부르지 않는다)
- [x] **Step 3: 확인**
  - 경고 0개 명령 → 종료 코드 0
  - `make test` → 종료 코드 0. 새 케이스: 404 응답을 3개 보낸 뒤 `[ -e /proc/$SP/fd/0 ]` (Review Focus 2)
  - `make asan && bash tests/http_test.sh` → 종료 코드 0, ASan 오류 없음
- [x] **Step 4: verifier** — `conn_send`/`conn_close`를 상태 전이 그림과 대조한다. 고의 결함: `conn_new`의 `file_fd = -1`을 지우면 fd 0 케이스가 `FAIL`인지 확인하고 되돌린다
- [x] **Step 5: 문서** — 문서 수정 배정 표의 Task 4 줄
- [x] **Step 6: 커밋, merge, push** — `feat: sendfile로 파일 본문 전송`

### Task 5: 경로 해석과 MIME (설계 4단계 중 결정 9~11) — Task 4와 병렬
담당: `builder` / `model: opus`, `effort: high` (경로 안전) / `isolation: worktree`

**Files:**
- Modify: `src/http.c` (경로 처리 표 1~11번의 `static` 함수, MIME, GET 분기에서 200 + `file_fd`), `tests/test_http.c` (디코딩, 정규화, MIME 케이스)

**Interfaces:**
- `http_handle`의 GET이 경로를 해석해 200(`resp.file_fd`, `resp.file_len`) / 400 / 403 / 404 / 500을 낸다. 오류를 정했으면 연 fd를 닫는다
- `static` 함수 이름은 builder가 정하고 보고한다

- [x] **Step 1: 실패 확인** — 단위 테스트 케이스를 먼저 쓰고 `make test` → 종료 코드가 0이 아니다
- [x] **Step 2: 구현** (경로 처리 표 그대로. 검사는 디코딩한 뒤, 디코딩은 한 번, index.html도 6~7번을 다시 거친다)
- [x] **Step 3: 확인**
  - 경고 0개 명령 → 종료 코드 0
  - `make test` → 종료 코드 0. 단위 케이스:
    - 바이트·디코딩: `0x80` 이상, `#`, 공백 → 400 / `%20` → 공백 / `%zz`, 끝의 `%`, `%0`, `%00` → 400 / `%2f` → `/` / `%252e` → `%2e` / `?` 뒤 제거
    - 정규화: `/img/../x` → `x`, `//a`, `/./a` → `a`, `/../`, `/a/../../` → 403, 끝 `/` 기록 (`/docs/` 참, `/docs` 거짓, `/docs%2f` 참)
    - 길이: 정규화 결과가 `PATH_MAX`를 넘음 → 404
    - MIME: `.html .HTML .css .js .png .PNG .jpg .jpeg`, 확장자 없음, `/v1.2/readme`, `notes..txt` → 기본값
  - `make clean && make test CFLAGS='... -fsanitize=address,undefined'` → 종료 코드 0
  - 기존 기능 테스트(빈 루트라 `GET /`는 계속 404)가 그대로 통과한다
- [x] **Step 4: verifier** — 경로 처리 표와 한 줄씩 대조한다. 고의 결함: 접두사 비교의 `real[n] == '/'` 조건을 지우면 형제 디렉토리 케이스가 잡히는지(Task 6 전이면 단위 테스트에 `http_init`으로 임시 루트를 두고 확인) 확인하고 되돌린다
- [x] **Step 5: 커밋, merge, push** — `feat: 요청 경로 해석과 MIME`

### Task 6: 정적 파일 기능 테스트 (설계 4단계 확인)
담당: `builder` / 강도: 기본

**Files:**
- Modify: `tests/http_test.sh` (픽스처와 케이스. Task 3의 `GET /` → 404 케이스는 `index.html`이 생기므로 없는 경로로 바꾼다). 케이스가 결함을 찾으면 그 코드를 맡은 Task의 강도로 고친다

**Interfaces:** 변경 없음

- [x] **Step 1: 픽스처** — 설계 4단계 목록을 스크립트가 임시 루트에 만든다(저장소에 넣지 않음). `out` 링크의 대상과 형제 디렉토리(`<루트>2`)도 임시 경로에 만든다
- [x] **Step 2: 확인** — `make test` 종료 코드 0. 설계 4단계 "확인"의 항목이 모두 케이스로 들어 있다
  - 파일마다 `curl -s -o <파일> -w '%{http_code} %{content_type}'` → 200과 MIME, `cmp`로 원본과 같다. `zero` → `Content-Length: 0`
  - 엣지 케이스 표의 경로 행(400/403/404/200) 전부, `curl --path-as-is`
  - FIFO 요청을 연 채로 다른 `curl localhost:8080/`이 1초 안에 200
  - `--limit-rate 200k`로 `big.bin`을 받는 중에 `curl localhost:8080/`이 1초 안에 200, 로그에 `EPOLLOUT on` → `off`, `curl -s localhost:8080/big.bin | wc -c` → `8388608`
  - `curl --max-time 0.1 --limit-rate 1M localhost:8080/big.bin` 뒤 `ls /proc/$SP/fd | wc -l`이 처음 값 (Review Focus 3)
  - `make asan && bash tests/http_test.sh` → 종료 코드 0, ASan 오류 없음
- [x] **Step 3: 커밋, push** — `test: 정적 파일 기능 테스트`

### Task 7: 요청 타이머와 408 (설계 5단계)
담당: `builder` / `model: opus`, `effort: high` (`req_start` 기록 시점, sweep 중 응답과 닫기)

**Files:**
- Modify: `src/conn.h` (`req_start`, `REQ_TIMEOUT_SEC 10`), `src/conn.c` (`req_start` 기록 규칙, `conn_sweep_idle`의 408 분기 → `conn_respond` → `conn_send`), `tests/http_test.sh` (타이머 케이스)

**Interfaces:** 공개 함수 변경 없음. `conn_respond`를 sweep도 쓴다

- [x] **Step 1: 실패 확인** — 타이머 케이스를 먼저 넣고 `make test` → 종료 코드가 0이 아니다 (Slowloris가 408을 받지 못한다)
- [x] **Step 2: 구현** (`struct conn` 변경 절의 기록 시점 표, sweep에서 `next`를 먼저 저장)
- [x] **Step 3: 확인** — `make test` 종료 코드 0. 세 케이스를 백그라운드로 동시에 돌리고 `wait`로 모은다. 시간은 python `recv`로 잰다
  - 1바이트씩 4초 간격 → 요청 시작 후 10~11초에 `HTTP/1.1 408 Request Timeout`, 이어서 `b''`
  - 아무것도 보내지 않음 → 10~11초에 응답 없이 `b''`
  - `GET /big.bin`과 `GET /index.html`의 앞부분을 한 번에 보내고 15초에 걸쳐 천천히 읽은 뒤 두 번째 요청의 나머지를 나눠 보냄 → 두 번째 응답이 200 (Review Focus 6)
  - `make asan && bash tests/http_test.sh` → 종료 코드 0, ASan 오류 없음
- [x] **Step 4: verifier** — 고의 결함: READING으로 돌아올 때의 `req_start = now`를 지우면 세 번째 케이스가 `FAIL`인지 확인하고 되돌린다
- [x] **Step 5: 커밋, push** — `feat: 요청 타이머와 408`

### Task 8: 종합 확인 (설계 6단계)
담당: `verifier` (결함 수정은 `builder`) / 강도: 기본

**Files:** 코드 변경 없음. 결함이 나오면 해당 Task의 강도로 고친다

- [x] **Step 1** — `make test` → 종료 코드 0
- [x] **Step 2: ASan** — `make clean && make asan && bash tests/http_test.sh` → 종료 코드 0, `AddressSanitizer`, `runtime error`가 없다
- [x] **Step 3: valgrind** — `make && SERVER_WRAP='valgrind --leak-check=full --log-file=/tmp/vg.log' bash tests/http_test.sh` → 종료 코드 0, `grep 'definitely lost' /tmp/vg.log` → `0 bytes` (`still reachable`은 허용). 스크립트는 `kill $SP`로 끝낸다 (lessons.md)
- [x] **Step 4: 동시 접속** — 픽스처 루트로 서버를 띄우고 `seq 200 | xargs -P 100 -I{} curl -s -o /dev/null -w '%{http_code}\n' localhost:8080/ | sort | uniq -c` → `200 200`
- [x] **Step 5: 원래대로 돌아오는지** — Step 4 전후 `ls /proc/$SP/fd | wc -l`이 같고, 마지막 로그가 `conn_count=0`
- [x] **Step 6: 브라우저 (사용자)** — 종료 코드로 판정할 수 없어서 사용자가 직접 한다. 픽스처 루트의 `index.html`을 열고 개발자 도구에서 CSS, JS, PNG가 200이고 `Content-Type`이 MIME 표와 같은지 본다
- [x] **Step 7: 커밋, push** (고친 것이 있을 때) — `fix: 종합 확인에서 찾은 결함` (고친 코드가 없어 해당 없음)

### Task 9: 최종 리뷰와 Minor 정리
담당: `final-reviewer` (강도를 낮추지 않는다) → `builder` → `verifier`

**Files:** 리뷰에서 지적된 파일

- [x] **Step 1: 최종 리뷰** — `final-reviewer`에게 `git diff 5758386..HEAD` 범위를 맡긴다
- [x] **Step 2: 판단** — 메인 세션이 지적마다 수정, 문서화, 범위 밖 중 하나로 정한다. Critical/Important와 아래 "Minor 모음"을 `builder`에게 한 번에 맡긴다
- [x] **Step 3: 확인** — 경고 0개 명령, `make test`, `make asan && bash tests/http_test.sh` → 모두 종료 코드 0. `verifier`가 고친 범위만 다시 본다
- [x] **Step 4: 커밋, push** — `fix: 최종 리뷰 Minor 정리`

---

## Minor 모음

Task마다 verifier가 보고한 Minor를 여기에 한 줄씩 모은다. Task 9에서 한 번에 고친다.

아래 항목은 "범위 밖"이라고 적은 것을 빼고 모두 Task 9에서 고쳤다.

- (Task 1) `src/main.c:17`: 포트 앞 공백(`" 80"`)을 `strtol`이 통과시킨다. 이전 코드와 같은 동작이다 → 범위 밖(설계의 인자 검사 그대로이고 해가 없다)
- (Task 2) `src/http.c:166`: `cap - head_len`은 `len > cap`이면 언더플로한다. 지금 호출자는 `len <= RBUF_SIZE`를 지킨다. 전제를 `http.h` 주석에 적거나 파서에서 막는다
- (Task 2) 계획 Task 2 Step 4 "`make test`가 1로 끝남": 테스트 바이너리는 1이고 `make`는 2로 끝난다. 문구를 "0이 아닌 값"으로 고친다
- (Task 3) `tests/http_test.sh`: 413을 POST로만 검사한다. Review Focus 4의 모르는 메서드(`BREW`) + `CL: 1000000` → 413, 뒤 바이트 미해석 케이스를 넣는다 (메인 세션이 직접 확인: 통과)
- (Task 3) `tests/http_test.sh`: 파이프라인은 요청 2개만 검사한다. 한 번의 send에 3개 케이스를 넣는다 (메인 세션이 직접 확인: 통과)
- (Task 4) `docs/architecture-tcp.md:98`과 "`conn_close` 순서" 절: `file_fd`를 아직 없는 것으로 적었다. `close(file_fd)` 줄을 더한다
- (Task 5) `tests/test_http.c`: `PATH_MAX` 경계의 4095와 4096이 둘 다 404라 `n >= sizeof(full)`의 off-by-one을 구분하지 못한다. 4095바이트짜리 실제 파일 200 케이스를 넣는다 (verifier가 직접 확인: 4095 → 200, 4096 → 404)
- (Task 5) `docs/architecture-http.md`: 경로를 만드는 `snprintf`의 잘림(음수 포함)은 404, 응답 헤더의 잘림은 500이라고 구분해 적는다
- (Task 5) `docs/architecture-http.md` 엣지 케이스: `/index.html/`(일반 파일 + 끝 `/`) → 200, `/docs/.` → 404, 권한 111 디렉토리의 `/dir/` → 403을 적는다
- (Task 4) CLAUDE.md 첫 문단 "HTTP 처리는 구현 중이라 아직 stub이다"를 고친다 (Task 3에서 넘어옴)
- (Task 6) `tests/http_test.sh`: 전송 중 파일이 줄어듦(`sendfile` 0 → 닫힘) 케이스를 넣는다 (메인 세션이 실제 서버로 직접 확인: 4194304바이트로 줄이면 서버가 닫고 fd 수 5 → 5). `realpath`와 `open` 사이 삭제는 시점을 맞출 수 없어 범위 밖이다 (Task 5 verifier가 `ENOENT` 주입으로 확인)
- (Task 7) 계획 Task 7 Step 4와 `docs/architecture-http.md` 5단계 "확인": "완전한 `GET /index.html`" 케이스는 `req_start = now`를 지워도 통과한다. 두 번째 요청을 나눠 보내는 케이스로 문구를 고친다 (테스트에는 이미 들어 있고, 고의 결함을 이 케이스가 잡는다)
- (Task 7) `src/http.h`: `status` 주석의 코드 목록에 408을 더한다
- (Task 7) `tests/http_test.sh`: 타이머 구간에서 SIGTERM으로 중단하면 python 클라이언트가 최대 20초쯤 남는다. `cleanup`에서 같이 끝낸다
- (Task 7) `tests/http_test.sh`: `t0`을 `connect` 뒤에 재서 초 경계에서 10초 하한에 못 미칠 수 있다(추정). `t0`을 `connect` 앞으로 옮긴다

최종 리뷰 (Task 9)
- Important 1: 느리게 읽는 클라이언트의 전송이 유휴 타이머에 잘림 → `SIOCOUTQ`로 고침(사용자 결정)
- keep-alive 파일 응답이 약 44ms 늦음(Nagle + 지연 ACK) → `MSG_MORE` 대신 `TCP_NODELAY`로 고침(사용자 결정)
- Minor: EPOLLOUT 테스트가 부분 전송을 못 잡음 → `send` 횟수로 고침, 낡은 문서 → 고침, request timeout 로그 → 문서에 추가, Task 8 항목 삭제(`tests/http_test.sh`가 이미 `AddressSanitizer`/`runtime error`를 검사한다)
