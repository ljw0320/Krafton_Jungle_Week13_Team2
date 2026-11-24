# Krafton Jungle Week 13 — Team 2 Web Server

`learning/05_echo_thread-nonblocking`의 **non-blocking 소켓 + poll + 고정 스레드 풀 + eventfd** 구조를 참고한 학습용 C 웹서버입니다. GET으로 HTML·CSS·이미지를 제공하고 POST `/echo`로 요청 본문을 그대로 돌려줍니다. CGI를 활성화하면 GET·POST `/cgi-bin/프로그램` 요청에 따라 애플리케이션을 실행하고 응답을 생성합니다. HTTP 메서드는 **GET, POST만** 지원합니다.

현재 컴퓨터의 **WSL Ubuntu 24.04**에서 빌드합니다. Windows 네이티브 실행이나 macOS 이식성은 이번 범위에 포함하지 않습니다. 완성된 HTTP·이벤트 루프 라이브러리는 사용하지 않습니다.

## 빌드와 실행

PowerShell이 아닌 **WSL Ubuntu 터미널**에서 실행합니다. C 컴파일러와 Make가 필요하며 없으면 `sudo apt install build-essential`로 설치합니다. 수동 테스트에는 curl을 사용합니다.

```bash
cd /mnt/c/Users/ljw/Desktop/Jungle_Project/Week_13_WebServer/Krafton_Jungle_Week13_Team2
make
./webserver 19006 ./www
```

브라우저에서 `http://127.0.0.1:19006/`를 엽니다. HTML, CSS, 나무 이미지가 표시됩니다. 종료는 실행 터미널의 **Ctrl+C** 또는 다른 WSL 터미널의 `kill -TERM <PID>`입니다.

```text
./webserver PORT DOCUMENT_ROOT [CGI_ROOT]
```

| 인자 | 의미 |
|---|---|
| `PORT` | 필수, 1~65535 정수. 사용 중인 포트면 bind 오류 후 실패 종료 |
| `DOCUMENT_ROOT` | 필수, 정적 파일 루트. 상대 경로는 실행 디렉터리 기준 |
| `CGI_ROOT` | 선택, CGI 실행 파일 디렉터리. 지정하면 `/cgi-bin/이름` 활성화. 생략하면 CGI 경로는 404 |

주소는 `127.0.0.1`로 고정합니다. Reverse Proxy 라우팅 인자는 구현하지 않습니다. 다른 포트·루트 예: `./webserver 8080 /tmp/my-site`.

## 동적 요청 처리: CGI

WSL에서 `make`하면 서버와 덧셈 예제 `cgi-bin/adder`를 함께 빌드합니다. **CGI를 사용하려면 세 번째 인자를 지정**하고 서버를 재시작합니다.

```bash
make
./webserver 19006 ./www ./cgi-bin
```

다른 WSL 터미널에서 실행합니다.

```bash
curl -i 'http://127.0.0.1:19006/cgi-bin/adder?a=12&b=30'
curl -i --data 'a=12&b=30' http://127.0.0.1:19006/cgi-bin/adder
```

두 요청 모두 `200 OK`, `Content-Type: application/json`, 본문 `{"a":12,"b":30,"sum":42}`와 줄바꿈을 반환합니다. GET은 쿼리를, POST는 `application/x-www-form-urlencoded` 본문을 계산에 사용합니다. 예제는 두 정수 `a`, `b`를 받으며, 잘못된 입력·누락·합계 오버플로는 CGI의 `Status` 헤더를 통해 400을 반환합니다. 예제 폼 입력은 255바이트까지이며 서버의 일반 POST 한도 64KiB와 별개입니다.

브라우저에서는 `http://127.0.0.1:19006/`의 **동적 요청 · CGI 테스트**를 사용합니다. GET 또는 POST를 선택하고 정수 `a`, `b`를 입력한 뒤 **덧셈 요청 보내기**를 누릅니다. 요청 주소·POST 본문, 예상 응답, 실제 HTTP 상태·Content-Type·소요 시간, 응답 본문과 검증 결과가 표시됩니다. **잘못된 입력 테스트 · 400**은 `b`를 생략하고, **없는 프로그램 테스트 · 404**는 없는 CGI 경로를 요청합니다. 입력값과 예상 합계는 BigInt로 비교하므로 64비트 정수의 반올림을 피합니다. 파일을 직접 열면 테스트가 비활성화됩니다. 페이지 변경은 새로고침으로 적용되지만 CGI를 활성화하려면 위 세 번째 인자를 포함해 서버를 실행해야 합니다.

