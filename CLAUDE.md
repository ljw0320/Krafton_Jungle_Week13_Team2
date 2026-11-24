# Krafton_Jungle_Week13_Team2

크래프톤 정글 13주차 웹 서버 과제다. C와 POSIX 소켓 API로 HTTP 웹 서버를 만든다. 이번 범위는 TCP 소켓 서버와 `epoll` 기반 동시 요청 처리, HTTP 파싱과 정적 파일 서빙, Reverse Proxy다.

## 문서
- 개요: `docs/overview.md`
- 요구사항: `docs/requirements.md`. 기능을 구현하기 전에 관련 절을 읽는다.
- 설계: `docs/architecture-tcp.md`. 구현 전에 해당 단계의 관련 절을 읽는다.
- 설계(HTTP): `docs/architecture-http.md`
- 설계(Proxy): `docs/architecture-proxy.md`. 다른 설계 문서와 다르면 프록시 단계에서는 이 문서를 따른다.
- 계획: `docs/superpowers/plans/`
- 배운 점: `docs/lessons.md`. 작업 전에 읽는다.

## 기본 원칙
- 정보 없이 코딩하지 않는다. 부족한 정보는 사용자에게 질문한다.
- 무조건 동의하지 않는다. 구현 전에 문제점과 대안을 검토해 알린다.
- 기본 테스트만으로 완료하지 않는다. 경계값, 잘못된 입력, 오류 경로까지 확인한다.
- 같은 실수를 반복하지 않는다.
  - 배운 점은 `docs/lessons.md`에 저장한다. 파일이 없으면 만든다.
  - 쌓이면 중복을 합쳐 줄인다.

## 명령어
모든 명령은 WSL(Ubuntu 24.04)의 레포 루트에서 실행한다. Windows 쪽 Claude는 `wsl.exe -e bash -lc 'cd /mnt/d/kraftonjungle/kraftonjungle_week13/Krafton_Jungle_Week13_Team2 && <명령>'`로 감싼다.
- 빌드: `make` (경고 0개여야 한다), `make asan` (`-fsanitize=address,undefined`), `make clean`
- 테스트: `make test` (파서 단위 테스트 `tests/test_http.c`와 기능 테스트 `tests/http_test.sh`를 둘 다 실행한다). ASan: `make clean && make test CFLAGS='-std=gnu11 -Wall -Wextra -g -fsanitize=address,undefined'`
- 실행: `./server <port> <root>` (예: `./server 8080 /tmp/www`). 프록시 2단계부터 뒤에 `[<prefix>=<ip>:<port> ...]` (예: `./server 8080 /tmp/www /api/=127.0.0.1:8001`)
- 그 밖의 검사: `make asan` 빌드로 같은 시나리오를 돌려 오류 0개, `valgrind --leak-check=full ./server 8080 /tmp/www`에서 `definitely lost: 0`

## 제약
- C 언어, 표준 C 라이브러리와 POSIX API만 쓴다. `libevent`, `libuv`, `nginx`, `Apache`, `mongoose` 같은 HTTP / 이벤트 루프 라이브러리는 금지다.
- 빌드는 `Makefile`, 컴파일러는 `gcc -std=gnu11 -Wall -Wextra -g`다 (`accept4` 때문에 `gnu11`).
- Linux 전용(WSL2)이다. `#ifdef` 이식성 분기를 두지 않는다.
- Forward Proxy, CGI, 캐싱, 죽은 백엔드 건너뛰기, 시그널 셧다운은 이번 범위가 아니다.

