# 배운 점

## 에이전트 실행
- Git Bash에서 `wsl.exe -e bash -lc '...'`는 경로 변환 때문에 실패한다. `MSYS_NO_PATHCONV=1`을 앞에 붙인다.
- 파일은 Write/Edit 도구로 쓴다. Bash heredoc으로 쓰다 따옴표가 어긋나면 명령이 입력을 기다리며 백그라운드에 걸리고, 에이전트가 그 작업을 기다리느라 15분 넘게 멈췄다(Task 2).
- 서버 확인은 run_in_background를 쓰지 않고, 한 번의 `bash -lc` 안에서 띄우기 → 확인 → `kill`로 끝낸다. `pause()`나 이벤트 루프는 스스로 끝나지 않는다.