| 항목 | 동작 |
|---|---|
| 라우팅 | `/cgi-bin/이름`은 정적 파일보다 먼저 CGI로 분기. CGI_ROOT 바로 아래의 일반 실행 파일만 허용 |
| 프로그램 이름 | 영문·숫자·`.`·`_`·`-` 허용. `.`·`..`, 하위 경로, 퍼센트 인코딩, 심볼릭 링크 차단 |
| 요청 전달 | `REQUEST_METHOD`, 원본 `QUERY_STRING`, `SCRIPT_NAME`, `CONTENT_LENGTH`, `CONTENT_TYPE`, `SERVER_PROTOCOL`, `GATEWAY_INTERFACE`, `SERVER_SOFTWARE` 환경 변수. POST 원본 바이트는 stdin으로 전달 |
| 실행 | 검증한 fd를 `/proc/self/fd/번호`로 `posix_spawn`. 셸 명령 문자열을 만들지 않으며 요청별 환경을 별도로 구성 |
| 응답 | stdout의 `Content-Type` 필수, `Status: 400 Bad Request` 같은 최종 상태(200~599) 선택. 헤더는 CRLF, 빈 줄 뒤 본문은 바이너리 가능 |
| 지원 헤더 | CGI 응답은 `Content-Type`, `Status`만 지원. 그 밖의 헤더·중복·잘못된 형식은 502. 리다이렉트용 Location·쿠키 전달·NPH·PATH_INFO는 미지원 |
| HTTP 생성 | 서버가 실제 본문 길이를 계산해 공통 응답 함수로 전송. 204·205·304는 빈 본문만 허용하고, 204·304는 Content-Length를 생략 |
| 한도 | CGI 실행 시작 후 절대 2초, 응답 헤더 16KiB·본문 64KiB. 시간 초과 504, 출력 한도 초과·비정상 종료·실행 형식 오류 502 |
| 동시성 | stdout 파이프를 non-blocking으로 수집하고 poll에 양보. EOF 뒤 종료 확인도 비차단이며 최대 50ms 간격으로 재확인 |
| 자원 회수 | 성공·오류·시간 초과·서버 종료 시 프로세스 그룹 종료, 직접 자식 waitpid 회수, 파이프 close. stderr는 `/dev/null` |

CGI_ROOT를 생략한 기존 실행 명령도 그대로 동작합니다. 없는 프로그램은 404, 실행 권한이 없는 파일은 403입니다. GET 본문 제한과 POST의 Content-Length 필수·64KiB 제한을 CGI에도 동일하게 적용하며, POST는 본문 수신을 완료한 뒤 프로그램을 실행합니다. CGI 입력에는 익명 메모리 파일(`memfd_create`)을 사용해 큰 stdin과 stdout 사이의 파이프 교착을 피합니다. Linux `/proc`, memfd, GNU/POSIX spawn API를 사용하므로 기존 WSL/Linux 실행 범위를 유지합니다.

CGI_ROOT에는 서버 관리자가 신뢰하는 프로그램만 배치하고 문서 루트와 분리하세요. CGI는 서버 사용자 권한으로 실행되며 파일시스템·네트워크 접근을 격리하는 샌드박스는 아닙니다. 별도 세션으로 이탈한 하위 프로세스까지 관리하는 기능은 포함하지 않습니다. 추가 프로그램은 CGI_ROOT에 실행 파일 또는 올바른 shebang과 실행 권한이 있는 스크립트로 배치합니다. 인자 전달·임의 경로 실행·백엔드 프록시는 추가하지 않았습니다.

## 요청에 따른 결정과 직접 정한 결정

### 사용자 요청으로 정한 사항

| 요청 | 반영 |
|---|---|
| `05`를 참고한 간단한 웹서버 | 연결 상태·작업 큐·제한된 한 차례 처리 모델을 HTTP에 적용 |
| C, 표준 C/POSIX API, HTTP·이벤트 라이브러리 금지 | C11, pthread, socket, poll 사용. 기존 Linux eventfd 유지 |
| GET, POST만 지원 | 다른 메서드는 405, `Allow: GET, POST` |
| TCP 서버와 동시 요청 처리 | non-blocking 리스너·연결, 작업자 4개, 연결 슬롯·큐 각 8개 |
| 요청 파싱·응답 생성·정적 파일 | 요청 줄·헤더·본문 경계, Content-Length, MIME, HTML/CSS/이미지, 404 |
| 함수당 30줄 이하, 주요 함수 주석 | 선언 시작부터 닫는 중괄호까지 빈 줄·주석을 포함한 물리적 줄 수 기준. 테스트 C 함수에도 적용 |
| 코드 `src`, 추가 문서 `docs` | 테스트 코드는 `src/tests`, 정적 파일은 `www` |
| 빌드 파일과 CLI 설정 제공 | Makefile과 `PORT DOCUMENT_ROOT` 인자 |
| 이 컴퓨터 실행 우선, 이식성 고려 축소 | WSL Ubuntu 24.04/Linux 대상. eventfd 플랫폼 분기 없음 |
| 자체 테스트 최소화, 10,000건 제외 | HTTP 5개·종료 처리 5개 단위 테스트 묶음. Windows–WSL 문제 재현에 필요한 소수 요청만 추가 확인하며 부하 테스트는 수행하지 않음 |
| 구조·실행 흐름·테스트·결정 사항 문서화 | 이 README와 `docs/ARCHITECTURE.md` |