## 설계 결정
- `epoll` LT, 단일 스레드 이벤트 루프다. `epoll` fd는 `event_loop.c`의 `static` 변수이고 다른 파일은 `loop_watch` / `loop_unwatch`로만 다룬다.
- 연결 핸들러는 살아 있으면 0, `conn_close`로 닫았으면 -1을 반환한다. -1을 받은 쪽은 `c`를 다시 건드리지 않는다.
- `conn_close` 순서: `loop_unwatch` → `close` → `free(wbuf)` → `close(file_fd)`(-1이면 생략) → 리스트 제거 → `free(c)`. 프록시 단계부터 맨 앞에 `proxy_close`, `close` 다음에 `loop_accept_resume`이 들어간다.
- WRITING 동안 `EPOLLIN`을 감시하지 않는다. 응답을 다 보낸 뒤 rbuf에 남은 바이트가 있으면 바로 다시 처리한다.
- HTTP는 `http_init`, `http_parse`, `http_handle` 세 함수로만 붙는다. 프록시 단계에서 `http_keep_alive`, `http_proxy_request`, `http_proxy_response`를 더한다.
- 프록시: 백엔드 연결은 `struct conn` 안의 `struct upstream`이고 A → B 방향으로만 닫는다. `loop_unwatch`가 지금 배치에 남은 그 대상의 이벤트를 지운다. 백엔드를 기다리는 동안 A는 `EPOLLERR`만 감시한다.
- `SIGPIPE`는 무시하고 `EPIPE`로 처리한다.

## 작업 방식
- 구현 전에 계획을 확인하고, 작은 단위로 구현한다.
- 단위마다 위 명령을 실행하고, 종료 코드가 0인 것을 확인한 뒤 완료로 보고한다.
- 요구사항에 없는 기능은 구현하지 않고 제안만 한다.
- 설계와 다르게 구현해야 하면 멈추고 보고한다.
- 자동화 범위: 단위가 검증을 통과하면 `shw` 브랜치에 커밋과 push까지 확인 없이 한다. PR과 이슈는 사용자에게 묻는다.
- 요구사항 변경, 설계 변경, 새 의존성 추가는 사용자에게 묻는다.
- 그 밖에 메인 세션이 정한 판단은 이유와 틀렸을 때의 비용을 적어 마지막에 보고한다.
- 에이전트가 확인하지 않았다고 보고한 항목은 메인 세션이 직접 확인한다.

## 작업 분담
- 설계는 메인 세션에서 사용자와 확정하고 설계 문서에 기록한다.
- 구현은 계획에 표시된 담당에게 맡기고, 메인 세션은 코드를 직접 쓰지 않는다.
- 구현 단위가 끝나면 `verifier`에게 검증을 맡긴다. 구현 과정을 설명하지 않고 검증할 범위만 알려 준다.
- 메인 세션은 지적마다 수정, 문서화, 범위 밖 처리 중 하나로 판단한다.
- 커밋은 검증을 통과한 뒤 메인 세션이 한다.
- 단계가 끝나면 `final-reviewer`에게 그 단계의 전체 변경을 맡긴다.

## 문서화
- 구조나 설계가 바뀌면 설계 문서에 변경 내용과 이유를 적는다.

## 토큰과 속도
- 학습, 설계, 구현처럼 작업의 성격이 바뀌거나 구현 단계가 바뀌면 새 세션에서 시작한다. 이어 갈 내용은 문서와 커밋에 남긴다.
- 에이전트의 기본은 sonnet, 중간 강도이다. 틀렸을 때 비용이 큰 단위만 호출할 때 `model: opus`, `effort: high`를 지정한다.
- 단계가 끝나면 `final-reviewer`로 전체 리뷰를 한 번 돌린다. 이 리뷰는 낮추지 않는다.
- 고의 결함 검사는 `verifier`만 한다. 무작위 검사는 1,000건을 넘기지 않는다.
- 에이전트 보고는 문제와 결론만 20줄 이내로 받는다. 긴 근거는 꼭 필요할 때만 저장소 밖 단위별 파일에 이어 쓴다.
- Critical과 Important는 그 단위 안에서 고친다. Minor는 모아서 단계 끝에 한 번에 고친다.
- 큰 문서는 전체를 읽지 않는다. 필요한 절이나 값만 찾아 담당에게 넘긴다.
- 파일이 겹치지 않는 단위는 병렬로 돌린다.
- 검증은 테스트 개수가 아니라 명령의 종료 코드로 확인한다. 파이프로 종료 코드를 가리지 않고, `git add` 같은 명령의 오류를 숨기지 않는다.
