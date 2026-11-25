#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")/.."
ulimit -n 65536
mkdir -p /tmp/chat
pids=()
cleanup() {
    if (( ${#pids[@]} )); then
        kill "${pids[@]}" 2>/dev/null || true
        wait "${pids[@]}" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
./chatd 9001 >/tmp/chat/chatd.log 2>&1 &
pids+=("$!")
./server 8080 www /ws/=127.0.0.1:9001 >/tmp/chat/server.log 2>&1 &
pids+=("$!")
wait -n