### 구현하면서 직접 정한 사항

| 결정 | 이유와 동작 |
|---|---|
| 응답마다 `Connection: close` | 제안한 첫 단계의 정적 응답에 범위를 맞춤. keep-alive·pipelining 미지원 |
| GET은 문서 루트 아래 정적 파일 | `/`와 끝이 `/`인 경로는 `index.html`, 쿼리는 파일 선택에서 제외 |
| POST는 `/echo` 또는 활성화된 `/cgi-bin/이름` | `/echo`는 원본 바이트 응답, CGI는 본문 수신 후 애플리케이션 실행 |
| POST에 Content-Length 필수 | 미지정 411, 0 허용, 최대 65,536바이트. 초과 413 |
| GET 요청 본문은 길이 0만 허용 | 양수 Content-Length는 400. 본문 수신 기능은 POST에 한정 |
| 헤더 전체 최대 16,384바이트 | 요청 줄과 마지막 빈 줄 포함. 초과 431, 누적 버퍼 상한 고정 |
| 본문과 헤더는 고정 크기 버퍼 | 직접 malloc/free 없이 연결 슬롯에서 보관. 파일은 최대 16KiB씩 읽음 |
| 경로는 openat 기반으로 조각별 검사 | URL 한 번 디코딩, `.`·`..`·빈 경로 조각·심볼릭 링크 차단. 디렉터리 목록 미제공 |
| 한 차례 수신·송신 각각 최대 64KiB, 최대 64단계 | 큰 응답이 작업자를 계속 점유하지 않도록 `05`의 예산 모델 유지 |
| 수신 무진행 5초, 송신 무진행 3초 | CLOCK_MONOTONIC 기준. 진전 시 갱신하며 요청 전체 시간 상한은 아님 |
| 응답 후 단계적 연결 종료 | 응답 즉시 전송 → 4KiB 버퍼로 알려진 남은 업로드 폐기 → `shutdown(SHUT_WR)` → 상대 EOF 또는 고정된 2초 기한 후 close. 남은 본문이 없으면 즉시 송신 방향 종료 |
| 연결 8개 초과는 새 소켓 종료 | 응답용 슬롯도 없으므로 503을 보장하지 않음. C10K는 구현·검증 범위 밖 |
| SIGINT/SIGTERM은 빠른 종료 | accept 중단 → 작업자 현재 차례 반환·join → 파일/소켓 회수. 프로세스 종료 시 진행 중 응답·연결의 정리 기한을 기다리지는 않음 |
| SIGPIPE 무시 | 상대가 응답 중 끊으면 send 오류로 해당 연결만 회수 |
| 이전 설계 문서 보존 | `docs/PREVIOUS_*`에 원본 복사. 현재 구현의 기준은 이 README |
| 후속 요청에 따른 브라우저 테스트 | 요청 수 4개·8개·12개·36개 선택. GET 응답 본문은 4·16·64·128KiB, POST 요청 본문은 추가로 512KiB·1MiB·100MiB를 선택하고 예상 응답과 비교 |

동적 요청 처리는 CGI 실행 방식으로 추가했습니다. Reverse Proxy, TLS, 업로드 저장, DB, 인증, 부하 도구는 구현하지 않았습니다.

## 프로그램 구조

```text
Krafton_Jungle_Week13_Team2/
├── Makefile
├── README.md
├── DESIGN_DECISIONS.md          # 현재 결정 사항 안내
├── src/
│   ├── main.c                  # CLI, 초기화, 종료·자원 회수
│   ├── server.h                # 서버·연결 상태와 고정 한도
│   ├── server.c                # accept, poll, 타임아웃, 작업 배정
│   ├── thread_pool.c           # FIFO 큐, 작업자 4개, join
│   ├── platform.c              # 소켓 설정, eventfd, 시각, 시그널
│   ├── http.h                  # 요청 구조와 HTTP/파일 함수 인터페이스
│   ├── http.c                  # 요청 경계·파싱·응답 헤더
│   ├── files.c                 # 경로 검사, 파일 열기, MIME
│   ├── connection.c            # 분할 수신·송신, 반쪽 종료, 남은 데이터 폐기
│   ├── cgi.c                   # CGI 라우팅, 비차단 출력 수집, 기한·자식 회수
│   ├── cgi_spawn.c             # 요청별 환경·stdin 준비, posix_spawn
│   ├── cgi_response.c          # 제한된 CGI 헤더 검증, HTTP 응답 변환
│   ├── cgi-bin/adder.c         # 동적 덧셈 CGI 예제 소스
│   └── tests/
│       ├── test_http.c         # HTTP·파일 최소 단위 테스트
│       ├── test_drain.c        # socketpair 기반 종료 최소 단위 테스트
│       └── test_cgi.py         # 실제 TCP CGI 통합 테스트
├── cgi-bin/adder               # make로 생성하는 CGI 실행 파일 (Git 제외)
├── www/                        # HTML(브라우저 테스트 JS 포함), CSS, SVG, test-data/*.bin
└── docs/
    ├── ARCHITECTURE.md          # 상태 전이·동시성·종료 설명
    ├── PREVIOUS_README.md       # 이전 문서 원본: 현재 사양 아님
    └── PREVIOUS_DESIGN_DECISIONS.md
```

