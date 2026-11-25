# 세 브랜치 공통 연결 유지 실험

원본 `Krafton_Jungle_Week13_Team2`는 수정하지 않는다. `sources/`는 고정된 원격 추적 커밋의 git archive 복사본이며, fetch로 원격 최신 상태를 조회하지 않았다. 커밋은 `results/commits.json`에 저장된다.

## 질문

정상적인 HTTP keep-alive 연결이 증가할 때 어떤 자원을 유지하며, 별도로 들어오는 정상 요청을 계속 처리할 수 있는가?

## 실행

WSL Ubuntu에서 이 폴더로 이동한다. Python 3.12+, GCC, make를 사용한다.

```sh
python3 test_benchmark.py
# 새 폴더에서 최초 준비 및 실행
python3 benchmark.py --prepare
# 이미 준비했다면 --prepare 없이 실행. 기존 결과 덮어쓰기 금지.
python3 benchmark.py --output rerun.jsonl
# 1000연결 측정 후 자원 여유를 확인한 경우 nuri 고연결 추가 실행
python3 benchmark.py --branches nuri --levels 3000,10000 --nuri-max 10000 --process-cap 12000 --output nuri-high.jsonl
# 중단된 실행은 이미 기록한 회차를 건너뛰어 재개
python3 benchmark.py --branches nuri --levels 3000,10000 --nuri-max 10000 --process-cap 12000 --output nuri-high.jsonl --resume
python3 summarize.py
python3 verify_results.py
```

`--levels`, `--repeats`, `--duration`, `--rate`, `--output`으로 조건을 지정한다. 예: `--levels 0,10 --repeats 1 --duration 2 --output smoke.jsonl`.

## 공통 조건과 실험용 변경

- 1024바이트 동일 파일 `public/payload.txt`, loopback, 같은 WSL, 서버 하나씩 실행.
- 모든 서버 `make CFLAGS='-std=gnu11 -Wall -Wextra -O2'` 빌드. pthread 및 각 Makefile의 CPPFLAGS 등은 유지.
- 연결 상한 12000: nuri CLI 허용 최대값만 4096→12000, 실행 인자 12000. ljw MAX_CONNECTIONS 16→12000. shw 원래 연결 상한을 별도로 두지 않음.
- 유휴 시간 600초: nuri CLI, ljw KEEP_ALIVE_TIMEOUT_MS, shw IDLE_TIMEOUT_SEC. 요청 수신 기한은 변경하지 않음.
- ljw 작업자 4개, 큐 4개, context 8개 등의 구조적 설정은 유지. 서로 다른 구현 전체를 비교하며 모델만 완벽히 분리한 실험이 아님.
- 패치 전문: `results/*-experiment.patch`, 컴파일 로그 `results/*-build.log`.
- RLIMIT_NOFILE 최대 65536 적용. 서버별 RLIMIT_NPROC 1100을 안전 제한으로 적용. nuri는 목표 1000개까지만 실행하며 3000/10000은 **미실행**. 같은 사용자 다른 프로세스도 NPROC 한도에 영향을 줄 수 있음.
- 가용 메모리 1GiB 미만 시 신규 대기 연결 생성을 중단. 대기 연결 수가 목표와 다르면 `target_not_maintained`이며 성공한 동일 조건으로 해석하지 않음.

고연결 추가 실행은 `--nuri-max 10000 --process-cap 12000`으로 프로세스 안전 한도를 명시적으로 완화한다. 기본 실험과 동시에 실행하지 않는다. 추가 실행 원자료는 별도 파일이며 요약기는 동일 브랜치·목표·회차의 미실행 표시를 실제 추가 측정으로 대체해 `combined.csv`를 생성한다. 따라서 초기 안전 제한과 실제 관찰한 실패를 혼동하지 않는다.

실제 환경에서는 systemd `init.scope`의 **TasksMax=9392**를 확인했다 (`results/system-limits.txt`). 이 제한은 변경하지 않았다. 해당 scope에는 측정기와 WSL의 다른 작업도 포함되어 서버가 사용할 수 있는 수는 이보다 작다. 첫 고연결 실행에서 nuri는 약 9382개 연결에서 fork EAGAIN을 기록했고, 측정기 정리용 `asyncio.to_thread`도 새 스레드를 만들지 못했다. 스레드 없는 비동기 종료 대기로 수정하고 회귀 테스트를 추가한 뒤 미기록 회차만 재개했다. 이미 완료한 회차는 덮어쓰지 않았다. 따라서 nuri 10000 목표는 **동일 환경의 실제 작업 수 한도에 걸린 결과**이며, 프로세스 모델이 모든 환경에서 10000 연결을 처리할 수 없다는 뜻이 아니다.

