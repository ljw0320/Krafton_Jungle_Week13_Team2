"""Isolated integration checks for the public room and its loopback frontend."""
from contextlib import ExitStack
import json
import socket
import subprocess
import time
from urllib.request import urlopen

from chat_bot import Client, frame, handshake


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def start(stack, command, listen):
    child = stack.enter_context(subprocess.Popen(command))
    stack.callback(lambda: child.terminate() if child.poll() is None else None)
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline:
        assert child.poll() is None, f"{command[0]} exited during startup: {child.returncode}"
        try:
            with socket.create_connection(("127.0.0.1", listen), timeout=.2):
                return child
        except OSError:
            time.sleep(.02)
    raise AssertionError(f"{command[0]} startup timed out")


def connect(stack, listen, backend, rcvbuf=0, path="/ws/live"):
    client = Client(("127.0.0.1", listen), path, rcvbuf=rcvbuf)
    stack.callback(client.close)
    assert client.recv(2) == (1, f'{{"backend":"{backend}"}}'.encode())
    return client


def check_room():
    with ExitStack() as stack:
        hub = port()
        relay = start(stack, ["./livehub", str(hub)], hub)
        a = port()
        start(stack, ["./chatd", str(a), "--live", str(hub), "A"], a)
        front = port()
        start(stack, ["./server", str(front), "www", "--loopback", f"/ws/=127.0.0.1:{a}"], front)
        with socket.create_connection(("127.0.0.1", a), timeout=2) as sock:
            request, _ = handshake(("127.0.0.1", a), "/ws/live")
            sock.sendall(request[:-2] + b"Origin: http://unrelated.invalid\r\n\r\n")
            assert sock.recv(128).startswith(b"HTTP/1.1 403 ")
        local = [connect(stack, front, "A", path="/ws/live?trace=1")]
        local.extend(connect(stack, front, "A") for _ in range(2))
        local[0].send_text("안녕")
        assert all(c.recv(2) == (1, "안녕".encode()) for c in local)
        with urlopen(f"http://127.0.0.1:{a}/ws/stat", timeout=2) as response:
            assert json.load(response)["events"] == []
        b = port()
        start(stack, ["./chatd", str(b), "--live", str(hub), "B"], b)
        remote = connect(stack, b, "B")
        local[0].send_raw(frame(1, b"x" * 2000, fin=False) + frame(0, b"y" * 2096))
        remote.send_text("other")
        streams = [[client.recv(2), client.recv(2)] for client in [*local, remote]]
        assert all(stream == streams[0] for stream in streams)
        assert set(streams[0]) == {(1, b"x" * 2000 + b"y" * 2096), (1, b"other")}
        for payload, op, code in [(b"x" * 4097, 1, 1009), (b"\xff", 1, 1007), (b"a", 2, 1003)]:
            invalid = connect(stack, b, "B")
            invalid.send_raw(frame(op, payload))
            assert invalid.recv(2) == (8, code.to_bytes(2, "big"))
        for c in local:
            c.close()
        # A small TCP receive window fills the inactive recipient's queue first.
        slow = connect(stack, a, "A", rcvbuf=1024)
        witness = connect(stack, a, "A")
        for i in range(100):
            payload = f"{i:04d}".encode() + b"x" * 4092
            remote.send_raw(frame(1, payload))
            assert remote.recv(2) == witness.recv(2) == (1, payload)
        with urlopen(f"http://127.0.0.1:{a}/ws/stat", timeout=2) as response:
            assert json.load(response)["slow_closed"] == 1
        slow.close()
        relay.terminate()
        relay.wait(2)
        for c in (witness, remote):
            try:
                received = c.recv(2)
            except EOFError:
                continue
            assert received[0] == 8
        with socket.create_connection(("127.0.0.1", a), timeout=2) as sock:
            request, _ = handshake(("127.0.0.1", a), "/ws/live")
            sock.sendall(request)
            assert sock.recv(128).startswith(b"HTTP/1.1 503 ")
        private = Client(("127.0.0.1", a), "/ws/chat")
        stack.callback(private.close)
        assert private.recv(2) == (1, b"w")


if __name__ == "__main__":
    check_room()
    print("public room relay, frame limits and slow-client isolation passed")
