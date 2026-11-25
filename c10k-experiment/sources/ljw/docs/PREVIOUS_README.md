# Krafton Jungle Week 13 — Team 2 Web Server

C17·POSIX 소켓·pthreads로 구현한 학습용 HTTP 서버다. 메인이 연결을 받고 고정 워커 풀이 연결 하나씩 처리한다. 모든 소켓은 non-blocking이고 준비 대기는 `poll()`을 사용한다.

승인한 범위는 [DESIGN_DECISIONS.md](DESIGN_DECISIONS.md), 설계와 모델 비교는 [docs/DESIGN.md](docs/DESIGN.md), 시연은 [docs/DEMO.md](docs/DEMO.md), 실제 측정 결과는 [results/REPORT.md](results/REPORT.md)를 참고한다.

## 빌드와 실행

macOS의 Command Line Tools 또는 Linux/WSL의 C 컴파일러, Make, POSIX 스레드가 필요하다. 테스트에는 Python 3.9 이상이 필요하다. Windows 네이티브 MinGW는 대상이 아니며 WSL 터미널에서 실행한다.

```sh
make
./webserver
```

다른 터미널에서 요청한다.

```sh
curl -i http://127.0.0.1:8000/
```

정상 응답은 고정 HTML이며 제목은 `Web Server`, 본문은 `Hello, World!`다. 종료는 실행 터미널의 Ctrl+C 또는 `kill -TERM <PID>`로 한다. 기본 주소는 로컬 전용이다. 외부 인터페이스에서도 받으려면 다음처럼 실행한다.

```sh
./webserver --address 0.0.0.0 --port 8080 --workers 4
./webserver --help
```

macOS에서는 `make CC=clang`도 사용할 수 있다. 실제 검증한 플랫폼과 미검증 플랫폼은 결과 보고서에서 구분한다.

## 설정

`server.h`의 기본 상수를 수정할 수 있다. 다음 CLI 인자는 해당 실행의 기본값만 덮어쓴다. `--port=8000` 형식도 지원한다.

| CLI | 기본값 | 의미 |
|---|---:|---|
| `--address` | `127.0.0.1` | 숫자로 된 IPv4 주소 |
| `--port` | `8000` | 1~65535 |
| `--workers` | `4` | 실행 중 고정되는 워커 수 |
| `--queue` | `64` | accept 후 워커 배정을 기다리는 연결 수 |
| `--backlog` | `64` | OS에 요청하는 listen backlog |
| `--idle-timeout` | `5` | keep-alive 다음 요청의 첫 바이트 대기, 초 |
| `--header-timeout` | `5` | 요청 헤더 수신 무진행 시간, 초 |
| `--send-timeout` | `5` | 응답 전송 무진행 시간, 초 |
| `--no-reuseaddr` | 옵션 생략 시 활성화 | SO_REUSEADDR 비활성화 |

큐 체류에는 시간 제한이 없다. 헤더 타이머는 워커 배정 이후 시작한다. 첫 요청은 헤더 타임아웃으로 기다리고, 후속 요청은 첫 바이트까지 idle 타임아웃을 적용한다. 첫 바이트 이후 헤더 타이머를 적용하며, 수신·전송에 진전이 있으면 각각의 기한을 갱신한다. 따라서 조금씩 계속 전송하는 요청에는 전체 소요 시간 상한이 없다.

`HEADER_LIMIT`는 요청 줄과 마지막 빈 줄까지 포함한 16KiB, `MAX_REQUESTS_PER_CONNECTION`은 100건, `SHUTDOWN_GRACE_MS`는 5000이다. 이 세 항목은 코드에서 수정한다.

## HTTP 범위와 제한

- 정확한 `GET /`만 정상 처리한다. `/missing`, `/?q=x` 등은 404다.
- GET 외 메서드는 405와 `Allow: GET`으로 응답한다. HEAD도 미지원이지만 HEAD 응답에는 본문을 보내지 않는다.
- HTTP/1.1은 Host가 필요하고 기본 keep-alive다. `Connection: close`, 100번째 요청, 종료 처리 시 연결을 닫는다. HTTP/1.0은 응답 후 닫는다.
- GET 본문은 없거나 Content-Length가 0이어야 한다. 양수·잘못된·중복 Content-Length, Transfer-Encoding, Expect는 400 후 종료한다.
- pipelining은 지원하지 않는다. 현재 헤더와 함께 수신된 여분 데이터가 있으면 첫 응답 뒤 닫는다. 도착 시점을 완벽하게 판별해 모든 pipelining을 탐지하는 기능은 없다.
- 잘못된 문법·미지원 버전·헤더 크기 초과·부분 헤더 타임아웃 등의 HTTP 오류는 결정대로 400에 통일한다. 요청 바이트가 전혀 없거나 소켓이 이미 끊어진 경우에는 응답 없이 닫을 수 있다.
- 오류 본문은 일반 텍스트이고 오류 응답 뒤에는 연결을 닫는다. TLS·정적 파일·POST·프록시는 구현하지 않는다.

chunked 수신 등 HTTP/1.1 규격이 요구하는 기능을 생략하고 오류 코드를 단순화한 **학습용 부분 구현**이다. 완전한 HTTP/1.1 준수나 운영 서비스 용도를 표방하지 않는다.

## 테스트

```sh
make test
```

실제 TCP 소켓으로 정상 응답, 순차 keep-alive, HTTP/1.0, 100요청 한도, 16KiB 경계, 잘못된 요청, 클라이언트 RST, bind 실패, 큐 포화, 타임아웃, 시그널 종료를 검증한다. Linux에서는 `/proc`으로 반복 연결 후 FD·스레드 수 회수를 확인한다. POSIX FD 한도를 낮춘 별도 서버로 accept 자원 부족도 검증한다.

Linux에서 메모리와 정의되지 않은 동작을 추가 검사할 수 있다.

```sh
make sanitize
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/test_server.py
make clean
make
```

## 부하 측정

8000번 포트를 비운 상태에서 실행한다. 도구가 단계마다 **인자 없는 기본 설정 서버**를 새로 실행하고 종료한다. 생성 로그는 `results/`에, 집계는 JSON에 저장한다.

```sh
python3 tests/load_test.py
```

TCP 유지와 반복 GET을 각각 동시접속 `1,4,16,64,128,1000`에서 측정한다. 사용자 추가 요청으로 10,000 접속 단계는 생략한다. 최초 연결 시도가 끝난 뒤 10초를 관찰하며 클라이언트 접속·GET 제한은 각각 5초다. TCP 유지 실험은 데이터를 보내거나 재접속하지 않는다. 반복 GET은 같은 연결을 사용하되 서버가 닫으면 다시 연결한다. 부하 도구의 asyncio는 클라이언트에서만 사용한다.

도구 검증용 짧은 실행도 가능하다. 이 결과는 승인한 정식 10초 측정과 구분한다.

```sh
python3 tests/load_test.py --levels 1,4 --duration 1 --output /tmp/webserver-smoke.json
```

TCP connect 성공은 애플리케이션의 accept나 HTTP 처리 성공과 다르다. 부하 발생기와 서버가 같은 컴퓨터에서 실행되며 로그·OS 제한·클라이언트 오버헤드도 결과에 반영된다. 측정 중 끝나지 않은 요청은 성공·실패와 별도 집계한다.
