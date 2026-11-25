# 문서와 검증 기록

설계 문서는 이 폴더 루트에, 검증 문서와 대표 결과는 `tests/` 아래 시험 단위로 묶는다.
기존 JSON 원본의 내용은 유지하며 폴더만 이동한 구조이다.

| 구분 | 위치 | 내용 |
| --- | --- | --- |
| 아키텍처 | [ARCHITECTURE.md](ARCHITECTURE.md) | 기본 구조와 자원 수명 |
| 연결 용량 | [tests/capacity/](tests/capacity/) | 100·1000·10000개 연결 시험 |
| 초기 구간 계측 | [tests/baseline-profile/](tests/baseline-profile/) | 최초 구간별 시간·CPU 측정 |
| 프록시 관리 구조 | [tests/proxy-management/](tests/proxy-management/) | poll 유지 상태의 구조 변경 비교 |
| 프록시 epoll | [tests/proxy-epoll/](tests/proxy-epoll/) | 프록시 LT epoll 전환 |
| 웹서버 계측 | [tests/server-profile/server-profiling.md](tests/server-profile/server-profiling.md) | 웹서버 내부 구간 계측 |
| 웹서버 epoll | [tests/server-epoll/server-epoll.md](tests/server-epoll/server-epoll.md) | LT epoll + ONESHOT |
| 웹서버 큐 | [tests/server-queues/server-queues.md](tests/server-queues/server-queues.md) | 완료 큐·배정 FIFO |
| C10K 유지 실행기 | [tests/c10k-retention/](tests/c10k-retention/) | 비동기 유지 요청·재접속 없는 검증 |
| 웹서버 힙 타이머 | [tests/server-timer-heap/](tests/server-timer-heap/) | 인덱스 최소 힙 전환·반복 비교 |
| 큐 반복 비교 | [tests/server-queues/comparison/README.md](tests/server-queues/comparison/README.md) | 대표 비교 6회·원본·메타데이터 |

## 결과 보관 원칙

- 소스·재현 테스트·핵심 문서와 선별한 대표 결과는 Git으로 관리한다.
- 이후 임시·반복 실행 결과는 프로젝트 루트 `artifacts/`에 저장하며 Git에서 제외한다.
- 용량 실행기와 버전 비교 실행기의 기본 출력은 타임스탬프가 포함된 artifacts 경로이다.
- 확정한 대표 실험만 해당 docs/tests 폴더로 선별 보관한다.
- 로그·실행 파일은 기존 제외 규칙을 유지한다.
- PREVIOUS 문서는 과거 기록이며 현재 구조의 기준은 루트 README이다.
