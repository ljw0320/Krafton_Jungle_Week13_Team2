# Krafton_Jungle_Week13_Team2

C/POSIX 소켓으로 구현한 fork 기반 HTTP 정적 파일 서버입니다.
외부 HTTP 라이브러리를 사용하지 않습니다. 기존 에코 처리는 HTTP 처리로 교체했습니다.
오늘 범위는 요청 파싱, 응답 생성, 정적 파일 서빙입니다. 동적 기능과 프록시는 다음 단계입니다.

## 빌드와 실행

WSL2/Linux에서 실행합니다.

```sh
make
./build/web-server 8080 public 64 5 5
# 포트 / 정적 루트 / 최대 자식 수 / 다음 요청 대기 초 / 종료 유예 초
# 생략 시 위 기본값 사용
curl -v http://localhost:8080/
curl --http1.0 -v http://localhost:8080/style.css
curl -v http://localhost:8080/sample.svg
curl -v http://localhost:8080/missing.html
curl -v --data 'name=kim' http://localhost:8080/
make test
make sanitize
```

브라우저에서 `http://localhost:8080/`을 열면 HTML·CSS·SVG 예제를 볼 수 있습니다.
모든 로컬 IPv4 인터페이스에 바인딩하므로 방화벽에 따라 외부에서도 접속할 수 있습니다.
정적 루트에는 공개할 파일만 넣어야 합니다.

## 확정 정책

| 항목 | 동작 |
|---|---|
| 요청 버전 | HTTP/1.0·1.1. 형식 오류 400, 그 외 버전 505 |
| 메서드 | GET으로 정적 파일 제공. POST 본문 수신 후 동적 처리 미구현을 501로 응답. 그 외 405 |
| 연결 | 1.0은 항상 종료. 1.1은 기본 유지, `Connection: close` 시 종료. 요청 횟수 제한 없음 |
| 요청 본문 | Content-Length만 지원. 없으면 0바이트. GET 본문도 길이만큼 소비 |
| 요청 거부 | 문법·길이 오류, 중복 Content-Length(같은 값 포함), HTTP/1.1 Host 누락·중복은 400 후 종료 |
| Transfer-Encoding | Content-Length와 함께 있으면 400. 1.1에서 단독 사용은 501, 1.0에서는 400. 모두 연결 종료 |
| Expect | HTTP/1.1의 100-continue는 헤더 검증 후 중간 응답. 그 외 417 후 종료 |
| URI 한도 | 경로·쿼리 포함 8KiB, 초과 414 |
| 헤더 한도 | 요청 라인·종료 빈 줄 포함 32KiB, 초과 431 |
| 본문 한도 | 1MiB, Content-Length로 초과 확인 시 본문을 기다리지 않고 413 |
| 유휴 시간 | 첫 요청 또는 다음 요청의 첫 바이트를 기본 5초 대기. 초과 시 조용히 종료 |
| 요청 수신 기한 | 첫 바이트를 받은 뒤 헤더·본문 전체에 고정 30초. 진행돼도 연장하지 않음. 초과 시 가능한 경우 408 후 종료 |
| 응답 | 최종 응답은 Content-Length 사용. 오류 본문은 UTF-8 일반 텍스트 |
| 전송 제한 | send가 10초 동안 진행되지 않으면 종료. 진행하면 갱신. 전체 응답 기한 없음 |
| 동시 처리 | 기본 자식 64개, CLI로 변경 가능(1~4096). 상한에서는 accept를 미룸. backlog 128은 별도 대기 큐 |

오류 상태를 보내기 전에 파일을 열고 종류·크기를 확인합니다.
이미 응답 헤더를 보낸 뒤 파일 읽기나 전송이 실패하면 다른 오류 응답을 덧붙이지 않고 연결을 닫습니다.
정적 파일은 16KiB 버퍼로 읽어 전송하고, 파일 전체를 메모리에 올리지 않습니다.
POST 본문은 현재 기능에서 사용하지 않으므로 16KiB 버퍼로 끝까지 소비하며 별도로 보관하지 않습니다.

## 파일 접근

