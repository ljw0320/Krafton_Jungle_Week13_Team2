"""Real TCP peers verify forwarding, framing, failure and resource ownership."""
import concurrent.futures
import http.client
import queue
import socket
import subprocess
import threading
import time
import unittest

from support import BINARY, ROOT, ServerTestCase, children, connect, wait_for


class Backend:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.port = self.listener.getsockname()[1]
        self.listener.listen()
        self.listener.settimeout(.2)
        self.requests = queue.Queue()
        self.errors = queue.Queue()
        self.reply = b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok'
        self.handler = None
        self.closed = threading.Event()
        self.workers = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.closed.is_set():
            try:
                peer, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            worker = threading.Thread(target=self.serve, args=(peer,), daemon=True)
            self.workers.append(worker)
            worker.start()

    def serve(self, peer):
        try:
            with peer:
                peer.settimeout(15)
                if self.handler:
                    self.handler(peer)
                    return
                stream = peer.makefile('rb')
                with stream:
                    line = stream.readline()
                    headers = []
                    while (field := stream.readline()) not in (b'\r\n', b''):
                        name, value = field.rstrip(b'\r\n').split(b':', 1)
                        headers.append((name.lower(), value.strip()))
                    length = int(dict(headers).get(b'content-length', b'0'))
                    body = stream.read(length)
                    self.requests.put((line, headers, body))
                    if isinstance(self.reply, list):
                        for piece in self.reply:
                            peer.sendall(piece)
                            time.sleep(.002)
                    else:
                        peer.sendall(self.reply)
                    # The proxy must close this request's upstream connection.
                    if peer.recv(1) != b'':
                        raise AssertionError('upstream connection reused')
        except (BrokenPipeError, ConnectionResetError):
            pass  # Expected for rejected response headers and disconnected clients.
        except Exception as exc:
            self.errors.put(exc)

    def close(self):
        self.closed.set()
        self.listener.close()
        self.thread.join(2)
        for worker in self.workers:
            worker.join(16)
        if not self.errors.empty():
            raise self.errors.get()


