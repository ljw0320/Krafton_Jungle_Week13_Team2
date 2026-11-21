# Krafton Jungle Week 13 — Team 2 Web Server

`learning/05_echo_thread-nonblocking`의 **non-blocking 소켓 + poll + 고정 스레드 풀 + eventfd** 구조를 참고한 학습용 C 웹서버입니다. GET으로 HTML·CSS·이미지를 제공하고 POST `/echo`로 요청 본문을 그대로 돌려줍니다. HTTP 메서드는 **GET, POST만** 지원합니다.

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
./webserver PORT DOCUMENT_ROOT
```

| 인자 | 의미 |
|---|---|
| `PORT` | 필수, 1~65535 정수. 사용 중인 포트면 bind 오류 후 실패 종료 |
| `DOCUMENT_ROOT` | 필수, 정적 파일 루트. 상대 경로는 실행 디렉터리 기준 |

주소는 `127.0.0.1`로 고정합니다. 포트와 문서 루트만 실행 인자로 받으며 Reverse Proxy 라우팅 인자는 구현하지 않습니다. 다른 포트·루트 예: `./webserver 8080 /tmp/my-site`.

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
| 자체 테스트 최소화, 10,000건 제외 | 빌드와 5개 단위 테스트 묶음만 실행. 부하 테스트는 작성·실행하지 않음 |
| 구조·실행 흐름·테스트·결정 사항 문서화 | 이 README와 `docs/ARCHITECTURE.md` |

### 구현하면서 직접 정한 사항

| 결정 | 이유와 동작 |
|---|---|
| 응답마다 `Connection: close` | 제안한 첫 단계의 정적 응답에 범위를 맞춤. keep-alive·pipelining 미지원 |
| GET은 문서 루트 아래 정적 파일 | `/`와 끝이 `/`인 경로는 `index.html`, 쿼리는 파일 선택에서 제외 |
| POST는 정확히 `/echo` | 별도 저장·업무 로직 없이 본문 경계 처리를 학습. 원본 바이트를 `application/octet-stream`으로 응답 |
| POST에 Content-Length 필수 | 미지정 411, 0 허용, 최대 65,536바이트. 초과 413 |
| GET 요청 본문은 길이 0만 허용 | 양수 Content-Length는 400. 본문 수신 기능은 POST에 한정 |
| 헤더 전체 최대 16,384바이트 | 요청 줄과 마지막 빈 줄 포함. 초과 431, 누적 버퍼 상한 고정 |
| 본문과 헤더는 고정 크기 버퍼 | 직접 malloc/free 없이 연결 슬롯에서 보관. 파일은 최대 16KiB씩 읽음 |
| 경로는 openat 기반으로 조각별 검사 | URL 한 번 디코딩, `.`·`..`·빈 경로 조각·심볼릭 링크 차단. 디렉터리 목록 미제공 |
| 한 차례 수신·송신 각각 최대 64KiB, 최대 64단계 | 큰 응답이 작업자를 계속 점유하지 않도록 `05`의 예산 모델 유지 |
| 수신 무진행 5초, 송신 무진행 3초 | CLOCK_MONOTONIC 기준. 진전 시 갱신하며 요청 전체 시간 상한은 아님 |
| 연결 8개 초과는 새 소켓 종료 | 응답용 슬롯도 없으므로 503을 보장하지 않음. C10K는 구현·검증 범위 밖 |
| SIGINT/SIGTERM은 빠른 종료 | accept 중단 → 작업자 현재 차례 반환·join → 파일/소켓 회수. 응답 완료를 기다리는 graceful drain 미구현 |
| SIGPIPE 무시 | 상대가 응답 중 끊으면 send 오류로 해당 연결만 회수 |
| 이전 설계 문서 보존 | `docs/PREVIOUS_*`에 원본 복사. 현재 구현의 기준은 이 README |

Reverse Proxy는 기술 스택 예시의 괄호 안 항목이지만 이번 필수 구현항목 ①~⑤에는 없으므로 구현하지 않았습니다. TLS, 업로드 저장, CGI, DB, 인증, 부하 도구도 추가하지 않았습니다.

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
│   ├── connection.c            # 분할 수신, POST 본문, 파일·부분 송신
│   └── tests/test_http.c       # 최소 단위 테스트
├── www/                        # HTML, CSS, SVG 시연 파일
└── docs/
    ├── ARCHITECTURE.md          # 상태 전이·동시성·종료 설명
    ├── PREVIOUS_README.md       # 이전 문서 원본: 현재 사양 아님
    └── PREVIOUS_DESIGN_DECISIONS.md
```

