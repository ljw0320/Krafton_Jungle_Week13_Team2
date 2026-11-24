#!/bin/bash
# 서버와 테스트 백엔드를 띄워 Reverse Proxy 동작을 확인하는 기능 테스트. 실패가 있으면 종료 코드 1
# 타이머 케이스(hang, stall, slow, drip, noaccept, nolenopen)는 동시에 돌려 약 15초가 걸린다
# 요청을 보내는 중 10초 초과(SENDING 타이머)는 테스트하지 않는다: 작은 요청은 바로 보내지고, 백엔드 수신 버퍼를 채우려면 수 MB가 필요하며 시점도 정하기 어렵다
cd "$(dirname "$0")/.." || exit 1
PORT=8080
TMP=$(mktemp -d)
ROOT=$TMP/www
LOG=$(mktemp)
LOG2=$(mktemp)
pass=0
fail=0
# 느린 서버(valgrind)에서 시간 제한이 헛되이 실패하지 않게 넓힌다
W=10; MT=5; XT=3; LIM=1
[ -n "$SERVER_WRAP" ] && { W=60; MT=25; XT=15; LIM=5; }

cleanup() {
    kill $(jobs -p) 2>/dev/null   # 서버, 백엔드, 백그라운드 클라이언트
    rm -rf "$TMP" "$LOG" "$LOG2"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

check() {   # 이름 기대 실제
    if [ "$2" == "$3" ]; then pass=$((pass + 1))
    else fail=$((fail + 1)); echo "FAIL $1: 기대 $2, 실제 $3" >&2; fi
}

# stdin을 보내고 서버가 닫을 때까지 받는다. 출력은 응답, 종료 코드 0이면 서버가 닫음
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
sys.exit(code)' "${1:-$XT}"
}
raw() { printf '%b' "$1" | xchg; }   # \r\n, \x01 같은 이스케이프를 해석해 보낸다
status() { head -n1 <<<"$1" | tr -d '\r'; }
hdr() { tr -d '\r' <<<"$1" | grep -m1 -i "^$2:"; }
rbody() { printf '%s' "${1#*$'\r\n\r\n'}"; }   # 응답에서 헤더를 뺀 본문
bodysha() {   # backend.py의 본문 규칙으로 N바이트의 sha256
    python3 -B -c 'import sys, hashlib; sys.path.insert(0, "tests"); import backend
print(hashlib.sha256(backend.body(int(sys.argv[1]))).hexdigest())' "$1"
}
fsha() { sha256sum "$1" | cut -d' ' -f1; }
cu() { curl -s --path-as-is --max-time $MT "$@"; }
code() { cu -o /dev/null -w '%{http_code}' "$U$1"; }
# 한 curl에 URL 여러 개: 상태 코드 목록을 출력하고 연결 수는 conns로 본다
multi() { local a=() p; for p; do a+=(-o /dev/null "$U$p"); done
          curl -sv --max-time $MT -w '%{http_code},' "${a[@]}" 2>"$TMP/v"; }
conns() { grep -c 'Connected to' "$TMP/v"; }
waitport() {   # 포트 pid: 안 열리면 바로 실패로 끝낸다
    local _
    for _ in $(seq $((W * 10))); do
        (exec 3<>/dev/tcp/127.0.0.1/$1) 2>/dev/null && return 0
        kill -0 "$2" 2>/dev/null || break
        sleep 0.1
    done
    echo "FAIL start: 포트 $1이 열리지 않음" >&2
    cat "$LOG" >&2
    exit 1
}

