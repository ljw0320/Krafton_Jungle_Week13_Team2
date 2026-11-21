# 현재 구현 결정 안내

현재 사용자 요구사항과 구현자가 직접 정한 결정은 [README.md](README.md#요청에-따른-결정과-직접-정한-결정)에 모았습니다.

현재 범위는 WSL Ubuntu의 GET 정적 파일, POST `/echo`, non-blocking 소켓과 poll·스레드 풀·eventfd, 응답 후 연결 종료입니다.

변경 전 문서 원문은 [docs/PREVIOUS_DESIGN_DECISIONS.md](docs/PREVIOUS_DESIGN_DECISIONS.md)와 [docs/PREVIOUS_README.md](docs/PREVIOUS_README.md)에 보존했습니다. 해당 문서의 고정 HTML·keep-alive·부하 측정 계획과 과거 검증 서술은 현재 구현·검증 결과에 적용되지 않습니다.