## 실행 흐름

1. `main()`이 포트·루트 인자를 검사하고 `socket → SO_REUSEADDR → bind → listen`을 수행합니다. 리스너는 non-blocking입니다. TCP 3-way handshake 자체는 커널이 처리합니다.
2. 문서 루트 fd와 eventfd를 열고 시그널 처리기·작업자 4개를 준비합니다.
3. 메인이 `poll()`에서 리스너, 완료 알림, WAITING 연결의 읽기/쓰기 준비를 기다립니다. 준비된 연결은 FIFO 큐에 한 번만 들어갑니다.
4. 작업자는 연결의 한 차례를 맡아 `\r\n\r\n`까지 헤더를 누적합니다. 일반 GET은 파일을 열고, POST는 Content-Length만큼 본문을 모읍니다. CGI 경로는 GET 즉시 또는 POST 본문 수신 후 프로그램을 실행하고 출력 파이프를 감시합니다.
5. 응답 헤더를 먼저 보내고 파일/본문을 전송합니다. 부분 송신·EAGAIN이면 오프셋을 남겨 다음 차례에서 이어갑니다.
6. 응답 전송 후 `DRAIN_INPUT`에서 남은 수신 데이터를 버립니다. 한도 초과 본문이 남았으면 먼저 폐기하고 `shutdown(SHUT_WR)`를 수행합니다. 작업자는 WAITING/FINISHED 상태를 mutex 아래 공개하고 eventfd로 메인을 깨웁니다. 메인만 연결·파일을 닫고 슬롯을 FREE로 돌립니다.
7. 종료 신호가 오면 신규 배정을 중단하고 작업자를 join한 뒤 모든 열린 연결·파일·리스너·eventfd 및 동기화 객체를 정리합니다.

```text
수신 누적 → 헤더 완성 → HTTP 파싱
                        ├─ GET 정적 파일 → 경로 검사 → 파일 열기
                        ├─ POST /echo → 본문 길이만큼 수신
                        └─ GET/POST /cgi-bin/이름 → [POST 본문 수신] → CGI 실행·출력 검증
        → 응답 헤더 → 파일/본문 분할 송신
        → 남은 업로드 폐기 → 송신 방향 종료 → 상대 EOF 대기 → 연결 종료
```

HTTP/1.0·1.1 요청 줄을 해석하지만 항상 HTTP/1.1 응답과 `Connection: close`를 보냅니다. 클라이언트의 `Connection: keep-alive` 요청에도 연결은 재사용하지 않습니다. 본문 길이 이후 추가 데이터와 다음 요청은 처리하지 않습니다.

## 응답 후 연결 종료 흐름

기존에는 응답 데이터를 모두 `send()`에 넘기면 `response_complete`로 바로 연결을 닫았습니다. 읽지 않은 업로드 데이터가 남아 있거나 추가 데이터가 도착하면 TCP 리셋 때문에 클라이언트가 마지막 413 응답을 읽지 못할 위험이 있었습니다. 이를 줄이기 위해 응답 전송 완료와 최종 close를 분리했습니다. HTTP의 단계적 종료 설명은 [RFC 9112 §9.6](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.6)을 참고하세요.

```text
한도 초과 확인 → 413 응답 헤더·본문 준비
    ↓
응답을 모두 send()에 넘김
    ↓
begin_drain(): DRAIN_INPUT 전환 + 현재 시각부터 고정된 2초 기한 설정
    ↓
한도 초과 본문이 남았으면 drain_remaining만큼 4KiB 버퍼로 읽어 버림
    ├─ EAGAIN: WAITING → poll(POLLIN) → 다음 차례
    └─ 상대 EOF: 업로드 중단으로 보고 남은 길이 대기 종료
    ↓
shutdown_write(): shutdown(fd, SHUT_WR)
남은 본문이 없던 요청은 위 폐기 단계를 생략
    ↓
drain_step(): 4KiB 임시 버퍼로 recv() 후 버림
    ├─ 양수: 읽은 데이터 폐기, 기한은 연장하지 않음
    ├─ EAGAIN: WAITING 반환 → poll(POLLIN) → 다음 작업자 차례
    ├─ recv == 0: 상대 송신 종료 확인 → response_complete
    └─ 오류 / 정리 기한 도달: drain_error / drain_timeout
    ↓
작업자가 감지하면 FINISHED 공개 → eventfd로 메인 깨움
    ↓
메인 connection_close(): 파일·소켓 close → 슬롯 FREE
```

