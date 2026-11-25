"""터널 기능 시험과 원시 소켓 백엔드."""
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
from contextlib import closing, contextmanager
from hashlib import sha256
import asyncio
import os
import json
from http.client import HTTPConnection
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory
import threading
import time
from types import SimpleNamespace

from backend import body, resp

MB = 1 << 20
UPGRADE = resp(101, "Switching Protocols", [("Connection", "Upgrade"), ("Upgrade", "websocket")])
REQUEST = b"GET /t/?%s HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n"
FIRST = b"\x81\x01A"


def read_head(s):
    data = bytearray()
    while b"\r\n\r\n" not in data:
        chunk = s.recv(8192)
        if not chunk:
            raise EOFError("헤더 전 연결 종료")
        data.extend(chunk)
    head, _, rest = data.partition(b"\r\n\r\n")
    return bytes(head), bytes(rest)


def reset(s):
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))


def handle(s):
    with s:
        try:
            head, rest = read_head(s)
            name = head.split(b" ")[1].split(b"?")[-1]
            if name == b"503":
                s.sendall(resp(503, "Service Unavailable", [("Content-Length", 4)], b"busy"))
                return
            if name == b"101h10":
                s.sendall(UPGRADE.replace(b"HTTP/1.1", b"HTTP/1.0"))
                return
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.sendall(UPGRADE + (FIRST if name == b"first" else b""))
            if name == b"close":
                s.sendall(body(4096))
                return
            if name == b"rst":
                s.recv(1)
                reset(s)
                return
            if name == b"fillrst":
                s.settimeout(.5)
                try:
                    while True:
                        s.sendall(body(65536))
                except TimeoutError:
                    pass
                s.settimeout(None)
                s.recv(1)
                reset(s)
                return
            if name == b"noread":
                time.sleep(40)
                return
            if name == b"late":
                time.sleep(15)
            if name in (b"bigdown", b"late"):
                s.sendall(body(8 * MB))
                return
            if rest:
                s.sendall(rest)
            while chunk := s.recv(65536):
                s.sendall(chunk)
        except (OSError, EOFError):
            pass


def backend(port):
    with socket.create_server(("127.0.0.1", port), backlog=128) as listener:
        while True:
            s, _ = listener.accept()
            threading.Thread(target=handle, args=(s,), daemon=True).start()


def connect(small=False):
    s = socket.socket()
    s.settimeout(30 if os.getenv("SERVER_WRAP") else 8)
    if small:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    s.connect(("127.0.0.1", 8080))
    return s


def upgrade(s, name, early=b""):
    s.sendall(REQUEST % name.encode() + early)
    head, rest = read_head(s)
    assert head.split(b" ")[1] == b"101", head
    return rest


def receive(s, n, rest=b""):
    data = bytearray(rest)
    while len(data) < n:
        chunk = s.recv(min(n - len(data), 65536))
        assert chunk, ("early EOF", len(data), n)
        data.extend(chunk)
    return data


def echo():
    with connect() as s, ThreadPoolExecutor(1) as pool:
        assert not upgrade(s, "echo")
        def send_chunks(payload):
            for offset in range(0, len(payload), 16384):
                s.sendall(payload[offset:offset + 16384])

        for size in (1, 8191, 8192, 8193, 16384, MB, 4 * MB):
            payload = body(size)
            sender = pool.submit(send_chunks, payload)
            assert sha256(receive(s, size)).digest() == sha256(payload).digest(), size
            sender.result()
        for b in body(1000):
            byte = bytes([b])
            s.sendall(byte)
            assert s.recv(1) == byte


def handshake():
    with connect() as s:
        rest = upgrade(s, "first", early=b"\x00early")
        assert receive(s, len(FIRST) + 6, rest) == FIRST + b"\x00early"
    with connect() as s:
        s.sendall(REQUEST % b"503")
        head, rest = read_head(s)
        assert head.split(b" ")[1] == b"503" and receive(s, 4, rest) == b"busy"
        assert not upgrade(s, "echo")
    for request in (REQUEST % b"101h10", (REQUEST % b"echo").replace(b"/t/", b"/dead/"),
                    b"GET /t/?echo HTTP/1.1\r\nHost: localhost\r\n\r\n"):
        with connect() as s:
            s.sendall(request)
            assert read_head(s)[0].split(b" ")[1] == b"502"


@contextmanager
def stopped(pid):
    try:
        os.kill(pid, signal.SIGSTOP)
        while Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0] != "T":
            time.sleep(.01)
        yield
    finally:
        os.kill(pid, signal.SIGCONT)


