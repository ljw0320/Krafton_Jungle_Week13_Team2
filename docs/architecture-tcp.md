범위: 웹 서버 과제 중 ① TCP 소켓 서버, ② 동시 요청 처리를 구현한다. Proxy와 시그널 셧다운은 이번 범위에서 빠진다. HTTP 파싱과 정적 파일은 세 함수(`http_init`, `http_parse`, `http_handle`)로 붙고, 자세한 것은 [architecture-http.md](architecture-http.md)에 있다. 이 문서의 5단계 stub은 HTTP 3단계에서 교체되었다.

개요: [overview.md](overview.md), 요구사항: [requirements.md](requirements.md)

---
# 결정 사항

| 항목 | 결정 | 이유 |
|---|---|---|
| 동시성 모델 | `epoll` LT, 단일 스레드 이벤트 루프 | 개발 환경이 WSL2라 Linux 전용이어도 된다. LT에서는 실수해도 처리가 느려질 뿐 연결이 멈추지 않는다 |
| 개발 환경 | WSL2 Ubuntu 24.04, gcc 13.3 | 확인 명령은 모두 WSL 안에서 실행한다 |
| 설정 | CLI 인자 `./server <port> <root>` | 정적 루트는 두 번째 인자로 받는다. 라우팅 규칙이 생기면 그때 설정 파일을 검토한다 |
| 연결 목록 | `struct conn` 양방향 리스트 | 제거가 O(1)이다. 타임아웃 검사는 1초마다 전체를 순회한다 |
| 수신 버퍼 | `rbuf[8192]` 고정 | 버퍼가 찼는데도 요청이 완성되지 않으면 414/431로 답하고 닫는다(`http_parse`) |

환경 확인 결과 (2026-11-20)
- WSL: Ubuntu 24.04, gcc 13.3, make, curl, nc, ss, python3가 있다. `ab`는 없다. `valgrind` 3.22, `strace` 6.8은 2026-11-20 Task 9 전에 설치했다.
- WSL의 `ulimit -n`은 10240이다. EMFILE 테스트는 한도를 직접 낮춰서 한다.
- 레포 git이 `core.autocrlf=true`다. Makefile, 셸 스크립트가 CRLF로 체크아웃되면 WSL에서 깨질 수 있으므로 `.gitattributes`를 둔다.

---
# 출력 형식 (2026-11-20 확정)

| 항목 | 형식 |
|---|---|
| 사용법 메시지 | stderr에 `usage: ./server <port> <root>` |
| 인자 검사 | `argc != 3`, `strtol` 뒤에 남은 문자가 있음(`8080abc`), 1~65535 밖이면 사용법 + `exit(1)`. 루트 검사는 `http_init`이 한다 |
| 1단계 `main` 출력 | stdout에 `port=<번호>` |
| 로그 | 모두 stderr. `accept fd=N`, `fd=N recv K bytes`, `fd=N send K bytes`, `fd=N EPOLLOUT on` / `off`, `close fd=N conn_count=M`, `accept paused`, `accept resumed`, `fd=N idle timeout`, `fd=N request timeout` |
| `/big` 본문, stub 응답 헤더 | (HTTP 3단계에서 제거됨. 응답 형식은 [architecture-http.md](architecture-http.md) "출력 형식") |

---
# 파일 구조

```
Makefile
.gitattributes        # * text=auto eol=lf
.gitignore            # server
src/
  main.c              # 인자 검사, http_init, SIGPIPE 무시, server_listen → loop_run
  server.c / .h       # listen 소켓 생성
  event_loop.c / .h   # epoll fd 소유, epoll_wait 루프, accept, 이벤트 분배, 1초 타이머
  conn.c / .h         # struct conn, 연결 리스트, 읽기/쓰기 처리, conn_close, 유휴 검사
  http.c / .h         # HTTP 파싱, 응답 생성, 정적 파일 (architecture-http.md)
```

