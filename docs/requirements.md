C 언어와 POSIX 소켓 API만으로 HTTP 웹 서버를 처음부터 만들고, Proxy 또는 Reverse Proxy 기능을 붙인다.
- 금지: `libevent`, `libuv`, `nginx`, `Apache`, `mongoose` 같은 완성된 HTTP / 이벤트 루프 라이브러리
- 허용: 표준 C 라이브러리, POSIX API (`<sys/socket.h>`, `<netinet/in.h>`, `<unistd.h>`, `<pthread.h>` 등)
- 빌드: `Makefile` 또는 `CMakeLists.txt`
- 실행: 포트, 정적 파일 루트, (Reverse Proxy면) 라우팅 규칙을 CLI 인자 또는 설정 파일로 받는다.
- 개발 환경: Windows는 WSL2(Ubuntu), Docker, 클라우드 중 하나에서 개발해야 한다.
- 이력서·포트폴리오에 넣을 수 있는 완성도

이번에 구현할 범위는 [overview.md](overview.md), 설계는 [architecture-tcp.md](architecture-tcp.md), [architecture-http.md](architecture-http.md), [architecture-proxy.md](architecture-proxy.md)에 있다.

---
# 필수 구현 항목

| 영역 | 해야 할 일 | 핵심 포인트 |
|---|---|---|
| TCP 소켓 서버 | `socket` → `bind` → `listen` → `accept` | `SO_REUSEADDR`, `accept` 블로킹 |
| 요청 파싱 | 요청 라인(메서드, URI, 버전), 헤더(`Host`, `Content-Length`, `Connection`), 본문 | `\r\n\r\n`으로 헤더 끝, `Content-Length`로 본문 끝 |
| 지원 메서드 | `GET`, `POST` | 나머지는 405 / 501 |
| 응답 생성 | 상태 라인, `Content-Type`, `Content-Length`, `Connection`, 본문 | `send`가 일부만 보낼 수 있음 → 루프 |
| 정적 파일 | 루트 디렉토리 파일 서빙, MIME 매핑(HTML, CSS, JS, PNG, JPG) | 루트 밖 접근(`../`) 차단 |
| 오류 응답 | 404 / 403 / 500 등 | 아래 상태 코드 표 |
| 동적 처리 | CGI 실행 또는 백엔드 서버로 전달 | Reverse Proxy로 백엔드 전달하면 이 항목도 같이 충족 |
| 동시 처리 | `fork` / `pthread`(스레드 풀) / I/O 멀티플렉싱 중 1개 이상 | 멀티플렉싱이면 이벤트 루프 필수 |
| Proxy | Forward 또는 Reverse 중 1개 이상 (둘 다면 가산) | 아래 비교 |

## Forward Proxy vs Reverse Proxy

| | Forward Proxy | Reverse Proxy |
|---|---|---|
| 누구 편인가 | **클라이언트**를 대신해 외부 서버에 요청 | **서버**를 대신해 외부 요청을 받음 |
| 흐름 | 클라이언트 → Proxy → 원 서버 | 클라이언트 → Reverse Proxy → 백엔드(1..N) |
| 클라이언트가 아는가 | 안다 (프록시 주소를 직접 설정) | 모른다 (그냥 서버라고 생각) |
| 목적지 결정 | 요청 URL의 호스트 (`GET http://example.com/ HTTP/1.1`) | 프록시의 라우팅 규칙 (경로, `Host` 헤더) |
| 활용 | 접근 제어, 클라이언트 익명화, 캐싱 | 로드 밸런싱, 백엔드 은닉·보안, TLS 종료, 캐싱 |
| 이번 과제 선택 기능 | 캐싱 | Round-Robin, 경로 라우팅(`/api/*` → 8001, `/static/*` → 8002) |

---
# 팀이 정해야 할 결정

1. **동시성 모델**
	- `epoll` 이벤트 루프: C10K 대응, 포트폴리오 가치 가장 큼. Linux 전용이라 macOS는 `kqueue` 분기 또는 `poll` 대체 필요.
	- `poll` 이벤트 루프: POSIX라 macOS/Linux 공통. 연결 수가 많아지면 O(n).
	- 스레드 풀: 코드가 블로킹 스타일이라 쉬움. 대신 락, 워커 수만큼만 동시 처리.
	- **결정됨**: `epoll` LT, 단일 스레드 이벤트 루프. 이유는 [architecture-tcp.md](architecture-tcp.md)의 결정 사항
