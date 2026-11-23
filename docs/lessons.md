# 배운 점

## 에이전트 실행
- Git Bash에서 `wsl.exe -e bash -lc '...'`는 경로 변환 때문에 실패한다. `MSYS_NO_PATHCONV=1`을 앞에 붙인다.
- 파일은 Write/Edit 도구로 쓴다. Bash heredoc으로 쓰다 따옴표가 어긋나면 명령이 입력을 기다리며 백그라운드에 걸리고, 에이전트가 그 작업을 기다리느라 15분 넘게 멈췄다(Task 2).
- 서버 확인은 run_in_background를 쓰지 않고, 한 번의 `bash -lc` 안에서 띄우기 → 확인 → `kill`로 끝낸다. `pause()`나 이벤트 루프는 스스로 끝나지 않는다.
- 비대화형 bash에서 `&`로 띄운 프로세스는 SIGINT를 무시한다. `valgrind ... & SP=$!; kill -INT $SP; wait $SP`는 영원히 멈춘다. `kill $SP`(SIGTERM)로 끝내도 valgrind는 HEAP/LEAK 요약을 출력한다.
- 확인용 스크립트는 `cd <사본> || exit 1`로 시작하고, 출력 문구의 `->`는 따옴표로 감싼다. `cd`가 실패하거나 `echo fds 6 -> 0`이 리다이렉션이 되어 저장소 루트에 파일이 생겼다(Task 6, 7). WSL이 종료되면 `/tmp`가 비워진다. 끝에 `git status`를 본다.
- `pkill -f <패턴>`은 그 패턴이 들어 있는 `bash -lc` 자신도 죽인다(종료 코드 15). `kill $SP`처럼 PID로 끝내거나 `pkill -x <이름>`을 쓴다.

## valgrind
- WSL에 `sudo` 비밀번호가 없어도 Windows에서 `wsl.exe -u root -e bash -lc '...'`로 설치할 수 있다. PowerShell에서는 `$?`가 먼저 확장되지 않게 작은따옴표로 감싼다.
- "0건"은 같은 방법으로 일부러 낸 오류가 잡히는지 확인한 뒤에 믿는다. `ASAN_OPTIONS`/`UBSAN_OPTIONS`의 `log_path`는 이 빌드에서 UBSan 출력을 파일로 보내지 않았다. UBSan은 서버 stderr를 저장해 `runtime error`를 찾는다.
