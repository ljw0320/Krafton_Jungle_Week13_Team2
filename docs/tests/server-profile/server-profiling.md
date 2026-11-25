# 웹서버 내부 구간 계측

아래는 웹서버 poll 전환 전 계측 기록이다. 현재 epoll 구현과 후속 비교는
[웹서버 epoll 전환](../server-epoll/server-epoll.md)에 있다. 현재 출력 키는 poll→epoll_wait,
dispatch_scan→ready_dispatch이며 후자는 반환된 이벤트 수를 units로 기록한다.

이번 변경은 계측 추가이며 서버의 poll·정렬 타이머·큐·자원·시간 정책은 유지한다.
`JUNGLE_PROFILE=1`일 때만 시계를 읽고, 종료 시 `[server_profile]` JSON 한 줄을
stderr로 출력한다. 메인 스레드와 작업자 전체를 구분한다. 작업자는 TLS에
합산하고 종료 때만 병합 잠금을 사용하므로 요청마다 계측용 공용 잠금을 잡지 않는다.
프로파일 함수는 errno를 보존한다. 기본값은 계측 비활성이다.

```bash
JUNGLE_PROFILE=1 ./webserver 8081 www cgi-bin
python3 tests/check_capacity.py --connections 1000 --empty-body --rounds 1 --server-profile --proxy-profile --output artifacts/server-profile/server-profile-1000.json
python3 tests/check_capacity.py --connections 10000 --empty-body --rounds 1 --server-profile --proxy-profile --output artifacts/server-profile/server-profile-10000.json
```

## 계측 범위

- 메인: 감시 배열 구성·전체 순회, poll, 이벤트 배정 순회, accept, 완료 알림 읽기.
- 타이머: 취소·삽입의 연결 목록 이동 횟수, 갱신·만료 처리.
- 큐: 넣기·꺼내기, 등록부터 작업자 선택까지의 체류 wall 시간.
- 동기화: server mutex 획득 wall/CPU, condition wait의 대기·재획득 wall/CPU.
- 작업자: process_turn(파싱·소켓 I/O·context 풀 동기화 등을 포함), 결과 반영,
  타이머 재등록, 메인 깨우기.

wall은 실제 경과 시간이고 CPU는 CLOCK_THREAD_CPUTIME_ID이다. 작업자 값은 4개
스레드 합계이므로 대기 wall 합계가 시험 경과 시간을 넘을 수 있다. condition wait는
순수 큐 대기와 mutex 재획득을 함께 포함한다. server mutex 계측은 명시적 lock
호출이며 context 풀 mutex나 라이브러리 내부 잠금은 별도 계측하지 않았다.

구간은 일부 중첩된다. prepare/dispatch/accept 안의 enqueue·타이머, publish 안의
update, update 안의 cancel을 모두 합산하면 중복이다. queue 체류는 스레드 간
전달 시간이라 CPU를 기록하지 않는다. units는 구간에 따라 슬롯 수·감시 FD 수·
목록 이동 횟수·처리 작업 수를 뜻하므로 다른 구간끼리 합산하지 않는다.
계측은 시작·종료 정리까지의 누적이고, 실행기의 요청 구간 프로세스 CPU와 정확히
같은 구간은 아니다. 계측 함수 호출과 탐색 횟수 집계의 오버헤드도 포함된다.

## 단회 결과

양쪽 연결 상한 1100/11000, 버퍼 각각8, 부하 동시성4, 빈 POST, 1회 순회,
프록시 LT epoll·웹서버 poll, 양쪽 계측 활성이다. 프로파일은 정상 종료 시
작업자 join 후 수집한다. 원본 JSON은 같은 폴더의 server-profile-1000.json,
server-profile-10000.json이다.

| 웹서버 누적 CPU 구간 | 1000개 | 10000개 |
| --- | --- | --- |
| 메인 poll | 0.186초 | 10.023초 |
| 메인 prepare_scan | 0.060초 | 1.677초 |
| 메인 dispatch_scan | 0.040초 | 1.215초 |
| 작업자 request_turn 합계 | 0.071초 | 0.855초 |
| 작업자 publish_result 합계 | 0.013초 | 0.849초 |
| 작업자 timer_update 합계(publish에 포함) | 0.012초 | 0.836초 |

10000개에서 메인 준비는 연결 슬롯 104,148,000개, 배정 순회는 FD 항목
21,260,528개를 확인했다. 타이머 취소 이동은 메인·작업자 합계113,153,963회,
삽입 이동은46,747,391회였다. 이동 수는 순회 비용의 근거이지만 그 자체가
전체 지연의 비율은 아니다.

작업자 server mutex 획득 wall 합계는2.155초, condition wait wall 합계는54.099초,
CPU 합계는 각각0.108초·0.582초였다. 이 대기 wall을 CPU 병목으로 해석하지 않는다.
큐 체류 wall 합계는17,732개 작업에6.772초(평균 약0.382ms)였다.

1000개 연결 유지 검사는 통과했다. 10000건의 첫 요청은 성공했고 요청 구간
wall14.147초·웹서버 프로세스 CPU16.30초였다. 연결 검사 시 클라이언트3227개·
백엔드2604개로 C10K 유지는 실패했다. 이전 시험보다 시간이 달라도 정책 개선의
효과로 해석하지 않는다. 이번 변경은 계측뿐이고 공유 WSL 단회 측정이다.

## 판단과 다음 우선순위