GET 파일 응답과 정상 POST 응답에도 같은 종료 절차를 적용합니다. 메인이 WAITING 연결의 정리 기한 만료를 먼저 확인한 경우에는 작업자 재배정 없이 `drain_timeout`으로 직접 회수합니다. 초기 통신 오류·응답 전송 실패·수신/송신 타임아웃은 기존처럼 연결을 정리하며, 모든 오류 경로에서 반쪽 종료를 보장하는 것은 아닙니다.

### Windows–WSL에서 조기 송신 종료를 보완한 이유

최초 개선은 응답 직후 SHUT_WR를 수행하고 남은 본문을 읽는 순서였습니다. 같은 실행 파일에 WSL 내부에서 요청하면 413·22바이트가 정상 도착했으나, Windows localhost 경로에서는 응답 헤더 또는 본문을 기다리다 멈추는 현상을 재현했습니다. 별도 비교 서버에서 413은 즉시 보내되 남은 업로드를 폐기한 다음 SHUT_WR를 수행하면 정상 완료했습니다.

따라서 현재 구현은 한도 초과 요청의 선언된 Content-Length 전체를 오버플로 검사와 함께 보존하고, 헤더와 함께 이미 읽은 본문을 뺀 나머지만 `drain_remaining`으로 추적합니다. 413 전송 자체는 기다리지 않습니다. 남은 본문 폐기 후 송신 방향을 종료하며, 클라이언트가 먼저 EOF를 보내면 전체 선언 길이를 기다리지 않습니다. 모든 정리는 최초 2초 기한 안에서 진행하며 고정 sleep이나 본문 전체 저장은 사용하지 않습니다.

