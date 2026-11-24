"""Bounded real-TCP persistence tests for Linux/WSL; not a load benchmark."""

from contextlib import ExitStack
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]


def request(target="/", body=None, version="1.1", headers=b""):
    method = "GET" if body is None else "POST"
    head = f"{method} {target} HTTP/{version}\r\nHost: localhost\r\n".encode()
    if body is not None:
        head += f"Content-Length: {len(body)}\r\n".encode()
    return head + headers + b"\r\n" + (body or b"")


class Peer:
    """One socket and one buffered reader, including coalesced response bytes."""

    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=8)
        self.reader = self.socket.makefile("rb")

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.reader.close()
        self.socket.close()

    def send(self, data):
        self.socket.sendall(data)

    def response(self):
        line = self.reader.readline()
        if not line:
            raise AssertionError("connection ended before response")
        status = int(line.split()[1])
        headers = {}
        while True:
            line = self.reader.readline()
            if line == b"\r\n":
                break
            if not line:
                raise AssertionError("connection ended inside headers")
            name, value = line.decode().split(":", 1)
            headers[name.lower()] = value.strip()
        length = int(headers.get("content-length", "0"))
        body = self.reader.read(length)
        if len(body) != length:
            raise AssertionError("truncated response body")
        return status, headers, body


class KeepAliveTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="jungle-keep-alive-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.www, self.cgi = self.directory / "www", self.directory / "cgi"
        self.www.mkdir()
        self.cgi.mkdir()
        (self.www / "index.html").write_bytes(b"home")
        (self.www / "empty.txt").write_bytes(b"")
        self.large = bytes(range(256)) * 512
        (self.www / "large.bin").write_bytes(self.large)
        shutil.copy2(ROOT / "cgi-bin/adder", self.cgi / "adder")
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            self.port = reservation.getsockname()[1]
        self.server = subprocess.Popen(
            [str(ROOT / "webserver"), str(self.port), str(self.www), str(self.cgi)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop_server)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.1):
                    return
            except OSError:
                self.assertIsNone(self.server.poll())
                time.sleep(0.01)
        self.fail("server did not start")

    def stop_server(self):
        if self.server.poll() is None:
            self.server.terminate()
            try:
                self.server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait()
                self.fail("server did not stop")

    def check(self, peer, body, status=200, connection="keep-alive"):
        actual, headers, data = peer.response()
        self.assertEqual(actual, status)
        self.assertEqual(headers["connection"], connection)
        self.assertEqual(data, body)
        return headers

    def test_sequential_static_echo_and_cgi_on_one_socket(self):
        with Peer(self.port) as peer:
            for path, body in [("/large.bin", self.large), ("/empty.txt", b""), ("/", b"home")]:
                peer.send(request(path))
                self.check(peer, body)
            for path, body, headers, expected in [
                ("/echo", b"\x00\xffhello", b"", b"\x00\xffhello"),
                ("/cgi-bin/adder?a=12&b=30", None, b"", b'{"a":12,"b":30,"sum":42}\n'),
                ("/cgi-bin/adder", b"a=-2&b=5",
                 b"Content-Type: application/x-www-form-urlencoded\r\n", b'{"a":-2,"b":5,"sum":3}\n'),
                ("/echo", b"again", b"", b"again"),
            ]:
                peer.send(request(path, body, headers=headers))
                self.check(peer, expected)

    def test_coalesced_mixed_requests_keep_binary_body_boundaries(self):
        payload = b"\x00\xff\r\nGET /fake HTTP/1.1\r\n\r\n"
        with Peer(self.port) as peer:
            peer.send(request() + request("/echo", payload) +
                      request("/cgi-bin/adder?a=2&b=7") +
                      request("/", headers=b"Connection: close\r\n"))
            self.check(peer, b"home")
            self.check(peer, payload)
            self.check(peer, b'{"a":2,"b":7,"sum":9}\n')
            self.check(peer, b"home", connection="close")
            self.assertEqual(peer.reader.read(1), b"")

    def test_partial_next_header_and_split_post_body(self):
        following = request("/echo", b"hello")
        with Peer(self.port) as peer:
            peer.send(request() + following[:8])
            self.check(peer, b"home")
            peer.send(following[8:-3])
            time.sleep(0.02)
            peer.send(following[-3:] + request("/empty.txt", headers=b"Connection: close\r\n"))
            self.check(peer, b"hello")
            self.check(peer, b"", connection="close")
            self.assertEqual(peer.reader.read(1), b"")

    def test_full_size_post_followed_by_another_request(self):
        body = bytes(range(256)) * 256
        with Peer(self.port) as peer:
            peer.send(request("/echo", body) + request())
            self.check(peer, body)
            self.check(peer, b"home")

    def test_buffered_pipeline_exceeds_turn_budget_and_closes_at_100(self):
        with Peer(self.port) as peer:
            peer.send(b"".join(request("/echo", str(i).encode()) for i in range(101)))
            for i in range(100):
                self.check(peer, str(i).encode(), connection="close" if i == 99 else "keep-alive")
            self.assertEqual(peer.reader.read(1), b"")

    def test_http10_and_explicit_close_stop_following_requests(self):
        cases = [request(version="1.0", headers=b"Connection: keep-alive\r\n"),
                 request(headers=b"Connection: Keep-Alive, cLoSe\r\nConnection: keep-alive\r\n")]
        for closing in cases:
            with self.subTest(request=closing), Peer(self.port) as peer:
                peer.send(request())
                self.check(peer, b"home")
                peer.send(closing + request())
                self.check(peer, b"home", connection="close")
                self.assertEqual(peer.reader.read(1), b"")

    def test_parse_route_and_size_errors_close_without_processing_tail(self):
        cases = [(400, b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n"),
                 (413, b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 65537\r\n\r\n"),
                 (431, b"GET / HTTP/1.1\r\nHost: x\r\nX-Pad: " + b"x" * 16384),
                 (404, request("/missing", b"unfinished"))]
        for expected, invalid in cases:
            with self.subTest(status=expected), Peer(self.port) as peer:
                peer.send(request())
                self.check(peer, b"home")
                peer.send(invalid + request())
                status, headers, _ = peer.response()
                self.assertEqual(status, expected)
                self.assertEqual(headers["connection"], "close")
                if expected == 413:
                    peer.socket.shutdown(socket.SHUT_WR)
                self.assertEqual(peer.reader.read(1), b"")

    def test_cgi_application_error_and_bodyless_response_allow_next_request(self):
        script = self.cgi / "empty"
        script.write_text("#!/usr/bin/python3\nimport sys\n"
                          "sys.stdout.write('Status: 204 No Content\\r\\nContent-Type: text/plain\\r\\n\\r\\n')\n")
        script.chmod(0o700)
        with Peer(self.port) as peer:
            peer.send(request("/cgi-bin/adder?a=bad&b=1") + request("/cgi-bin/empty") + request())
            status, headers, body = peer.response()
            self.assertEqual(status, 400)
            self.assertIn("error", json.loads(body))
            self.assertEqual(headers["connection"], "keep-alive")
            headers = self.check(peer, b"", status=204)
            self.assertNotIn("content-length", headers)
            self.check(peer, b"home")

    def test_idle_connection_closes_after_five_seconds(self):
        with Peer(self.port) as peer:
            peer.send(request())
            self.check(peer, b"home")
            started = time.monotonic()
            self.assertEqual(peer.reader.read(1), b"")
            elapsed = time.monotonic() - started
            self.assertGreaterEqual(elapsed, 4.5)
            self.assertLess(elapsed, 7.5)

    def test_sixteen_idle_connections_leave_workers_available(self):
        with ExitStack() as stack:
            peers = []
            for _ in range(16):
                peer = stack.enter_context(Peer(self.port))
                peer.send(request())
                self.check(peer, b"home")
                peers.append(peer)
            with Peer(self.port) as extra:
                try:
                    extra.send(request())
                    self.assertEqual(extra.reader.read(1), b"")
                except (ConnectionResetError, BrokenPipeError):
                    pass
            peers[0].send(request("/echo", b"still working"))
            self.check(peers[0], b"still working")

    def test_request_resources_are_released_before_reusing_connection(self):
        with Peer(self.port) as peer:
            peer.send(request("/echo", b"ready"))
            self.check(peer, b"ready")
            descriptors = Path(f"/proc/{self.server.pid}/fd")
            baseline = len(list(descriptors.iterdir()))
            for _ in range(20):
                peer.send(request("/large.bin") + request("/cgi-bin/adder?a=1&b=2"))
                self.check(peer, self.large)
                self.check(peer, b'{"a":1,"b":2,"sum":3}\n')
            peer.send(request("/echo", b"done"))
            self.check(peer, b"done")
            self.assertLessEqual(len(list(descriptors.iterdir())), baseline)
            for task in Path(f"/proc/{self.server.pid}/task").iterdir():
                self.assertEqual((task / "children").read_text().strip(), "")


if __name__ == "__main__":
    unittest.main(verbosity=2)
