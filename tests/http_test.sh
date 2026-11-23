#!/bin/bash
# 서버를 띄워 HTTP 응답을 확인하는 기능 테스트. 실패가 있으면 종료 코드 1
cd "$(dirname "$0")/.." || exit 1
PORT=8080
ROOT=$(mktemp -d)
LOG=$(mktemp)
pass=0
fail=0

check() {   # 이름 기대 실제
    if [ "$2" == "$3" ]; then pass=$((pass + 1))
    else fail=$((fail + 1)); echo "FAIL $1: 기대 $2, 실제 $3" >&2; fi
}

# stdin을 보내고 서버가 닫을 때까지(최대 3초) 받는다. 출력은 응답, 종료 코드 0이면 서버가 닫음
xchg() {
    python3 -c '
import socket, sys
s = socket.create_connection(("127.0.0.1", 8080))
s.settimeout(float(sys.argv[1]))
s.sendall(sys.stdin.buffer.read())
out = b""
code = 0
try:
    while True:
        d = s.recv(65536)
        if not d: break
        out += d
except socket.timeout:
    code = 3
sys.stdout.buffer.write(out)
sys.exit(code)' "${1:-3}"
}

$SERVER_WRAP ./server $PORT "$ROOT" 2>"$LOG" &
SP=$!
for _ in $(seq 100); do
    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break
    sleep 0.1
done

H=$'Host: x\r\n'
status() { head -n1 <<<"$1" | tr -d '\r'; }

# 404, 헤더 순서, Date
out=$(printf 'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | xchg)
check "404 status" "HTTP/1.1 404 Not Found" "$(status "$out")"
check "header order" "HTTP/1.1 404 Not Found|Date|Content-Type|Content-Length|Connection" \
    "$(tr -d '\r' <<<"$out" | sed -n '1,5p' | cut -d: -f1 | paste -sd'|')"
check "Date format" 1 "$(tr -d '\r' <<<"$out" | grep -acE '^Date: [A-Z][a-z]{2}, [0-9]{2} [A-Z][a-z]{2} [0-9]{4} [0-9:]{8} GMT$')"
check "404 body" "<h1>404 Not Found</h1>" "$(tail -n1 <<<"$out")"
check "404 content-length" "Content-Length: 23" "$(tr -d '\r' <<<"$out" | grep -a '^Content-Length')"

# 닫는 응답: 상태, Connection: close, 서버가 닫음
closing() {   # 이름 코드 이유 입력
    local o rc
    o=$(printf '%b' "$4" | xchg); rc=$?
    check "$1 status" "HTTP/1.1 $2 $3" "$(status "$o")"
    check "$1 close header" 1 "$(grep -ac '^Connection: close' <<<"$o")"
    check "$1 closed" 0 "$rc"
}
closing "400 Host space" 400 "Bad Request" 'GET / HTTP/1.1\r\nHost : x\r\n\r\n'
closing "505 HTTP/2.0" 505 "HTTP Version Not Supported" 'GET / HTTP/2.0\r\nHost: x\r\n\r\n'
closing "501 BREW" 501 "Not Implemented" 'BREW / HTTP/1.1\r\nHost: x\r\n\r\n'
closing "501 TE" 501 "Not Implemented" 'GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n'
closing "411 POST no CL" 411 "Length Required" 'POST / HTTP/1.1\r\nHost: x\r\n\r\n'
closing "413 CL 1000000" 413 "Content Too Large" 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1000000\r\n\r\n'

o=$(printf 'GET /%s' "$(head -c 8187 /dev/zero | tr '\0' a)" | xchg); rc=$?
check "414 status" "HTTP/1.1 414 URI Too Long" "$(status "$o")"
check "414 closed" 0 "$rc"
o=$(python3 -c '
import sys
s = b"GET / HTTP/1.1\r\nHost: x\r\nX: "
sys.stdout.buffer.write(s + b"a" * (8192 - len(s)))' | xchg); rc=$?
check "431 status" "HTTP/1.1 431 Request Header Fields Too Large" "$(status "$o")"
check "431 closed" 0 "$rc"

# 405 + Allow, 같은 연결의 다음 요청
o=$(printf 'DELETE / HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | xchg); rc=$?
check "405 then 404 statuses" "HTTP/1.1 405 Method Not Allowed|HTTP/1.1 404 Not Found" \
    "$(tr -d '\r' <<<"$o" | grep -a '^HTTP/' | paste -sd'|')"
check "405 Allow" 1 "$(grep -ac '^Allow: GET' <<<"$o")"
check "405 keep-alive" 1 "$(grep -ac '^Connection: keep-alive' <<<"$o")"
check "405 then 404 closed" 0 "$rc"
o=$(printf 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhelloGET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | xchg)
check "POST body then GET" "HTTP/1.1 405 Method Not Allowed|HTTP/1.1 404 Not Found" \
    "$(tr -d '\r' <<<"$o" | grep -a '^HTTP/' | paste -sd'|')"

# 연결 유지 판단
o=$(printf 'GET / HTTP/1.0\r\n\r\n' | xchg); rc=$?
check "1.0 no header close" "Connection: close" "$(tr -d '\r' <<<"$o" | grep -a '^Connection')"
check "1.0 no header closed" 0 "$rc"
o=$(printf 'GET / HTTP/1.0\r\nConnection: Keep-Alive, Upgrade\r\n\r\n' | xchg 1); rc=$?
check "1.0 keep-alive" "Connection: keep-alive" "$(tr -d '\r' <<<"$o" | grep -a '^Connection')"
check "1.0 keep-alive kept open" 3 "$rc"
o=$(printf 'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | xchg); rc=$?
check "1.1 Connection close" 0 "$rc"

# curl이 연결을 재사용한다
check "curl reuse" 1 "$(curl -sv http://127.0.0.1:$PORT/ http://127.0.0.1:$PORT/ 2>&1 | grep -c 'Re-using existing connection')"

kill $SP
wait $SP 2>/dev/null
if grep -qE 'AddressSanitizer|runtime error' "$LOG"; then
    fail=$((fail + 1)); echo "FAIL sanitizer: 기대 오류 없음, 실제 서버 로그에 있음" >&2
    grep -E 'AddressSanitizer|runtime error' "$LOG" >&2
fi
rm -rf "$ROOT" "$LOG"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
