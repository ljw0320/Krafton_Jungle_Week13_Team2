#!/usr/bin/env python3
"""프록시 테스트용 백엔드. python3 tests/backend.py <포트> [noaccept]

요청 대상 /<접두사>/<이름>[/<인자>][?쿼리]의 이름으로 동작을 고른다. 모르는 이름은 200 + 요청 대상(본문).
요청은 헤더와 Content-Length 본문까지 다 읽은 뒤 답하고, 응답 뒤에는 연결을 닫는다.
"""
import socket
import struct
import sys
import threading
import time


def body(n):
    """정해진 규칙의 본문. 테스트가 같은 식으로 sha256을 계산한다."""
    pat = bytes((i * 7 + 3) % 251 for i in range(251))
    return (pat * (n // 251 + 1))[:n]


def resp(code, reason, headers, data=b""):
    h = "HTTP/1.1 %d %s\r\n" % (code, reason)
    for k, v in headers:
        h += "%s: %s\r\n" % (k, v)
    return h.encode() + b"\r\n" + data


def ok(data, extra=()):
    return resp(200, "OK", [("Content-Length", len(data))] + list(extra), data)


def read_request(c):
    buf = b""
    while b"\r\n\r\n" not in buf:
        d = c.recv(65536)
        if not d:
            return None
        buf += d
    head, _, rest = buf.partition(b"\r\n\r\n")
    n = 0
    for line in head.split(b"\r\n")[1:]:
        k, _, v = line.partition(b":")
        if k.strip().lower() == b"content-length":
            n = int(v)
    while len(rest) < n:
        d = c.recv(65536)
        if not d:
            return None
        rest += d
    return head + b"\r\n\r\n" + rest[:n], head.split(b"\r\n")[0].decode("latin-1")


def handle(c, port):
    try:
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        got = read_request(c)
        if got is None:
            return
        raw, line = got
        target = line.split(" ")[1] if line.count(" ") >= 2 else ""
        parts = [p for p in target.split("?")[0].split("/") if p]
        name = parts[1] if len(parts) > 1 else ""
        arg = parts[2] if len(parts) > 2 else ""
        send = c.sendall
        if name == "echo":
            send(ok(raw))
        elif name == "cl":
            send(ok(b"cl-body\n"))
        elif name == "chunked":
            send(resp(200, "OK", [("Transfer-Encoding", "chunked")],
                      b"6\r\nhello \r\n6\r\nworld\n\r\n0\r\n\r\n"))
        elif name == "nolen":
            send(resp(200, "OK", [], b"nolen-body\n"))
        elif name == "short":
            send(resp(200, "OK", [("Content-Length", 100)], b"0123456789"))
        elif name == "over":
            if arg == "split":
                send(resp(200, "OK", [("Content-Length", 5)], b"he"))  # 본문 단계에서 넘치게
                time.sleep(0.3)
                send(b"lloEXTRA-BYTES")
            else:
                send(resp(200, "OK", [("Content-Length", 5)], b"helloEXTRA-BYTES"))
        elif name == "empty":
            send(resp(204, "No Content", [("Content-Length", 0)]))
        elif name == "304":
            send(resp(304, "Not Modified", [("Content-Length", 5)]))
        elif name == "1xx":
            send(resp(100, "Continue", []) + resp(103, "Early Hints", [("Link", "</a>; rel=preload")])
                 + resp(103, "Early Hints", [("Link", "</b>; rel=preload")])
                 + ok(b"after-1xx\n"))
        elif name == "101":
            send(resp(101, "Switching Protocols", [("Upgrade", "websocket"), ("Connection", "Upgrade")]))
        elif name == "bad":
            send(b"NOT-HTTP garbage\r\n\r\n")
        elif name == "lf":
            send(b"HTTP/1.1 200 OK\nContent-Length: 2\n\nhi")
        elif name == "dupcl":
            send(resp(200, "OK", [("Content-Length", 2), ("Content-Length", 2)], b"hi"))
        elif name == "tecl":
            send(resp(200, "OK", [("Transfer-Encoding", "chunked"), ("Content-Length", 5)],
                      b"0\r\n\r\n"))
        elif name == "bighead":
            send(resp(200, "OK", [("Content-Length", 2), ("X-Big", "a" * 9000)], b"hi"))
        elif name == "head":
            fixed = len(resp(200, "OK", [("Content-Length", 2), ("X-Pad", "")]))
            k = int(arg) - fixed  # fixed는 빈 줄까지 포함한 길이(X-Pad 값 없음)
            if k < 0:
                send(resp(500, "Internal Server Error", [("Content-Length", 0)]))
            else:
                h = resp(200, "OK", [("Content-Length", 2), ("X-Pad", "a" * k)])
                assert h.index(b"\r\n\r\n") + 4 == int(arg)
                send(h + b"ok")
        elif name == "close":
            pass
        elif name == "noclose":  # 길이만큼 보내고 닫지 않는다
            send(ok(b"noclose-body\n"))
            time.sleep(60)
        elif name == "rstafter":  # 다 보낸 직후 RST
            send(ok(b"rstafter-body\n"))
            c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        elif name == "rst":
            c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        elif name == "hang":
            time.sleep(60)
        elif name == "stall":
            send(resp(200, "OK", [("Content-Length", 1000)], b"x" * 10))
            time.sleep(60)
        elif name == "slow":
            time.sleep(9)
            send(ok(b"slow-body\n"))
        elif name == "drip":  # 5초마다 1바이트, 총 15초
            send(resp(200, "OK", [("Content-Length", 3)]))
            for b in (b"a", b"b", b"c"):
                time.sleep(5)
                send(b)
        elif name == "big":
            send(ok(body(8 << 20)))
        elif name == "bignolen":
            send(resp(200, "OK", [], body(8 << 20)))
        elif name == "port":
            send(ok(str(port).encode()))
        elif name == "size":
            send(ok(body(int(arg))))
        elif name == "split":
            for b in ok(b"split-body\n"):
                send(bytes([b]))
                time.sleep(0.001)
        else:
            send(ok(target.encode()))
    except OSError:
        pass
    finally:
        c.close()


def main():
    port = int(sys.argv[1])
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    if len(sys.argv) > 2 and sys.argv[2] == "noaccept":
        s.listen(0)  # accept를 부르지 않는다
        while True:
            time.sleep(3600)
    s.listen(128)
    while True:
        c, _ = s.accept()
        threading.Thread(target=handle, args=(c, port), daemon=True).start()


if __name__ == "__main__":
    main()
