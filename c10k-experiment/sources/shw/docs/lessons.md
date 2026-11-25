# 배운 점

## 에이전트 실행
- Git Bash에서 `wsl.exe -e bash -lc '...'`는 경로 변환 때문에 실패한다. `MSYS_NO_PATHCONV=1`을 앞에 붙인다.
- 파일은 Write/Edit 도구로 쓴다. Bash heredoc으로 쓰다 따옴표가 어긋나면 명령이 입력을 기다리며 백그라운드에 걸리고, 에이전트가 그 작업을 기다리느라 15분 넘게 멈췄다(Task 2).
- 서버 확인은 run_in_background를 쓰지 않고, 한 번의 `bash -lc` 안에서 띄우기 → 확인 → `kill`로 끝낸다. `pause()`나 이벤트 루프는 스스로 끝나지 않는다.
- 비대화형 bash에서 `&`로 띄운 프로세스는 SIGINT를 무시한다. `valgrind ... & SP=$!; kill -INT $SP; wait $SP`는 영원히 멈춘다. `kill $SP`(SIGTERM)로 끝내도 valgrind는 HEAP/LEAK 요약을 출력한다.
- 확인용 스크립트는 `cd <사본> || exit 1`로 시작하고, 출력 문구의 `->`는 따옴표로 감싼다. `cd`가 실패하거나 `echo fds 6 -> 0`이 리다이렉션이 되어 저장소 루트에 파일이 생겼다(Task 6, 7). WSL이 종료되면 `/tmp`가 비워진다. 끝에 `git status`를 본다.
- 테스트 스크립트에 실제 CR 바이트를 넣지 않는다. `.gitattributes`의 `eol=lf` 때문에 커밋하면 사라져 요청이 깨진다. `\r\n`은 `printf '%b'`로 만든다. `$(...)`는 끝의 줄바꿈을 지우므로 `\r\n\r\n`으로 끝나는 요청은 파이프로 보낸다(프록시 Task 4, 5).
- 검증 에이전트가 끝난 뒤 백그라운드 작업이 남아 있으면 8080이 비었는지 `ss -tln`으로 보고 끝낸다(프록시 Task 6).
- `pkill -f <패턴>`은 그 패턴이 들어 있는 `bash -lc` 자신도 죽인다(종료 코드 15). `kill $SP`처럼 PID로 끝내거나 `pkill -x <이름>`을 쓴다.
- WSL 벽시계가 약 32초마다 0.4~0.5초 앞으로 뛰었다. 서버가 `time()`을 쓰면 테스트도 `time.time()`으로 잰다. `time.monotonic()`으로 재서 10초 하한이 9.8초로 나와 드물게 실패했다.

## valgrind
- WSL에 `sudo` 비밀번호가 없어도 Windows에서 `wsl.exe -u root -e bash -lc '...'`로 설치할 수 있다. PowerShell에서는 `$?`가 먼저 확장되지 않게 작은따옴표로 감싼다.
- `tests/http_test.sh`는 서버 로그에서 `AddressSanitizer`/`runtime error`를 이미 검사하므로 스크립트 종료 코드로 충분하다. 다른 방법(`log_path` 등)으로 "0건"을 확인할 때는 일부러 낸 오류가 잡히는지(양성 대조) 먼저 본다.
- 테스트 코드에 검사가 없다고 적기 전에 `grep`으로 그 검사를 찾는다(Task 8에서 있는 검사를 없다고 적었다).
