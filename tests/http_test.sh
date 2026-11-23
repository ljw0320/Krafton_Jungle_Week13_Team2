#!/bin/bash
# 서버를 띄워 HTTP 응답을 확인하는 기능 테스트. 실패가 있으면 종료 코드 1
cd "$(dirname "$0")/.." || exit 1
PORT=8080
TMP=$(mktemp -d)
ROOT=$TMP/www
SIB=${ROOT}2        # 루트 이름으로 시작하는 형제 디렉토리 (/srv/www2 유형)
OUT=$TMP/outside
LOG=$(mktemp)
SP=
pass=0
fail=0
# 느린 서버(valgrind)에서 1초 제한이 헛되이 실패하지 않게 넓힌다
LIM=1; [ -n "$SERVER_WRAP" ] && LIM=5

cleanup() {
    [ -n "$SP" ] && kill $SP 2>/dev/null
    chmod -R u+rwx "$TMP" 2>/dev/null   # chmod 000 항목도 지워지게
    rm -rf "$TMP" "$LOG"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

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

# 픽스처 (저장소에 넣지 않는다. 심볼릭 링크와 FIFO가 Windows 체크아웃에서 깨진다)
mkdir -p "$ROOT/docs" "$ROOT/empty" "$ROOT/v1.2" "$ROOT/dirindex/index.html" "$ROOT/outidx" \
    "$ROOT/noperm" "$OUT" "$SIB"
printf '<h1>home</h1>\n' >"$ROOT/index.html"
printf 'body{}\n' >"$ROOT/style.css"
printf 'var a=1;\n' >"$ROOT/app.js"
head -c 100 /dev/urandom >"$ROOT/img.png"
head -c 100 /dev/urandom >"$ROOT/photo.PNG"
head -c 100 /dev/urandom >"$ROOT/a.jpg"
head -c 100 /dev/urandom >"$ROOT/b.jpeg"
printf 'dots\n' >"$ROOT/notes..txt"
printf 'space\n' >"$ROOT/my file.txt"
printf 'noext\n' >"$ROOT/noext"
printf 'readme\n' >"$ROOT/v1.2/readme"
printf '<h1>docs</h1>\n' >"$ROOT/docs/index.html"
printf 'outside\n' >"$OUT/secret.txt"
printf 'sibling\n' >"$SIB/x.txt"
printf 'perm\n' >"$ROOT/noperm/f"
printf 'locked\n' >"$ROOT/secret"
head -c 8388608 /dev/urandom >"$ROOT/big.bin"
: >"$ROOT/zero"
ln -s "$OUT" "$ROOT/out"
ln -s "$ROOT/docs" "$ROOT/in"
ln -s "$SIB" "$ROOT/sib"
ln -s "$OUT/secret.txt" "$ROOT/outidx/index.html"
mkfifo "$ROOT/pipe"
chmod 000 "$ROOT/secret" "$ROOT/noperm"

$SERVER_WRAP ./server $PORT "$ROOT" </dev/null 2>"$LOG" &
SP=$!
for _ in $(seq 100); do
    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break
    sleep 0.1
done

H=$'Host: x\r\n'
status() { head -n1 <<<"$1" | tr -d '\r'; }
U=http://127.0.0.1:$PORT

# 404, 헤더 순서, Date
out=$(printf 'GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | xchg)
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
o=$(printf 'DELETE / HTTP/1.1\r\nHost: x\r\n\r\nGET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | xchg); rc=$?
check "405 then 404 statuses" "HTTP/1.1 405 Method Not Allowed|HTTP/1.1 404 Not Found" \
    "$(tr -d '\r' <<<"$o" | grep -a '^HTTP/' | paste -sd'|')"
check "405 Allow" 1 "$(grep -ac '^Allow: GET' <<<"$o")"
check "405 keep-alive" 1 "$(grep -ac '^Connection: keep-alive' <<<"$o")"
check "405 then 404 closed" 0 "$rc"
o=$(printf 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhelloGET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | xchg)
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
check "curl reuse" 1 "$(curl -sv $U/ $U/ 2>&1 | grep -c 'Re-using existing connection')"

# 파일 없는 연결이 서버의 fd 0을 닫지 않는다. 포트 대기 연결도 요청 없이 끊으므로
# 시작 값을 읽지 않고 서버에 준 /dev/null과 비교한다
(exec 3<>/dev/tcp/127.0.0.1/$PORT)
for _ in 1 2 3; do
    o=$(printf 'GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | xchg)
    check "fd 0 404" "HTTP/1.1 404 Not Found" "$(status "$o")"
done
check "fd 0 kept" /dev/null "$(readlink /proc/$SP/fd/0)"

# --- 정적 파일 ---
D=$TMP/dl
HTML='text/html; charset=utf-8'
OCT=application/octet-stream

# 경로 상태 코드 (curl --path-as-is)
code() { curl -s --path-as-is -o /dev/null -w '%{http_code}' "$U$1"; }
# 인코딩하지 않은 바이트를 그대로 보내는 상태 코드 (curl이 바꾸는 경우)
rawcode() {
    local o
    o=$(printf 'GET %b HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' "$1" | xchg)
    status "$o" | cut -d' ' -f2
}
# 경로 기대 코드 이름
codes() { local p; while [ $# -ge 2 ]; do check "code $1" "$2" "$(code "$1")"; shift 2; done; }

# 파일마다 상태 + Content-Type + 내용
filecase() {   # 경로 Content-Type 원본
    local r
    r=$(curl -s --path-as-is -o "$D" -w '%{http_code} %{content_type}' "$U$1")
    check "file $1" "200 $2" "$r"
    cmp -s "$D" "$3"; check "cmp $1" 0 $?
}
filecase /index.html "$HTML" "$ROOT/index.html"
filecase / "$HTML" "$ROOT/index.html"
filecase /style.css text/css "$ROOT/style.css"
filecase /app.js text/javascript "$ROOT/app.js"
filecase /img.png image/png "$ROOT/img.png"
filecase /photo.PNG image/png "$ROOT/photo.PNG"
filecase /a.jpg image/jpeg "$ROOT/a.jpg"
filecase /b.jpeg image/jpeg "$ROOT/b.jpeg"
filecase /notes..txt "$OCT" "$ROOT/notes..txt"
filecase /my%20file.txt "$OCT" "$ROOT/my file.txt"
filecase /noext "$OCT" "$ROOT/noext"
filecase /v1.2/readme "$OCT" "$ROOT/v1.2/readme"
filecase /docs/ "$HTML" "$ROOT/docs/index.html"
filecase /docs/index.html "$HTML" "$ROOT/docs/index.html"
filecase /in/index.html "$HTML" "$ROOT/docs/index.html"
filecase /in/ "$HTML" "$ROOT/docs/index.html"
filecase "/img.png?v=3" image/png "$ROOT/img.png"
filecase "/?x=1" "$HTML" "$ROOT/index.html"
filecase /img/../index.html "$HTML" "$ROOT/index.html"
filecase //index.html "$HTML" "$ROOT/index.html"
filecase /./index.html "$HTML" "$ROOT/index.html"
filecase /zero "$OCT" "$ROOT/zero"
filecase /big.bin "$OCT" "$ROOT/big.bin"
check "zero Content-Length" "Content-Length: 0" \
    "$(curl -s -D - -o /dev/null $U/zero | tr -d '\r' | grep -a '^Content-Length')"
check "404 keep-alive" "Connection: keep-alive" \
    "$(curl -s -D - -o /dev/null $U/nope | tr -d '\r' | grep -a '^Connection')"

# 403 / 404 (확인 항목 순서: 경로 탈출, 링크, 특수 파일, 디렉토리)
codes /../etc/passwd 403 /%2e%2e/%2e%2e/etc/passwd 403 /img/../../x 403 \
    /%252e%252e/etc/passwd 404 /notes..txt 200 \
    /out/secret.txt 403 /sib/x.txt 403 /pipe 403 /noperm/f 403 \
    /nope 404 /docs 404 /empty/ 404 /empty 404 /in 404 \
    /dirindex/ 403 /outidx/ 403 /no/such/dir/x 404 /index.html/x 404
[ "$(id -u)" -ne 0 ] && check "code /secret" 403 "$(code /secret)"

# 400: curl이 바꾸는 바이트는 그대로 보낸다
for p in '/%zz' '/abc%' '/a%0' '/a%00' '/a\x80' '/a\xff' '/index.html#x' '/a%2' ; do
    check "raw 400 $p" 400 "$(rawcode "$p")"
done
check "raw 200 /index.html?v=3" 200 "$(rawcode '/index.html?v=3')"
check "raw 200 %2e%2e inside" 200 "$(rawcode '/docs/%2e%2e/index.html')"

# FIFO 요청을 연 채로 다른 연결이 멈추지 않는다
exec 5<>/dev/tcp/127.0.0.1/$PORT
printf 'GET /pipe HTTP/1.1\r\nHost: x\r\n\r\n' >&5
check "fifo other curl" 200 "$(curl -s -o /dev/null -m $LIM -w '%{http_code}' $U/)"
read -r -t 3 -u 5 line; check "fifo 403" "HTTP/1.1 403 Forbidden" "$(tr -d '\r' <<<"$line")"
exec 5>&-

# 느린 클라이언트가 받는 중에도 다른 연결이 멈추지 않는다
curl -s --limit-rate 200k -m 3 -o /dev/null $U/big.bin & CP=$!
sleep 0.5
check "slow other curl" 200 "$(curl -s -o /dev/null -m $LIM -w '%{http_code}' $U/)"
wait $CP; check "slow curl timed out" 28 $?
grep -aq 'EPOLLOUT on' "$LOG"; check "EPOLLOUT on logged" 0 $?

# 8MB를 다 받는다. 로그에 EPOLLOUT on -> off
L0=$(wc -l <"$LOG")
curl -s --limit-rate 4M -o "$D" $U/big.bin; check "big curl rc" 0 $?
check "big size" 8388608 "$(wc -c <"$D")"
cmp -s "$D" "$ROOT/big.bin"; check "big cmp" 0 $?
seq=$(tail -n +$((L0 + 1)) "$LOG" | grep -a EPOLLOUT | sed 's/fd=[0-9]* //' | paste -sd,)
[[ $seq == "EPOLLOUT on,"*"EPOLLOUT off" ]]; check "EPOLLOUT on then off" 0 $?

# 같은 연결에서 파일 두 개 (keep-alive)
check "two files reuse" 1 "$(curl -sv -o "$D" -o "$D.2" $U/index.html $U/style.css 2>&1 | grep -c 'Re-using existing connection')"
cmp -s "$D" "$ROOT/index.html"; check "two files cmp 1" 0 $?
cmp -s "$D.2" "$ROOT/style.css"; check "two files cmp 2" 0 $?

# 파일 + 404 파이프라인
o=$(printf 'GET /style.css HTTP/1.1\r\nHost: x\r\n\r\nGET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | xchg); rc=$?
check "pipeline statuses" "HTTP/1.1 200 OK|HTTP/1.1 404 Not Found" "$(tr -d '\r' <<<"$o" | grep -a '^HTTP/' | paste -sd'|')"
check "pipeline lengths" "Content-Length: 7|Content-Length: 23" "$(tr -d '\r' <<<"$o" | grep -a '^Content-Length' | paste -sd'|')"
check "pipeline order body" "body{}|<h1>404 Not Found</h1>" "$(tr -d '\r' <<<"$o" | grep -a -e '^body{}' -e '^<h1>' | paste -sd'|')"
check "pipeline closed" 0 "$rc"
# 8MB 파일 + 404: 부분 전송이 끝난 뒤 다음 응답이 이어서 나온다
printf 'GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\nGET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | xchg 5 >"$D"; rc=$?
check "big pipeline closed" 0 $rc
check "big pipeline tail" "<h1>404 Not Found</h1>" "$(tail -n1 "$D")"
check "big pipeline headers" "HTTP/1.1 200 OK|Content-Length: 8388608" \
    "$(head -c 400 "$D" | tr -d '\r' | grep -a -e '^HTTP/1.1 200' -e '^Content-Length' | paste -sd'|')"
tail -c +$(( $(head -c 400 "$D" | grep -abo $'^\r$' | head -n1 | cut -d: -f1) + 3 )) "$D" | head -c 8388608 | cmp -s - "$ROOT/big.bin"
check "big pipeline body" 0 $?

# 전송 도중 끊어도 fd가 새지 않는다
curl -s -o /dev/null $U/ ; sleep 0.2
n0=$(ls /proc/$SP/fd | wc -l)
for i in 1 2 3; do
    curl -s --max-time 0.1 --limit-rate 1M -o /dev/null $U/big.bin; check "abort rc $i" 28 $?
done
n1=
for _ in $(seq 30); do
    n1=$(ls /proc/$SP/fd | wc -l)
    [ "$n1" == "$n0" ] && break
    sleep 0.1
done
check "fd count after abort" "$n0" "$n1"

kill $SP
wait $SP 2>/dev/null
SP=
if grep -qE 'AddressSanitizer|runtime error' "$LOG"; then
    fail=$((fail + 1)); echo "FAIL sanitizer: 기대 오류 없음, 실제 서버 로그에 있음" >&2
    grep -E 'AddressSanitizer|runtime error' "$LOG" >&2
fi
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
