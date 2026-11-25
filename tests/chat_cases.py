"""터널 기능 시험과 원시 소켓 백엔드."""
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
from hashlib import sha256
import os
from pathlib import Path
import socket
import struct
import sys
import threading
import time

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


def closes(pid):
    def fds():
        return len(os.listdir(f"/proc/{pid}/fd"))

    baseline = fds()
    with connect() as s:
        rest = upgrade(s, "close")
        assert receive(s, 4096, rest) == body(4096)
        assert s.recv(1) == b""
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
            return
        time.sleep(.05)
    raise AssertionError(("server fds", baseline, fds()))


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
            return
        raise AssertionError("64 MiB sent without backpressure")


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
    elif case in ("closes", "noread"):
        {"closes": closes, "noread": noread}[case](int(sys.argv[2]))
    else:
        {"handshake": handshake, "echo": echo}[case]()