def closes(pid):
    def fds():
        return len(os.listdir(f"/proc/{pid}/fd"))

    quiet = lambda s: s["conns"] == 1 and s["tunnels"] == s["pending"] == 0
    wait_stats("/_stat", quiet)
    time.sleep(.5)  # 방금 조회한 연결까지 닫힌 뒤의 수를 기준으로 삼는다.
    baseline = fds()
    with connect() as s:
        rest = upgrade(s, "close")
        assert receive(s, 4096, rest) == body(4096)
        assert s.recv(1) == b""
    # 서버가 멈춘 동안 보낼 자리와 FIN을 함께 만들어 한 이벤트에 IN과 OUT이 온다.
    with connect(small=True) as s:
        upgrade(s, "bigdown")
        wait_stats("/_stat", lambda state: state["pending"] > 0)
        with stopped(pid):
            s.shutdown(socket.SHUT_WR)
            s.settimeout(.5)
            try:
                while s.recv(65536):
                    pass
            except TimeoutError:
                pass
        s.settimeout(3)
        try:
            while s.recv(65536):
                pass
        except ConnectionResetError:
            pass
    with ExitStack() as stack:
        sockets = [stack.enter_context(connect()) for _ in range(100)]
        for i, s in enumerate(sockets):
            upgrade(s, "rst" if i % 2 else "echo")

        def finish(pair):
            i, s = pair
            if i % 2:
                s.sendall(b"x")
                try:
                    assert s.recv(1) == b""
                except ConnectionResetError:
                    pass
            elif i % 4 == 0:
                reset(s)
            s.close()

        with ThreadPoolExecutor(32) as pool:
            list(pool.map(finish, enumerate(sockets)))
    for _ in range(100):
        if fds() == baseline:
            break
        time.sleep(.05)
    else:
        raise AssertionError(("server fds", baseline, fds()))
    # 같은 연결을 두 번 닫으면 conns가 어긋난다.
    wait_stats("/_stat", quiet)


def assert_idle(pid):
    from chat_bot import cpu_seconds

    used = cpu_seconds(pid)
    time.sleep(1)
    used = cpu_seconds(pid) - used
    assert used < .2, ("busy loop", used)


def unwatch(pid):
    with connect(small=True) as s:
        upgrade(s, "fillrst")
        wait_stats("/_stat", lambda state: state["pending"] > 0)
        assert_idle(pid)
        s.sendall(b"x")
        time.sleep(.5)
        assert stats()["tunnels"] == 1
        assert_idle(pid)


def noread(pid):
    def rss():
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
        raise AssertionError("VmRSS missing")

    with connect() as s:
        upgrade(s, "noread")
        s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 16384)
        s.settimeout(.5)
        block = body(65536)
        initial = rss()
        try:
            for _ in range(1024):
                s.sendall(block)
        except socket.timeout:
            assert rss() - initial < 1024, (initial, rss())
            assert_idle(pid)
            reset(s)
            return
        raise AssertionError("64 MiB sent without backpressure")


def quiet_stall():
    """조용하던 터널의 첫 전송이 EAGAIN이면 그때부터 10초를 센다(up.since)."""
    def push(s, n):
        before = stats()
        s.sendall(bytes(n))
        goal = before["down"]["bytes"] + before["pending"] + n
        state = wait_stats("/_stat", lambda now: now["down"]["bytes"] + now["pending"] == goal)
        return state["pending"] > before["pending"]

    @contextmanager
    def tunnel():
        with connect(small=True) as s:
            upgrade(s, "echo")
            yield s
        wait_stats("/_stat", lambda state: state["tunnels"] == 0)

    def fill(s, chunks, count):
        return any(push(s, 2048) for _ in range(chunks)) or any(push(s, 1) for _ in range(count))

    # 송신 버퍼가 정확히 차는 양은 커널이 정한다. 먼저 재고, 같은 순서로 다시 채운다.
    with tunnel() as s:
        chunks = 0
        while not push(s, 2048):
            chunks += 1
    chunks -= 2
    with tunnel() as s:
        assert not fill(s, chunks, 0)
        count = 0
        while not push(s, 1):
            count += 1
    for _ in range(3):
        with ExitStack() as stack:
            quiet = []
            for extra in range(-1, 4):
                s = stack.enter_context(connect(small=True))
                upgrade(s, "echo")
                if fill(s, chunks, count + extra):
                    s.close()
                    wait_stats("/_stat", lambda state: state["tunnels"] == len(quiet))
                else:
                    quiet.append(s)
            time.sleep(5)
            blocked = [push(s, 1) for s in quiet]
            if any(blocked):
                time.sleep(7.5)
                assert stats()["tunnels"] == len(quiet), "stalled tunnel closed early"
                return
        wait_stats("/_stat", lambda state: state["tunnels"] == 0)
    raise AssertionError("send buffer never filled exactly")


def slow():
    with connect(small=True) as s:
        data = bytearray(upgrade(s, "bigdown"))
        start = time.time()
        while time.time() - start < 12:
            time.sleep(.5)
            chunk = s.recv(8192)
            assert chunk
            data.extend(chunk)
            if len(data) == len(chunk) or time.time() - start < 1.5:
                t = time.time()
                with connect() as other:
                    upgrade(other, "echo")
                    other.sendall(b"x")
                    assert other.recv(1) == b"x"
                assert time.time() - t < 1
        assert sha256(receive(s, 8 * MB, data)).digest() == sha256(body(8 * MB)).digest()


def timer(name, delay, timeout):
    with connect(small=True) as s:
        upgrade(s, name)
        start = time.time()
        # 클라이언트를 읽지 않고 서버 쪽 연결이 ESTABLISHED에서 빠지는 시점을 잰다.
        peer = f"0100007F:{s.getsockname()[1]:04X}"
        while time.time() - start < delay + timeout + 4:
            rows = Path("/proc/net/tcp").read_text().splitlines()[1:]
            if not any(row.split()[1:4] == ["0100007F:1F90", peer, "01"] for row in rows):
                break
            time.sleep(.05)
        elapsed = time.time() - start - delay
        assert timeout <= elapsed < timeout + 4, (name, elapsed)