| 파일 | 공개 함수 | 책임 |
|---|---|---|
| `main.c` | `main` | `argv[1]`을 `strtol`로 검사한다(1~65535가 아니면 사용법을 출력하고 `exit(1)`). `http_init(argv[2])`가 -1이면 `exit(1)`. `signal(SIGPIPE, SIG_IGN)` 후 `server_listen(port)`를 부르고, -1이면 `exit(1)`한다. 이어서 `loop_run(listen_fd)`를 부르고, -1을 반환하면 `exit(1)`한다 |
| `server.c` | `int server_listen(uint16_t port)` | `socket` → `SO_REUSEADDR` → `bind(0.0.0.0)` → `listen(SOMAXCONN)` → `O_NONBLOCK`. 실패하면 `perror` 후 -1을 반환한다 |
| `event_loop.c` | `int loop_run(int listen_fd)`<br>`int loop_watch(struct conn *c, uint32_t events)`<br>`void loop_unwatch(struct conn *c)` | `loop_run`은 `epoll_create1`이나 `listen_fd` 등록이 실패하거나 `epoll_wait`이 `EINTR`이 아닌 오류를 내면 `perror` 후 -1을 반환하고, 그 밖에는 반환하지 않는다(2026-11-20 Task 3: `epoll_wait` 실패를 `continue`하면 같은 오류로 CPU 100% 무한 루프가 되므로 추가). `epoll` fd는 이 파일의 `static` 변수로 두고 다른 파일은 직접 만지지 않는다. `loop_watch`는 처음이면 `EPOLL_CTL_ADD`, 이후에는 `EPOLL_CTL_MOD`를 부르고 `c->events`에 현재 값을 기록한다. `loop_unwatch`는 `EPOLL_CTL_DEL`을 부르고, accept가 멈춰 있으면 다시 켠다(EMFILE 처리) |
| `conn.c` | `struct conn *conn_new(int fd)`<br>`void conn_close(struct conn *c)`<br>`int conn_on_readable(struct conn *c)`<br>`int conn_on_writable(struct conn *c)`<br>`void conn_sweep_idle(time_t now)` | 연결 하나의 수명 전체를 맡는다. 핸들러는 연결이 살아 있으면 0, `conn_close`로 닫았으면 -1을 반환한다. -1을 받은 쪽은 `c`를 다시 건드리지 않는다 |
| `http.c` | `http_init`, `http_parse`, `http_handle` ([architecture-http.md](architecture-http.md)) | 요청 파싱, 응답 생성, 경로 해석, MIME |

- `listen_fd`는 `epoll`에 `data.ptr = NULL`로 등록한다. 이벤트 루프는 `ptr == NULL`이면 새 연결 이벤트, 아니면 `struct conn *` 이벤트로 구분한다.
- 상수는 각 헤더에 `#define`으로 둔다: `MAX_EVENTS 64`, `RBUF_SIZE 8192`, `IDLE_TIMEOUT_SEC 10`(개발 중 확인하기 쉽게 짧게 둔다).

---
# HTTP 자리 (인터페이스)

HTTP는 `http_init`, `http_parse`, `http_handle` 세 함수로만 붙는다. 시그니처와 흐름은 [architecture-http.md](architecture-http.md) "HTTP 인터페이스"에 있다. 처음 두 함수(`http_request_len`, `http_handle`)와 `hello\n` / `/big` stub은 HTTP 3단계에서 교체되었고, 아래 5단계 설명은 TCP 단계 당시의 기록이다.

---
# `struct conn`

```C
enum conn_state { CONN_READING, CONN_WRITING };

struct conn {
    int    fd;
    enum conn_state state;
    char   rbuf[RBUF_SIZE];   // 받았지만 아직 처리 안 한 바이트
    size_t rlen;
    char  *wbuf;              // http_handle이 만든 응답, 다 보내면 free
    size_t wlen, woff;        // 응답 길이, 보낸 위치
    int    keep_alive;        // 응답 후 READING으로 돌아갈지
    uint32_t events;          // 지금 epoll에 등록된 이벤트 (0이면 미등록)
    time_t last_active;       // recv/send 성공 시각 (WRITING 중에는 architecture-http.md의 SIOCOUTQ 규칙도 갱신)
    struct conn *prev, *next; // 유휴 검사용 리스트
};
```

- `file_fd`, `file_off`, `file_len`, `req_start`, `outq`는 HTTP 단계에서 추가되었다([architecture-http.md](architecture-http.md) "`struct conn` 변경"). `peer`(프록시)는 그 기능을 만들 때 추가하고, 그때 `conn_close`에 해당 정리 줄도 함께 넣는다.

