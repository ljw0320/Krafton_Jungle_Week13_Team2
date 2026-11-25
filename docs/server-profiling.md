# 웹서버 내부 구간 계측

이번 변경은 계측 추가이며 서버의 poll·정렬 타이머·큐·자원·시간 정책은 유지한다.
`JUNGLE_PROFILE=1`일 때만 시계를 읽고, 종료 시 `[server_profile]` JSON 한 줄을
stderr로 출력한다. 메인 스레드와 작업자 전체를 구분한다. 작업자는 TLS에
합산하고 종료 때만 병합 잠금을 사용하므로 요청마다 계측용 공용 잠금을 잡지 않는다.
프로파일 함수는 errno를 보존한다. 기본값은 계측 비활성이다.

```bash
JUNGLE_PROFILE=1 ./webserver 8081 www cgi-bin
python3 tests/check_capacity.py --connections 1000 --empty-body --rounds 1 --server-profile --proxy-profile --output docs/server-profile-1000.json
python3 tests/check_capacity.py --connections 10000 --empty-body --rounds 1 --server-profile --proxy-profile --output docs/server-profile-10000.json
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
