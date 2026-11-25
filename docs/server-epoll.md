# 웹서버 LT epoll + EPOLLONESHOT

아래는 전환 당시 기록이다. 후속 전체 순회 축소는 [완료 큐·배정 FIFO](server-queues.md)에 있다.

## 범위와 동작

웹서버의 poll 감시를 LT epoll로 교체했다. 메인이 감시하고 워커4개가 HTTP I/O를
처리하는 구조, 작업 큐4개, 버퍼8개, 연결 상한 환경변수, FIFO 버퍼 대기,
keep-alive·수신·송신·CGI 시간 정책은 유지한다. 프록시는 변경하지 않았다.

- 리스너·eventfd는 지속적인 LT 감시이며 한 번에 최대64개 이벤트를 받는다.
- 연결은 LT + EPOLLONESHOT이다. 이벤트 전달 뒤 자동으로 통지가 중단된다.
  처리 완료 후 메인이 EPOLL_CTL_MOD로 재활성화한다. 데이터가 남아 있으면 다시 통지된다.
- 이미 버퍼에 다음 요청이 있으면 커널 이벤트를 기다리지 않고 기존 작업 큐로 배정한다.
  타이머·버퍼 반환에 의해 직접 배정하는 경우 활성 감시를 제거한다.
- 작업 큐가 가득 차면 기존 pending 재시도 정책을 유지한다. 버퍼가 없으면
  읽기 통지 없이 오류·종료만 감시하고 최초 고갈의 절대2초 기한을 유지한다.
- CGI는 클라이언트 소켓에서 stdout 파이프로 감시 대상을 전환한다. 워커가
  파이프를 닫거나 FD를 재사용하기 전에 메인이 등록을 제거한다. EOF 후 자식이
  살아 있으면 FD 감시 없이 기존 타이머로 종료를 재확인한다.
- 이벤트에는 슬롯과 등록 식별값을 담는다. 종료·슬롯 재사용·재활성화 이전 이벤트는
  무시한다. 32비트 식별값은 순환하지 않으며 소진 후 새 등록은 실패 처리한다.
- 개별 등록 오류는 해당 연결을 회수한다. epoll 생성·기본 등록·대기 오류와
  작업 배정 전 감시 제거 오류는 서버를 종료하고 실패 코드로 반환한다.
  EINTR은 종료 요청이 없으면 재시도한다. poll 자동 대체는 없다.

## 검증

- test_server_epoll: 실제 socketpair/pipe로 ONESHOT 자동 중단·MOD 재활성화,
  작업 중/오래된 이벤트 무시, 슬롯 재사용, CGI 전환·EOF, 등록 오류 검사.
- test_scheduler, test_context, test_server_profile.py 통과.
  test_context의 삭제된 poll_client 호출을 실제 타이머 만료 경로로 변경했고,
  과거 버퍼 고갈 시 종료 기대값을 현재 대기 정책으로 수정했다.
- 기존 TCP 통합11개와 CGI8개 검사 통과. keep-alive, 파이프라인, 큰 응답,
  백프레셔, 오류·중단·시간 제한, CGI 자식 회수 등을 포함한다.

```sh
make all test_scheduler test_context test_server_epoll
./test_scheduler && ./test_context && ./test_server_epoll
python3 tests/test_server_profile.py
python3 proxy/tests/test_proxy.py
python3 src/tests/test_cgi.py
python3 tests/check_capacity.py --connections 1000 --empty-body --rounds 1 --server-profile --proxy-profile --output docs/server-epoll-1000.json
python3 tests/check_capacity.py --connections 10000 --empty-body --rounds 1 --server-profile --proxy-profile --output docs/server-epoll-10000.json
```

## 단회 비교

기존 server-profile-10000.json과 같은 빈 POST·동시 요청4개·1회 순회·버퍼8개,
연결 상한11000, 양쪽 계측 활성 조건이다. 실행기는 변경하지 않았다.

| 10000건 | 전환 전 | 전환 후 |
| --- | --- | --- |
| 요청 구간 wall | 14.147초 | 8.201초 |
| 요청 구간 웹서버 프로세스 CPU | 16.30초 | 7.50초 |
| 메인 poll / epoll_wait 누적 CPU | 10.023초 | 0.118초 |
| 메인 prepare_scan 누적 CPU | 1.677초 | 2.601초 |
| 메인 dispatch_scan / ready_dispatch 누적 CPU | 1.215초 | 0.574초 |
| 검사 시 앞단 / 백엔드 연결 | 3227 / 2604 | 5911 / 4734 |

1000개는 요청 wall0.758초, 웹서버 CPU0.25초로 연결 유지·재사용·회수 검사를
통과했다. 10000건의 첫 요청은 모두 성공했지만 동시 연결 유지 검사는 실패했다.
표의 연결 수는 첫 검사 시점이며 실패 처리 중 재측정한 observed_established와는
시점이 다르다. 전환 후 실패 처리 시점은5735 / 4575개다.

감시 구간의 CPU 비용은 줄었지만 전체 슬롯 준비 순회는178,761,000회로 남았다.
루프 횟수 변화로 준비 순회·타이머 비용이 늘 수도 있다. 다음 개선 후보는 완료·
작업 배정 대기 큐로 전체 순회를 줄이고, 타이머 갱신 자료구조를 개선하는 것이다.
검증 실행기는 한 순회 후 유지 여부를 검사하므로 순회 중 기존 연결에 유지 요청을
보내지 않는다. 이번8.2초 순회는 백엔드4초·앞단5초 유휴 정책보다 길다.

내부 구간 CPU는 시작·종료를 포함한 누적이고 요청 구간 프로세스 CPU와 범위가
다르다. 중첩 구간을 합산하지 않는다. 공유 WSL 단회 결과이므로 일반적인 개선율이나
C10K 달성을 보장하지 않는다. 원본은 server-epoll-1000.json, server-epoll-10000.json이다.