## 연결 상태 전이

```
accept4 ──▶ READING (EPOLLIN 감시)
              │ recv → rbuf에 이어 붙임 → http_parse != 0
              ▼
            http_handle → wbuf 생성, rbuf에서 요청을 memmove로 제거
              ▼
            WRITING: conn_on_writable를 바로 호출
              │ 다 못 보냄(EAGAIN) → EPOLLOUT 감시로 바꾸고 다음 쓰기 이벤트 대기
              │ 다 보냄 → free(wbuf)
              ▼
            keep_alive? ── 예 ──▶ READING (EPOLLIN 감시로 되돌림)
              │                    rbuf에 남은 바이트가 있으면 바로 다시 처리 (파이프라이닝)
              └─ 아니오 ──▶ conn_close

모든 상태 ──▶ conn_close
  recv 0, ECONNRESET/EPIPE 등 오류, EPOLLHUP/EPOLLERR,
  유휴 타임아웃, 닫는 응답(rbuf가 찼는데 요청 미완성이면 414/431)을 다 보냄, http_handle 실패, malloc 실패
```

- WRITING 동안에는 `EPOLLIN`을 감시하지 않는다. 응답을 보내는 동안 새 요청을 받지 않는다. `EPOLLHUP`/`EPOLLERR`는 등록하지 않아도 항상 보고된다.
- 다 보낸 뒤 rbuf를 다시 확인해야 하는 이유: 다음 요청이 이미 rbuf(사용자 공간)에 있으면 커널 수신 버퍼가 비어 있어서 `EPOLLIN`이 오지 않는다.

## `conn_close` 순서

1. `loop_unwatch(c)`: `EPOLL_CTL_DEL` (`close` 전에 해야 한다). `c->events == 0`(등록 전에 `conn_new`의 `loop_watch`가 실패한 경우)이면 DEL을 건너뛴다. 어느 쪽이든 accept가 멈춰 있으면 다시 켠다. 이를 위해 `listen_fd`를 `event_loop.c`의 `static int lfd`로 저장한다(2026-11-20 Task 8).
2. `close(c->fd)`
3. `free(c->wbuf)`
4. `if (c->file_fd >= 0) close(c->file_fd)` (HTTP 단계에서 추가)
5. 리스트에서 제거
6. `free(c)` (마지막에 해야 한다)

---
# 구현 순서

모든 명령은 WSL의 `/mnt/d/kraftonjungle/kraftonjungle_week13/Krafton_Jungle_Week13_Team2`에서 실행한다. 각 단계는 확인을 통과한 뒤 커밋한다.

### 0단계: 환경
- `sudo apt install valgrind strace`
- `.gitattributes`(`* text=auto eol=lf`), `.gitignore`(`server`)
- 확인: `git ls-files --eol`에서 새 파일이 `i/lf`로 보인다.

### 1단계: Makefile, `main.c` 인자 검사
- Makefile 타깃
	- `server`: `gcc -std=gnu11 -Wall -Wextra -g -o server src/*.c`. `accept4`를 쓰려면 `gnu11`이 필요하다.
	- `asan`: 같은 명령에 `-fsanitize=address,undefined`를 붙인다.
	- `clean`
- `main`은 포트를 검사하고 출력만 한다.
- 확인
	- `make`가 경고 0개로 끝난다.
	- `./server`, `./server abc`, `./server 0`, `./server 70000`은 모두 사용법을 출력하고 `echo $?`가 1이다.

### 2단계: `server_listen`
- 이 단계의 `main`은 `listen` 후 `pause()`로 멈춰 둔다.
- 확인
	- `./server 8080` 실행 중 `ss -tlnp | grep 8080`에 `LISTEN`이 보인다.
	- 같은 포트로 하나 더 실행하면 `bind: Address already in use`가 나온다.
	- `nc localhost 8080`이 accept 없이도 연결된다. `ss -tan`에 `ESTABLISHED`가 보인다. handshake는 커널이 끝내고 연결은 accept 큐에 쌓인다.

