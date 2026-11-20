# TCP 서버 + 동시 요청 처리 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `./server <port>`로 뜨는 `epoll` LT 단일 스레드 TCP 서버를 만든다. 여러 연결을 동시에 받고, HTTP stub이 고정 응답을 돌려준다.

**Architecture:** `main` → `server_listen` → `loop_run`. `event_loop.c`가 `epoll`과 accept를 맡고, `conn.c`가 연결 하나의 수명(읽기 → `http_handle` → 쓰기 → keep-alive/close)을 맡는다. `http.c`는 stub이다.

**Tech Stack:** C (gnu11), gcc 13.3, POSIX 소켓, `epoll`, Make. WSL2 Ubuntu 24.04.

**Spec:** `docs/architecture-tcp.md` (파일 구조, 공개 함수 표, `struct conn`, 상태 전이, `conn_close` 순서, 엣지 케이스 표). 각 Task는 설계의 같은 번호 단계를 구현한다. 함수 책임과 오류 처리는 설계 표를 따르고 여기서 다시 적지 않는다.

## 실행 방법

- Windows 쪽 Claude는 명령을 `wsl.exe -e bash -lc 'cd /mnt/d/kraftonjungle/kraftonjungle_week13/Krafton_Jungle_Week13_Team2 && <명령>'`로 감싼다 (2026-11-20 확인, 종료 코드 0).
- 서버를 띄우는 확인은 한 번의 `bash -lc` 안에서 끝낸다. 띄우기 → 확인 → 종료 순서다.
  ```bash
  ./server 8080 2>/tmp/server.log & SP=$!; sleep 0.3
  # ... 확인 명령 ...
  kill $SP; cat /tmp/server.log
  ```
- CPU 0% 확인: `top -b -n 2 -d 1 -p $SP | tail -1 | awk '{print $9}'`가 `0.0`이다.
- 응답을 `grep`할 때는 `-a`를 붙인다 (`/big` 본문 때문에 바이너리로 판정될 수 있다).
- 서버가 연결을 닫았는지는 `nc` 대신 python `recv`가 `b''`를 반환하는지로 본다. OpenBSD `nc`는 원격이 닫아도 stdin이 열려 있으면 끝나지 않을 수 있다.

## Global Constraints

- 컴파일: `gcc -std=gnu11 -Wall -Wextra -g -o server src/*.c`, ASan 빌드는 `-fsanitize=address,undefined`를 더한다.
- 상수: `MAX_EVENTS 64`, `RBUF_SIZE 8192`, `IDLE_TIMEOUT_SEC 10`. 각 헤더에 `#define`으로 둔다.
- `listen_fd`는 `data.ptr = NULL`로 등록한다. `epoll_wait` 타임아웃은 1000ms다.
- 테스트 프레임워크는 (미정)이다. 각 Task의 확인 명령이 테스트다.

## 출력 형식

사용법 메시지, 인자 검사, 로그, `/big` 본문, stub 응답 헤더는 `docs/architecture-tcp.md`의 "출력 형식" 절을 따른다 (2026-11-20 확정).

## Review Focus

1. 요청이 끝나지 않은 채로 rbuf 8192바이트가 가득 참 → 연결만 닫히고 서버는 다음 연결을 받는다 → Task 5
2. 큰 응답을 보내는 중에 같은 연결로 파이프라이닝된 다음 요청 → 두 응답이 순서대로 온다 → Task 6
3. 요청만 보내고 응답을 읽지 않는 클라이언트 → 보내기가 멈춘 연결도 유휴 10초 후 닫힌다 → Task 7
4. 포트 인자 경계: `1`(root가 아니면 `bind` 권한 오류), `65535` → `1`은 `exit(1)`, `65535`는 LISTEN → Task 2
5. 요청 직후 클라이언트가 쓰기 방향을 닫음(`nc -N`) → 응답을 받고 서버는 연결을 닫는다 → Task 5

---

### Task 1: 환경, Makefile, 인자 검사 (설계 0~1단계)
담당: `builder` / 강도: 기본

**Files:**
- Create: `.gitattributes` (`* text=auto eol=lf`), `.gitignore` (`server`), `Makefile`, `src/main.c`

**Interfaces:**
- Produces: `make` → `./server`, `make asan`, `make clean`