CHAT = ("127.0.0.1", 8080)
DIRECT = ("127.0.0.1", 9001)
KEY = b"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
VERSION = b"Sec-WebSocket-Version: 13\r\n"
CHAT_REQ = (b"GET /ws/chat?x HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\n"
            b"Upgrade: websocket\r\n" + KEY + VERSION + b"\r\n")


def chat_http():
    from chat_bot import frame
    plain = b"GET /ws/stat HTTP/1.1\r\nHost: localhost\r\n\r\n"
    for request, status, header in [
        (plain, 200, b"content-type: application/json"),
        (plain.replace(b"GET", b"POST").replace(b"/ws/stat", b"/bad"), 405, b"allow: get"),
        (plain.replace(b"/ws/stat", b"/ws/statue"), 404, b""),
        (plain.replace(b"/ws/stat", b"/ws/chat"), 400, b""),
        (CHAT_REQ.replace(KEY, b"").replace(VERSION, b""), 400, b""),
        (CHAT_REQ.replace(KEY, KEY * 2), 400, b""),
        (CHAT_REQ.replace(b"Q==", b"Q="), 400, b""),
        (CHAT_REQ.replace(b": 13", b": 12"), 426, b"sec-websocket-version: 13"),
        (CHAT_REQ.replace(VERSION, b""), 426, b"sec-websocket-version: 13"),
        (b"bad\r\n\r\n", 400, b""),
        (plain[:-2] + b"X: " + b"x" * 8192 + b"\r\n\r\n", 431, b""),
    ]:
        with socket.create_connection(DIRECT, 3) as s:
            s.sendall(request)
            head, rest = read_head(s)
            assert int(head.split()[1]) == status, (head, status)
            assert header in head.lower() and b"connection: close" in head.lower(), head
            length, = [int(h.split(b":")[1]) for h in head.lower().splitlines()
                       if h.startswith(b"content-length:")]
            data = receive(s, length, rest)
            if status == 200:
                assert isinstance(json.loads(data), dict)
    with_body = CHAT_REQ[:-2] + b"Content-Length: 4\r\n\r\nbody"
    for request, status in [(CHAT_REQ.replace(b"websocket", b"h2c"), 400),
                            (CHAT_REQ.replace(b"HTTP/1.1", b"HTTP/1.0"), 400),
                            (with_body, 400), (with_body.replace(b"GET", b"POST"), 405)]:
        with connect() as s:
            s.sendall(request)
            head, _ = read_head(s)
            # chatd의 오류에는 본문과 Content-Type이 없다.
            assert int(head.split()[1]) == status and b"content-type" not in head.lower(), (head, status)
    for early in (False, True):
        with socket.create_connection(DIRECT, 3) as s:
            ping = frame(9, b"x")
            if early:
                s.sendall(CHAT_REQ + ping)
            else:
                for byte in CHAT_REQ:
                    s.sendall(bytes([byte]))
                s.sendall(ping)
            head, rest = read_head(s)
            assert head.split()[1] == b"101" and b"s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" in head
            assert receive(s, 6, rest) == b"\x81\x01w\x8a\x01x"


def status_request(path, method="GET", addr=CHAT):
    conn = HTTPConnection(*addr, timeout=3)
    with closing(conn):
        conn.request(method, path)
        res = conn.getresponse()
        data = res.read()
        return res.status, res.headers, data


def stats(path="/_stat", addr=CHAT):
    status, headers, data = status_request(path, addr=addr)
    assert status == 200 and headers["Content-Type"] == "application/json", (status, headers)
    return json.loads(data)


def wait_stats(path, ready, addr=CHAT):
    deadline = time.monotonic() + 3
    while True:
        state = stats(path, addr)
        if ready(state):
            return state
        assert time.monotonic() < deadline, state
        time.sleep(.002)


def tunnel_stats():
    before = wait_stats("/_stat", lambda s: s["tunnels"] == s["pending"] == 0)
    assert before.keys() == {"now_us", "conns", "tunnels", "up", "down", "pending", "rss_kb", "events"}
    assert before["rss_kb"] > 0
    assert before["up"].keys() == before["down"].keys() == {"n", "bytes"}
    status, headers, _ = status_request("/_stat", "POST")
    assert status == 405 and headers["Allow"] == "GET"
    for path in ("/_stat?x", "/_stat/"):
        assert status_request(path)[0] == 404
    with closing(connect()) as a, closing(connect()) as b:
        assert receive(a, len(FIRST), upgrade(a, "first")) == FIRST
        assert not upgrade(b, "count")
        expected = 0
        for sock in (a, b):
            for i in range(10):
                data = bytes([i]) * (i + 1)
                sock.sendall(data)
                assert receive(sock, len(data)) == data
                expected += len(data)
        during = stats()
        assert during["tunnels"] == 2 and during["conns"] >= 3 and during["pending"] == 0, during
        assert during["up"]["bytes"] - before["up"]["bytes"] == expected, during
        assert during["down"]["bytes"] - before["down"]["bytes"] == expected + len(FIRST), during
        for direction in ("up", "down"):
            assert during[direction]["n"] > before[direction]["n"], during
    wait_stats("/_stat", lambda s: s["tunnels"] == s["pending"] == 0)
    with closing(connect(small=True)) as slow:
        upgrade(slow, "bigdown")
        wait_stats("/_stat", lambda s: s["tunnels"] == 1 and s["pending"] > 0)
        reset(slow)
    wait_stats("/_stat", lambda s: s["tunnels"] == s["pending"] == 0)