- CLI로 지정한 정적 루트 안의 일반 파일만 제공합니다.
- `/`와 하위 디렉터리는 `index.html`을 제공합니다. 기본 파일이 없으면 403입니다.
- 디렉터리 목록은 제공하지 않습니다. 심볼릭 링크와 숨김 경로 요소는 금지합니다(`.well-known` 포함).
- 쿼리를 먼저 분리하고 경로를 한 번 URL 디코딩합니다. 잘못된 인코딩·NUL·제어 문자·역슬래시는 400입니다.
- `..`·`.`·숨김 경로 요소는 403입니다. 파일을 여는 각 단계에 `openat`와 `O_NOFOLLOW`를 적용합니다.
- 접근 정책 위반은 403, 없는 경로는 404, 예상하지 못한 파일 오류는 500입니다.
- 디렉터리는 끝의 `/` 유무와 관계없이 기본 파일을 제공합니다. 일반 파일 뒤의 `/`는 403입니다.
- HTML·CSS·JS·PNG·JPG·SVG 등을 확장자로 MIME 매핑합니다. 알 수 없는 확장자는 application/octet-stream입니다.

## 코드 읽는 순서

| 파일 | 역할 |
|---|---|
| `main.c` | CLI 인자 해석·검증과 서버 실행 호출 |
| `server/server.c` | 서버 초기화, 시그널, 연결 수락, 자식 관리와 종료 |
| `server/server.h` | 서버 실행 진입점 `server_run` |
| `http/http.c` | 연결 반복, 요청 처리 순서, 메서드별 응답 결정 |
| `http/http_parse.c` | 요청 라인·헤더 해석과 제약 검증 |
| `http/http_io.c` | 시간 제한, 헤더 경계 탐색, 부분 송수신, 본문 소비 |
| `http/http_response.c` | 응답 헤더·오류 본문 생성과 전송 |
| `http/http_static.c` | 경로 디코딩, 안전한 파일 열기, MIME, 파일 전송 |
| `http/http.h` | 서버에서 사용하는 공개 진입점 `serve_http` |
| `http/http_internal.h` | HTTP 모듈 사이에서만 공유하는 요청 타입·한도·함수 계약 |

1. `main.c/main` → `server/server.c/server_run`: CLI 검증 → 정적 루트 열기 → 시그널 설정 → 리스너 → 부모 루프 → 종료 정리.
2. `server/server.c/run_parent`: 자식 회수 → 상한 확인 → pselect → accept → non-blocking 연결 → fork.
3. `server/server.c/run_child`: 부모 전용 자원 정리 → 시그널 복원 → serve_http → 연결·루트 fd 정리 → _exit.
4. `http/http.c/serve_http`: 연결별 저장소 할당 → `serve_request` 반복 → 저장소 해제. 연결 유지·종료·입출력 실패는 `connection_action`으로 구분합니다.
5. `serve_request`·`read_request`: 요청 하나 수신·검증 → POST 거부 또는 정적 파일 응답 → 연결 유지 여부 판단. 오류 응답 전송 실패도 실패로 반환합니다.
6. `http/http_io.c/receive_header`: 첫 바이트 대기와 요청 기한 시작. `receive_header_content`는 peek → `find_header_end`로 경계 탐색 → `receive_exact`로 현재 헤더만 소비합니다.
7. `http/http_parse.c/parse_request`: 요청 초기화 → `parse_request_line` → `parse_headers` → `validate_request_headers`. 개별 문법과 헤더 간 제약을 나눕니다.
8. `http/http_io.c/receive_body`·`http/http_static.c/decode_path`: 본문을 정확히 소비한 뒤 용량을 확인하며 경로를 디코딩합니다.
9. `http/http_static.c/open_static`·`serve_static` → `http/http_response.c/send_headers` → `http/http_io.c/send_all`: 루트 기준 파일 접근 검사 → 파일 크기·MIME → 헤더 → 부분 송신 처리.