- [ ] **Step 1: 사용자에게 `sudo apt install valgrind strace` 실행을 요청한다** (sudo 비밀번호가 필요하다. Task 9 전까지만 끝나면 된다)
- [ ] **Step 2: 실패 확인** — `make` → 종료 코드가 0이 아니다 (Makefile 없음)
- [ ] **Step 3: 파일 작성.** `main`은 인자를 검사하고 `port=<번호>`만 출력한다.
- [ ] **Step 4: 확인**
  - `make 2>&1 | grep -c warning` → `0`, `make` 종료 코드 0
  - `for a in "" abc 0 70000 8080abc; do ./server $a; echo $?; done` → 다섯 번 모두 사용법 출력, `1`
  - `./server 8080` → `port=8080`, 종료 코드 0
  - `git add .gitattributes .gitignore Makefile src/main.c && git ls-files --eol .gitattributes .gitignore Makefile src/main.c` → 네 줄 모두 `i/lf`
- [ ] **Step 5: 커밋, push** — `feat: Makefile과 포트 인자 검사`

### Task 2: `server_listen` (설계 2단계)
담당: `builder` / 강도: 기본

**Files:**
- Create: `src/server.c`, `src/server.h`
- Modify: `src/main.c` (`server_listen` 후 `pause()`)

**Interfaces:**
- Produces: `int server_listen(uint16_t port)` → non-blocking listen fd, 실패 시 -1

- [ ] **Step 1: 실패 확인** — `make` 후 `ss -tln | grep -c ':8080 '` → `0`
- [ ] **Step 2: 구현** (설계 표의 순서 그대로, -1이면 `exit(1)`)
- [ ] **Step 3: 확인** (서버 띄운 상태에서)
  - `ss -tlnp | grep 8080` → `LISTEN`
  - `./server 8080; echo $?` → `bind: Address already in use`, `1`
  - `sleep 3 | nc localhost 8080 & sleep 0.5; ss -tan | grep 8080 | grep -c ESTAB` → 1 이상 (accept 없이도 연결됨)
  - Review Focus 4: `./server 1; echo $?` → `bind: Permission denied`, `1` / `./server 65535` 실행 중 `ss -tln | grep -c ':65535 '` → `1`
- [ ] **Step 4: 커밋, push** — `feat: listen 소켓 생성`

### Task 3: 이벤트 루프 뼈대 + accept (설계 3단계)
담당: `builder` / 강도: 기본

**Files:**
- Create: `src/event_loop.c`, `src/event_loop.h`
- Modify: `src/main.c` (`pause()` 대신 `loop_run`, -1이면 `exit(1)`)

**Interfaces:**
- Consumes: `server_listen`
- Produces: `int loop_run(int listen_fd)` (이 Task에서는 accept한 fd를 로그 후 바로 `close`)

- [ ] **Step 1: 실패 확인** — Task 2 서버에 `nc -w1 localhost 8080 </dev/null`를 실행해도 로그에 `accept`가 없다
- [ ] **Step 2: 구현** (`accept4(..., SOCK_NONBLOCK)`을 `EAGAIN`까지 반복, `EINTR` 처리는 설계 엣지 케이스 표)
- [ ] **Step 3: 확인**
  - `python3 -c "import socket; s=socket.create_connection(('127.0.0.1',8080)); print(s.recv(1))"` → 즉시 `b''`, 로그에 `accept fd=5`
  - 연결 없이 CPU → `0.0`
  - nc 3번 접속 후 `ss -tan | grep 8080 | grep -c TIME-WAIT` → 1 이상. 서버를 `kill` 후 바로 다시 띄우면 `bind` 성공
  - (학습용, 커밋하지 않음) `setsockopt` 줄을 빼면 같은 상황에서 `EADDRINUSE`인지 확인하고 되돌린다
- [ ] **Step 4: 커밋, push** — `feat: epoll 루프와 accept`

### Task 4: `struct conn`, 읽기, `conn_close` (설계 4단계)
담당: `builder` / 강도: 높음 (연결 수명, `conn_close` 순서, use-after-free 위험)

**Files:**
- Create: `src/conn.c`, `src/conn.h`
- Modify: `src/event_loop.c` (`conn_new` 호출, `EPOLLERR|EPOLLHUP` → `conn_close` 먼저, 그다음 `EPOLLIN`/`EPOLLOUT` 분배, `loop_watch`/`loop_unwatch`)

**Interfaces:**
- Produces:
  - `struct conn` (설계의 필드 그대로), `enum conn_state { CONN_READING, CONN_WRITING }`
  - `struct conn *conn_new(int fd)`, `void conn_close(struct conn *c)`, `int conn_on_readable(struct conn *c)`, `int conn_on_writable(struct conn *c)` (이 Task에서는 0만 반환하는 자리)
  - `int loop_watch(struct conn *c, uint32_t events)`, `void loop_unwatch(struct conn *c)`
  - 연결 수 `conn_count` (`conn.c` static)