def chat_stats():
    from chat_bot import Client

    path = "/ws/stat"
    before = wait_stats(path, lambda s: s["users"] == 0)
    assert before.keys() == {"now_us", "users", "waiting", "rooms", "msgs", "img_bytes",
                             "paused", "queued", "slow_closed", "rejected", "rss_kb", "events"}
    assert before["rss_kb"] > 0
    with closing(Client(CHAT, "/ws/chat?trace=7")) as a:
        assert next_frame(a) == (1, b"w")
        waiting = stats(path)
        assert (waiting["users"], waiting["waiting"], waiting["rooms"]) == (1, 1, 0)
        with closing(Client(CHAT, "/ws/chat")) as b:
            assert next_frame(b) == (1, b"w")
            assert next_frame(a) == next_frame(b) == (1, b"c")
            srv_before, chat_before = stats(), stats(path)
            a.send_text("mhello")
            assert b.recv(1) == (1, b"mhello") and a.recv(1) == (1, b"a")
            srv, chat = stats(), stats(path)
            assert (chat["users"], chat["waiting"], chat["rooms"]) == (2, 0, 1)
            assert chat["msgs"] == before["msgs"] + 1
            old_srv = {e["seq"] for e in srv_before["events"]}
            old_chat = {e["seq"] for e in chat_before["events"]}
            server_events = [e for e in srv["events"] if e["seq"] not in old_srv]
            chat_events = [e for e in chat["events"] if e["seq"] not in old_chat]
            assert all(e["trace"] == 7 for e in server_events + chat_events)
            assert [e["stage"] for e in server_events] == ["A>S", "S>B", "B>S", "S>A"]
            assert [(e["stage"], e["kind"], e["bytes"]) for e in chat_events] == [
                ("recv", "m", 6), ("send", "a", 1)]
            timeline = server_events[:2] + chat_events + server_events[2:]
            times = [e["t_us"] for e in timeline]
            assert times == sorted(times), timeline
            a.send_text("ipng 3")
            a.send_bin(b"abc")
            assert b.recv(1) == (1, b"ipng 3") and b.recv(1) == (2, b"abc")
            assert stats(path)["img_bytes"] == before["img_bytes"] + 3
            for _ in range(100):
                a.send_text("mhello")
                assert b.recv(1) == (1, b"mhello") and a.recv(1) == (1, b"a")
            srv_ring, chat_ring = stats(), stats(path)
            for snapshot, prev in ((srv_ring, srv), (chat_ring, chat)):
                seq = [e["seq"] for e in snapshot["events"]]
                assert len(seq) == 64 and seq == list(range(seq[0], seq[0] + 64)), seq
                assert seq[0] > prev["events"][-1]["seq"], seq
            for i in range(16):
                steps = srv_ring["events"][i * 4:i * 4 + 4]
                parsed, ack = chat_ring["events"][32 + i * 2:34 + i * 2]
                assert [e["stage"] for e in steps] == ["A>S", "S>B", "B>S", "S>A"], steps
                assert (parsed["stage"], parsed["kind"], ack["stage"], ack["kind"]) == ("recv", "m", "send", "a")
                timeline = steps[:2] + [parsed, ack] + steps[2:]
                times = [e["t_us"] for e in timeline]
                assert times == sorted(times), timeline
            assert chat_ring["msgs"] == before["msgs"] + 101
    wait_stats(path, lambda s: s["users"] == s["rooms"] == 0)


def next_frame(client, timeout=1.5):
    deadline = time.monotonic() + timeout
    while True:
        item = client.recv(max(0.001, deadline - time.monotonic()))
        if item[0] != 1 or not item[1].startswith(b"s"):
            return item


@contextmanager
def group(n, addr=CHAT, rcvbuf=0):
    from chat_bot import Client
    with ExitStack() as stack:
        clients = [stack.enter_context(closing(Client(addr, "/ws/chat", rcvbuf))) for _ in range(n)]
        for client in clients:
            assert next_frame(client) == (1, b"w")
        yield clients


@contextmanager
def pair(addr=CHAT, rcvbuf=0):
    with group(2, addr, rcvbuf) as clients:
        for client in clients:
            assert next_frame(client) == (1, b"c")
        yield clients