2. **Proxy 종류**
	- 품질 요구사항의 "백엔드 응답 없음 / 타임아웃" 엣지 케이스는 Reverse Proxy를 전제로 한다.
	- Reverse Proxy는 라우팅 + Round-Robin + 타임아웃까지 설계 요소가 많아 보여줄 거리가 많다.
	- **결정됨**: Reverse Proxy만. Round-Robin은 마지막 단계. 이유는 [architecture-proxy.md](architecture-proxy.md)의 결정 사항
3. **동적 처리 방식**: CGI(`fork` + `exec` + 파이프 + 환경 변수) vs 백엔드 전달
	- **결정됨**: 백엔드 전달(Reverse Proxy). CGI는 하지 않는다
4. **설정 방식**: CLI 인자만 vs 설정 파일 (라우팅 규칙이 여러 줄이면 설정 파일이 편함)
	- **결정됨**: CLI 인자 `./server <port> <root> [<prefix>=<ip>:<port> ...]`. 설정 파일은 쓰지 않는다
5. **이식성**: 팀원 개발 환경(WSL / macOS)에 따라 `#ifdef __linux__` 분기 여부
	- **결정됨**: Linux 전용(WSL2). 분기를 두지 않는다
6. **`Transfer-Encoding: chunked` 요청**: 지원할지, 411 / 501로 거절할지
	- **결정됨**: `Transfer-Encoding`이 있으면 501 + 닫기. 이유는 [architecture-http.md](architecture-http.md)의 결정 사항

---
# 상태 코드와 엣지 케이스

| 상황                                 | 응답                                      |
| ---------------------------------- | --------------------------------------- |
| 요청 라인·헤더 형식이 깨짐, 불완전한 헤더           | `400 Bad Request`                       |
| 루트 밖 경로 접근, 읽기 권한 없음               | `403 Forbidden`                         |
| 파일 없음                              | `404 Not Found`                         |
| 서버는 아는 메서드지만 이 자원엔 허용 안 함          | `405 Method Not Allowed` (+ `Allow` 헤더) |
| 클라이언트가 요청을 끝까지 안 보내고 멈춤            | `408 Request Timeout`                   |
| POST인데 `Content-Length` 없음         | `411 Length Required`                   |
| 본문이 너무 큼                           | `413 Content Too Large`                 |
| URI가 너무 김                          | `414 URI Too Long`                      |
| 헤더 전체가 너무 큼                        | `431 Request Header Fields Too Large`   |
| 서버 내부 오류 (`malloc` 실패, 파일 읽기 오류 등) | `500 Internal Server Error`             |
| 모르는 메서드                            | `501 Not Implemented`                   |
| 백엔드 연결 실패, 백엔드 응답이 깨짐              | `502 Bad Gateway`                       |
| 백엔드가 시간 안에 응답 안 함                  | `504 Gateway Timeout`                   |
| HTTP 버전이 `HTTP/1.0`, `HTTP/1.1` 외  | `505 HTTP Version Not Supported`        |
| 클라이언트가 갑자기 끊음                      | 응답 없음. fd·버퍼·(프록시면) 백엔드 연결까지 회수         |

> 과제에 적힌 RFC 7230~7235는 현재 RFC 9110(의미), RFC 9112(HTTP/1.1 메시지 형식)로 대체되었다. 내용은 거의 같다.

---
# 숨은 함정 (요구사항에 직접 안 적혀 있지만 꼭 걸리는 것)

- **TCP는 바이트 스트림이다.** `recv` 한 번 ≠ 요청 하나. 요청 반쪽이 오거나 두 요청이 한 번에 온다. → 처리 방법은 [architecture-tcp.md](architecture-tcp.md)의 연결 상태 전이
- **`send`는 일부만 보낼 수 있다.** 반환값만큼만 나갔다고 보고 나머지를 이어서 보내야 한다.
- **`SIGPIPE`**: 끊긴 소켓에 쓰면 서버 프로세스가 죽는다. 무시하고 `EPIPE`로 처리.
- **헤더 이름은 대소문자 구분 없음**: `content-length`, `Content-Length` 둘 다 받아야 한다.
- **경로 정규화**: `/../../etc/passwd`, `%2e%2e/` 같은 우회. 디코딩 후 검사해야 한다.
- **keep-alive**: HTTP/1.1은 기본이 연결 유지, HTTP/1.0은 기본이 닫기. 유휴 연결 타임아웃이 없으면 fd가 고갈된다.
- **프록시 응답 중계**: 백엔드 응답이 `chunked`일 수 있다. 백엔드 연결을 응답마다 닫으면 "닫힐 때까지 그대로 중계"로 단순화 가능.

