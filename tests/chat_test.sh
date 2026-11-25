#!/bin/bash
cd "$(dirname "$0")/.." || exit 1
TMP=$(mktemp -d)
LOG=$TMP/server.log
pass=0
fail=0
cleanup() {
    kill $(jobs -p) 2>/dev/null
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

check() {
    if [ "$2" == "$3" ]; then pass=$((pass + 1))
    else fail=$((fail + 1)); echo "FAIL $1: 기대 $2, 실제 $3" >&2; fi
}
waitport() {
    local _
    for _ in $(seq 100); do
        (exec 3<>/dev/tcp/127.0.0.1/$1) 2>/dev/null && return 0
        kill -0 "$2" 2>/dev/null || break
        sleep 0.1
    done
    echo "FAIL start: 포트 $1이 열리지 않음" >&2
    cat "$LOG" >&2
    exit 1
}
for p in 8080 9001 8004 8009; do
    if (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null; then
        echo "FAIL start: 포트 $p가 이미 열려 있음" >&2; exit 1
    fi
done
python3 -B tests/chat_cases.py backend 8004 </dev/null &
BP=$!
$CHATD_WRAP ./chatd 9001 </dev/null 2>"$TMP/chatd.log" &
CP=$!
mkdir "$TMP/www" || exit 1
$SERVER_WRAP ./server 8080 "$TMP/www" /t/=127.0.0.1:8004 /dead/=127.0.0.1:8009 /ws/=127.0.0.1:9001 /=127.0.0.1:9001 </dev/null 2>"$LOG" &
SP=$!
waitport 8004 $BP
waitport 8080 $SP
waitport 9001 $CP
cases=(handshake echo closes tunnel_stats chat_stats)
if [ -z "$SERVER_WRAP" ]; then cases+=(noread timers); fi
cases+=(chat_http chat_messages chat_matching chat_departures chat_invalid chat_echo_bot)
cases+=(chat_images chat_image_invalid chat_image_departure chat_pressure chat_fd_limit)
if [ -z "$CHATD_WRAP" ]; then cases+=(chat_stalled chat_clocks); fi
if (( $# )); then cases=("$@"); fi
for name in "${cases[@]}"; do
    python3 -B tests/chat_cases.py "$name" "$SP"
    check "$name" 0 $?
done
kill "$SP" "$CP"
wait "$SP" "$CP" 2>/dev/null
if grep -qE 'AddressSanitizer|runtime error' "$LOG" "$TMP/chatd.log"; then
    fail=$((fail + 1))
    grep -E 'AddressSanitizer|runtime error' "$LOG" "$TMP/chatd.log" >&2
fi
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