### 3단계: 이벤트 루프 뼈대 + `accept`
- `epoll_create1` 후 `listen_fd`를 `EPOLLIN`, `data.ptr = NULL`로 등록한다. `epoll_wait(…, 1000)` 루프를 돈다.
- 새 연결 이벤트에서 `accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK)`를 `EAGAIN`이 나올 때까지 반복한다. 받은 fd는 로그만 찍고 바로 `close`한다.
- `epoll_wait`이 -1 + `EINTR`이면 `continue`한다.
- 확인
	- `nc localhost 8080`: 서버에 `accept fd=5`가 찍히고 nc가 바로 끝난다.
	- 아무 연결도 없을 때 `top`에서 CPU가 0%다.
	- `SO_REUSEADDR` 확인: nc를 몇 번 접속시키면 서버가 먼저 닫았으므로 `ss -tan | grep 8080`에 `TIME_WAIT`가 남는다. 이 상태에서 서버를 껐다 바로 켜도 `bind`가 성공한다. `setsockopt` 줄을 주석 처리하면 `EADDRINUSE`로 실패하는 것도 한 번 확인한다.

### 4단계: `struct conn` + 읽기 + `conn_close`
- `conn_new`(calloc, 리스트 추가, `loop_watch(EPOLLIN)`)를 만든다. calloc이 실패하면 fd를 `close`한다.
- `conn_on_readable`: `recv(fd, rbuf + rlen, RBUF_SIZE - rlen, 0)`를 한 번 호출한다(LT라 한 번이면 된다).
	- `> 0`: `rlen`을 늘리고 `last_active`를 갱신한 뒤 받은 바이트 수를 로그로 찍는다.
	- `0`, 그 밖의 오류: `conn_close` 후 -1을 반환한다.
	- `EAGAIN`/`EWOULDBLOCK`/`EINTR`: 0을 반환한다(다음 이벤트를 기다린다).
	- 응답은 아직 없다. rbuf가 차면 닫는다.
- 이벤트 루프: `EPOLLERR|EPOLLHUP`면 `conn_close`를 먼저 하고, 그 다음 `EPOLLIN`/`EPOLLOUT`을 분배한다.
- 살아 있는 연결 수 `conn_count`를 두고 `conn_close`에서 로그로 찍는다.
- 확인
	- 터미널 3개에서 `nc localhost 8080`을 각각 열고 번갈아 입력하면 세 연결이 모두 독립적으로 찍힌다(동시 처리 확인).
	- nc를 `Ctrl+C`로 끄면 `recv 0 → close`가 찍히고 `conn_count`가 줄어든다. `ss -tan | grep CLOSE_WAIT`는 비어 있다.
	- `ls /proc/$(pidof server)/fd | wc -l`: 연결 전, 접속 중, 모두 끊은 후를 비교하면 처음 값으로 돌아온다.
	- `make asan` 후 같은 시나리오를 돌려 ASan 오류가 0개다.

### 5단계: HTTP stub + 쓰기 경로
- `http.c` stub을 만든다.
- `conn_process(c)`: `http_request_len`
	- 0이면 대기한다(rbuf가 차면 닫는다).
	- -1이면 닫는다.
	- `> 0`이면 `http_handle` → `memmove`로 처리한 요청 제거 → `WRITING` → `conn_on_writable`을 직접 호출한다.
	- (2026-11-20 Task 5) 실제로는 `static int conn_send(c)`(-1 닫힘 / 0 남음 / 1 다 보내고 READING)로 나누고 `conn_process`를 루프로 둔다. `conn_on_writable`은 `conn_send`가 1이면 `conn_process`를 부른다. `conn_process` ↔ `conn_on_writable` 상호 재귀를 그대로 두면 파이프라이닝된 요청 수만큼 스택이 깊어지기 때문이다. 동작은 같다.
- `conn_on_writable`: `send(fd, wbuf + woff, wlen - woff, 0)`를 한 번 호출한다.
	- 남으면 `events`에 `EPOLLOUT`이 없을 때만 `loop_watch(EPOLLOUT)`를 부른다.
	- 다 보내면 `free(wbuf)` 후 `keep_alive`면 `READING` + `loop_watch(EPOLLIN)` + rbuf가 남았을 때 `conn_process`, 아니면 `conn_close`.