---
# 품질 체크리스트

- [x] 단위 테스트: HTTP 파서, MIME 매핑, 라우팅, 프록시 전달 로직
- [x] 기능 테스트: `curl`, 브라우저로 정적 파일 / 프록시 동작 확인
- [x] 기능 테스트: 동시 다중 클라이언트 접속
- [ ] 부하 테스트: `ab`, `wrk`, `hey`로 동시 요청 수별 처리량 측정
- [x] 메모리: `valgrind` 또는 `-fsanitize=address,leak`
- [x] 엣지 케이스: 불완전한 헤더, 비정상 메서드, 잘못된 버전, 매우 큰 본문, 매우 긴 URI, 백엔드 무응답, 클라이언트 강제 종료
- [ ] 종료 처리: `SIGINT` / `SIGTERM` 안전한 셧다운, `SIGPIPE` 무시

---
# 채팅 서비스 (2026-11-25 추가)

과제의 필수 항목이 아니라, 직접 만든 웹 서버 위에 얹는 서비스다. 위의 제한(C, 완성된 HTTP / 이벤트 루프 라이브러리 금지, `Makefile`, CLI 인자)을 그대로 지킨다. 설계는 [architecture-chat.md](architecture-chat.md)에 있다.

| # | 요구사항 | 확인 결과 |
|---|---|---|
| C1 | 직접 만든 웹 서버를 통해 1:1 채팅을 한다 | `chat_messages`에서 8080의 WebSocket 터널을 거쳐 글과 ACK를 수신했다. |
| C2 | 여러 사람이 들어오면 무작위의 사람과 1:1로 잇는다 | `chat_matching`에서 6명을 10회 매칭해 서로 다른 짝 조합을 확인했다. |
| C3 | 글을 주고받는다 | 한글과 연속 글 100건의 전달·ACK·순서를 자동 확인했다. 브라우저 두 탭과 실제 한글 조합 입력은 미확인이다. |
| C4 | 이미지를 주고받는다 (JPG, PNG, 2MB까지) | JPG·PNG 프로토콜 검사와 최대 크기 SHA-256 대조를 통과했다. 브라우저의 파일 선택·표시는 미확인이다. |
| C5 | 테스트용 상대가 내 글에 아무 말이나 답한다 | `chat_echo_bot`에서 실제 `echo` 봇의 답장을 받았다. |
| C6 | 수천 명(5,000명, 1인당 초당 1건)이 동시에 주고받는다 | 60초 동안 송신·ACK·수신 각각 300,000건, 초당 5,000.0건을 확인했다. |
| C7 | 순간에 아무리 많이 보내도 죽지 않는다. 메모리 상한이 있고 글이 사라지지 않는다 | `burst`, `flood`, `slow`, `churn` 통과. `churn`은 전송 중 강제 종료하므로 수신 순서와 사용자 수 복귀를 확인한다. |
| C8 | 클라이언트, 내 서버, 백엔드로 요청이 이동하는 모습을 실제 값으로 보여 주는 페이지 | 상태 API와 추적 값을 확인했다. Node에서 실제 흐름 JS로 늦게 연 화면, 상대 변경, iframe 간 추적 분리, 링 순환 뒤 새 추적의 시간 표시, 이벤트 유실 시 `-` 표시를 확인했다. 실제 브라우저의 애니메이션과 휴대폰 폭은 미확인이다. |
| C9 | 로컬에서 사용할 수 있으면 완료다 | WSL에서 `tools/run_chat.sh`로 4명 채팅을 확인했다. 두 HTML·JS·CSS의 HTTP 응답과 MIME, 두 상태 API의 200 JSON 응답, 종료 후 포트 정리를 확인했다. |

로컬 공용방 데모는 텍스트만 지원하는 공용방 하나와 고정된 봇 10명 비교를 제공한다. 한 페이지에서 수동 채팅과 시험을 동시에 사용하며, 시험 결과에 실제 백엔드 배정과 양방향 수신 증거를 표시한다. 시험과 수동 채팅은 독립된 로컬 연결을 사용한다.

범위 밖: 로그인, 대화 기록과 이미지 저장, 공용방 데모 외 3인 이상의 방, 공개 배포, TLS