이 결과는 이 컴퓨터의 Windows–WSL localhost 전달 경로와 조기 반쪽 종료의 상호작용을 가리킵니다. 특정 WSL 내부 결함까지 확정한 것은 아닙니다. WSL의 localhost 중계 구조는 [Microsoft WSL 문서](https://github.com/microsoft/WSL/blob/master/doc/docs/technical-documentation/localhost.md)에 설명돼 있습니다. 최초 socketpair 테스트만으로는 이 전달 경로를 검증할 수 없었습니다.

| 핵심 함수 / 상태 | 역할 |
|---|---|
| `response_sent()` | 헤더·메모리 본문·파일의 송신 잔량이 없는지 확인 |
| `reject_request()` | 413을 준비하면서 이미 읽은 본문을 제외한 남은 업로드 길이 기록 |
| `begin_drain()` | 정리 단계와 절대 마감 시각 설정. 알려진 남은 업로드가 없으면 바로 송신 방향 종료 |
| `shutdown_write()` | 송신 방향만 종료하고 `write_shutdown` 기록 |
| `drain_step()` / `discard_received()` | 남은 업로드 폐기 후 송신 방향 종료. EAGAIN이면 양보, EOF·오류·기한 만료면 종료 |
| `process_turn()` | 정리 수신도 일반 수신과 합산해 차례당 64KiB·64단계 예산 적용 |
| `connection_deadline()` / `poll_client()` | DRAIN_INPUT은 갱신되는 유휴 시각 대신 고정 마감 시각과 POLLIN 사용 |
| `connection_close()` | 작업자가 사용하지 않는 연결의 fd를 메인만 최종 close |

정리 기한 `DRAIN_TIMEOUT_MS = 2000`과 임시 버퍼 `DRAIN_BUFFER_SIZE = 4096`은 `src/server.h`에서 정했습니다. 2초는 정리 단계 진입부터의 전체 기한이며 수신할 때 연장하지 않습니다. 스케줄링 때문에 실제 close는 기한보다 늦을 수 있습니다. 그동안 연결 슬롯은 유지되지만 EAGAIN 때 작업자를 반환하므로 유휴 연결이 작업자를 계속 점유하지 않습니다.

읽어서 버리는 데이터는 정상 업로드로 처리하거나 저장하지 않습니다. POST 본문 허용량 64KiB는 그대로입니다. 브라우저 테스트의 8초 제한은 별도의 전체 요청 시간 제한이며 그대로 유지합니다. 기한이 끝나면 상대가 계속 보내더라도 자원을 회수하므로 모든 상황에서 응답 전달을 보장하지는 않습니다.

| 종료 로그 | 의미 |
|---|---|
| `response_complete` | 응답 송신 후 정리 단계에서 상대 EOF까지 확인. 브라우저의 HTTP 해석 완료를 보증하는 로그는 아님 |
| `drain_timeout` | 상대 EOF 전 고정된 정리 기한에 도달하여 회수 |
| `drain_error` | 남은 데이터를 읽는 중 실제 recv 오류 발생 |
| `shutdown_error` | 송신 방향 종료 호출 실패 |

이 HTTP 연결 정리와 Ctrl+C·SIGTERM에 의한 서버 프로그램 종료는 구분합니다. 프로그램 종료는 기존처럼 accept·작업 배정을 중단하고 작업자를 join한 다음 남은 자원을 회수합니다. 진행 중 응답과 2초 정리 기한이 끝나기를 기다리는 프로세스 종료 정책은 추가하지 않았습니다.

코드 변경 적용 시 **실행 중인 서버를 Ctrl+C로 종료 → `make` → `./webserver 19006 ./www`로 재시작**해야 합니다. 브라우저 새로고침만으로 기존 프로세스의 C 코드가 바뀌지는 않습니다.

## 지원 범위와 오류 응답

| 상황 | 결과 |
|---|---|
| GET 파일 존재 | 200, Content-Length·Content-Type, 파일 바이트 |
| POST `/echo` | 200, Content-Length만큼 원본 본문. 빈 본문도 허용 |
| GET·POST `/cgi-bin/이름` | CGI가 생성한 상태·Content-Type·본문. CGI_ROOT 지정 시 활성화 |
| 파일 없음 / 지원하지 않는 경로 | 404 |
| CGI 비정상 종료·잘못된 출력·응답 한도 초과 / 실행 기한 초과 | 502 / 504 |
| `..` 등 차단된 경로 / 일반 파일 아님 | 403. 중간 심볼릭 링크는 Linux 오류에 따라 404일 수도 있음 |
| GET·POST 외 메서드 | 405 + `Allow: GET, POST`. HEAD도 미지원이며 HEAD 오류 응답에는 본문 생략 |
| 잘못된 문법 / HTTP/1.1 Host 누락·중복 / 중복 Content-Length | 400 |
| GET의 양수 본문 길이 / Transfer-Encoding | 400 |
| POST Content-Length 누락 / 본문 한도 초과 | 411 / 413 |
| Expect 헤더 / 헤더 한도 초과 / 지원하지 않는 HTTP 버전 | 417 / 431 / 505 |
| 예상하지 못한 파일 처리 오류 | 500 |
| 수신·송신 타임아웃 / 끊긴 소켓 | 연결 종료, HTTP 오류 응답 보장 없음 |

HTTP 전체 명세 구현은 아닙니다. chunked, 100-continue, Range, 조건부 요청, keep-alive는 구현하지 않습니다. 파일을 보내는 동안 내용이 바뀌면 응답 일관성을 보장하지 않습니다. 디스크 파일 읽기는 일반 blocking I/O이므로 느린 저장장치는 작업자를 지연시킬 수 있습니다.

## 최소 단위 테스트와 실제 검증 결과

```bash
make test
make test_cgi
```

`make test`는 네트워크 서버를 띄우지 않고 HTTP 5개 묶음과 종료 처리 5개 묶음을 검사합니다. HTTP·파일 테스트는 저장소의 `www` 샘플을 기준으로 합니다. `make test_cgi`는 Python 3 표준 라이브러리로 실제 TCP 서버를 임시 포트에 띄우는 CGI 통합 테스트 8개를 실행합니다. CGI GET·POST, 환경 변수·바이너리/분할 입력, 실행 경로 차단, 출력/크기 오류, 실행 기한, 4개 CGI 대기 중 정적 응답, 서버 종료 시 자식 회수를 확인합니다. 테스트 서버와 임시 CGI 디렉터리는 종료 시 정리합니다.

| 테스트 묶음 | 기대 결과 |
|---|---|
| GET·헤더 경계 | 마지막 빈 줄이 덜 오면 미완성, 완성된 GET·HTTP/1.0 파싱 성공 |
| POST 길이 경계 | 0·65,536 허용, 미지정 411, 65,537은 413 |
| 오류 요청 | Host 누락·중복 길이·잘못된 줄바꿈 등 400, HEAD·DELETE는 405 |
| 응답 헤더 | Content-Length·Connection·Allow 포함, 작은 출력 버퍼 오류 반환 |
| 파일·경로·MIME | index 파일 열기, 없는 파일 404, 인코딩된 `..` 403, NUL 경로 400, CSS MIME |

기존 HTTP 테스트가 성공하면 `All 5 unit groups passed.`가 출력됩니다. 이어지는 `test_drain`은 로컬 `socketpair`로 다음 종료 동작을 검사하며 실제 TCP 리스너·브라우저는 사용하지 않습니다.

| 종료 테스트 묶음 | 기대 결과 |
|---|---|
| 512KiB 한도 초과 | 파싱 후 413·본문 22바이트를 먼저 수신, 남은 본문 폐기 후 송신 종료·상대 EOF 시 회수 |
| 1MiB 한도 초과 | 512KiB와 같은 흐름, 추가 수신에도 정리 기한 유지 |
| 정상 POST | 200·본문 hello 수신 후 동일한 반쪽 종료·상대 EOF 처리 |
| 클라이언트 업로드 중단 | 413 수신 뒤 상대 EOF가 오면 선언 길이가 남아 있어도 송신 종료·회수 |
| 고정 정리 기한 | EAGAIN이면 정리 단계 유지, 수신으로 기한 비연장, 기한을 지난 상태에서는 읽을 데이터가 있어도 drain_timeout |

종료 테스트까지 성공하면 `All 5 drain unit groups passed.`가 출력됩니다. 기한 만료 테스트는 실제로 2초를 기다리는 대신 마감 시각을 지난 값으로 설정합니다. 업로드는 차례당 64KiB로 공급하여 예산 경계에서 마지막 본문을 버린 뒤에도 즉시 송신 종료되는지 확인합니다.

**2026-11-23, 이 컴퓨터의 WSL Ubuntu 24.04에서 확인:**

- `make all test test_cgi` 통과: C11, `-Wall -Wextra -Wpedantic -Werror`, pthread 빌드.
- HTTP 5개·종료 5개 단위 테스트 묶음과 CGI 통합 테스트 8개 통과.
- 테스트·CGI 예제를 포함한 C 함수의 선언·중괄호 포함 30줄 이하 조건 확인.
- Windows–WSL 문제 재현 후, 수정한 별도 서버에 Windows Node.js fetch로 512KiB·1MiB POST 각각 4건을 함께 보내 모두 413·22바이트 수신 확인. WSL 내부에서도 같은 크기 요청의 정상 응답 확인.
- 실제 브라우저 UI와 부하 테스트는 실행하지 않았습니다. 아래는 사용자가 실행할 수 있는 수동 확인 절차와 기대 결과입니다.

## 수동 테스트 방법과 기대 결과

서버를 `./webserver 19006 ./www`로 실행한 상태에서 다른 **WSL 터미널**에서 수행합니다. `curl -I`는 HEAD이므로 사용하지 않고, GET 헤더도 보려면 `curl -i`를 사용합니다.

| 명령 / 조작 | 기대 결과 |
|---|---|
| 브라우저 `http://127.0.0.1:19006/` | 제목 Team 2 Web Server, 스타일과 나무 이미지 표시 |
| `curl -i http://127.0.0.1:19006/` | 200, `text/html; charset=utf-8`, HTML 본문 |
| `curl -i http://127.0.0.1:19006/style.css` | 200, `text/css; charset=utf-8` |
| `curl -i http://127.0.0.1:19006/jungle.svg` | 200, `image/svg+xml`, SVG 본문 |
| `curl -i http://127.0.0.1:19006/missing-file` | 404, 텍스트 오류 본문 |
| `curl -i --data-binary 'hello' http://127.0.0.1:19006/echo` | 200, Content-Length: 5, 본문 `hello` |
| `curl -i --data-binary '' http://127.0.0.1:19006/echo` | 200, Content-Length: 0 |
| `curl -i -X PUT http://127.0.0.1:19006/` | 405, `Allow: GET, POST` |
| `curl -i -X POST http://127.0.0.1:19006/echo` | Content-Length 없는 POST이므로 411 |
| `curl -i --path-as-is http://127.0.0.1:19006/%2e%2e/README.md` | 403, 루트 밖 파일 내용 미노출 |

각 응답에는 `Connection: close`가 있고 응답 후 연결이 종료됩니다. 다른 크기의 이미지/파일은 `www`에 추가해 같은 방식으로 확인할 수 있습니다.

### index.html에서 동시 요청 확인

1. 서버를 실행하고 브라우저에서 `http://127.0.0.1:19006/`에 접속합니다. HTML 파일을 직접 열면 테스트가 비활성화됩니다.
2. 페이지 아래 **동시 HTTP 요청 테스트**에서 요청 수 4개·8개·12개·36개 중 하나를 선택합니다.
3. **테스트 종류**를 선택합니다. GET은 응답 파일 크기를 검사하고 POST는 실제 보내는 요청 본문의 크기를 검사합니다.
4. GET에서는 **응답 본문 크기**를 4·16·64·128KiB 중 선택합니다. POST에서는 **요청 본문 크기**를 4·16·64·128·512KiB·1MiB·100MiB 중 선택합니다. 512KiB는 524,288바이트, 1MiB는 1,048,576바이트, 100MiB는 104,857,600바이트입니다. 크기는 요청 하나당 기준이며, POST 응답은 에코이므로 별도로 선택하지 않습니다.
5. **동시 요청 보내기**를 누르면 GET은 `/test-data/body-선택크기kib.bin`, POST는 정확히 `/echo`로 모든 fetch를 먼저 시작한 뒤 응답을 모읍니다.
6. 표의 **요청 본문 (설정)**과 **응답 본문 (수신)**을 구분해서 확인합니다. 요청 본문 크기는 브라우저에 전달한 데이터 크기이며 헤더를 제외합니다. 서버가 해당 바이트를 모두 받았다는 뜻은 아닙니다.

GET의 예상 결과는 다음과 같습니다. HTTP 200이면서 실제 응답 크기가 선택값과 일치해야 검증 통과입니다.

| 선택한 크기 | 예상 HTTP 코드 | 예상 Content-Length 및 실제 본문 크기 |
|---|---|---:|
| 4KiB | 200 | 4,096바이트 |
| 16KiB | 200 | 16,384바이트 |
| 64KiB | 200 | 65,536바이트 |
| 128KiB | 200 | 131,072바이트 |

128KiB도 GET 정적 파일 응답이므로 정상 전송 대상입니다. POST 요청 본문의 64KiB 제한과는 별개이며, 큰 파일 응답은 여러 차례에 나눠 보냅니다. 모든 응답은 `Content-Type: application/octet-stream`, `Connection: close`를 포함합니다. 테스트 파일은 `www/test-data`에 정확한 크기의 바이너리 데이터로 저장하며, 다른 문서 루트로 실행하려면 해당 루트에도 이 파일들이 있어야 합니다.

POST는 선택한 크기만큼 바이트 값 65(문자 A)를 채운 데이터를 보내며, Content-Length는 브라우저가 설정합니다. 예상 결과는 다음과 같습니다.

본문은 테스트 시작 시에만 1MiB 조각으로 Blob을 한 번 생성해 동시 요청에서 공유합니다. 선택값이나 예상 결과만 바꿀 때는 큰 본문을 만들지 않습니다. 브라우저의 내부 전송 버퍼는 별도로 사용될 수 있습니다.

| POST 요청 본문 | 예상 HTTP 코드 | 예상 응답 본문 |
|---|---|---|
| 4KiB | 200 | 4,096바이트, 보낸 데이터와 동일 |
| 16KiB | 200 | 16,384바이트, 보낸 데이터와 동일 |
| 64KiB | 200 | 65,536바이트, 보낸 데이터와 동일 |
| 128KiB | 413 | `413 Content Too Large` + 줄바꿈, 22바이트 |
| 512KiB | 413 | `413 Content Too Large` + 줄바꿈, 22바이트 |
| 1MiB | 413 | `413 Content Too Large` + 줄바꿈, 22바이트 |
| 100MiB | 413 | `413 Content Too Large` + 줄바꿈, 22바이트 |

POST 200 응답은 길이와 본문 내용까지 검사합니다. POST 128KiB·512KiB·1MiB에서 413 응답 본문까지 수신하면 **검증 통과 · 413 차단**으로 표시합니다. 이는 예상한 한도 초과 거절을 확인했다는 뜻이며 업로드 성공을 의미하지 않습니다. 현재 서버는 즉시 413을 보내고 알려진 남은 업로드를 읽어 버린 뒤 송신 방향을 종료합니다. 상대 EOF 또는 처음 설정한 2초 기한에 연결을 닫습니다. 연결 / 수신 실패나 시간 초과는 413 검증 통과로 간주하지 않습니다. 본문 허용량과 브라우저의 8초 제한은 그대로이며, 브라우저 실동작은 위 절차로 별도 확인해야 합니다.

100MiB도 동일하게 413을 기대하는 한도 초과 테스트입니다. 큰 업로드를 2초 정리 기한 내에 폐기하지 못하면 연결 / 수신 실패 또는 브라우저의 8초 시간 초과가 발생할 수 있습니다. 옵션 추가를 위해 서버 허용량이나 기한을 늘리지는 않았으며 100MiB 실제 전송 테스트는 실행하지 않았습니다.

GET은 요청별 고유 쿼리를 붙이며, GET·POST 모두 `cache: no-store`를 사용하고 응답 본문을 모두 받은 뒤 완료로 기록합니다. 8초 안에 끝나지 않는 요청은 중단하고 시간 초과로 표시합니다. 결과는 **검증 통과 / 실패** 건수로 집계합니다. 부하 측정 도구는 아니며 서버 재빌드 없이 새로고침하면 적용됩니다.

개발자 도구의 **Network**를 열고 GET은 `concurrency=`, POST는 `echo`로 필터링하면 요청별 시간대를 확인할 수 있습니다. 서버 터미널의 accept·close 로그도 함께 확인하세요. 브라우저 자체의 연결 수 제한과 스케줄링 때문에 fetch 개수가 서버의 실제 동시 연결 수나 작업자 동시 실행 수를 증명하지는 않습니다. 표시 시간에는 브라우저 대기와 본문 수신이 포함됩니다. 다른 탭의 연결 등으로 서버의 8개 슬롯이 가득 차면 연결 실패가 표시될 수 있습니다.

소수 동시 요청을 수동 확인하려면 다음을 실행합니다. 이는 부하 측정이 아니며 자동 실행하지 않았습니다.

```bash
for i in 1 2 3 4; do
    curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:19006/ &
done
wait
```

기대 결과는 `200` 네 줄입니다. 서버 터미널에는 accept와 `response_complete` 종료 로그가 보입니다. Ctrl+C로 종료하면 `[server] stopped`가 출력됩니다. 실행 파일 정리는 `make clean`입니다.