def chat_messages():
    from chat_bot import frame
    with pair() as (a, b):
        msg = "m안녕".encode()
        for byte in frame(1, msg):
            a.send_raw(bytes([byte]))
        assert b.recv(1) == (1, msg) and a.recv(1) == (1, b"a")
        messages = [f"m{i}".encode() for i in range(100)]
        a.send_raw(b"".join(frame(1, m) for m in messages))
        for expected in messages:
            assert b.recv(1) == (1, expected) and a.recv(1) == (1, b"a")
        a.send_raw(frame(1, msg[:2], fin=False) + frame(9, b"ping"))
        assert a.recv(1) == (10, b"ping")
        a.send_raw(frame(0, msg[2:]))
        assert b.recv(1) == (1, msg) and a.recv(1) == (1, b"a")
        maximum = b"m" + b"X" * 4095
        for raw in (frame(1, maximum), frame(1, maximum[:7], fin=False) + frame(0, maximum[7:])):
            a.send_raw(raw)
            assert b.recv(1) == (1, maximum) and a.recv(1) == (1, b"a")
    for op, start in ((1, b"mold"), (1, b"i"), (2, b"old")):
        with group(1) as (a,):
            a.send_raw(frame(op, start, fin=False) + frame(9, b"before"))
            assert next_frame(a) == (10, b"before")
            with group(1) as (b,):
                assert next_frame(a) == (1, b"c") and next_frame(b) == (1, b"c")
                a.send_raw(frame(0, b"end") + frame(1, b"mnew") + frame(9, b"after"))
                assert b.recv(1) == (1, b"mnew")
                assert a.recv(1) == (1, b"a") and a.recv(1) == (10, b"after")


def chat_matching():
    variations = set()
    for _ in range(10):
        with group(6) as clients:
            for i, c in enumerate(clients):
                assert next_frame(c) == (1, b"c")
                c.send_text(f"m{i}")
            peers = []
            for c in clients:
                items = [c.recv(1), c.recv(1)]
                items.remove((1, b"a"))
                (op, msg), = items
                assert op == 1 and msg.startswith(b"m")
                peers.append(int(msg[1:]))
            assert all(j != i and peers[j] == i for i, j in enumerate(peers))
            variations.add(tuple(peers))
    assert len(variations) > 1
    with group(3) as clients:
        def after_match(c):
            end = time.monotonic() + 1.5
            while True:
                item = c.recv(max(.001, end - time.monotonic()))
                if item in ((1, b"c"), (1, b"s3 1")):
                    return item[1]

        with ThreadPoolExecutor(3) as pool:
            first = list(pool.map(after_match, clients))
        assert sorted(first) == [b"c", b"c", b"s3 1"], first
        last = clients[first.index(b"s3 1")]
        start = time.monotonic()
        assert last.recv(1.5) == (1, b"s3 1")
        assert .5 < time.monotonic() - start < 1.5


def chat_departures():
    from chat_bot import Client, frame
    for method in ("next", "tcp", "rst", "close"):
        with pair() as (a, b):
            if method == "next":
                a.send_text("n")
                assert next_frame(a) == (1, b"w")
            elif method == "close":
                a.send_raw(frame(8, b"\x03\xe8bye"))
                assert a.recv(1) == (8, b"\x03\xe8")
            else:
                if method == "rst":
                    reset(a.sock)
                a.close()
            assert b.recv(1) == (1, b"l")
            b.send_text("mold")
            b.send_text("n")
            assert next_frame(b) == (1, b"w")
            with ExitStack() as stack:
                other = a
                if method != "next":
                    other = stack.enter_context(closing(Client(CHAT, "/ws/chat")))
                    assert next_frame(other) == (1, b"w")
                assert next_frame(other) == (1, b"c") and next_frame(b) == (1, b"c")
                b.send_text("mnew")
                assert other.recv(1) == (1, b"mnew")
    with group(1) as (departed,):
        departed.close()
    with group(1) as (last,):
        last.send_text("n")
        last.send_text("m")
        last.send_text("i")
        last.send_bin(b"ignored")
        assert last.recv(1.5) == (1, b"s1 1")


def chat_invalid():
    from chat_bot import frame
    wrong = [
        (b"\x81\x00", 1002), (b"\xc1\x80", 1002), (b"\x83\x80", 1002),
        (b"\x81\xfe\x00\x7d", 1002), (b"\x09\x80", 1002),
        (b"\x89\xfe\x00\x7e", 1002), (b"\x82\xfe\x40\x01", 1009),
        (frame(0, b"x"), 1002), (frame(1, b"m", fin=False) + frame(1, b"x"), 1002),
        (frame(8, b"x"), 1002), (frame(1, b""), 1008), (frame(1, b"q"), 1008),
        (frame(1, b"n!"), 1008), (frame(1, b"m"), 1008),
        (frame(1, b"i?"), 1008), (frame(2, b"undeclared"), 1008),
        (frame(1, b"m" * 4097), 1009),
        (frame(1, b"m" * 4096, fin=False) + frame(0, b"x"), 1009),
    ]
    wrong.extend((frame(1, b"m" + data), 1007) for data in
                 (b"\xc2", b"\xed\xa0\x80", b"\xc0\xaf", b"\xf4\x90\x80\x80"))
    for raw, code in wrong:
        with pair() as (a, b):
            a.send_raw(raw)
            assert a.recv(1) == (8, struct.pack("!H", code)), (raw[:8], code)
            assert b.recv(1) == (1, b"l")
    for raw in (b"", b"q", b"nx"):
        with group(1) as (a,):
            a.send_raw(frame(1, raw))
            assert next_frame(a) == (8, struct.pack("!H", 1008))
    with group(1) as (a,):
        a.send_raw(frame(8, b""))
        assert next_frame(a) == (8, b"")


