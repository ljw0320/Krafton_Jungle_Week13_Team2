"""Bounded real-TCP proxy checks; no pre-running server required."""
import concurrent.futures
import http.client
from pathlib import Path
import queue
import socket
import socketserver
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Backend(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(2)
        data = b''
        while b'\r\n\r\n' not in data:
            chunk = self.request.recv(4096)
            if not chunk:
                return
            data += chunk
        self.server.seen.put(data)
        try:
            self.server.reply(self.request, data)
        except (OSError, socket.timeout):
            pass  # Client/proxy abort is intentional in deadline and truncation cases.


class ProxyTests(unittest.TestCase):
    def start(self, backend_port):
        port = free_port()
        process = subprocess.Popen([str(ROOT/'reverse_proxy'), str(port), '127.0.0.1', str(backend_port)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.addCleanup(self.stop, process)
        end = time.monotonic()+3
        while time.monotonic() < end:
            try:
                with socket.create_connection(('127.0.0.1', port), .1):
                    return port, process
            except OSError:
                self.assertIsNone(process.poll())
                time.sleep(.01)
        self.fail('proxy did not listen')

    def stop(self, process):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                self.fail('shutdown hung')
        if process.stderr:
            process.stderr.close()

    def mock(self, reply):
        backend = Backend(('127.0.0.1', 0), Handler)
        backend.reply = reply
        backend.seen = queue.Queue()
        thread = threading.Thread(target=backend.serve_forever, kwargs={'poll_interval': .02}, daemon=True)
        thread.start()
        self.addCleanup(backend.server_close)
        self.addCleanup(backend.shutdown)
        port, process = self.start(backend.server_address[1])
        return port, backend, process

    def request(self, port, path='/', body=None):
        c = http.client.HTTPConnection('127.0.0.1', port, timeout=12)
        try:
            c.request('POST' if body is not None else 'GET', path, body,
                      {'Connection': 'close', 'Content-Type': 'application/octet-stream'})
            r = c.getresponse()
            data = r.read()
            self.assertEqual(r.getheader('Connection'), 'close')
            return r.status, data
        finally:
            c.close()

    def test_real_webserver_binary_post_cgi_large_file(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            payload = bytes(range(256))*8192  # 2MiB, far larger than the relay buffer.
            (root/'large.bin').write_bytes(payload)
            backend_port = free_port()
            server = subprocess.Popen([str(ROOT/'webserver'), str(backend_port), folder, str(ROOT/'cgi-bin')],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.addCleanup(self.stop, server)
            for _ in range(100):
                try:
                    with socket.create_connection(('127.0.0.1', backend_port), .1): break
                except OSError: time.sleep(.01)
            port, _ = self.start(backend_port)
            self.assertEqual(self.request(port, '/large.bin'), (200, payload))
            self.assertEqual(self.request(port, '/echo', payload[:65536]), (200, payload[:65536]))
            self.assertEqual(self.request(port, '/missing')[0], 404)
            status, data = self.request(port, '/cgi-bin/adder?a=12&b=30')
            self.assertEqual(status, 200)
            self.assertIn(b'42', data)

    def test_request_fully_buffered_path_headers_and_close(self):
        def reply(s, data):
            header, body = data.split(b'\r\n\r\n', 1)
            while len(body) < 6: body += s.recv(6-len(body))
            backend.body = body
            s.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: X-Secret\r\n'
                      b'X-Secret: hidden\r\nX-End: preserved\r\n\r\n'+body)
        port, backend, _ = self.mock(reply)
        with socket.create_connection(('127.0.0.1', port), 2) as s:
            s.sendall(b'POST /echo?q=%26 HTTP/1.1\r\nHost: original\r\nContent-Length: 6\r\n'
                      b'Connection: close, X-Remove\r\nX-Remove: hidden\r\n\r\nabc')
            time.sleep(.08)
            self.assertTrue(backend.seen.empty(), 'must not contact backend before full body')
            s.sendall(b'\x00\xffz')
            r = http.client.HTTPResponse(s); r.begin()
            self.assertEqual(r.read(), b'abc\x00\xffz')
            self.assertIsNone(r.getheader('X-Secret'))
            self.assertEqual(r.getheader('X-End'), 'preserved')
            self.assertEqual(s.recv(1), b'')
        forwarded = backend.seen.get(timeout=1)
        self.assertTrue(forwarded.startswith(b'POST /echo?q=%26 HTTP/1.1\r\n'))
        self.assertIn(b'Connection: close\r\n', forwarded)
        self.assertIn(('Host: 127.0.0.1:%d\r\n' % backend.server_address[1]).encode(), forwarded)
        self.assertNotIn(b'X-Remove', forwarded)

    def test_invalid_requests_and_backend_framing(self):
        response = [b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n']
        port, backend, _ = self.mock(lambda s, data: s.sendall(response[0]))
        cases = [
            (b'PUT / HTTP/1.1\r\nHost: x\r\n\r\n', 405),
            (b'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 65537\r\n\r\n', 413),
            (b'POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n', 400),
            (b'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nContent-Length: 1\r\n\r\n', 400),
        ]
        for request, status in cases:
            with socket.create_connection(('127.0.0.1', port), 2) as s:
                s.sendall(request)
                r = http.client.HTTPResponse(s); r.begin()
                self.assertEqual(r.status, status); r.read()
        self.assertTrue(backend.seen.empty())
        for invalid in [response[0], b'HTTP/1.1 200 OK\r\n\r\n',
                        b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n']:
            response[0] = invalid
            self.assertEqual(self.request(port)[0], 502)
        for valid in [b'HTTP/1.1 204 No Content\r\n\r\n',
                      b'HTTP/1.1 304 Not Modified\r\nContent-Length: 123\r\n\r\n']:
            response[0] = valid
            status, body = self.request(port)
            self.assertIn(status, (204, 304)); self.assertEqual(body, b'')

    def test_capacity_recovery_abort_and_connection_failure(self):
        release = threading.Event()
        def reply(s, data):
            release.wait(3)
            s.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n')
        port, backend, process = self.mock(reply)
        self.addCleanup(release.set)
        peers = []
        try:
            for _ in range(8):
                s = socket.create_connection(('127.0.0.1', port), 2); peers.append(s)
                s.sendall(b'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')
                backend.seen.get(timeout=2)
            self.assertEqual(self.request(port)[0], 503)
            release.set()
            for s in peers:
                r = http.client.HTTPResponse(s); r.begin(); self.assertEqual(r.status, 200); r.read()
                s.close()
            peers.clear()
            time.sleep(.15)
            self.assertEqual(self.request(port)[0], 200)
            time.sleep(.15)
            baseline_fds = len(list(Path('/proc/%d/fd' % process.pid).iterdir()))
            # Abandon an incomplete request; other clients must still work.
            for _ in range(12):
                with socket.create_connection(('127.0.0.1', port), 2) as s: s.sendall(b'GET /')
                time.sleep(.01)
            self.assertEqual(self.request(port)[0], 200)
            time.sleep(.15)
            self.assertLessEqual(len(list(Path('/proc/%d/fd' % process.pid).iterdir())), baseline_fds)
            self.assertIsNone(process.poll())
        finally:
            for s in peers: s.close()
        closed_port = free_port()
        failed, _ = self.start(closed_port)
        self.assertEqual(self.request(failed)[0], 502)

    def test_keep_alive_reuse_pipeline_error_and_limit(self):
        def reply(s, data):
            s.sendall(b'HTTP/1.1 404 Missing\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok')
        port, backend, _ = self.mock(reply)
        with socket.create_connection(('127.0.0.1', port), 3) as sock:
            stream = sock.makefile('rb')
            def read():
                status = stream.readline()
                fields = {}
                while True:
                    line = stream.readline()
                    if line == b'\r\n': break
                    self.assertTrue(line)
                    k, v = line.split(b':', 1); fields[k.lower()] = v.strip()
                return status, fields, stream.read(int(fields[b'content-length']))
            # POST and next GET in the same write: preserve their boundary.
            sock.sendall(b'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc'
                         b'GET /next HTTP/1.1\r\nHost: x\r\n\r\n')
            for _ in range(2):
                status, fields, body = read()
                self.assertIn(b'404', status)
                self.assertEqual(body, b'ok')
                self.assertEqual(fields[b'connection'], b'keep-alive')
            for number in range(3, 101):
                sock.sendall(b'GET / HTTP/1.1\r\nHost: x\r\n\r\n')
                _, fields, body = read()
                self.assertEqual(fields[b'connection'], b'close' if number == 100 else b'keep-alive')
                self.assertEqual(body, b'ok')
            self.assertEqual(stream.read(1), b'')
            stream.close()
        self.assertEqual(backend.seen.qsize(), 100) # Separate backend connection for each request.
        while not backend.seen.empty():
            self.assertIn(b'Connection: close\r\n', backend.seen.get())
        with socket.create_connection(('127.0.0.1', port), 3) as sock:
            sock.sendall(b'BAD / HTTP/1.1\r\nHost: x\r\n\r\n'
                         b'GET /must-not-run HTTP/1.1\r\nHost: x\r\n\r\n')
            r = http.client.HTTPResponse(sock); r.begin()
            self.assertEqual(r.status, 405); r.read()
            self.assertEqual(sock.recv(1), b'')
        self.assertTrue(backend.seen.empty())

    def test_idle_connections_release_active_capacity(self):
        port, _, _ = self.mock(lambda s, data: s.sendall(
            b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n'))
        peers = []
        try:
            for _ in range(15):
                sock = socket.create_connection(('127.0.0.1', port), 3)
                peers.append(sock)
                sock.sendall(b'GET / HTTP/1.1\r\nHost: x\r\n\r\n')
                r = http.client.HTTPResponse(sock); r.begin()
                self.assertEqual(r.status, 200); r.read(); r.close()
            # Fifteen idle keep-alive connections must not hold the eight active buffers.
            self.assertEqual(self.request(port)[0], 200)
            time.sleep(5.15)
            for sock in peers: self.assertEqual(sock.recv(1), b'')
            with socket.create_connection(('127.0.0.1', port), 3) as sock:
                sock.sendall(b'GET / HTTP/1.0\r\n\r\n')
                r = http.client.HTTPResponse(sock); r.begin()
                self.assertEqual(r.getheader('Connection'), 'close'); r.read()
                self.assertEqual(sock.recv(1), b'')
        finally:
            for sock in peers: sock.close()

    def test_idle_and_absolute_deadlines(self):
        def reply(s, data):
            if data.startswith(b'GET /idle '):
                time.sleep(6)
                return
            # No headers before deadline: keep making progress to defeat the idle timeout.
            for _ in range(12):
                s.sendall(b'H')
                time.sleep(1)
        port, _, _ = self.mock(reply)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            started = time.monotonic()
            result = pool.submit(self.request, port)
            idle = pool.submit(self.request, port, '/idle')
            with socket.create_connection(('127.0.0.1', port), 2) as s:
                s.sendall(b'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\n')
                s.settimeout(7)
                r = http.client.HTTPResponse(s); r.begin()
                self.assertEqual(r.status, 408); r.read()
            self.assertEqual(idle.result(timeout=2)[0], 504)
            self.assertLess(time.monotonic()-started, 7)
            self.assertEqual(result.result(timeout=7)[0], 504)
            elapsed = time.monotonic()-started
            self.assertGreaterEqual(elapsed, 9.7)
            self.assertLess(elapsed, 11.5)

    def test_partial_response_backpressure_and_truncation(self):
        payload = bytes(range(256))*16384
        def reply(s, data):
            if data.startswith(b'GET /truncated '):
                s.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort')
                return
            s.sendall(('HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n' % len(payload)).encode())
            for offset in range(0, len(payload), 3071):
                s.sendall(payload[offset:offset+3071])
        port, _, process = self.mock(reply)
        with socket.create_connection(('127.0.0.1', port), 3) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
            s.sendall(b'GET /slow HTTP/1.1\r\nHost: x\r\n\r\n')
            # Pause consumption, then verify every byte after the relay resumes.
            time.sleep(.15)
            r = http.client.HTTPResponse(s); r.begin()
            self.assertEqual(r.read(), payload)
        with self.assertRaises(http.client.IncompleteRead):
            self.request(port, '/truncated')
        self.assertEqual(self.request(port)[0], 200)
        self.assertIsNone(process.poll())


if __name__ == '__main__':
    unittest.main(verbosity=2)