`request_storage`는 연결 동안 헤더·경로 버퍼를 소유합니다. 요청의 method·target은 헤더 버퍼를
빌려 쓰는 읽기 전용 포인터이며 다음 요청 수신 전까지만 유효합니다. 헤더 수신과 경로 디코딩에는
NUL 공간을 포함한 실제 버퍼 용량을 전달합니다. `open_static`은 경로 버퍼를 변경하고,
성공할 때만 파일 fd 소유권을 호출자에게 넘깁니다. `serve_static`이 파일 fd를 닫습니다.
`serve_http`는 소켓·루트 fd를 빌려 쓰고 닫지 않습니다. 호출자는 non-blocking 소켓과 SIGPIPE 무시를 설정합니다.
각 모듈의 보조 함수는 `static`으로 숨깁니다. HTTP 단위 테스트도 `.c` 파일을 직접 포함하지 않고
내부 헤더를 통해 실제 모듈을 링크합니다. MIME은 TCP 응답으로 검증합니다.

수신은 현재 요청에 필요한 바이트만 소비합니다. 다음 요청이 함께 도착해도 소켓에 보존합니다.
수신·전송의 시간 제한은 non-blocking 소켓과 poll, CLOCK_MONOTONIC으로 처리합니다.
부모의 pselect는 연결 수락 대기용이며 클라이언트 동시 처리는 fork가 담당합니다.
SIGCHLD 회수·SIGPIPE 무시·SIGINT/SIGTERM 종료 처리는 기존 구조를 유지합니다.
부모만 SIGTERM을 받으면 종료 유예 후 남은 자식에 SIGTERM, 이후 SIGKILL을 보내고 회수합니다.
Ctrl+C는 프로세스 그룹 전체에 전달되므로 자식이 즉시 종료될 수 있습니다.

## 검증과 남은 범위

`make test`는 파서·경로 디코딩·MIME·프로세스 오류 단위 테스트와 실제 TCP 통합 테스트를 실행합니다.
단위 테스트는 요청 라인 문법·숫자 오버플로·버퍼 용량·헤더 경계·오류 전송 실패와
고정 시드 변형 입력 12,000건의 파서·경로 디코더 경계 조건을 확인합니다.
통합 테스트는 바이너리 파일, 모든 분할 위치의 헤더, 파이프라이닝, POST 본문 경계, 100회 초과 재사용과 fd 회수,
잘못된 요청, 크기 한도, 경로 순회·심볼릭 링크·숨김 파일·FIFO 차단, 동시 접속·자식 상한,
유휴·전송 정지·전체 요청 기한, 연결 리셋, 자식 회수·강제 종료·재시작을 확인합니다.
실제 10초·30초 타이머를 검증하므로 테스트에는 약 1분이 걸립니다.
`make sanitize`는 AddressSanitizer와 UndefinedBehaviorSanitizer로 같은 테스트를 실행합니다.
Python 3 표준 라이브러리와 Linux `/proc`를 사용합니다.

메시지 길이 모호성·헤더 줄바꿈·중복 헤더·잘못된 요청 뒤 추가 요청을 이용한 우회,
인코딩된 경로 순회·응답 줄바꿈 주입·기본 파일 심볼릭 링크·FIFO 차단도 테스트합니다.
메시지 경계 검토 기준은 [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html)의 5·6·11절입니다.
이 검증은 현재 지원 정책에 대한 회귀·보안 점검이며 전체 RFC 적합성이나 모든 공격의 부재를 보증하지 않습니다.
Host는 누락·중복·기본 금지 문자만 검사하며 URI authority 전체 문법은 검증하지 않습니다.
가상 호스트 라우팅이나 프록시에서 Host를 사용하기 전에는 이 검증을 확장해야 합니다.

- chunked를 지원하지 않으므로 HTTP/1.1 전체 규격을 구현한 서버는 아닙니다.
- POST 애플리케이션 로직·동적 응답·Reverse Proxy·라우팅은 아직 없습니다.
- HTTPS·캐싱·Range 응답은 구현하지 않습니다.
- 공개 루트는 신뢰할 수 있는 관리자가 관리해야 합니다. 서비스 중 디렉터리를 루트 밖으로 옮기는 변경은 피해야 합니다.
- 활동 중인 64개 연결이 슬롯을 계속 차지할 수 있고, 계속 진행되는 느린 응답의 총 시간은 제한하지 않습니다.
- 종료 유예 후 강제 종료되면 미전송 데이터가 손실될 수 있습니다.
- 최종 과제의 처리량 부하 측정과 프록시 검증은 이후 단계입니다.