빈 POST 부하에서는 실제 요청 내용보다 메인 poll과 연결 관리 경로가 더 큰 CPU
비용이다. 우선 웹서버 poll, 이후 전체 연결 순회·타이머 구조를 개선하는 순서가
측정에 부합한다. 큐·잠금은 이번 결과에서 가장 큰 CPU 항목이 아니다.
다른 요청 본문·정적 파일·CGI 부하에서도 같은 비율이라는 보장은 없다.
부하 실행기의 한 바퀴 갱신 지연도 남아 있으므로 서버 CPU 개선만으로 C10K
통과를 보장하지 않는다.

## 검증

```bash
python3 tests/test_server_profile.py
make test_scheduler
./test_scheduler
JUNGLE_PROFILE=1 python3 proxy/tests/test_proxy.py
```

계측 비활성 시 출력 없음, 활성 JSON·작업자 집계의 일관성, 동일 HTTP 응답을
확인했다. 스케줄러·타이머 검사와 기존 TCP 11개도 실행한다.

후속 완료 큐 적용 후 prepare_scan은 prepare_changed로 바뀌며 units도 변경·선두 확인 수이다. 상세는 [완료 큐·배정 FIFO](../server-queues/server-queues.md)에 있다.


## 후속: 작업 배정·큐 대기 지연 진단

JUNGLE_PROFILE=1에서 기존 누적 계측에 max_wall_seconds와 max_cpu_seconds를
추가했다. 최대값은 작업자별 최대값 중 최대이며, 평균 wall은 wall_seconds/calls다.
스레드별 집계 후 종료 시 병합하므로 요청별 로그나 계측용 공용 잠금을 추가하지 않는다.
기본 비활성·기존 스케줄링 정책을 유지한다.

pending_residence_wall_only는 배정 FIFO 등록부터 작업 큐 이동 직전까지,
queue_residence_wall_only는 작업 큐 등록부터 워커 인출까지 측정한다.
이벤트가 커널에서 준비된 시점부터 메인이 감지하기 전까지의 대기는 포함하지 않는다.
배정 취소·종료로 큐에 들어가지 못한 항목도 pending 완료 통계에는 포함하지 않는다.

전체 집계에는 시험 종료의 대량 EOF 처리가 섞이므로 nonterminal_pending_wall_only,
nonterminal_queue_wall_only, nonterminal_turn을 추가했다. 이들은 process_turn 반환 후
종료 사유(reason)가 없는 작업만 집계한다. HTTP 요청 단위가 아닌 워커 작업 차례
단위이며, EOF·오류로 끝난 차례는 제외한다. 종료 처리 중의 모든 비용을 완벽하게
분리한 통계는 아니다. nonterminal_turn은 전체 request_turn 구간과 중첩되므로
합산하지 않는다. 잠금·타이머 등 다른 중첩 구간도 합산하지 않는다.

### 단회 결과

공유 WSL, 빈 POST, 목표 10,000개, 동시 요청 4개, 버퍼 각각 8개,
서버·프록시 내부 프로파일과 생성기 시각 계측을 활성화했다.

| 구간 | 평균 wall | 최대 wall |
| --- | --- | --- |
| 계속 서비스한 작업의 배정 FIFO 대기 | 0.003039ms | 2.324ms |
| 계속 서비스한 작업의 작업 큐 대기 | 0.086731ms | 5.096ms |
| 계속 서비스한 작업의 워커 처리 | 0.070526ms | 5.577ms |
| 워커 server mutex 획득 (종료 포함) | 0.010858ms | 4.248ms |
| 생성기 유지 요청 예정→송신 | 105.374ms | 550.254ms |
| 생성기 유지 요청 송신→응답 | 0.553351ms | 74.051ms |

10,000개 연결 유지 시험은 통과했고 실행 시간은 32.743672초다.
반면 종료·오류를 포함한 전체 pending 대기는 평균 5.343ms·최대 311.000ms였다.
같은 실행에서 nonterminal pending 평균은 0.003039ms이므로 전체 pending 값을
그대로 서비스 요청 배정 병목으로 해석할 수 없다.

이번 통과 실행에서는 워커 작업 큐에 수백 ms 수준의 대기가 확인되지 않았다.
생성기의 전송 전 대기가 상대적으로 크지만 그 근본 원인이 생성기 코드,
동시 요청 상한, 다른 프로세스 처리, OS 스케줄링 중 무엇인지는 확정하지 못한다.
서버 계측은 이벤트 감지 이후의 차례 단위이며 프록시 처리 시간과 커널 준비 대기는
별도다. 최대값끼리는 같은 요청·시점이 아니므로 합산하거나 인과관계로 연결하지 않는다.
과거 실패 실행은 이 계측이 없었으므로 이번 결과로 실패 원인을 소급 확정하지 않는다.
계측 오버헤드가 있어 과거 프로파일 비활성 실행과 성능 개선율로 비교하지 않는다.

### 검증·재현

스케줄러·epoll 검사와 프로파일 비활성 무출력, 활성 JSON·작업자 집계,
nonterminal 대기·처리 건수 일치 및 HTTP 응답 동일성 검사가 통과했다.

```sh
make all test_scheduler test_server_epoll
./test_scheduler
./test_server_epoll
python3 tests/test_server_profile.py
python3 tests/check_c10k.py --connections 10000 --profile --output artifacts/c10k-server-stages-10000.json
```

원본 JSON에는 바이너리 SHA256과 서버·프록시 내부 계측이 함께 들어 있다.
이전 계측 형식의 baseline 및 종료 포함 계측 진단은 각각
artifacts/c10k-server-stages-baseline-10000.json,
artifacts/c10k-server-stages-inclusive-10000.json으로 구분했다.
