"""표준 라이브러리로 구현한 WebSocket 클라이언트와 대화 상대."""
import argparse
import base64
import hashlib
import os
import random
import socket
import struct
import time


def frame(op, payload, fin=True):
    mask = os.urandom(4)
    length = len(payload)
    if length < 126:
        size = bytes([128 + length])
    elif length <= 65535:
        size = b"\xfe" + struct.pack("!H", length)
    else:
        size = b"\xff" + struct.pack("!Q", length)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return bytes([(128 if fin else 0) | op]) + size + mask + masked


class Client:
    def __init__(self, addr, path):
        if isinstance(addr, str):
            host, port = addr.rsplit(":", 1)
            addr = host, int(port)
        self.sock = socket.create_connection(addr, 5)
        self.buf = bytearray()
        try:
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            key = base64.b64encode(os.urandom(16))
            self.send_raw(f"GET {path} HTTP/1.1\r\nHost: {addr[0]}:{addr[1]}\r\n".encode()
                          + b"Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Key: "
                          + key + b"\r\nSec-WebSocket-Version: 13\r\n\r\n")
            while b"\r\n\r\n" not in self.buf:
                if len(self.buf) >= 8192:
                    raise ValueError("upgrade response header too large")
                self._read()
            head, _, self.buf = self.buf.partition(b"\r\n\r\n")
            accept = base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
            headers = {}
            for line in bytes(head).splitlines()[1:]:
                name, value = line.split(b":", 1)
                headers[name.lower()] = value.strip()
            if head.split()[:2] != [b"HTTP/1.1", b"101"] or headers.get(b"sec-websocket-accept") != accept:
                raise ValueError(f"upgrade failed: {head!r}")
        except BaseException:
            self.close()
            raise

    def _read(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise EOFError("WebSocket closed")
        self.buf.extend(chunk)

    def send_raw(self, payload):
        self.sock.sendall(payload)

    def send_text(self, text):
        self.send_raw(frame(1, text.encode()))

    def send_bin(self, payload):
        self.send_raw(frame(2, payload))

    def recv(self, timeout):
        deadline = None if timeout is None else time.monotonic() + timeout

        def need(n):
            while len(self.buf) < n:
                remaining = None if deadline is None else deadline - time.monotonic()
                if remaining is not None and remaining <= 0:
                    raise TimeoutError()
                self.sock.settimeout(remaining)
                self._read()

        previous = self.sock.gettimeout()
        try:
            while True:
                need(2)
                op, size = self.buf[0] & 15, self.buf[1]
                if size & 128:
                    raise ValueError("masked server frame")
                start = 2
                if size >= 126:
                    start += 2 if size == 126 else 8
                    need(start)
                    size = int.from_bytes(self.buf[2:start], "big")
                need(start + size)
                payload = bytes(self.buf[start:start + size])
                del self.buf[:start + size]
                if op != 9:
                    return op, payload
                self.send_raw(frame(10, payload))
        finally:
            self.sock.settimeout(previous)

    def close(self):
        self.sock.close()


def echo(addr, path):
    c = Client(addr, path)
    try:
        while True:
            op, data = c.recv(None)
            if op == 8:
                return
            if op != 1:
                continue
            if data == b"l":
                c.send_text("n")
            elif data.startswith(b"m"):
                time.sleep(.3)
                c.send_text("m" + random.choice(("반가워!", "그렇구나.", "더 들려줘.")))
            elif data.startswith(b"i"):
                c.send_text("m사진 잘 봤다")
    finally:
        c.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["echo"])
    parser.add_argument("--addr", default="127.0.0.1:8080")
    parser.add_argument("--path", default="/ws/chat")
    args = parser.parse_args()
    echo(args.addr, args.path)
