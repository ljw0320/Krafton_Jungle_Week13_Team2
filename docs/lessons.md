# 배운 점

## 에이전트 실행
- Git Bash에서 `wsl.exe -e bash -lc '...'`는 경로 변환 때문에 실패한다. `MSYS_NO_PATHCONV=1`을 앞에 붙인다.
- 파일은 Write/Edit 도구로 쓴다. Bash heredoc으로 쓰다 따옴표가 어긋나면 명령이 입력을 기다리며 백그라운드에 걸리고, 에이전트가 그 작업을 기다리느라 15분 넘게 멈췄다(Task 2).
- 서버 확인은 run_in_background를 쓰지 않고, 한 번의 `bash -lc` 안에서 띄우기 → 확인 → `kill`로 끝낸다. `pause()`나 이벤트 루프는 스스로 끝나지 않는다.
- 비대화형 bash에서 `&`로 띄운 프로세스는 SIGINT를 무시한다. `valgrind ... & SP=$!; kill -INT $SP; wait $SP`는 영원히 멈춘다. `kill $SP`(SIGTERM)로 끝내도 valgrind는 HEAP/LEAK 요약을 출력한다.
- `pkill -f <패턴>`은 그 패턴이 들어 있는 `bash -lc` 자신도 죽인다(종료 코드 15). `kill $SP`처럼 PID로 끝내거나 `pkill -x <이름>`을 쓴다.

## valgrind
- WSL에 `sudo` 비밀번호가 없어도 Windows에서 `wsl.exe -u root -e bash -lc '...'`로 설치할 수 있다. PowerShell에서는 `$?`가 먼저 확장되지 않게 작은따옴표로 감싼다.