- 확인
	- `curl -v localhost:8080/`: `200 OK`, 본문 `hello`
	- `curl -v localhost:8080/ localhost:8080/`: 두 번째 요청에서 `Re-using existing connection`이 보인다(keep-alive).
	- 나뉜 요청: `(printf 'GET / HT'; sleep 1; printf 'TP/1.1\r\n\r\n') | nc -q 1 localhost 8080` → 1초 뒤 응답 하나
	- 파이프라이닝: `printf 'GET / HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n\r\n' | nc -q 1 localhost 8080` → 응답 두 개

### 6단계: 부분 `send`와 쓰기 감시 켜기/끄기
- `EPOLLOUT`을 켜고 끌 때 로그를 찍는다.
- 확인
	- `curl -s --limit-rate 200k localhost:8080/big -o /dev/null &` 실행 중 `curl localhost:8080/`이 즉시 응답한다. 큰 응답 하나가 루프를 막지 않는다는 뜻이다.
	- 로그에 부분 send 여러 번과 `EPOLLOUT on` → `off`가 보인다.
	- `curl -s localhost:8080/big | wc -c`가 8388608이다.
	- busy waiting 확인: `printf 'GET /big HTTP/1.1\r\n\r\n' | nc -q 30 localhost 8080 > /dev/null`로 응답을 다 받은 뒤에도 30초 동안 연결을 유지한다. 이 동안 `top`에서 서버 CPU가 0%여야 한다. `EPOLLOUT`을 끄는 줄을 빼면 100%가 되는 것도 한 번 확인한다.
	- SIGPIPE: `curl -s localhost:8080/big --max-time 0.1 -o /dev/null`로 받다가 끊어도 서버는 살아 있고 `EPIPE`/`ECONNRESET` → close 로그만 찍힌다. `signal(SIGPIPE, SIG_IGN)`을 빼면 서버가 죽는 것을 확인한다.

### 7단계: 유휴 타임아웃
- 루프에서 `time(NULL)`이 바뀔 때마다 `conn_sweep_idle(now)`를 부른다. 순회 중 `conn_close`가 현재 노드를 지우므로 `next`를 먼저 저장한다.
- 확인
	- `nc localhost 8080`을 열어 두고 아무것도 보내지 않으면 약 10초 후 nc가 끝난다.
	- `ss -tan`에 서버 쪽 `TIME_WAIT`가 보이고 `CLOSE_WAIT`는 없다.
	- `curl` keep-alive 연결도 유휴 10초 후 닫힌다.

### 8단계: `EMFILE`
- `accept4`가 `EMFILE`/`ENFILE`을 반환하면 `listen_fd`를 `EPOLL_CTL_MOD`(events=0)로 멈추고 `accept_paused = 1`로 둔다.
	- LT에서는 대기열에 연결이 남아 있는 한 매번 깨어나서 busy waiting이 생긴다.
- `loop_unwatch`(연결이 하나 닫힘)에서 `accept_paused`면 `EPOLLIN`을 다시 켠다.
- 확인
	- `ulimit -n 20; ./server 8080` 후 `for i in $(seq 30); do nc localhost 8080 & done` → `accept paused` 로그, `top` CPU 0%
	- `kill %1 %2 …`로 몇 개를 끊으면 `accept resumed`가 찍히고 대기열의 연결이 accept된다.

### 9단계: 종합 확인
- 동시 접속: `seq 200 | xargs -P 100 -I{} curl -s -o /dev/null -w '%{http_code}\n' localhost:8080/ | sort | uniq -c` → `200 200`
- RST: `python3 -c "import socket,struct; s=socket.create_connection(('127.0.0.1',8080)); s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii',1,0)); s.close()"` → 서버는 `ECONNRESET` 또는 `EPOLLHUP`으로 close하고 살아 있다.
- `valgrind --leak-check=full ./server 8080`로 위 시나리오를 돌린 뒤 `Ctrl+C`를 누르면 `definitely lost: 0`이다. 남은 연결은 `still reachable`로 나와도 된다. 시그널 셧다운은 범위 밖이다.
- `make asan`으로 같은 시나리오를 돌려 오류가 0개다.
- fd 수와 `conn_count`가 처음 값으로 돌아온다.

---
# 엣지 케이스

