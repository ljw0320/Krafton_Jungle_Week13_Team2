# 개요

크래프톤 정글 13주차 2팀 과제다. C 언어와 POSIX 소켓 API만으로 HTTP 웹 서버를 처음부터 만들고, Proxy 기능을 붙인다. 전체 요구사항은 [requirements.md](requirements.md)에 있다.

# 이번 범위

| 항목 | 내용 |
|---|---|
| 구현 | ① TCP 소켓 서버, ② 동시 요청 처리 (`epoll` LT, 단일 스레드 이벤트 루프), HTTP 요청 파싱, 정적 파일 서빙, 오류 응답, Reverse Proxy(동적 처리 포함) |
| 담당 | 한 명이 혼자 맡는다 (`shw` 브랜치) |
| HTTP | `http_init`, `http_parse`, `http_handle` 세 함수로 붙는다. 프록시는 여기에 요청과 응답을 다시 만드는 함수를 더한다 |
| 설계 | [architecture-tcp.md](architecture-tcp.md), [architecture-http.md](architecture-http.md), [architecture-proxy.md](architecture-proxy.md) |
| 개발 환경 | WSL2 Ubuntu 24.04, gcc 13.3. Linux 전용이다 |

# 이번 범위에서 빠지는 것

- Forward Proxy, CGI, 캐싱, 죽은 백엔드 건너뛰기
- 시그널 셧다운 (`SIGINT` / `SIGTERM`)
- Dockerfile, 부하 테스트 도구(`ab` / `wrk`), 설정 파일

# 단계

| 단계 | 상태 | 설계 문서 |
|---|---|---|
| TCP 서버, 동시 요청 처리 | 완료 | [architecture-tcp.md](architecture-tcp.md) |
| HTTP 파싱, 정적 파일, 오류 응답 | 완료 | [architecture-http.md](architecture-http.md) |
| Reverse Proxy | 설계 작성 (2026-11-24), 구현 전 | [architecture-proxy.md](architecture-proxy.md) |
| 시그널 셧다운 | 미정 | (미정) |