- [ ] **Step 1: 실패 확인** — 연결 후 보낸 바이트가 로그에 없다
- [ ] **Step 2: 구현** (`recv` 한 번, 반환값 분기는 설계 4단계. 응답은 아직 없음, rbuf가 차면 닫는다)
- [ ] **Step 3: 확인**
  - 동시 3연결: `python3 -c "import socket,time; s=[socket.create_connection(('127.0.0.1',8080)) for _ in range(3)]; [(x.send(b'ab'), time.sleep(0.1)) for _ in range(2) for x in s]; [x.close() for x in s]"` → 로그에 서로 다른 fd 3개가 번갈아 `recv 2 bytes`를 두 번씩 찍고, `close ... conn_count=0`으로 끝난다
  - 위 시나리오 후 `ss -tan | grep -c CLOSE-WAIT` → `0`
  - `ls /proc/$SP/fd | wc -l`: 연결 전과 모두 끊은 후 값이 같다
  - `make clean && make asan` 후 같은 시나리오 → 로그에 `ERROR: AddressSanitizer`, `runtime error`가 없다
- [ ] **Step 4: 커밋, push** — `feat: 연결 구조체와 읽기 경로`

### Task 5: HTTP stub + 쓰기 경로 (설계 5단계)
담당: `builder` / 강도: 높음 (쓰기 경로, 파이프라이닝, 상태 전이)

**Files:**
- Create: `src/http.c`, `src/http.h`
- Modify: `src/conn.c` (`static int conn_process(struct conn *c)`, `conn_on_writable`)

**Interfaces:**
- Produces: `ssize_t http_request_len(const char *buf, size_t len)`, `int http_handle(const char *req, size_t req_len, char **out, size_t *out_len, int *keep_alive)` (설계 "HTTP 자리"의 stub 동작)

- [ ] **Step 1: 실패 확인** — `curl -s -m 2 localhost:8080/; echo $?` → `28` (타임아웃)
- [ ] **Step 2: 구현** (`conn_process`와 `conn_on_writable` 분기는 설계 5단계)
- [ ] **Step 3: 확인**
  - `curl -s localhost:8080/` → `hello`
  - `curl -sv localhost:8080/ localhost:8080/ 2>&1 | grep -c 'Re-using existing connection'` → `1`
  - 나뉜 요청: `(printf 'GET / HT'; sleep 1; printf 'TP/1.1\r\n\r\n') | nc -q 1 localhost 8080 | grep -ac 'HTTP/1.1 200 OK'` → `1`
  - 파이프라이닝: `printf 'GET / HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n\r\n' | nc -q 1 localhost 8080 | grep -ac 'HTTP/1.1 200 OK'` → `2`
  - Review Focus 1: `python3 -c "import socket; s=socket.create_connection(('127.0.0.1',8080)); s.sendall(b'a'*8192); print(s.recv(1))"` → `b''`. 이어서 `curl -s localhost:8080/` → `hello`
  - Review Focus 5: `printf 'GET / HTTP/1.1\r\n\r\n' | nc -N localhost 8080 | tail -c 6` → `hello`, 로그에 `close`
- [ ] **Step 4: 커밋, push** — `feat: HTTP stub과 응답 쓰기`

### Task 6: 부분 send, `EPOLLOUT` 켜기/끄기 (설계 6단계)
담당: `builder` / 강도: 기본

**Files:**
- Modify: `src/conn.c` (`EPOLLOUT on` / `off` 로그)

**Interfaces:** 변경 없음

- [ ] **Step 1: 실패 확인** — `curl -s localhost:8080/big | wc -c`를 하면 로그에 `EPOLLOUT on`이 없다
- [ ] **Step 2: 구현** (로그 추가, 감시 전환이 설계와 다르면 고친다)
- [ ] **Step 3: 확인**
  - `curl -s localhost:8080/big | wc -c` → `8388608`
  - `curl -s --limit-rate 200k localhost:8080/big -o /dev/null & sleep 1; time curl -s localhost:8080/` → `hello`, real 0.1초 미만
  - 로그에 `send`가 여러 번, `EPOLLOUT on` → `off`
  - busy waiting: `printf 'GET /big HTTP/1.1\r\n\r\n' | nc -q 30 localhost 8080 >/dev/null &` 후 5초 뒤 CPU → `0.0`
  - (학습용, 커밋하지 않음) `EPOLLOUT`을 끄는 줄을 빼면 CPU가 100% 가까이 되는지 확인하고 되돌린다
  - SIGPIPE: `curl -s --limit-rate 1M localhost:8080/big --max-time 0.5 -o /dev/null` 후 (`--limit-rate`가 없으면 localhost에서 0.1초 안에 다 받아 끊김이 안 생길 수 있다) `kill -0 $SP; echo $?` → `0`, 로그에 `close`
  - Review Focus 2: `printf 'GET /big HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n\r\n' | nc -q 3 localhost 8080 > /tmp/out; grep -ao 'HTTP/1.1 200 OK' /tmp/out | wc -l; tail -c 6 /tmp/out` → `2`, `hello`