for p in $PORT 8001 8002 8003 8081 8009; do   # 남은 프로세스가 있으면 엉뚱한 서버를 시험하게 된다
    if (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null; then
        echo "FAIL start: 포트 $p가 이미 열려 있음" >&2; exit 1
    fi
done
mkdir -p "$ROOT"
printf '<h1>home</h1>\n' >"$ROOT/index.html"

python3 -B tests/backend.py 8001 </dev/null &
BP=$!
python3 -B tests/backend.py 8002 </dev/null &   # 자원 회수 케이스에서 kill -9로 죽인다
BP2=$!
python3 -B tests/backend.py 8003 noaccept </dev/null &
$SERVER_WRAP ./server $PORT "$ROOT" /api/=127.0.0.1:8001 /dead/=127.0.0.1:8009 \
    /noaccept/=127.0.0.1:8003 /b2/=127.0.0.1:8002 </dev/null 2>"$LOG" &
SP=$!
waitport 8001 $BP
waitport 8002 $BP2
waitport $PORT $SP

U=http://127.0.0.1:$PORT

# 요청 다시 만들기: 새로 만들 헤더, 뺄 헤더, 이름 대소문자, 값 공백 정리
out=$(raw 'POST /api/echo?x=1 HTTP/1.1\r\nHost: shop.com\r\nConnection: close, X-Debug\r\nX-Debug: 1\r\nX-Forwarded-For: 1.2.3.4\r\nKeep-Alive: 5\r\nProxy-Connection: x\r\nTE: trailers\r\nUpgrade: h2c\r\nExpect: 100-continue\r\nForwarded: for=1\r\nX-Real-IP: 1\r\naccept:   */*  \r\nContent-Length: 5\r\n\r\nhello')
check "rewrite status" "HTTP/1.1 200 OK" "$(status "$out")"
check "rewrite request" $'POST /api/echo?x=1 HTTP/1.1\r\nHost: shop.com\r\nConnection: close\r\nContent-Length: 5\r\naccept: */*\r\n\r\nhello' "$(rbody "$out")"
out=$(raw 'GET /api/echo HTTP/1.0\r\n\r\n')
check "rewrite 1.0 without Host" $'GET /api/echo HTTP/1.0\r\nHost: 127.0.0.1:8001\r\nConnection: close\r\n\r' "$(rbody "$out")"   # $()가 끝의 \n을 지운다
out=$(raw 'POST /api/echo HTTP/1.1\r\nHost: h\r\nConnection: close, Content-Length, Host\r\nContent-Length: 2\r\n\r\nhi')
check "Connection names CL and Host stay" $'POST /api/echo HTTP/1.1\r\nHost: h\r\nConnection: close\r\nContent-Length: 2\r\n\r\nhi' "$(rbody "$out")"
out=$(raw 'GET /api/echo HTTP/1.1\r\nHost: h\r\nConnection: close\r\nContent-Length: 2\r\n\r\nhi')
check "GET with body forwarded" $'GET /api/echo HTTP/1.1\r\nHost: h\r\nConnection: close\r\nContent-Length: 2\r\n\r\nhi' "$(rbody "$out")"
out=$(raw 'GET /api/echo/a%2Fb?x=%20 HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n')
check "target forwarded as is" $'GET /api/echo/a%2Fb?x=%20 HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r' "$(rbody "$out")"
check "a..b passes" 200 "$(code /api/a..b)"
check "query ../ passes" 200 "$(code '/api/x?y=../z')"

# 요청 검사: 400, 405, 411, 501
RQ='\r\nHost: x\r\nConnection: close\r\n\r\n'
i=0
for t in '/api/../admin' '/api/%2e%2e/admin' '/api/a%2f..%2f..%2fadmin' '/api/%zz' '/api/x#y' '/api/a%00b' \
    '/api/\x01x' '/api/\x80x'; do
    i=$((i + 1))
    check "400 target $i" "HTTP/1.1 400 Bad Request" "$(status "$(raw "GET $t HTTP/1.1$RQ")")"
done
check "400 header 0x01" "HTTP/1.1 400 Bad Request" "$(status "$(raw 'GET /api/x HTTP/1.1\r\nHost: x\r\nX-A: a\x01b\r\nConnection: close\r\n\r\n')")"
check "400 header 0x7F" "HTTP/1.1 400 Bad Request" "$(status "$(raw 'GET /api/x HTTP/1.1\r\nHost: x\r\nX-A: a\x7fb\r\nConnection: close\r\n\r\n')")"
check "400 Host with space" "HTTP/1.1 400 Bad Request" "$(status "$(raw 'GET /api/x HTTP/1.1\r\nHost: a b/c\r\nConnection: close\r\n\r\n')")"
check "400 empty Host" "HTTP/1.1 400 Bad Request" "$(status "$(raw 'GET /api/x HTTP/1.1\r\nHost:\r\nConnection: close\r\n\r\n')")"
check "static path ignores odd Host" "HTTP/1.1 200 OK" "$(status "$(raw 'GET /index.html HTTP/1.1\r\nHost: a b/c\r\nConnection: close\r\n\r\n')")"
out=$(raw "HEAD /api/x HTTP/1.1$RQ")
check "405 backend path" "HTTP/1.1 405 Method Not Allowed|Allow: GET, POST" "$(status "$out")|$(hdr "$out" Allow)"
out=$(raw 'POST /index.html HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
check "405 static path" "HTTP/1.1 405 Method Not Allowed|Allow: GET" "$(status "$out")|$(hdr "$out" Allow)"
check "411 POST without CL" "HTTP/1.1 411 Length Required" "$(status "$(raw "POST /api/x HTTP/1.1$RQ")")"
check "501 unknown method" "HTTP/1.1 501 Not Implemented" "$(status "$(raw "BREW /api/x HTTP/1.1$RQ")")"

# 백엔드 실패는 502. 본문 형식은 다른 오류 응답과 같다
out=$(raw "GET /dead/x HTTP/1.1$RQ")
check "502 backend down" "HTTP/1.1 502 Bad Gateway|Connection: close|<h1>502 Bad Gateway</h1>" \
    "$(status "$out")|$(hdr "$out" Connection)|$(rbody "$out")"
# 한 배치에 A의 RST와 B의 이벤트가 같이 온다(8009는 스크립트가 열어서 /dead/ 케이스 뒤에 둔다)
out=$(python3 -B tests/proxy_cases.py batch $SP AB BA)
check "RST and backend event in one batch" "AB alive,BA alive" "$(tr '
' , <<<"$out" | sed 's/,$//')"
for n in bad lf dupcl tecl bighead 101 close rst; do
    check "502 $n" 502 "$(code /api/$n)"
done
check "head 8191 is 200" 200 "$(code /api/head/8191)"
check "head 8192 is 200" 200 "$(code /api/head/8192)"
check "head 8193 is 502" 502 "$(code /api/head/8193)"
check "502 HTTP/1.0 request, chunked reply" "HTTP/1.1 502 Bad Gateway" "$(status "$(raw 'GET /api/chunked HTTP/1.0\r\nHost: x\r\n\r\n')")"
out=$(raw "GET /api/1xx HTTP/1.1$RQ")
check "1xx single status" "HTTP/1.1 200," "$(tr -d '\r' <<<"$out" | grep -o '^HTTP/1\.[01] [0-9]*' | tr '\n' ,)"
check "1xx body" "after-1xx" "$(rbody "$out" | tr -d '\n')"
cu -o /dev/null $U/api/short; check "short body: curl exit 18" 18 $?

# 응답 길이와 유지
check "cl body" "cl-body" "$(cu $U/api/cl)"
out=$(multi /api/cl /index.html /api/cl)
check "cl, static, cl reuse A" "200,200,200,|1" "$out|$(conns)"
check "chunked body" "hello world" "$(cu $U/api/chunked)"
out=$(multi /api/chunked /api/chunked)
check "chunked closes A" "200,200,|2" "$out|$(conns)"
nerr=$(grep -c "proxy error" "$LOG")
check "nolen body" "nolen-body" "$(cu $U/api/nolen)"
out=$(multi /api/nolen /api/nolen)
check "nolen closes A" "200,200,|2" "$out|$(conns)"
check "nolen normal end logs no proxy error" "$nerr" "$(grep -c "proxy error" "$LOG")"
out=$(multi /api/empty /api/cl)
check "204 keeps A" "204,200,|1" "$out|$(conns)"
check "204 has no Content-Length" 0 "$(curl -si --max-time $MT $U/api/empty | grep -ic '^content-length')"
out=$(multi /api/304 /api/cl)
check "304 keeps A" "304,200,|1" "$out|$(conns)"
check "304 keeps Content-Length" "Content-Length: 5" "$(curl -si --max-time $MT $U/api/304 | tr -d '\r' | grep -i '^content-length')"
for p in over over/split; do
    cu -o "$TMP/o1" $U/api/$p -o "$TMP/o2" $U/index.html
    check "$p body" "hello" "$(<"$TMP/o1")"
    check "$p next request" "<h1>home</h1>" "$(<"$TMP/o2")"
    multi /api/$p /index.html >/dev/null
    check "$p keeps A" 1 "$(conns)"
done
# 길이를 알면 B의 FIN을 기다리지 않는다: 응답과 같은 A의 다음 요청이 LIM초 안에 끝난다
out=$(curl -sv --max-time $LIM -o "$TMP/nc1" -w '%{http_code},' $U/api/noclose -o "$TMP/nc2" $U/api/cl 2>"$TMP/v")
check "noclose: length known, no wait for FIN" "200,200,|noclose-body|cl-body|1" "$out|$(<"$TMP/nc1")|$(<"$TMP/nc2")|$(conns)"
for i in 1 2 3 4 5; do
    check "rstafter $i" "rstafter-body" "$(cu $U/api/rstafter)"
done
out=$(raw "GET /api/cl HTTP/1.1$RQ")
check "Connection close request" "HTTP/1.1 200 OK|Connection: close" "$(status "$out")|$(hdr "$out" Connection)"
check "split response body" "split-body" "$(cu $U/api/split)"

# Review Focus 1: 파이프라이닝 20개
out=$({ for _ in $(seq 19); do printf 'GET /api/cl HTTP/1.1\r\nHost: x\r\n\r\n'; done
        printf 'GET /api/cl HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'; } | xchg)
check "pipeline 20 responses" "20|20" "$(tr -d '\r' <<<"$out" | grep -c '^HTTP/1.1 200')|$(grep -ao 'cl-body' <<<"$out" | wc -l)"
# 파이프라이닝 3개: 순서
out=$(raw 'GET /api/a HTTP/1.1\r\nHost: x\r\n\r\nGET /index.html HTTP/1.1\r\nHost: x\r\n\r\nGET /api/b HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')
check "pipeline order" "/api/a,home,/api/b," "$(grep -aoE '/api/[ab]|home' <<<"$out" | tr '\n' ,)"
# nc -N처럼 요청 뒤 쓰기만 닫음
out=$(python3 -c '
import socket
s = socket.create_connection(("127.0.0.1", 8080)); s.settimeout(3)
s.sendall(b"GET /api/cl HTTP/1.1\r\nHost: x\r\n\r\n"); s.shutdown(socket.SHUT_WR)
out = b""
try:
    while True:
        d = s.recv(4096)
        if not d: break
        out += d
except socket.timeout: pass
print(out.decode().split("\r\n")[0], out.decode().split("\r\n\r\n")[1].strip())')
check "half-closed request gets response" "HTTP/1.1 200 OK cl-body" "$out"

# Review Focus 2: 응답 본문 길이가 버퍼 경계에 걸림. 같은 A의 다음 요청도 정상
for n in 0 1 16383 16384 16385 32768; do
    curl -sv --max-time $MT -o "$TMP/sz" -w '%{http_code},' $U/api/size/$n -o "$TMP/nx" $U/api/cl 2>"$TMP/v" >"$TMP/w"
    check "size $n" "200,200,|1|$(bodysha $n)|cl-body" "$(<"$TMP/w")|$(conns)|$(fsha "$TMP/sz")|$(<"$TMP/nx")"
done

# Review Focus 4: 헤더와 본문을 합쳐 8192바이트인 POST가 여러 조각으로 옴
out=$(python3 -c '
import socket, time
def mk(k): return b"POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % k
k = 8192 - len(mk(8192))
while len(mk(k)) + k != 8192: k -= 1
payload = bytes((i * 13 + 5) % 256 for i in range(k))
req = mk(k) + payload
assert len(req) == 8192
s = socket.create_connection(("127.0.0.1", 8080)); s.settimeout(5)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
for i in range(0, 8192, 1000):
    s.sendall(req[i:i + 1000]); time.sleep(0.05)
out = b""
while True:
    d = s.recv(65536)
    if not d: break
    out += d
echo = out.split(b"\r\n\r\n", 1)[1]
print(out.split(b"\r\n")[0].decode(), echo.split(b"\r\n\r\n", 1)[1] == payload)')
check "POST 8192 in pieces" "HTTP/1.1 200 OK True" "$out"

# 큰 응답: 8MB. 길이를 아는 것은 sha256, 모르는 것은 느린 클라이언트와 backpressure
cu -o "$TMP/big" $U/api/big
check "big sha256" "$(bodysha 8388608)" "$(fsha "$TMP/big")"
slog=$(wc -l <"$LOG")   # 이 구간의 로그만 센다
python3 -c '
import socket, hashlib, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
s.settimeout(30); s.connect(("127.0.0.1", 8080))
s.sendall(b"GET /api/bignolen HTTP/1.1\r\nHost: x\r\n\r\n")
time.sleep(1.5)
parts = []
got = 0
while True:
    d = s.recv(65536)
    if not d: break
    parts.append(d)
    got += len(d)
    if got >= 1 << 20:   # 1MB마다 쉬어 서버가 느린 읽기를 여러 번 겪게 한다(valgrind에서도 off/on이 반복된다)
        got = 0; time.sleep(0.2)
data = b"".join(parts)
head, _, b = data.partition(b"\r\n\r\n")
print(head.split(b"\r\n")[0].decode(), hashlib.sha256(b).hexdigest())' >"$TMP/slow" &
slowpid=$!
sleep 0.5
fast() { curl -s -o /dev/null -w '%{http_code}' --max-time $LIM "$U$1"; }
r1=$(fast /index.html); r2=$(fast /api/cl)
kill -0 $slowpid 2>/dev/null && alive=alive || alive=done
check "other requests fast while A is slow" "200,200,alive" "$r1,$r2,$alive"
wait $slowpid
check "slow reader bignolen sha256, A closes" "HTTP/1.1 200 OK $(bodysha 8388608)" "$(<"$TMP/slow")"
seg=$(tail -n +$((slog + 1)) "$LOG")
offs=$(grep -Ec 'backend fd=[0-9]+ off' <<<"$seg"); ons=$(grep -Ec 'backend fd=[0-9]+ on' <<<"$seg")
check "backpressure off and on repeat" "yes" "$([ "$offs" -ge 2 ] && [ "$ons" -ge 2 ] && echo yes || echo "off=$offs on=$ons")"
check "connect logged" "yes" "$(grep -q 'backend fd=[0-9]* connect 127.0.0.1:8001' "$LOG" && echo yes || echo no)"

# --- 타이머와 504 ---
# 모두 백그라운드로 동시에 돌린다. 시간은 서버와 같은 벽시계(time.time())로 잰다. sweep이 최대 1초 늦어 상한은 한도 + 3이다
H=0; [ -n "$SERVER_WRAP" ] && H=40   # valgrind에서는 상한만 넓힌다
ptimer() {   # 이름 경로 하한 상한 [hold]. 출력: 상태 라인|Connection|본문|ok. exec라서 $!가 python의 PID다
    exec python3 -c '
import socket, sys, time
name, path, lo, hi, hold = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4]), sys.argv[5]
keep = []
if hold == "1":   # accept 큐를 채워 둔다. 이어 오는 연결은 SYN이 버려져 connect가 끝나지 않는다
    for _ in range(2):
        h = socket.socket(); h.settimeout(1)
        try: h.connect(("127.0.0.1", 8003))
        except OSError: pass
        keep.append(h)
t0 = time.time()
s = socket.create_connection(("127.0.0.1", 8080))
s.settimeout(hi + 10)
s.sendall(("GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n" % path).encode())
out = b""
try:
    while True:
        d = s.recv(65536)
        if not d: break
        out += d
except socket.timeout: pass
t = time.time() - t0
head, _, body = out.decode("latin-1").partition("\r\n\r\n")
lines = head.split("\r\n")
conn = [l for l in lines if l.startswith("Connection:")]
print(lines[0], ",".join(conn), body.replace("\n", ""), "ok" if lo <= t < hi else "t=%.2f" % t, sep="|")' "$1" "$2" "$3" "$4" "${5:-0}" >"$TMP/pt.$1" 2>&1
}
stallcurl() {   # A가 10~13초에 닫히고 curl 종료 코드가 18이다
    local a b rc
    a=$(date +%s.%N)
    curl -s --max-time $((20 + H)) -o /dev/null $U/api/stall; rc=$?
    b=$(date +%s.%N)
    echo "$rc $(awk -v a="$a" -v b="$b" -v h=$H 'BEGIN { t = b - a; print (t >= 10 && t < 13 + h) ? "ok" : sprintf("t=%.2f", t) }')" >"$TMP/pt.stall"
}
TP=
ptimer hang /api/hang 10 $((13 + H)) & TP="$TP $!"
ptimer noaccept /noaccept/x 3 $((6 + H)) 1 & TP="$TP $!"
ptimer nolenopen /api/nolenopen 10 $((13 + H)) & TP="$TP $!"
ptimer slow /api/slow 9 $((12 + H)) & TP="$TP $!"
ptimer drip /api/drip 15 $((19 + H)) & TP="$TP $!"
ptimer driphead /api/driphead 15 $((19 + H)) & TP="$TP $!"
stallcurl & TP="$TP $!"
wait $TP    # 인자 없는 wait는 서버까지 기다린다
check "timer hang" "HTTP/1.1 504 Gateway Timeout|Connection: close|<h1>504 Gateway Timeout</h1>|ok" "$(<"$TMP/pt.hang")"
check "timer noaccept" "HTTP/1.1 504 Gateway Timeout|Connection: close|<h1>504 Gateway Timeout</h1>|ok" "$(<"$TMP/pt.noaccept")"
check "timer nolenopen" "HTTP/1.1 200 OK|Connection: close|nolenopen-body|ok" "$(<"$TMP/pt.nolenopen")"
check "timer slow 9s is 200" "HTTP/1.1 200 OK|Connection: close|slow-body|ok" "$(<"$TMP/pt.slow")"
check "timer drip 15s received" "HTTP/1.1 200 OK|Connection: close|abc|ok" "$(<"$TMP/pt.drip")"
check "timer driphead 15s received" "HTTP/1.1 200 OK|Connection: close|abc|ok" "$(<"$TMP/pt.driphead")"
check "timer stall curl 18" "18 ok" "$(<"$TMP/pt.stall")"

# --- 자원 회수 (fd 수와 conn_count가 처음 값으로 돌아온다) ---
fdn() { ls /proc/$SP/fd 2>/dev/null | wc -l; }   # valgrind면 자기 fd가 있으니 처음 값과 비교한다
cnt() { local n; n=$(grep -o 'conn_count=[0-9]*' "$LOG" | tail -1 | cut -d= -f2); echo "${n:-0}"; }
waitlog() {   # 패턴 개수 상한초: 로그에 패턴이 개수 이상 나올 때까지
    local i
    for i in $(seq $(($3 * 10))); do
        [ "$(grep -c "$1" "$LOG")" -ge "$2" ] && return 0
        sleep 0.1
    done
    return 1
}
reclaim() {   # 이름 상한초
    local i
    for i in $(seq $(($2 * 10))); do
        [ "$(fdn)" == "$FD0" ] && [ "$(cnt)" == "$CN0" ] && break
        sleep 0.1
    done
    check "$1: fd and conn_count back" "$FD0 $CN0" "$(fdn) $(cnt)"
}
# 앞의 케이스가 남긴 연결이 다 닫힌 뒤의 값이 처음 값이다
for _ in $(seq $(((13 + H) * 10))); do [ "$(cnt)" == 0 ] && break; sleep 0.1; done
a=$(fdn)
for _ in $(seq 50); do sleep 0.2; b=$(fdn); [ "$a" == "$b" ] && break; a=$b; done
FD0=$(fdn); CN0=$(cnt)
check "baseline conn_count" 0 "$CN0"
# 요청을 보내고 쉰 뒤 클라이언트가 닫는다. 모드 rst는 RST, fin은 FIN
closer() {   # 모드 경로 쉬는시간
    python3 -c '
import socket, struct, sys, time
s = socket.create_connection(("127.0.0.1", 8080))
s.sendall(("GET %s HTTP/1.1\r\nHost: x\r\n\r\n" % sys.argv[2]).encode())
time.sleep(float(sys.argv[3]))
if sys.argv[1] == "rst": s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
s.close()' "$@"
}
# 1. 기다리는 중 RST
closer rst /api/hang 0.5
reclaim "RST while waiting" $LIM

# 2, 3. 13초 안에 회수되는 것들과 멈춘 느린 클라이언트를 동시에 돌린다. 모두 닫혀야 처음 값이다
slowclient() {   # A가 10~14초에 닫힌다. 클라이언트는 닫힘을 볼 수 없어(송신 버퍼가 안 빠진다) 서버 로그의 idle timeout 줄로 잰다
    python3 -c '
import os, socket, sys, time
log, hi = sys.argv[1], float(sys.argv[2])
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
s.settimeout(30); s.connect(("127.0.0.1", 8080))
s.sendall(b"GET /api/big HTTP/1.1\r\nHost: x\r\n\r\n")
s.recv(1000)   # 조금 받고 읽지 않는다
size0 = os.path.getsize(log); t0 = time.time()
while time.time() - t0 < hi + 10:
    with open(log, "rb") as f:
        f.seek(size0)
        if b"idle timeout" in f.read(): break
    time.sleep(0.1)
t = time.time() - t0
print("ok" if 10 <= t < hi else "t=%.2f" % t)' "$LOG" $((14 + H)) >"$TMP/pt.slowclient" 2>&1
}
slowclient & SC=$!
T0=$(date +%s.%N)
curl -s --max-time 0.1 -o /dev/null $U/api/stall; c1=$?    # 중계 중 끊기. 본문 10바이트 뒤 백엔드가 멈춘다
curl -s --limit-rate 1M --max-time 0.3 -o /dev/null $U/api/big; c2=$?   # 8MB를 느리게 받다가 끊긴다
check "curl cut while relaying: exit 28" "28 28" "$c1 $c2"
closer fin /api/hang 0.3                            # 기다리는 중 FIN
curl -s --max-time $((20 + H)) -o /dev/null -w '%{http_code}' $U/b2/hang >"$TMP/pt.b2hang" & B2=$!
{ curl -s --max-time $((20 + H)) -o /dev/null $U/b2/stall; echo $? >"$TMP/pt.b2stall"; } & B2="$B2 $!"
waitlog 'connect 127.0.0.1:8002' 2 $W
sleep 0.5   # 요청이 hang에 닿고 stall이 헤더를 받을 시간
check "held: fds above baseline" yes "$([ "$(fdn)" -gt "$FD0" ] && echo yes || echo no)"
kill -9 $BP2 2>/dev/null; wait $BP2 2>/dev/null
TK=$(date +%s.%N)
wait $B2
TE=$(date +%s.%N)
check "kill -9 to 502 and curl 18 within 3s" ok "$(awk -v a=$TK -v b=$TE -v h=$H 'BEGIN { t = b - a; print (t < 3 + h) ? "ok" : sprintf("t=%.2f", t) }')"
check "backend killed while waiting: 502" 502 "$(<"$TMP/pt.b2hang")"
check "backend killed while relaying: curl 18" 18 "$(<"$TMP/pt.b2stall")"
# 끊은 것들은 13초 안에 닫힌다. 멈춘 느린 클라이언트(A, B 2개)는 10~14초에 닫히므로 아직 열려 있거나 이미 닫힌 두 경우를 본다
for _ in $(seq $(((15 + H) * 10))); do
    f=$(fdn); n=$(cnt)
    { [ "$f" == "$FD0" ] && [ "$n" == "$CN0" ]; } || { [ "$f" == "$((FD0 + 2))" ] && [ "$n" == "$((CN0 + 1))" ]; } && break
    sleep 0.1
done
TF=$(date +%s.%N)
check "cut ones reclaimed within 13s" ok "$(awk -v a=$T0 -v b=$TF -v h=$H 'BEGIN { t = b - a; print (t < 13 + h) ? "ok" : sprintf("t=%.2f", t) }')"
wait $SC
check "stuck slow client closes at 10-14s" ok "$(<"$TMP/pt.slowclient")"
reclaim "curl cut, FIN waiting, backend kill, stuck client" $((14 + H))

# 4. 동시 요청 200개
out=$(seq 200 | xargs -P 100 -I{} curl -s -o /dev/null --max-time $((MT * 3)) -w '%{http_code}\n' $U/api/cl | sort | uniq -c | tr -s ' ' | sed 's/^ //')
check "200 concurrent requests" "200 200" "$out"
reclaim "200 concurrent" $LIM

# 5. RST 클라이언트와 정상 요청을 섞은 300건(한 배치 안의 해제)
out=$(python3 -c '
import random, socket, struct, time
from concurrent.futures import ThreadPoolExecutor
rnd = random.Random(1)
jobs = [(i % 3 == 0, rnd.choice(["/api/cl", "/api/size/50000", "/api/hang"]), rnd.random() * 0.03) for i in range(300)]
def one(j):
    rst, path, d = j
    try:
        s = socket.create_connection(("127.0.0.1", 8080)); s.settimeout(30)
        if rst:
            s.sendall(("GET %s HTTP/1.1\r\nHost: x\r\n\r\n" % path).encode())
            time.sleep(d)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            s.close()
            return None
        s.sendall(("GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n" % path.replace("hang", "cl")).encode())
        out = b""
        while True:
            c = s.recv(65536)
            if not c: break
            out += c
        s.close()
        return out.startswith(b"HTTP/1.1 200 ")
    except OSError:
        return False
with ThreadPoolExecutor(100) as ex: r = list(ex.map(one, jobs))
n = [x for x in r if x is not None]
print(len(n), sum(n))')
check "300 mixed: all normal requests 200" "200 200" "$out"
check "alive after RST mix" 200 "$(code /api/cl)"
reclaim "300 mixed" $LIM

# 6. fd 한도가 낮은 서버: 프록시 요청은 fd를 2개 쓴다
UL=8; NR=60
[ -n "$SERVER_WRAP" ] && { UL=80; NR=150; }
startlow() {   # ulimit: 상속한 fd를 닫아 환경과 상관없이 빈 fd 수를 정한다(0,1,2 + listen + epoll 뒤 남은 만큼)
    ( for f in /proc/self/fd/*; do n=${f##*/}; [ "$n" -gt 2 ] && eval "exec $n>&-"; done   # 2>/dev/null로 감싸면 닫는 fd가 저장해 둔 stderr라서 서버의 stderr가 /dev/null이 된다
      ulimit -n $1; exec $SERVER_WRAP ./server 8081 "$ROOT" /api/=127.0.0.1:8001 ) </dev/null 2>>"$LOG2" &
    SP2=$!
    waitport 8081 $SP2
    for _ in $(seq 30); do [ "$(ls /proc/$SP2/fd | wc -l)" -le 5 ] && break; sleep 0.1; done   # waitport의 연결이 닫힐 때까지
}
stoplow() { kill $SP2; wait $SP2 2>/dev/null; }
startlow $UL
out=$(seq $NR | xargs -P $NR -I{} curl -s -o /dev/null --max-time $((MT * 3)) -w '%{http_code} %{exitcode}\n' http://127.0.0.1:8081/api/split | sort | uniq -c | tr -s ' ' | sed 's/^ //' | tr '\n' ,)
# valgrind의 accept4 래퍼는 fd가 모자라면 이미 대기열에서 뺀 연결을 닫아 RST가 된다(curl 56). 멈춤(28)이 아니니 그때만 허용한다
okre="^[0-9]+ (200 0|500 0)$|^$"; [ -n "$SERVER_WRAP" ] && okre="^[0-9]+ (200 0|500 0|000 56)$|^$"
bad=$(tr ',' '\n' <<<"$out" | grep -vE "$okre" | tr '\n' ' ')
check "ulimit: every request ends 200 or 500 (got $out)" "" "$bad"
check "ulimit: 500 or accept paused logged" yes "$(grep -qE 'accept paused|proxy error 500' "$LOG2" && echo yes || echo no)"
check "ulimit: curl 200 afterwards" 200 "$(curl -s -o /dev/null --max-time $((MT * 3)) -w '%{http_code}' http://127.0.0.1:8081/api/cl)"
stoplow
# 7. 멈춘 accept가 풀린다. valgrind는 자기 fd 때문에 빈 fd를 3개로 맞출 수 없어 건너뛴다
if [ -z "$SERVER_WRAP" ]; then
    startlow 8   # 빈 fd 3개
    check "accept resumes when only B closes (proxy_close)" "resume ok" "$(python3 -B tests/proxy_cases.py resume)"
    stoplow
    startlow 8
    check "accept resumes when a client closes (conn_close)" "idle ok" "$(python3 -B tests/proxy_cases.py idle)"
    stoplow
fi

# 시작 뒤에도 서버가 산다
check "server alive: static" 200 "$(code /index.html)"
check "server alive: proxy" 200 "$(code /api/cl)"

kill $SP
wait $SP 2>/dev/null
if grep -qE 'AddressSanitizer|runtime error' "$LOG" "$LOG2"; then
    fail=$((fail + 1)); echo "FAIL sanitizer: 기대 오류 없음, 실제 서버 로그에 있음" >&2
    grep -E 'AddressSanitizer|runtime error' "$LOG" "$LOG2" >&2
fi
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