## 절차

각 조건마다 서버를 재시작한다. 0/100/500/1000/3000/10000개 연결을 25개씩 생성하고, 각 연결에서 정상 GET의 상태·본문을 확인한 뒤 유휴 상태로 보관한다. 준비 과정은 측정 구간 밖이다. 클라이언트 연결·응답 제한 시간은 각각 2초다.

목표 연결을 만든 뒤 초당 10회씩 8초 동안 별도 새 연결로 GET을 전송한다. 각 요청은 정상 응답 후 닫는다. 응답 시간은 연결 생성부터 1KiB 본문 수신 완료까지다. 총 80개 요청이며 조건별 3회 반복한다. 반복별 서버 순서를 순환한다.

## 측정 정의

- `held_before/after`: 정상 응답을 받은 대기 연결 중 측정 전후 클라이언트가 EOF를 관찰하지 않은 연결 수. 전체 연결의 생존을 구간 내내 연속 검증한 값은 아님.
- `pss_kib`: 서버 프로세스 그룹 전체의 `/proc/PID/smaps_rollup` PSS 합계. 공유 페이지 중복을 줄인 사용자 공간 메모리 지표. **소켓 버퍼·커널 프로세스 구조·페이지 테이블 등 전체 시스템 비용은 포함하지 않음.**
- `processes/threads`: 측정 직전 서버 그룹의 프로세스/스레드 합계. 일시적인 정상 요청 담당 자식의 최대 수는 아님.
- `server_cpu_percent`: 구간 전후 서버 그룹의 utime/stime 및 회수된 자식 CPU 차이. 100%=논리 CPU 한 개. /proc 순회는 원자적이지 않으므로 작은 오차가 있고, 저부하에서는 커널 tick 해상도 영향이 큼.
- `client_cpu_percent`, `client_peak_rss_kib`: 측정기 비용. peak RSS는 해당 측정기 프로세스 시작 이후 누적 최대값이며 개별 단계 현재값이 아님.
- p95는 성공 요청만의 nearest-rank 백분위. 실패는 별도 표시하며 지연값에서 제외. JSONL에 실패 종류 기록.

## 해석 제한

서버와 클라이언트가 같은 WSL CPU·메모리를 공유한다. 서버 코드와 로그는 Windows 마운트(`/mnt/c`)에 있으며 파일 I/O 및 로그 출력 비용도 지연에 포함된다. localhost 네트워크이므로 실제 원격 사용자 환경을 재현하지 않는다. 정상 요청 10회/초는 포화 처리량 시험이 아니다.

메모리의 기본 고정 할당이 다르므로 절대값과 0연결 대비 증가량을 함께 봐야 한다. 단일 이벤트 루프의 CPU 작업 지연이나 스레드 동기화의 개발 복잡성은 이 실험으로 직접 증명하지 않는다. 10000연결 조건은 성공한 해당 서버·커밋·설정에 대해서만 해석한다. nuri 미실행 조건은 실패나 모델 한계가 아니다.

## 산출물

`benchmark.py`: 공통 준비·빌드·측정기. `test_benchmark.py`: 응답 경계/불완전 응답/백분위 검증. `results/measurements.jsonl` 및 CSV: 본 실험 원자료. 환경 JSON: 커널·CPU·메모리·파일 한도·실행 인자. 각 로그: 서버 표준출력/표준오류.

## 완료 검증

총 54회(세 구현 × 여섯 연결 단계 × 세 반복)의 실제 측정을 `results/combined.csv`에 통합했다. 정상 요청 4320회 중 4080회 성공, 240회 실패이며 실패는 모두 nuri의 10000 목표 조건에서 발생했다. 측정기 회귀 테스트 4개 통과. `verify_results.py`로 회차 누락·중복, 요청 집계, SVG 형식 및 실험 서버 잔류 여부를 확인했다. 원본 저장소 `git diff --exit-code`도 통과했다.