- [ ] **Step 4: 커밋, push** — `feat: 부분 send와 쓰기 감시 전환`

### Task 7: 유휴 타임아웃 (설계 7단계)
담당: `builder` / 강도: 기본 (순회 중 삭제 주의)

**Files:**
- Modify: `src/conn.c` (`void conn_sweep_idle(time_t now)`), `src/event_loop.c` (`time(NULL)`이 바뀔 때마다 호출)

**Interfaces:**
- Produces: `void conn_sweep_idle(time_t now)`

아래 `IDLE`은 `python3 -c "import socket,time; s=socket.create_connection(('127.0.0.1',8080)); t=time.time(); print(s.recv(1), round(time.time()-t))"`이다.

- [ ] **Step 1: 실패 확인** — `timeout 15 IDLE; echo $?` → `124`
- [ ] **Step 2: 구현** (순회 중 `next` 먼저 저장)
- [ ] **Step 3: 확인**
  - `IDLE` → `b'' 10` 또는 `b'' 11`, 로그에 `idle timeout`
  - 직후 `ss -tan | grep 8080 | grep -c TIME-WAIT` → 1 이상, `grep -c CLOSE-WAIT` → `0`
  - keep-alive: `python3 -c "import socket,time; s=socket.create_connection(('127.0.0.1',8080)); s.send(b'GET / HTTP/1.1\r\n\r\n'); s.recv(100); t=time.time(); print(s.recv(1), round(time.time()-t))"` → `b'' 10` 또는 `b'' 11`
  - Review Focus 3: `python3 -c "import socket,time; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4096); s.connect(('127.0.0.1',8080)); s.send(b'GET /big HTTP/1.1\r\n\r\n'); time.sleep(15)"` 동안 로그에 그 fd의 `idle timeout`이 찍히고, 끝난 뒤 `conn_count=0`
- [ ] **Step 4: 커밋, push** — `feat: 유휴 연결 타임아웃`

### Task 8: `EMFILE` (설계 8단계)
담당: `builder` / 강도: 기본

**Files:**
- Modify: `src/event_loop.c` (`static int accept_paused`, 멈춤/재개)

**Interfaces:** 변경 없음

- [ ] **Step 1: 실패 확인** — `ulimit -n 20` 서버에 30연결을 걸면 CPU가 `0.0`보다 크다
- [ ] **Step 2: 구현** (설계 8단계)
- [ ] **Step 3: 확인** (`ulimit -n 20; ./server 8080 2>/tmp/server.log & SP=$!`)
  - `for i in $(seq 30); do sleep 60 | nc localhost 8080 & done; sleep 1` → 로그에 `accept paused`, CPU `0.0`
  - `pkill -o -x nc`를 세 번 (가장 오래된 nc는 이미 accept된 연결이다) → 로그에 `accept resumed`, 이어서 새 `accept fd=`
  - 이 확인은 Task 7의 타임아웃 10초 안에 끝낸다. 늦으면 타임아웃 close가 먼저 재개를 일으킨다
- [ ] **Step 4: 커밋, push** — `feat: fd 고갈 시 accept 멈춤과 재개`

### Task 9: 종합 확인 (설계 9단계)
담당: `verifier` (결함 수정은 `builder`) / 강도: 기본

**Files:** 코드 변경 없음. 확인에서 결함이 나오면 해당 파일을 고친다.

- [ ] **Step 1: 동시 접속** — `seq 200 | xargs -P 100 -I{} curl -s -o /dev/null -w '%{http_code}\n' localhost:8080/ | sort | uniq -c` → `200 200`
- [ ] **Step 2: RST** — 설계 9단계의 `SO_LINGER` python 명령 후 `kill -0 $SP; echo $?` → `0`, 로그에 그 fd의 `close`
- [ ] **Step 3: valgrind** — `valgrind --leak-check=full ./server 8080 2>/tmp/vg.log & SP=$!`로 Step 1~2와 Task 5~7의 시나리오를 돌린 뒤 `kill -INT $SP` → `grep 'definitely lost' /tmp/vg.log` → `0 bytes` (`still reachable`은 허용)
- [ ] **Step 4: ASan** — `make clean && make asan` 후 같은 시나리오 → `ERROR: AddressSanitizer`, `runtime error`가 없다
- [ ] **Step 5: 원래대로 돌아오는지** — 시나리오 전후 `ls /proc/$SP/fd | wc -l`가 같고, 마지막 로그의 `conn_count=0`
- [ ] **Step 6: 커밋, push** (고친 것이 있을 때) — `fix: 종합 확인에서 찾은 결함`
