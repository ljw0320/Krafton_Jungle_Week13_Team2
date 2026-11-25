# 개요

크래프톤 정글 13주차 2팀 과제다. C 언어와 POSIX 소켓 API만으로 HTTP 웹 서버를 처음부터 만들고, Proxy 기능을 붙인다. 전체 요구사항은 [requirements.md](requirements.md)에 있다.

# 이번 범위

| 항목 | 내용 |
|---|---|
| 구현 | ① TCP 소켓 서버, ② 동시 요청 처리 (`epoll` LT, 단일 스레드 이벤트 루프), HTTP 요청 파싱, 정적 파일 서빙, 오류 응답, Reverse Proxy(동적 처리 포함) |
| 담당 | 한 명이 혼자 맡는다 (`shw` 브랜치) |
| HTTP | `http_init`, `http_parse`, `http_handle`과, 프록시가 쓰는 `http_keep_alive`, `http_proxy_request`, `http_proxy_response`로 붙는다 |
| 설계 | [architecture-tcp.md](architecture-tcp.md), [architecture-http.md](architecture-http.md), [architecture-proxy.md](architecture-proxy.md) |
| 개발 환경 | WSL2 Ubuntu 24.04, gcc 13.3. Linux 전용이다 |

## 채팅 서비스 (2026-11-25 시작)

| 항목 | 내용 |
|---|---|
| 구현 | 무작위 1:1 채팅과 이미지 전송. 서버에 WebSocket 터널을 더하고, 리버스 프록시 뒤에 C 백엔드 `chatd`를 둔다 |
| 확인 | 로컬에서 테스트 봇과 대화, 5,000명 부하에서 생존, 요청 흐름 페이지 |
| 브랜치 | `feat/chat`에서 진행하고 `shw`로 머지한다 |
| 구현 담당 | Codex |
| 설계 | [architecture-chat.md](architecture-chat.md) |

# 이번 범위에서 빠지는 것

- Forward Proxy, CGI, 캐싱, 죽은 백엔드 건너뛰기
- 시그널 셧다운 (`SIGINT` / `SIGTERM`)
- Dockerfile, 부하 테스트 도구(`ab` / `wrk`), 설정 파일. 채팅의 부하는 직접 만든 봇으로 확인한다

# 단계

| 단계 | 상태 | 설계 문서 |
|---|---|---|
| TCP 서버, 동시 요청 처리 | 완료 | [architecture-tcp.md](architecture-tcp.md) |
| HTTP 파싱, 정적 파일, 오류 응답 | 완료 | [architecture-http.md](architecture-http.md) |
| Reverse Proxy | 완료 (2026-11-25) | [architecture-proxy.md](architecture-proxy.md) |
| 무작위 1:1 채팅 | 2026-11-25 구현 및 WSL 자동 검증 완료. 실제 브라우저·한글 IME·휴대폰 확인 대기 | [architecture-chat.md](architecture-chat.md) |
| 시그널 셧다운 | 미정 | (미정) |

## 로컬 데모

WSL의 저장소 루트에서 `make && bash tools/run_chat.sh`를 실행한다. 브라우저 탭 두 개에서 [채팅](http://localhost:8080/chat.html)을 열고 매칭을 시작한다. 혼자 시험하려면 두 번째 탭 대신 다른 WSL 터미널에서 `python3 tests/chat_bot.py echo`를 실행한다. 실행 스크립트는 `Ctrl+C`로 종료한다.

[요청 흐름](http://localhost:8080/flow.html)의 채팅 칸에서 글을 보내면 해당 연결의 추적을 볼 수 있다. 같은 8080 포트에서 서버와 백엔드의 원본 상태를 각각 확인한다.

```bash
curl -fsS http://localhost:8080/_stat
curl -fsS http://localhost:8080/ws/stat
```

### 이전 단계의 프록시 데모

- WSL에서 서버 1개와 `tests/backend.py` 2개(8001, 8002)로 데모할 수 있다. HTML·CSS·JS를 정적 파일 루트에 두고 같은 서버의 상대 경로로 프록시를 호출한다.
- `/index.html`은 정적 파일, `/api/echo`는 백엔드에 전달된 요청, `/rr/port`는 라운드 로빈으로 선택한 백엔드 포트를 보여준다.
- 2026-11-25 WSL 임시 사본에서 정적 파일 200, 백엔드 요청 전달, 포트 `8001 → 8002 → 8001 → 8002`를 확인했다. 데모용 화면과 공개 배포 구성은 별도 작업이다.
