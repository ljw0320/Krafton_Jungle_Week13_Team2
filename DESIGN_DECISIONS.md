# 현재 구현 결정 안내

현재 사용자 요구사항과 구현자가 직접 정한 결정은 [README.md](README.md#요청에-따른-결정과-직접-정한-결정)에 모았습니다.

현재 범위는 WSL Ubuntu의 GET 정적 파일, POST `/echo`, GET·POST CGI 실행, non-blocking 소켓과 poll·스레드 풀·eventfd, HTTP/1.1 keep-alive입니다. 사용자 선택에 따라 연결 슬롯·큐 16개, 작업자 4개, 유휴 5초, 연결당 최대 100개 요청, 미리 도착한 후속 요청의 순차 처리를 적용합니다. HTTP/1.0·Connection: close·100번째 응답·서버 오류 응답은 종료하며, 요청 파싱 오류 뒤 연결을 재사용하지 않습니다. 상세 계약은 [README의 keep-alive 절](README.md#keep-alive-정책과-요청-경계)에 있습니다.

동적 처리는 선택 인자 `CGI_ROOT`와 `/cgi-bin/이름` 라우팅으로 활성화하며 실행 기한·출력 한도·자식 회수를 적용합니다. 상세 계약은 [README의 CGI 절](README.md#동적-요청-처리-cgi)에 있습니다.

웹페이지는 기존 경로로 HTTP 응답을 확인합니다. TCP 연결 정책 7개는 별도 실행기가 빌드된 서버의 임시 인스턴스에 소켓 요청을 보내 검사하며 결과를 JSON으로 저장합니다. 페이지는 이 파일을 로컬에서 읽어 표시합니다. 테스트를 위해 추가했던 운영 서버의 추적 ID·헤더와 CGI 시간 예외는 제거했습니다. 모든 CGI의 2초 기한을 유지하고 페이지 최대 폭은 80rem입니다. [검사와 실행기 사용법](README.md#웹페이지-검사와-별도-tcp-실행기)을 참고하세요.

변경 전 문서 원문은 [docs/PREVIOUS_DESIGN_DECISIONS.md](docs/PREVIOUS_DESIGN_DECISIONS.md)와 [docs/PREVIOUS_README.md](docs/PREVIOUS_README.md)에 보존했습니다. 해당 문서의 고정 HTML·keep-alive·부하 측정 계획과 과거 검증 서술은 현재 구현·검증 결과에 적용되지 않습니다.