def chat_images():
    from chat_bot import frame
    with pair() as (a, b):
        for kind, size in (("jpg", 1), ("png", 16384), ("jpg", 16385),
                           ("png", 2 * MB), ("jpg", 2 * MB)):
            header = f"i{kind} {size}".encode()
            a.send_raw(frame(1, header))
            assert b.recv(1) == (1, header)
            sent, received = sha256(), sha256()
            for offset in range(0, size, 16384):
                chunk = bytes([offset // 16384]) * min(size - offset, 16384)
                sent.update(chunk)
                split = len(chunk) // 2
                a.send_raw(frame(2, chunk[:split], fin=False) + frame(0, chunk[split:]))
                for part in (chunk[:split], chunk[split:]):
                    if part:
                        op, data = b.recv(2)
                        assert op == 2 and len(data) == len(part)
                        received.update(data)
                if offset == 0:
                    a.send_text("mhello")
                    assert b.recv(1) == (1, b"mhello") and a.recv(1) == (1, b"a")
            assert received.digest() == sent.digest()


def image_frames(payload):
    from chat_bot import frame
    return frame(1, f"ipng {len(payload)}".encode()) + b"".join(
        frame(2, payload[i:i + 16384]) for i in range(0, len(payload), 16384))


def chat_upload():
    payload = body(2 * MB)
    with pair() as (a, b), ThreadPoolExecutor(1) as pool:
        raw = image_frames(payload)
        start = time.monotonic()
        sender = pool.submit(a.send_raw, raw)
        assert b.recv(3) == (1, b"ipng 2097152")
        received = bytearray()
        while len(received) < len(payload):
            op, data = b.recv(3)
            assert op == 2
            received.extend(data)
            assert time.monotonic() - start < 3, len(received)
        sender.result()
        assert sha256(received).digest() == sha256(payload).digest()


def chat_image_invalid():
    from chat_bot import frame
    for tail in (b"png 2097153", b"jpg 0", b"gif 1", b"jpg -1", b"png 1x",
                 b"jpg 00000001", b"png "):
        with pair(DIRECT) as (a, b):
            a.send_raw(frame(1, b"i" + tail))
            assert a.recv(1) == (8, b"\x03\xf0") and b.recv(1) == (1, b"l")
    for invalid in (frame(1, b"ijpg 1"), frame(2, b"XX")):
        with pair(DIRECT) as (a, b):
            a.send_raw(frame(1, b"ipng 1") + invalid)
            assert b.recv(1) == (1, b"ipng 1") and b.recv(1) == (1, b"l")
            assert a.recv(1) == (8, b"\x03\xf0")


def chat_image_departure():
    from chat_bot import frame
    for leaver in ("sender", "peer"):
        with pair(DIRECT) as (a, b):
            for sender, receiver in ((a, b), (b, a)):
                sender.send_raw(frame(1, b"ipng 8") + frame(2, b"p"))
                assert receiver.recv(1) == (1, b"ipng 8") and receiver.recv(1) == (2, b"p")
            first, second = (a, b) if leaver == "sender" else (b, a)
            first.send_text("n")
            assert next_frame(first) == (1, b"w") and second.recv(1) == (1, b"l")
            second.send_raw(frame(2, b"old") + frame(1, b"n"))
            assert next_frame(second) == (1, b"w")
            for client in (a, b):
                assert next_frame(client) == (1, b"c")
            a.send_raw(frame(1, b"ijpg 1") + frame(2, b"x"))
            assert b.recv(1) == (1, b"ijpg 1") and b.recv(1) == (2, b"x")
    for leaver in ("sender", "peer"):
        with pair(DIRECT) as (a, b):
            a.send_raw(frame(1, b"ipng 9") + frame(2, b"p"))
            assert b.recv(1) == (1, b"ipng 9") and b.recv(1) == (2, b"p")
            first, second = (a, b) if leaver == "sender" else (b, a)
            if second is a:
                second.send_raw(frame(2, b"old"))
            reset(first.sock)
            first.close()
            if second is a:
                second.send_raw(frame(2, b"old"))
            assert second.recv(1) == (1, b"l")
            second.send_raw(frame(2, b"o") + frame(1, b"n"))
            assert next_frame(second) == (1, b"w")
            with group(1, DIRECT) as (fresh,):
                assert next_frame(fresh) == (1, b"c") and next_frame(second) == (1, b"c")
                for sender, receiver in ((second, fresh), (fresh, second)):
                    sender.send_raw(frame(1, b"ijpg 2") + frame(2, b"xy"))
                    assert receiver.recv(1) == (1, b"ijpg 2") and receiver.recv(1) == (2, b"xy")


def chat_pressure():
    from chat_bot import frame
    messages = [f"m{i:099}".encode() for i in range(5000)]
    flood = b"".join(frame(1, msg[:1], fin=False) + frame(0, msg[1:]) for msg in messages)
    with pair(DIRECT) as (a, b), pair(DIRECT) as (c, d):
        b.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)
        a.send_raw(flood)
        pressured = wait_stats("/ws/stat", lambda s: s["paused"] >= 1 and s["queued"] > 0)
        assert pressured["rooms"] == 2 and pressured["users"] == 4, pressured
        start = time.monotonic()
        c.send_text("mroom")
        assert d.recv(1) == (1, b"mroom") and c.recv(1) == (1, b"a")
        assert time.monotonic() - start < 1
        short = [f"m{i}".encode() for i in range(1000)]
        b.send_raw(b"".join(frame(1, msg) for msg in short))
        time.sleep(.1)
        def collect(client, expected, acks):
            forwarded, acknowledged = [], 0
            for _ in range(len(expected) + acks):
                op, data = client.recv(5)
                assert op == 1
                if data == b"a":
                    acknowledged += 1
                else:
                    forwarded.append(data)
            assert acknowledged == acks and forwarded == expected
        with ThreadPoolExecutor(2) as pool:
            first = pool.submit(collect, a, short, len(messages))
            collect(b, messages, len(short))
            first.result()
    with pair(DIRECT) as (a, b):
        b.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)
        a.send_raw(flood)
        assert a.recv(1) == (1, b"a")
        time.sleep(.1)
        reset(a.sock)
        a.close()
        for expected in messages:
            item = b.recv(3)
            if item == (1, b"l"):
                break
            assert item == (1, expected)
        else:
            raise AssertionError("all messages arrived before the blocked sender left")