class ProxyTests(ServerTestCase):
    def setUp(self):
        super().setUp()
        (self.root / 'index.html').write_bytes(b'local')
        self.backend = Backend()
        self.addCleanup(self.backend.close)
        self.other = Backend()
        self.addCleanup(self.other.close)
        self.other.reply = b'HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nother'
        self.config = self.work / 'routes.conf'
        self.config.write_text(f'# routes\n/api 127.0.0.1 {self.backend.port}\n'
                               f'/api/admin 127.0.0.1 {self.other.port}\n')
        self.proc = None

    def launch(self):
        self.proc = subprocess.Popen(
            [str(BINARY), str(self.port), str(self.root), '16', '2', '1',
             '--routes', str(self.config)], cwd=ROOT,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(self.shutdown)
        self.assertTrue(self.proc.stdout.readline().startswith('READY'),
                        'proxy CLI must accept --routes and start')

    def shutdown(self):
        if self.proc and not self.proc.stdout.closed:
            self.proc.terminate()
            _, self.logs = self.proc.communicate(timeout=5)
            self.assertEqual(self.proc.returncode, 0, self.logs)

    def exchange(self, path='/api/users?q=1', method='GET', body=None, headers=None):
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=15)
        try:
            conn.request(method, path, body, headers or {'Host': 'external.example:8080'})
            result = conn.getresponse()
            return result.status, result.getheaders(), result.read()
        finally:
            conn.close()

    def raw(self, payload):
        with connect(self.port) as peer:
            peer.sendall(payload)
            result = http.client.HTTPResponse(peer)
            result.begin()
            return result.status, result.getheaders(), result.read()

    def test_routes_longest_boundary_and_local_fallback(self):
        self.launch()
        self.assertEqual(self.exchange()[2], b'ok')
        self.assertEqual(self.exchange('/api/admin/users')[2], b'other')
        self.assertEqual(self.exchange('/')[2], b'local')
        self.assertEqual(self.exchange('/apix')[0], 404)
        self.assertEqual(self.exchange('/cgi-bin/adder?a=1&b=2')[2], b'3\n')

    def test_preserves_target_host_binary_body_and_filters_headers(self):
        self.launch()
        body = bytes(range(256)) * 128
        result = self.exchange('/api/a%20b?x=%2F', 'POST', body, {
            'Host': 'external.example:8080', 'Content-Type': 'application/octet-stream',
            'Connection': 'keep-alive, X-Temporary', 'X-Temporary': 'secret',
            'Forwarded': 'for=forged', 'X-Forwarded-For': 'forged',
            'X-Forwarded-Custom': 'forged', 'Authorization': 'Bearer demo'})
        self.assertEqual(result[0], 200)
        line, pairs, received = self.backend.requests.get(timeout=2)
        headers = dict(pairs)
        self.assertEqual(line, b'POST /api/a%20b?x=%2F HTTP/1.1\r\n')
        self.assertEqual(headers[b'host'], b'external.example:8080')
        self.assertEqual(headers[b'connection'], b'close')
        self.assertEqual(headers[b'authorization'], b'Bearer demo')
        self.assertEqual(received, body)
        for field in (b'x-temporary', b'forwarded', b'x-forwarded-for', b'x-forwarded-custom'):
            self.assertNotIn(field, headers)

    def test_keepalive_creates_new_backend_connection_and_preserves_cookies(self):
        self.backend.reply = (b'HTTP/1.0 200 OK\r\nContent-Length: 2\r\n'
                              b'Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n'
                              b'Connection: close, X-Remove\r\nX-Remove: secret\r\n\r\nok')
        self.launch()
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=5)
        try:
            for _ in range(3):
                conn.request('GET', '/api')
                result = conn.getresponse()
                self.assertEqual(result.read(), b'ok')
                self.assertEqual(result.getheader('Connection'), 'keep-alive')
                self.assertEqual(result.headers.get_all('Set-Cookie'), ['a=1', 'b=2'])
                self.assertIsNone(result.getheader('X-Remove'))
            self.assertEqual(self.backend.requests.qsize(), 3)
        finally:
            conn.close()

    def test_rejects_ambiguous_or_unsupported_responses_before_forwarding(self):
        self.launch()
        cases = [b'Content-Length: 2\r\nContent-Length: 2', b'Content-Length: -1',
                 b'Content-Length: 18446744073709551616', b'Content-Length: 2, 2',
                 b'Transfer-Encoding: chunked', b'X: y',
                 b'Content-Length: 2\r\nConnection: Content-Length',
                 b'Content-Length: 2\r\nConnection: Transfer-Encoding',
                 b'Content-Length: 2\r\n folded: value', b'Content-Length: 2\r\nX: a\x00b']
        for fields in cases:
            with self.subTest(fields=fields):
                self.backend.reply = b'HTTP/1.1 200 OK\r\n' + fields + b'\r\n\r\nok'
                self.assertEqual(self.exchange()[0], 502)
        for line in (b'HTTP/1.9 200 OK', b'HTTP/1.1 100 Continue', b'HTTP/1.1 101 Switching',
                     b'HTTP/1.1 099 Nope', b'HTTP/1.1 600 Nope'):
            self.backend.reply = line + b'\r\nContent-Length: 0\r\n\r\n'
            self.assertEqual(self.exchange()[0], 502)

    def test_empty_and_bodyless_responses(self):
        self.launch()
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=5)
        try:
            for reply, status in [(b'HTTP/1.1 204 No Content\r\n\r\n', 204),
                                  (b'HTTP/1.1 304 Not Modified\r\nContent-Length: 999\r\n\r\n', 304),
                                  (b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n', 200)]:
                self.backend.reply = reply
                conn.request('GET', '/api')
                result = conn.getresponse()
                self.assertEqual((result.status, result.read()), (status, b''))
        finally:
            conn.close()
        self.backend.reply = b'HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n'
        self.assertEqual(self.exchange()[0], 502)

    def test_header_size_boundary_fragmentation_and_large_binary(self):
        self.launch()
        prefix = b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX: '
        self.backend.reply = prefix + b'a' * (32768 - len(prefix) - 4) + b'\r\n\r\n'
        self.assertEqual(self.exchange()[0], 200)
        self.backend.reply = prefix + b'a' * (32769 - len(prefix) - 4) + b'\r\n\r\n'
        self.assertEqual(self.exchange()[0], 502)
        binary = bytes(range(256)) * 8192
        header = f'HTTP/1.1 200 OK\r\nContent-Length: {len(binary)}\r\n\r\n'.encode()
        self.backend.reply = [bytes([c]) for c in header] + [binary]
        self.assertEqual(self.exchange()[2], binary)

    def test_early_eof_closes_without_appending_error_and_logs_counts(self):
        def truncated(peer):
            peer.recv(32768)
            peer.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\nabc')
        self.backend.handler = truncated
        self.launch()
        with connect(self.port) as peer:
            peer.sendall(b'GET /api HTTP/1.1\r\nHost: example.com\r\n\r\n')
            data = b''
            while piece := peer.recv(4096):
                data += piece
        self.assertTrue(data.startswith(b'HTTP/1.1 200'))
        self.assertEqual(data.split(b'\r\n\r\n', 1)[1], b'abc')
        self.shutdown()
        self.assertIn('expected=1000 received=3 sent=3', self.logs)

    def test_request_host_and_connection_validation(self):
        self.launch()
        for fields in (b'Host: example.com\r\nConnection: Host',
                       b'Host: example.com\r\nConnection: Content-Length',
                       b'Host: example.com:abc', b'Host: [broken', b'Host: a:b:c',
                       b'Host: example..com'):
            self.assertEqual(self.raw(b'GET /api HTTP/1.1\r\n' + fields + b'\r\n\r\n')[0], 400)
        self.assertEqual(self.raw(b'GET /api HTTP/1.0\r\n\r\n')[0], 400)

    def test_expect_is_handled_locally(self):
        self.launch()
        with connect(self.port) as peer:
            peer.sendall(b'POST /api HTTP/1.1\r\nHost: example.com\r\n'
                         b'Content-Length: 3\r\nExpect: 100-continue\r\n\r\n')
            self.assertEqual(peer.recv(1024), b'HTTP/1.1 100 Continue\r\n\r\n')
            peer.sendall(b'abc')
            result = http.client.HTTPResponse(peer)
            result.begin()
            self.assertEqual(result.read(), b'ok')
        _, fields, body = self.backend.requests.get(timeout=2)
        self.assertNotIn(b'expect', dict(fields))
        self.assertEqual(body, b'abc')

    def test_root_rule_cannot_override_encoded_cgi(self):
        self.config.write_text(f'/ 127.0.0.1 {self.backend.port}\n')
        self.launch()
        self.assertEqual(self.exchange('/')[2], b'ok')
        self.assertEqual(self.exchange('/%63gi-bin/adder?a=2&b=3')[2], b'5\n')
        self.assertEqual(self.exchange('//cgi-bin/missing')[0], 404)

    def test_connection_refused_no_static_fallback(self):
        probe = socket.socket()
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
        probe.close()
        self.config.write_text(f'/api 127.0.0.1 {port}\n')
        self.launch()
        self.assertEqual(self.exchange()[0], 502)

    def test_concurrent_requests_and_fd_recovery(self):
        self.launch()
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            self.assertEqual(list(pool.map(lambda _: self.exchange()[2], range(40))), [b'ok'] * 40)
        wait_for(lambda: not children(self.proc.pid))

    def test_header_timeout_is_fixed_even_with_trickling_bytes(self):
        def trickle(peer):
            peer.recv(32768)
            until = time.monotonic() + 12
            while time.monotonic() < until:
                peer.sendall(b'H')
                time.sleep(.4)
        self.backend.handler = trickle
        self.launch()
        start = time.monotonic()
        self.assertEqual(self.exchange()[0], 504)
        self.assertTrue(9 <= time.monotonic() - start < 13)

    def test_body_timeout_after_headers_only_closes_and_logs(self):
        def stalled(peer):
            peer.recv(32768)
            peer.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabc')
            self.assertEqual(peer.recv(1), b'')
        self.backend.handler = stalled
        self.launch()
        start = time.monotonic()
        with connect(self.port) as peer:
            peer.settimeout(14)
            peer.sendall(b'GET /api HTTP/1.1\r\nHost: example.com\r\n\r\n')
            data = b''
            while piece := peer.recv(4096):
                data += piece
        self.assertTrue(9 <= time.monotonic() - start < 13)
        self.assertEqual(data.split(b'\r\n\r\n')[1], b'abc')
        self.shutdown()
        self.assertIn('phase=response_body status=504', self.logs)

    def test_early_final_response_during_post_does_not_deadlock(self):
        def early(peer):
            peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            header = b''
            while not header.endswith(b'\r\n\r\n'):
                header += peer.recv(1)
            peer.sendall(b'HTTP/1.1 413 Too Large\r\nContent-Length: 0\r\n\r\n')
            # Do not read the request body. Leave time for the proxy to read the
            # rejection before closing a socket with unread inbound bytes.
            time.sleep(.5)
        self.backend.handler = early
        self.launch()
        start = time.monotonic()
        self.assertEqual(self.exchange(method='POST', body=b'x' * 1048576)[0], 413)
        self.assertLess(time.monotonic() - start, 5)

    def test_slow_client_does_not_reset_backend_receive_timeout(self):
        # First pause belongs to downstream backpressure, then backend gets a new
        # receive window. Miscounting downstream time as upstream idle breaks this.
        size = 8 * 1024 * 1024
        self.backend.reply = f'HTTP/1.1 200 OK\r\nContent-Length: {size}\r\n\r\n'.encode() + b'z' * size
        self.launch()
        with connect(self.port) as peer:
            peer.sendall(b'GET /api HTTP/1.1\r\nHost: example.com\r\n\r\n')
            stream = peer.makefile('rb')
            with stream:
                self.assertTrue(stream.readline().startswith(b'HTTP/1.1 200'))
                while stream.readline() != b'\r\n':
                    pass
                for _ in range(size // 262144):
                    self.assertEqual(stream.read(262144), b'z' * 262144)
                    time.sleep(.4)

    def test_client_reset_stops_upstream_and_reaps_child(self):
        self.backend.reply = b'HTTP/1.1 200 OK\r\nContent-Length: 8388608\r\n\r\n' + b'z' * 8388608
        self.launch()
        with connect(self.port) as peer:
            peer.sendall(b'GET /api HTTP/1.1\r\nHost: example.com\r\n\r\n')
            self.assertTrue(peer.recv(128).startswith(b'HTTP/1.1 200'))
            import struct
            peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        wait_for(lambda: not children(self.proc.pid))

    def test_backend_error_status_is_preserved(self):
        self.backend.reply = b'HTTP/1.1 500 Internal Error\r\nContent-Length: 4\r\n\r\noops'
        self.launch()
        self.assertEqual((self.exchange()[0], self.exchange()[2]), (500, b'oops'))

    def test_invalid_config_fails_with_line_number(self):
        cases = ['/api 127.0.0.1 0', '/api localhost 8000', '/api/ 127.0.0.1 8000',
                 '/api?x 127.0.0.1 8000', '/cgi-bin 127.0.0.1 8000',
                 '/cgi-bin/x 127.0.0.1 8000', '/api 127.0.0.1 8000 extra',
                 '/api 127.0.0.1 8000\n/api 127.0.0.1 8001']
        for text in cases:
            self.config.write_text('# comment\n' + text + '\n')
            result = subprocess.run([str(BINARY), str(self.port), str(self.root),
                                     '--routes', str(self.config)], capture_output=True, timeout=3)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'line', result.stderr)

    def test_config_rejects_embedded_nul(self):
        self.config.write_bytes(f'/api 127.0.0.1 {self.backend.port}'.encode() + b'\x00 hidden\n')
        result = subprocess.run([str(BINARY), str(self.port), str(self.root),
                                 '--routes', str(self.config)], capture_output=True, timeout=2)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'line', result.stderr)


if __name__ == '__main__':
    unittest.main()
