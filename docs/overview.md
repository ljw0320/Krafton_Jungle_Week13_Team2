# 개요

크래프톤 정글 13주차 2팀 과제다. C 언어와 POSIX 소켓 API만으로 HTTP 웹 서버를 처음부터 만들고, Proxy 기능을 붙인다. 전체 요구사항은 [requirements.md](requirements.md)에 있다.

# 이번 범위

| 항목 | 내용 |
|---|---|
| 구현 | ① TCP 소켓 서버, ② 동시 요청 처리 (`epoll` LT, 단일 스레드 이벤트 루프) |
| 담당 | 한 명이 혼자 맡는다 (`shw` 브랜치) |
| HTTP | `http.c`는 고정 응답을 돌려주는 stub이다. 인터페이스 두 함수는 이번 범위 안에서 정한 경계이고, HTTP 단계에서 바꿀 수 있다 |
| 설계 | [architecture-tcp.md](architecture-tcp.md) |
| 개발 환경 | WSL2 Ubuntu 24.04, gcc 13.3. Linux 전용이다 |

# 이번 범위에서 빠지는 것

- HTTP 요청 파싱, 정적 파일 서빙, 오류 응답(400/404 등)
- Proxy / Reverse Proxy
- 시그널 셧다운 (`SIGINT` / `SIGTERM`)
- Dockerfile, 부하 테스트 도구(`ab` / `wrk`), 설정 파일

# 이후 단계

| 단계 | 담당 | 설계 문서 |
|---|---|---|
| HTTP 파싱, 정적 파일, 오류 응답 | (미정) | (미정) |
| Proxy | (미정) | (미정) |
| 시그널 셧다운 | (미정) | (미정) |