## 실행 흐름

1. `main()`이 포트·루트 인자를 검사하고 `socket → SO_REUSEADDR → bind → listen`을 수행합니다. 리스너는 non-blocking입니다. TCP 3-way handshake 자체는 커널이 처리합니다.
2. 문서 루트 fd와 eventfd를 열고 시그널 처리기·작업자 4개를 준비합니다.
3. 메인이 `poll()`에서 리스너, 완료 알림, WAITING 연결의 읽기/쓰기 준비를 기다립니다. 준비된 연결은 FIFO 큐에 한 번만 들어갑니다.
4. 작업자는 연결의 한 차례를 맡아 `\r\n\r\n`까지 헤더를 누적합니다. GET은 파일을 열고, POST는 Content-Length만큼 본문을 모읍니다.
5. 응답 헤더를 먼저 보내고 파일/본문을 전송합니다. 부분 송신·EAGAIN이면 오프셋을 남겨 다음 차례에서 이어갑니다.
6. 작업자는 WAITING/FINISHED 상태를 mutex 아래 공개하고 eventfd로 메인을 깨웁니다. 메인만 연결·파일을 닫고 슬롯을 FREE로 돌립니다.
7. 종료 신호가 오면 신규 배정을 중단하고 작업자를 join한 뒤 모든 열린 연결·파일·리스너·eventfd 및 동기화 객체를 정리합니다.

```text
수신 누적 → 헤더 완성 → HTTP 파싱
                        ├─ GET  → 경로 검사 → 파일 열기
                        └─ POST → /echo 확인 → 본문 길이만큼 수신
        → 응답 헤더 → 파일/본문 분할 송신 → 연결 종료
```

HTTP/1.0·1.1 요청 줄을 해석하지만 항상 HTTP/1.1 응답과 `Connection: close`를 보냅니다. 클라이언트의 `Connection: keep-alive` 요청에도 연결은 재사용하지 않습니다. 본문 길이 이후 추가 데이터와 다음 요청은 처리하지 않습니다.

## 지원 범위와 오류 응답

| 상황 | 결과 |
|---|---|
| GET 파일 존재 | 200, Content-Length·Content-Type, 파일 바이트 |
| POST `/echo` | 200, Content-Length만큼 원본 본문. 빈 본문도 허용 |
| 파일 없음 / POST 다른 경로 | 404 |
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
```

네트워크 서버를 띄우지 않고 다음 5개 묶음만 검사합니다. 저장소의 `www` 샘플을 기준으로 합니다.

| 테스트 묶음 | 기대 결과 |
|---|---|
| GET·헤더 경계 | 마지막 빈 줄이 덜 오면 미완성, 완성된 GET·HTTP/1.0 파싱 성공 |
| POST 길이 경계 | 0·65,536 허용, 미지정 411, 65,537은 413 |
| 오류 요청 | Host 누락·중복 길이·잘못된 줄바꿈 등 400, HEAD·DELETE는 405 |
| 응답 헤더 | Content-Length·Connection·Allow 포함, 작은 출력 버퍼 오류 반환 |
| 파일·경로·MIME | index 파일 열기, 없는 파일 404, 인코딩된 `..` 403, NUL 경로 400, CSS MIME |

성공하면 `PASS ...` 5줄과 `All 5 unit groups passed.`가 출력됩니다.

**2026-11-21, 이 컴퓨터의 WSL Ubuntu 24.04에서 확인:**

- `make all test` 통과: C11, `-Wall -Wextra -Wpedantic -Werror`, pthread 빌드.
- 위 5개 단위 테스트 묶음 통과.
- 테스트를 포함한 C 함수 63개 모두 선언·중괄호 포함 30줄 이하. 가독성을 위한 빈 줄 정리 후 최댓값 30줄.
- 요청에 따라 네트워크 통합 테스트·브라우저 확인·부하 테스트는 실행하지 않았습니다. 아래는 사용자가 실행할 수 있는 수동 확인 절차와 기대 결과입니다.

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

소수 동시 요청을 수동 확인하려면 다음을 실행합니다. 이는 부하 측정이 아니며 자동 실행하지 않았습니다.

```bash
for i in 1 2 3 4; do
    curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:19006/ &
done
wait
```

기대 결과는 `200` 네 줄입니다. 서버 터미널에는 accept와 `response_complete` 종료 로그가 보입니다. Ctrl+C로 종료하면 `[server] stopped`가 출력됩니다. 실행 파일 정리는 `make clean`입니다.
