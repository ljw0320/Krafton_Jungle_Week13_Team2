"""Small real-TCP CGI integration suite; runs in Linux/WSL, without load testing."""

import concurrent.futures
import http.client
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]


class CgiTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="jungle-cgi-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.cgi = self.directory / "cgi"
        self.cgi.mkdir()
        shutil.copy2(ROOT / "cgi-bin/adder", self.cgi / "adder")
        self.start_server(self.cgi)

    def start_server(self, cgi_root):
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            self.port = reservation.getsockname()[1]
        args = [str(ROOT / "webserver"), str(self.port), str(ROOT / "www")]
        if cgi_root is not None:
            args.append(str(cgi_root))
        self.server = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop_server)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.1):
                    return
            except OSError:
                self.assertIsNone(self.server.poll(), "server failed to start")
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

    def program(self, name, code):
        path = self.cgi / name
        path.write_text("#!/usr/bin/python3\n" + code, encoding="utf-8")
        path.chmod(0o700)
        return path

    def request(self, target, body=None, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            request_headers = {"Connection": "close", **(headers or {})}
            connection.request("POST" if body is not None else "GET", target, body, request_headers)
            response = connection.getresponse()
            data = response.read()
            if response.status in (204, 304):
                self.assertIsNone(response.getheader("Content-Length"))
            else:
                self.assertEqual(int(response.getheader("Content-Length")), len(data))
            self.assertEqual(response.getheader("Connection"), "close")
            return response.status, response.getheader("Content-Type"), data
        finally:
            connection.close()

    def test_adder_and_existing_routes(self):
        for target, body, headers in [
            ("/cgi-bin/adder?a=12&b=30", None, {}),
            ("/cgi-bin/adder", b"a=12&b=30", {"Content-Type": "application/x-www-form-urlencoded"}),
        ]:
            status, content_type, data = self.request(target, body, headers)
            self.assertEqual((status, content_type), (200, "application/json"))
            self.assertEqual(json.loads(data), {"a": 12, "b": 30, "sum": 42})
        for query in ["a=1", "a=bad&b=2", "a=9223372036854775807&b=1"]:
            self.assertEqual(self.request("/cgi-bin/adder?" + query)[0], 400)
        self.assertEqual(self.request("/")[0], 200)
        self.assertEqual(self.request("/echo", b"hello")[2], b"hello")

    def test_environment_binary_body_and_split_receive(self):
        self.program("inspect", "import os, sys, json\n"
                     "data = {k: os.environ.get(k) for k in "
                     "['REQUEST_METHOD', 'QUERY_STRING', 'SCRIPT_NAME', 'CONTENT_LENGTH', "
                     "'CONTENT_TYPE', 'SERVER_PROTOCOL', 'GATEWAY_INTERFACE']}\n"
                     "data['body'] = sys.stdin.buffer.read().hex()\n"
                     "sys.stdout.buffer.write(b'Content-Type: application/json\\r\\n\\r\\n' "
                     "+ json.dumps(data).encode())\n")
        body = b"\x00\xff\r\n&" * 10000
        status, _, raw = self.request("/cgi-bin/inspect?raw=%26+value", body,
                                      {"Content-Type": "application/octet-stream"})
        # Large JSON would exceed the output cap; verify binary input with a small request.
        self.assertEqual(status, 502)
        status, _, raw = self.request("/cgi-bin/inspect?raw=%26+value", body[:10],
                                      {"Content-Type": "application/octet-stream"})
        data = json.loads(raw)
        self.assertEqual(status, 200)
        self.assertEqual(data["body"], body[:10].hex())
        self.assertEqual(data["QUERY_STRING"], "raw=%26+value")
        self.assertEqual(data["SCRIPT_NAME"], "/cgi-bin/inspect")
        self.assertEqual(data["CONTENT_LENGTH"], "10")
        self.assertEqual(data["CONTENT_TYPE"], "application/octet-stream")
        self.assertEqual(data["REQUEST_METHOD"], "POST")
        self.assertEqual(data["SERVER_PROTOCOL"], "HTTP/1.1")
        self.assertEqual(data["GATEWAY_INTERFACE"], "CGI/1.1")
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            jobs = [pool.submit(self.request, "/cgi-bin/inspect?" + value, value.encode())
                    for value in ("alpha", "beta")]
            for value, job in zip(("alpha", "beta"), jobs):
                result = json.loads(job.result()[2])
                self.assertEqual((result["QUERY_STRING"], result["body"]),
                                 (value, value.encode().hex()))
        with socket.create_connection(("127.0.0.1", self.port), timeout=5) as peer:
            peer.sendall(b"POST /cgi-bin/adder HTTP/1.0\r\nContent-Length: 10\r\n"
                         b"Content-Type: application/x-www-form-urlencoded\r\n\r\na=12")
            time.sleep(0.03)
            peer.sendall(b"&b=30")  # nine bytes in total; final byte arrives separately
            peer.sendall(b"0")
            response = http.client.HTTPResponse(peer)
            response.begin()
            self.assertEqual(response.status, 200)
            self.assertEqual(json.loads(response.read())["sum"], 312)

    def test_routing_and_executable_boundary(self):
        plain = self.cgi / "plain"
        plain.write_text("this is not executable")
        plain.chmod(0o600)
        (self.cgi / "link").symlink_to(ROOT / "cgi-bin/adder")
        for target, expected in [
            ("/cgi-bin", 404), ("/cgi-bin/", 404), ("/cgi-bin/missing", 404),
            ("/cgi-bin/plain", 403), ("/cgi-bin/link", 403),
            ("/cgi-bin/../README.md", 403), ("/cgi-bin/%2e%2e", 403),
            ("/cgi-bin/nested/adder", 403), ("/cgi-bin/adder;id", 400),
        ]:
            with self.subTest(target=target):
                self.assertEqual(self.request(target)[0], expected)
        self.assertEqual(self.request("/cgi-bin/adder", b"a" * 65537)[0], 413)

    def test_output_bounds_binary_and_status(self):
        self.program("binary", "import sys\n"
                     "sys.stdout.buffer.write(b'Content-Type: application/octet-stream\\r\\n\\r\\n' "
                     "+ bytes(range(256)) * 256)\n")
        status, _, body = self.request("/cgi-bin/binary")
        self.assertEqual(status, 200)
        self.assertEqual(body, bytes(range(256)) * 256)
        self.program("copy", "import sys\n"
                     "sys.stdout.buffer.write(b'Content-Type: application/octet-stream\\r\\n\\r\\n' "
                     "+ sys.stdin.buffer.read())\n")
        self.assertEqual(self.request("/cgi-bin/copy", body)[2], body)
        self.program("empty", "import sys\n"
                     "sys.stdout.buffer.write(b'Status: 204 No Content\\r\\nContent-Type: text/plain\\r\\n\\r\\n')\n")
        self.assertEqual(self.request("/cgi-bin/empty")[::2], (204, b""))

    def test_invalid_program_output(self):
        outputs = [
            b"not a CGI response", b"Content-Type: text/plain\r\n\r\n" + b"x" * 65537,
            b"Content-Type: " + b"a" * 16384 + b"\r\n\r\n",
            b"Content-Type: text/plain\x00bad\r\n\r\nsecret",
            b"Content-Type: text/plain\r\nContent-Type: text/html\r\n\r\n",
            b"Status: 999 Bad\r\nContent-Type: text/plain\r\n\r\n",
            b"Status: 204 No Content\r\nContent-Type: text/plain\r\n\r\nbody",
            b"Content-Type: text/plain\r\nConnection: keep-alive\r\n\r\n",
        ]
        for index, output in enumerate(outputs):
            name = f"invalid{index}"
            self.program(name, f"import sys\nsys.stdout.buffer.write({output!r})\n")
            with self.subTest(name=name):
                self.assertEqual(self.request("/cgi-bin/" + name)[0], 502)
        self.program("crash", "import sys\nsys.exit(1)\n")
        self.assertEqual(self.request("/cgi-bin/crash")[0], 502)
        invalid = self.cgi / "badexec"
        invalid.write_text("not a binary or shebang script")
        invalid.chmod(0o700)
        self.assertEqual(self.request("/cgi-bin/badexec")[0], 502)

    def wait_for_files(self, files):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if all(path.exists() for path in files):
                return
            time.sleep(0.01)
        self.fail("CGI children did not start")

    def assert_reaped(self, files):
        for path in files:
            pid = int(path.read_text())
            self.assertFalse(Path(f"/proc/{pid}").exists(), f"CGI {pid} leaked")

    def test_timeout_does_not_occupy_workers(self):
        markers = [self.directory / f"pid{i}" for i in range(4)]
        for i, marker in enumerate(markers):
            self.program(f"slow{i}", "import os, time\n"
                         f"open({str(marker)!r}, 'w').write(str(os.getpid()))\n"
                         "time.sleep(10)\n")
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            jobs = [pool.submit(self.request, f"/cgi-bin/slow{i}") for i in range(4)]
            self.wait_for_files(markers)
            self.assertEqual(self.request("/cgi-bin/adder?a=1&b=2")[0], 503)
            started = time.monotonic()
            self.assertEqual(self.request("/")[0], 200)
            self.assertLess(time.monotonic() - started, 1.0)
            self.assertEqual([job.result()[0] for job in jobs], [504] * 4)
        self.assert_reaped(markers)
        self.assertEqual(self.request("/cgi-bin/adder?a=1&b=2")[0], 200)
        self.program("closed", "import os, time\nos.close(1)\ntime.sleep(10)\n")
        self.assertEqual(self.request("/cgi-bin/closed")[0], 504)

    def test_shutdown_reaps_active_cgi(self):
        marker = self.directory / "shutdown-pid"
        self.program("slow", "import os, time\n"
                     f"open({str(marker)!r}, 'w').write(str(os.getpid()))\n"
                     "time.sleep(10)\n")
        with socket.create_connection(("127.0.0.1", self.port), timeout=2) as peer:
            peer.sendall(b"GET /cgi-bin/slow HTTP/1.0\r\n\r\n")
            self.wait_for_files([marker])
            self.stop_server()
        self.assert_reaped([marker])

    def test_cgi_is_optional(self):
        self.stop_server()
        self.start_server(None)
        self.assertEqual(self.request("/cgi-bin/adder?a=1&b=2")[0], 404)
        self.assertEqual(self.request("/")[0], 200)


if __name__ == "__main__":
    unittest.main(verbosity=2)