| 상황 | 어디서 | 처리 |
|---|---|---|
| `EAGAIN` / `EWOULDBLOCK` | `accept4` | 대기열이 비었다. 반복을 끝낸다 |
| | `recv` / `send` | 0을 반환하고 다음 이벤트를 기다린다. `send`에서 나오면 `EPOLLOUT`을 켠다 |
| `EINTR` | `epoll_wait` | `continue` |
| | `accept4` | 다시 시도한다 |
| | `recv` / `send` | `EAGAIN`과 같게 다음 이벤트에 맡긴다(LT라 다시 알림이 온다) |
| `ECONNABORTED` | `accept4` | 대기열에서 클라이언트가 끊었다. 다음 accept로 넘어간다 |
| `recv()`가 0 | `conn_on_readable` | 상대 FIN. `conn_close` |
| `ECONNRESET` | `recv` / `send` | `conn_close` |
| `EPIPE` | `send` | `conn_close`. `SIGPIPE`를 무시했기 때문에 프로세스는 죽지 않는다 |
| `EPOLLHUP` / `EPOLLERR` | 이벤트 루프 | 읽기/쓰기 분배 전에 `conn_close` |
| `EMFILE` / `ENFILE` | `accept4` | accept를 멈추고, 연결 하나가 닫히면 재개한다 |
| 그 밖의 오류 (`ENOBUFS`, `ENOMEM`, `EPROTO` 등) | `accept4` | `perror` 후 이번 accept 반복을 끝낸다. LT라 대기열이 남아 있으면 다음 `epoll_wait`에서 다시 시도한다 |
| `loop_watch`(`epoll_ctl`) 실패 | `conn_new`, 상태 전환 | `perror` 후 `conn_close` |
| rbuf가 찼는데 요청 미완성 | `conn_process` | `http_parse`가 `len == cap`에서 414/431 + 닫기. `conn_process`의 `conn_close`는 방어용으로 남았다. 크기 0으로 `recv`하면 0이 반환되어 FIN과 구분되지 않으므로, 버퍼가 꽉 찬 채로 루프에 돌아가면 안 된다 |
| 한 번의 recv에 요청 두 개 | `conn_process` | `memmove` 후 남은 바이트를 응답이 끝난 뒤 다시 처리한다 |
| `calloc`/`malloc` 실패 | `conn_new`, `http_handle` | `conn_new`는 fd를 닫는다. `http_handle`은 경로 버퍼 실패면 500 응답을 만들고, 응답 버퍼 실패면 응답 없이 `conn_close`한다 |
| 핸들러 안에서 연결이 닫힘 | 이벤트 루프, 핸들러 | 핸들러가 -1을 반환하면 `c`를 다시 쓰지 않는다 (use-after-free 방지) |
| 유휴 순회 중 삭제 | `conn_sweep_idle` | `next`를 먼저 저장한다 |

---
# 알려진 한계 (나중 작업)

- 유휴 타임아웃은 `last_active` 기준이라 Slowloris를 막지 못했다. HTTP 5단계의 `req_start`와 408로 해결되었다([architecture-http.md](architecture-http.md)).
- 1초마다 전체 연결을 순회한다(O(연결 수)). 연결이 많아지면 `last_active` 순서 리스트로 바꾼다. 활동이 생긴 연결을 리스트 끝으로 옮기면 앞에서부터 검사하다가 멈출 수 있다.
- 프록시를 붙이면 한 이벤트 배치 안에서 다른 연결(`peer`)이 해제될 수 있다. 그때 해제를 배치 끝으로 미루는 방법을 검토한다.
- 셧다운이 없어서 valgrind는 `Ctrl+C` 종료 후 `definitely lost`만 확인할 수 있다. 시그널 셧다운을 만들면 남은 연결 전부를 `conn_close`하고 다시 확인한다.
- accept 재개는 연결이 닫힐 때만 일어난다. 열린 연결이 0개인데 `ENFILE`(다른 프로세스가 시스템 전체 fd를 씀)로 멈추면 다시 켜지지 않는다. 필요해지면 1초 sweep에서 재개를 다시 시도한다(2026-11-20 최종 리뷰).
- 이번 계획에서 뺀 것: Dockerfile, 부하 테스트 도구(`ab`/`wrk`), 설정 파일, 시그널 셧다운.
