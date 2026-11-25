#!/bin/bash
cd "$(dirname "$0")/.." || exit 1
if [[ ${1:-} == load ]]; then ulimit -n 65536 || exit 1; fi
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
export SERVER_PID=$SP CHATD_PID=$CP
bot() { python3 -B tests/chat_bot.py "$@"; }
load_suite() {
    bot load --users 5000 --rate 1 --sec 60 &&
    bot burst --users 5000 --n 20 --rounds 3 &&
    bot flood --users 200 --sec 30 &&
    bot slow --users 200 --witness 100 &&
    bot image --users 200 &&
    bot churn --users 1000 --sec 30 &&
    python3 -B tests/chat_cases.py full
}
short_suite() {
    bot load --users 20 --rate 1 --sec 10 &&
    bot burst --users 20 --n 20 --rounds 1 &&
    bot flood --users 20 --sec 5 &&
    bot image --users 20 &&
    bot slow --users 20 --witness 10 &&
    bot churn --users 20 --sec 5
}
cases=(handshake echo closes unwatch tunnel_stats chat_stats)
if [ -z "$SERVER_WRAP" ]; then cases+=(noread quiet_stall timers); fi
cases+=(chat_http chat_messages chat_matching chat_departures chat_invalid chat_echo_bot)
cases+=(chat_images chat_upload chat_image_invalid chat_image_departure chat_pressure)
cases+=(chat_same_batch chat_fd_limit chat_killed)
if [ -z "$CHATD_WRAP" ]; then cases+=(chat_stalled chat_clocks); fi
cases+=(chat_bot_drop short)
if (( $# )); then cases=("$@"); fi
for name in "${cases[@]}"; do
    case "$name" in
        short) short_suite ;;
        load) load_suite ;;
        *) python3 -B tests/chat_cases.py "$name" "$SP" ;;
    esac
    check "$name" 0 $?
done
for p in "$SP" "$CP"; do
    kill -0 "$p" 2>/dev/null
    check "alive $p" 0 $?
done
kill "$SP" "$CP"
wait "$SP" "$CP" 2>/dev/null
if grep -qE 'AddressSanitizer|runtime error' "$LOG" "$TMP/chatd.log"; then
    fail=$((fail + 1))
    grep -E 'AddressSanitizer|runtime error' "$LOG" "$TMP/chatd.log" >&2
fi
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
