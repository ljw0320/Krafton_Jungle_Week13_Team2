"""Collect per-recipient observations from the fixed ten-client scenario."""
from contextlib import ExitStack
import json
import math
from pathlib import Path
from queue import Empty, Queue
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from chat_bot import Client


BOTS = [f"bot-{number:02d}" for number in range(1, 11)]
MESSAGES = [f"[{sequence:02d}] {BOTS[(sequence - 1) % len(BOTS)]}의 시험 메시지"
            for sequence in range(1, 31)]
SEQUENCES = {text.encode(): sequence for sequence, text in enumerate(MESSAGES, 1)}


def read_bot(bot, client, inbox, finished):
    while not finished.is_set():
        try:
            op, data = client.recv(.2)
        except TimeoutError:
            continue
        except (EOFError, OSError, ValueError):
            inbox.put((bot, None, time.monotonic()))
            return
        if op == 10:
            continue
        inbox.put((bot, data if op == 1 else None, time.monotonic()))
        if op != 1:
            return


def run_bots(addr, processes, count, stop, sides, sent, seen, observations):
    clients = {}
    inbox = Queue()
    finished = threading.Event()
    readers = []

    def receive_until(deadline):
        while not stop.is_set():
            for name, process in processes.items():
                code = process.poll()
                if code is not None:
                    raise RuntimeError(f"{name} 프로세스가 종료됐습니다 ({code}).")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            try:
                bot, data, received_at = inbox.get(timeout=min(.1, remaining))
            except Empty:
                continue
            if data is None:
                raise RuntimeError(f"{bot} WebSocket 연결이 종료됐습니다.")
            sequence = SEQUENCES.get(data)
            if sequence not in sent:
                raise RuntimeError(f"{bot}이 예상하지 않은 메시지를 받았습니다.")
            observations.append((bot, sequence, received_at - sent[sequence]))
            seen[bot].append(sequence)
        raise RuntimeError("시험이 중단됐습니다.")

    with ExitStack() as stack:
        try:
            for bot in BOTS:
                if stop.is_set():
                    raise RuntimeError("시험이 중단됐습니다.")
                client = Client(addr, "/ws/live")
                clients[bot] = client
                stack.callback(client.close)
                op, data = client.recv(2)
                greeting = json.loads(data) if op == 1 else None
                label = greeting.get("backend") if isinstance(greeting, dict) else None
                if label not in tuple("AB"[:count]):
                    raise RuntimeError(f"{bot}의 백엔드 배정을 확인하지 못했습니다.")
                sides[bot] = label
                reader = threading.Thread(target=read_bot, args=(bot, client, inbox, finished))
                reader.start()
                readers.append(reader)

            start = time.monotonic()
            for sequence, text in enumerate(MESSAGES, 1):
                receive_until(start + sequence * .5)
                bot = BOTS[(sequence - 1) % len(BOTS)]
                sent[sequence] = time.monotonic()
                clients[bot].send_text(text)

            # Allow all recipients to receive the last send and report late duplicates.
            receive_until(start + 17)
        finally:
            finished.set()
            for reader in readers:
                reader.join()


def compare(addr, processes, count, stop):
    sides, sent = {}, {}
    seen = {bot: [] for bot in BOTS}
    observations = []
    error = None
    try:
        run_bots(addr, processes, count, stop, sides, sent, seen, observations)
    except (EOFError, OSError, RuntimeError, ValueError) as caught:
        error = str(caught) or "봇 메시지 수신 시간이 초과됐습니다."

    assignments = {label: [bot for bot in BOTS if sides.get(bot) == label] for label in "AB"}
    expected = len(BOTS) * len(MESSAGES)
    unique = sum(len(set(sequences)) for sequences in seen.values())
    received = len(observations)
    missing, duplicates = expected - unique, received - unique
    out_of_order = 0
    for sequences in seen.values():
        highest = 0
        for sequence in dict.fromkeys(sequences):
            out_of_order += sequence < highest
            highest = max(sequence, highest)

    cross = {"A_to_B": 0, "B_to_A": 0}
    for bot, sequences in seen.items():
        for sequence in set(sequences):
            source = sides[BOTS[(sequence - 1) % len(BOTS)]]
            target = sides[bot]
            if source != target:
                cross[f"{source}_to_{target}"] += 1
    latencies = sorted(delay * 1000 for _, _, delay in observations)

    def percentile(fraction):
        return round(latencies[math.ceil(len(latencies) * fraction) - 1], 3) if latencies else None

    recent = []
    for sequence in list(sent)[-5:]:
        bot = BOTS[(sequence - 1) % len(BOTS)]
        recent.append({"sequence": sequence, "text": MESSAGES[sequence - 1], "from": bot,
                       "backend": sides[bot],
                       "received_by": [recipient for recipient in BOTS if sequence in seen[recipient]]})

    expected_a = 10 // count
    if not error:
        if len(assignments["A"]) != expected_a or len(assignments["B"]) != 10 - expected_a:
            error = "백엔드별 실제 연결 수가 예상과 다릅니다."
        elif missing or duplicates or out_of_order:
            error = "봇 수신에서 누락, 중복 또는 순서 불일치가 발생했습니다."
        elif count == 2 and cross != {"A_to_B": 75, "B_to_A": 75}:
            error = "양방향 백엔드 간 수신을 확인하지 못했습니다."
    return ({"assignments": assignments, "expected": expected, "received": received,
             "missing": missing, "duplicates": duplicates, "out_of_order": out_of_order,
             "p50_ms": percentile(.5), "p95_ms": percentile(.95),
             "cross_backend": cross, "recent": recent}, error)