def chat_same_batch():
    pid = int(os.environ["CHATD_PID"])
    zero = ("users", "rooms", "waiting", "paused", "queued")
    with pair(DIRECT) as (a, b), pair(DIRECT) as (c, d):
        a.send_raw(image_frames(body(MB)))
        wait_stats("/ws/stat", lambda s: s["paused"] == 1 and s["queued"] > 0)
        with stopped(pid):
            for client in (a, c, b, d):
                if client in (a, c):
                    reset(client.sock)
                client.close()
    wait_stats("/ws/stat", lambda s: not any(s[key] for key in zero))
    with pair(DIRECT) as (a, b):
        a.send_text("mnew")
        assert b.recv(1) == (1, b"mnew")


def free_addr():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()


def wait_open(addr):
    for _ in range(100):
        try:
            with socket.create_connection(addr, .1):
                return
        except ConnectionRefusedError:
            time.sleep(.02)


def chat_killed():
    from chat_bot import Client
    chat, proxy = free_addr(), free_addr()
    with TemporaryDirectory() as root, ExitStack() as stack:
        daemon = stack.enter_context(subprocess.Popen(["./chatd", str(chat[1])]))
        stack.callback(daemon.kill)
        server = stack.enter_context(subprocess.Popen(
            ["./server", str(proxy[1]), root, f"/ws/=127.0.0.1:{chat[1]}"], stderr=subprocess.DEVNULL))
        stack.callback(server.kill)
        wait_open(chat)
        wait_open(proxy)
        with pair(proxy) as clients:
            assert stats("/_stat", proxy)["tunnels"] == 2
            daemon.kill()
            daemon.wait()
            for client in clients:
                try:
                    item = next_frame(client)
                except (EOFError, ConnectionResetError):
                    continue
                raise AssertionError(("chat stayed open", item))
            try:
                Client(proxy, "/ws/chat").close()
            except ValueError as exc:
                assert "502" in str(exc), exc
            else:
                raise AssertionError("upgraded without chatd")
        wait_stats("/_stat", lambda s: s["tunnels"] == s["pending"] == 0, proxy)
        assert server.poll() is None


def established(client):
    peer = f"0100007F:{client.sock.getsockname()[1]:04X}"
    rows = [row.split() for row in Path("/proc/net/tcp").read_text().splitlines()[1:]]
    return any(r[1] == "0100007F:2329" and r[2] == peer and r[3] == "01" for r in rows)


def chat_stalled():
    from chat_bot import frame
    with pair(DIRECT) as clients:
        block = frame(1, b"m" * 4096) * 192
        for c in clients:
            c.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)
        start = time.monotonic()
        for c in clients:
            c.send_raw(block)
        while time.monotonic() - start < 16:
            if not all(map(established, clients)):
                assert time.monotonic() - start >= 10
                break
            time.sleep(.1)
        else:
            raise AssertionError("stalled queues stayed connected")
    wait_stats("/ws/stat", lambda s: s["users"] == 0)
    # 4초 먼저 막힌 쪽만 끊긴다. 이미지 조각은 a가 없어 자기 큐에 막히지 않는다.
    with pair(DIRECT, 4096) as (first, last):
        block = image_frames(body(MB))
        start = time.monotonic()
        last.send_raw(block)
        wait_stats("/ws/stat", lambda s: s["paused"] == 1)
        time.sleep(4)
        first.send_raw(block)
        wait_stats("/ws/stat", lambda s: s["paused"] == 2)
        while established(first):
            assert time.monotonic() - start < 13, "first stalled client stayed connected"
            assert established(last), "later client closed first"
            time.sleep(.1)
        assert time.monotonic() - start >= 10
        assert last.recv(1) == (1, b"ipng 1048576")
        while (item := last.recv(3))[0] == 2:
            pass
        assert item == (1, b"l"), item
        last.send_text("n")
        assert next_frame(last) == (1, b"w")


def chat_clocks():
    from chat_bot import frame
    def half():
        with socket.create_connection(DIRECT, 13) as s:
            start = time.monotonic()
            s.sendall(b"GET /ws")
            assert not s.recv(1) and 10 <= time.monotonic() - start < 13
    def heartbeat(respond):
        with socket.create_connection(DIRECT, 12) as s, s.makefile("rb") as f:
            start = time.monotonic()
            s.sendall(CHAT_REQ)
            assert b"101" in f.readline()
            while True:
                line = f.readline()
                assert line
                if line == b"\r\n":
                    break
            last = start
            pings = 0
            while True:
                head = f.read(2)
                if not head:
                    assert not respond and 30 <= time.monotonic() - start < 34
                    return
                op, length = head
                payload = f.read(length)
                if op == 0x89:
                    now = time.monotonic()
                    assert 9 <= now - last < 12
                    last = now
                    pings += 1
                    if respond:
                        s.sendall(frame(10, payload))
                        if pings >= 4 and now - start >= 40:
                            return
    with ThreadPoolExecutor(3) as pool:
        jobs = [pool.submit(half), pool.submit(heartbeat, False), pool.submit(heartbeat, True)]
        for job in jobs:
            job.result()


def chat_fd_limit():
    import resource
    from chat_bot import Client
    addr = free_addr()
    def limit():
        resource.setrlimit(resource.RLIMIT_NOFILE, (20, 20))
    with subprocess.Popen(["./chatd", str(addr[1])], preexec_fn=limit) as daemon, ExitStack() as stack:
        stack.callback(daemon.terminate)
        wait_open(addr)
        with pair(addr) as (a, b):
            pending = [stack.enter_context(socket.create_connection(addr, 2)) for _ in range(30)]
            a.send_text("mfd")
            assert b.recv(1) == (1, b"mfd")
            for s in pending:
                s.close()
            with closing(Client(addr, "/ws/chat")) as fresh:
                assert next_frame(fresh) == (1, b"w")


def chat_echo_bot():
    cmd = [sys.executable, "-B", str(Path(__file__).with_name("chat_bot.py")), "echo"]
    with subprocess.Popen(cmd) as bot:
        try:
            for _ in range(2):
                with group(1) as (c,):
                    assert next_frame(c) == (1, b"c")
                    start = time.monotonic()
                    c.send_text("m안녕")
                    assert c.recv(1) == (1, b"a")
                    op, reply = c.recv(1.5)
                    assert op == 1 and reply.startswith(b"m") and len(reply) > 1
                    assert time.monotonic() - start >= .28
        finally:
            bot.terminate()


def chat_bot_drop():
    result = subprocess.run([sys.executable, "-B", "tests/chat_bot.py", "load",
                             "--users", "20", "--sec", "3", "--self-test-drop"],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 1 and "sequence" in result.stderr, (
        result.returncode, result.stdout, result.stderr)


def full():
    from chat_bot import Bot, handshake, raise_fd_limit

    async def capacity():
        args = SimpleNamespace(mode="full", users=6000, addr=DIRECT, path="/ws/chat", self_test_drop=False)
        bots = [Bot(i, args) for i in range(args.users)]
        slots = asyncio.Semaphore(24)
        readers = []
        try:
            async with asyncio.timeout(90), asyncio.TaskGroup() as tasks:
                async def hold(bot):
                    async with slots:
                        await bot.open()
                    readers.append(tasks.create_task(bot.read()))
                    await bot.connected.wait()

                await asyncio.gather(*(hold(b) for b in bots))
                reader, writer = await asyncio.open_connection(*DIRECT)
                try:
                    request, _ = handshake(DIRECT, args.path)
                    writer.write(request)
                    header = await reader.readuntil(b"\r\n\r\n")
                    assert header.split()[:2] == [b"HTTP/1.1", b"503"], header
                finally:
                    writer.close()
                    await writer.wait_closed()
                state = await asyncio.to_thread(stats, "/ws/stat", DIRECT)
                assert state["users"] == 6000, state
                bots[0].messages(1)
                await bots[0].until(lambda: bots[0].acks == 1)
                while not (peer := next((b for b in bots if b.peer == 0), None)):
                    await asyncio.sleep(.01)
                peer.messages(1)
                await peer.until(lambda: peer.acks == 1)
                await bots[0].until(lambda: bots[0].received == 1)
                for reader_task in readers:
                    reader_task.cancel()
        finally:
            for b in bots:
                b.close()

    raise_fd_limit()
    asyncio.run(capacity())


if __name__ == "__main__":
    case = sys.argv[1]
    if case == "backend":
        backend(int(sys.argv[2]))
    elif case == "timers":
        with ThreadPoolExecutor(4) as pool:
            runs = [pool.submit(timer, "bigdown", 0, 10), pool.submit(timer, "late", 15, 10),
                    pool.submit(timer, "echo", 0, 30), pool.submit(slow)]
            for run in runs:
                run.result()
    elif case in ("closes", "noread", "unwatch"):
        {"closes": closes, "noread": noread, "unwatch": unwatch}[case](int(sys.argv[2]))
    else:
        {"full": full, "chat_bot_drop": chat_bot_drop, "handshake": handshake, "echo": echo, "chat_http": chat_http,
         "chat_messages": chat_messages, "chat_matching": chat_matching,
         "chat_departures": chat_departures, "chat_invalid": chat_invalid,
         "chat_images": chat_images, "chat_image_invalid": chat_image_invalid,
         "chat_image_departure": chat_image_departure, "chat_pressure": chat_pressure,
         "quiet_stall": quiet_stall, "chat_upload": chat_upload, "chat_same_batch": chat_same_batch, "chat_killed": chat_killed,
         "chat_stalled": chat_stalled, "chat_clocks": chat_clocks, "chat_fd_limit": chat_fd_limit,
         "chat_echo_bot": chat_echo_bot, "tunnel_stats": tunnel_stats, "chat_stats": chat_stats}[case]()
