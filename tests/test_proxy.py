"""Real TCP peers verify forwarding, framing, failure and resource ownership."""
import concurrent.futures
import errno
import http.client
import os
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

    def launch(self, *options, fd_limit=None, max_children=16, idle_seconds=2):
        def restrict_fds():
            import resource
            resource.setrlimit(resource.RLIMIT_NOFILE, (fd_limit, fd_limit))
        self.proc = subprocess.Popen(
            [str(BINARY), str(self.port), str(self.root), str(max_children), str(idle_seconds), '1',
             '--routes', str(self.config), *options], cwd=ROOT,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            preexec_fn=restrict_fds if fd_limit is not None else None)
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

    def test_parent_survives_fd_pressure_and_recovers_without_slot_or_fd_leaks(self):
        self.launch('--backend-limit', '1', fd_limit=32, max_children=64, idle_seconds=30)
        keeper = http.client.HTTPConnection('127.0.0.1', self.port, timeout=3)
        self.addCleanup(keeper.close)
        keeper.request('GET', '/api')
        self.assertEqual(keeper.getresponse().read(), b'ok')
        keeper.request('GET', '/')
        self.assertEqual(keeper.getresponse().read(), b'local')
        original_workers = children(self.proc.pid)
        original_fds = len(os.listdir(f'/proc/{self.proc.pid}/fd'))
        for _ in range(2):
            holders = []
            try:
                for _ in range(36):
                    try:
                        holders.append(connect(self.port))
                    except ConnectionRefusedError:
                        self.fail('fd pressure closed the server listener')
                    time.sleep(.01)
                time.sleep(.15)
                self.assertIsNone(self.proc.poll(), 'fd pressure must not stop the server')
                # Admission/release IPC must work while accepting new clients is paused.
                keeper.request('GET', '/api')
                self.assertEqual(keeper.getresponse().read(), b'ok')
            finally:
                for peer in holders:
                    peer.close()
            wait_for(lambda: children(self.proc.pid) == original_workers)
            self.assertEqual(len(os.listdir(f'/proc/{self.proc.pid}/fd')), original_fds)
            self.assertEqual(self.exchange()[2], b'ok')
            wait_for(lambda: children(self.proc.pid) == original_workers)
        keeper.close()
        wait_for(lambda: not children(self.proc.pid))
        self.shutdown()
        self.assertIn('socketpair control:', self.logs)
        self.assertLessEqual(self.logs.count('socketpair control:'), 2)

    def logged_failures(self):
        self.shutdown()
        entries = []
        for line in self.logs.splitlines():
            self.assertTrue(line.startswith('proxy '), line)
            fields = dict(field.split('=', 1) for field in line.split()[1:])
            self.assertTrue({'phase', 'status', 'errno', 'expected', 'received', 'sent',
                             'backend', 'route', 'elapsed_ms', 'reason'} <= fields.keys(), line)
            self.assertGreaterEqual(float(fields['elapsed_ms']), 0)
            entries.append(fields)
        return entries

    def test_failure_logs_identify_backend_reason_and_exclude_payload(self):
        self.launch()
        cases = [
            (b'HTTP/1.1 200 OK\r\nBad Header\r\n\r\n', 'invalid_response_header'),
            (b'HTTP/1.9 200 OK\r\nContent-Length: 0\r\n\r\n', 'invalid_status_line'),
            (b'HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n', 'invalid_content_length'),
            (b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n',
             'unsupported_transfer_encoding'),
        ]
        for reply, _ in cases:
            self.backend.reply = reply
            self.assertEqual(self.exchange('/api/private?token=query-secret', 'POST',
                                           b'body-secret', {'Host': 'example.com',
                                           'Authorization': 'Bearer header-secret'})[0], 502)
        self.other.reply = b'HTTP/1.1 200 OK\r\nX: upstream-secret\r\nBad Header\r\n\r\n'
        self.assertEqual(self.exchange('/api/admin/private')[0], 502)
        logs = self.logged_failures()
        self.assertEqual(len(logs), 5)
        self.assertEqual([entry['reason'] for entry in logs[:4]], [reason for _, reason in cases])
        for entry in logs[:4]:
            self.assertEqual(entry['backend'], f'127.0.0.1:{self.backend.port}')
            self.assertEqual(entry['route'], '/api')
            self.assertEqual(entry['errno'], '0')
        self.assertEqual(logs[-1]['backend'], f'127.0.0.1:{self.other.port}')
        self.assertEqual(logs[-1]['route'], '/api/admin')
        for secret in ('query-secret', 'body-secret', 'header-secret', 'upstream-secret', 'private'):
            self.assertNotIn(secret, self.logs)

    def test_concurrent_failures_log_once_and_measure_wait(self):
        def delayed_invalid(peer):
            peer.recv(32768)
            time.sleep(.1)
            peer.sendall(b'HTTP/1.1 200 OK\r\nBad Header\r\n\r\n')
        self.backend.handler = delayed_invalid
        self.launch()
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            self.assertEqual(list(pool.map(lambda _: self.exchange()[0], range(8))), [502] * 8)
        entries = self.logged_failures()
        self.assertEqual(len(entries), 8)
        for entry in entries:
            self.assertEqual(entry['reason'], 'invalid_response_header')
            self.assertEqual(entry['errno'], '0')
            self.assertTrue(80 <= float(entry['elapsed_ms']) < 5000)

    def test_failure_log_bounds_configured_route(self):
        prefix = '/' + 'a' * 600
        self.config.write_text(f'{prefix} 127.0.0.1 {self.backend.port}\n')
        self.backend.reply = b'HTTP/1.1 200 OK\r\nBad Header\r\n\r\n'
        self.launch()
        self.assertEqual(self.exchange(prefix + '/private')[0], 502)
        entries = self.logged_failures()
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]['route'], prefix[:256])
        self.assertEqual(entries[0]['route_truncated'], '1')
        self.assertLess(len(self.logs.encode()), 1024)

    def test_backend_limit_isolates_services_and_shares_endpoint_limit(self):
        entered = threading.Event()
        release = threading.Event()
        def stalled(peer):
            peer.recv(32768)
            peer.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\no')
            entered.set()
            release.wait(4)
            peer.sendall(b'k')
        self.backend.handler = stalled
        self.addCleanup(release.set)
        with self.config.open('a') as config:
            config.write(f'/shared 127.0.0.1 {self.backend.port}\n')
        self.launch('--backend-limit', '1')
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            first = pool.submit(self.exchange)
            try:
                self.assertTrue(entered.wait(2))
                start = time.monotonic()
                status, headers, body = self.exchange('/shared')
                self.assertEqual(status, 503)
                self.assertIn(('Connection', 'close'), headers)
                self.assertIn(b'Service Unavailable', body)
                self.assertLess(time.monotonic() - start, 2)
                self.assertEqual(self.exchange('/api/admin')[2], b'other')
                self.assertEqual(self.exchange('/')[2], b'local')
                self.assertEqual(self.exchange('/cgi-bin/adder?a=1&b=2')[2], b'3\n')
            finally:
                release.set()
            self.assertEqual(first.result(timeout=3)[2], b'ok')
        self.backend.handler = None
        self.assertEqual(self.exchange('/shared')[2], b'ok')
        entries = self.logged_failures()
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]['reason'], 'backend_limit')
        self.assertEqual(entries[0]['status'], '503')
        self.assertEqual(entries[0]['errno'], '0')
        self.assertEqual(entries[0]['route'], '/shared')

    def test_backend_limit_reclaims_killed_worker_and_client_cancellation(self):
        import signal
        import struct
        entered = threading.Event()
        def stalled(peer):
            peer.recv(32768)
            entered.set()
            peer.recv(1)
        self.backend.handler = stalled
        self.launch('--backend-limit', '1')
        for termination in ('kill', 'reset'):
            with self.subTest(termination=termination):
                entered.clear()
                with connect(self.port) as peer:
                    peer.sendall(b'GET /api HTTP/1.1\r\nHost: example.com\r\n\r\n')
                    self.assertTrue(entered.wait(2))
                    self.assertEqual(self.exchange()[0], 503)
                    if termination == 'kill':
                        wait_for(lambda: len(children(self.proc.pid)) == 1)
                        worker, = children(self.proc.pid)
                        os.kill(int(worker), signal.SIGKILL)
                    else:
                        peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                wait_for(lambda: not children(self.proc.pid))
                self.backend.handler = None
                self.assertEqual(self.exchange()[2], b'ok')
                self.backend.handler = stalled

    def test_backend_limit_cli_validation(self):
        for options in [('--backend-limit',), ('--backend-limit', '0'),
                        ('--backend-limit', '-1'), ('--backend-limit', '4097'),
                        ('--backend-limit', 'bad'),
                        ('--backend-limit', '1', '--backend-limit', '2'),
                        ('--routes', str(self.config)), ('--unknown', '1')]:
            with self.subTest(options=options):
                result = subprocess.run([str(BINARY), str(self.port), str(self.root),
                                         '--routes', str(self.config), *options],
                                        capture_output=True, timeout=3)
                self.assertNotEqual(result.returncode, 0)

    def test_backend_limit_option_before_routes(self):
        self.proc = subprocess.Popen(
            [str(BINARY), str(self.port), str(self.root), '1', '2', '1',
             '--backend-limit', '1',
             '--routes', str(self.config)], cwd=ROOT,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(self.shutdown)
        self.assertTrue(self.proc.stdout.readline().startswith('READY'))
        self.assertEqual(self.exchange()[2], b'ok')

    def test_backend_limit_releases_between_keepalive_requests_and_failures(self):
        self.launch('--backend-limit', '1')
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=4)
        self.addCleanup(conn.close)
        conn.request('GET', '/api')
        self.assertEqual(conn.getresponse().read(), b'ok')
        # A local request on this connection completes only after the release ACK.
        conn.request('GET', '/')
        self.assertEqual(conn.getresponse().read(), b'local')
        self.assertEqual(self.exchange()[2], b'ok')
        conn.request('GET', '/api/admin')
        self.assertEqual(conn.getresponse().read(), b'other')
        self.backend.reply = b'HTTP/1.1 200 OK\r\nBad Header\r\n\r\n'
        self.assertEqual(self.exchange()[0], 502)
        self.backend.reply = b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok'
        self.assertEqual(self.exchange()[2], b'ok')

    def test_backend_limit_honors_two_slots_under_concurrent_load(self):
        entered = queue.Queue()
        release = threading.Event()
        def stalled(peer):
            peer.recv(32768)
            entered.put(True)
            release.wait(4)
            peer.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok')
        self.backend.handler = stalled
        self.addCleanup(release.set)
        self.launch('--backend-limit', '2')
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            first = [pool.submit(self.exchange) for _ in range(2)]
            try:
                for _ in first:
                    entered.get(timeout=2)
                rejected = list(pool.map(lambda _: self.exchange()[0], range(6)))
                self.assertEqual(rejected, [503] * 6)
                self.assertTrue(entered.empty())
            finally:
                release.set()
            self.assertEqual([result.result(timeout=3)[2] for result in first], [b'ok'] * 2)

    def test_parent_shutdown_services_pending_slot_release(self):
        entered = threading.Event()
        release = threading.Event()
        def stalled(peer):
            peer.recv(32768)
            entered.set()
            release.wait(4)
            peer.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok')
        self.backend.handler = stalled
        self.addCleanup(release.set)
        self.launch('--backend-limit', '1')
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            result = pool.submit(self.exchange)
            try:
                self.assertTrue(entered.wait(2))
                self.proc.terminate()
                time.sleep(.05)
            finally:
                release.set()
            self.assertEqual(result.result(timeout=3)[2], b'ok')
        self.shutdown()
        self.assertNotIn('proxy phase=', self.logs)

    def test_routes_longest_boundary_and_local_fallback(self):
        self.launch()
        self.assertEqual(self.exchange()[2], b'ok')
        self.assertEqual(self.exchange('/api/admin/users')[2], b'other')
        self.assertEqual(self.exchange('/')[2], b'local')
        self.assertEqual(self.exchange('/apix')[0], 404)
        self.assertEqual(self.exchange('/cgi-bin/adder?a=1&b=2')[2], b'3\n')

    def test_rejects_ambiguous_proxy_paths_before_forwarding(self):
        self.launch()
        paths = ['/api/admin/../users', '/api/./admin/users', '/api/.', '/api/..',
                 '/api/%2e/admin', '/api/admin/%2E%2e/users', '/api//admin/users',
                 '/api%2Fadmin/users', '/api%2fadmin/users',
                 '/api/%252e%252e/admin', '/api/a%3fb', '/api/a%23b']
        for path in paths:
            with self.subTest(path=path):
                status, headers, _ = self.exchange(path)
                self.assertEqual(status, 400)
                self.assertIn(('Connection', 'close'), headers)
        self.assertTrue(self.backend.requests.empty())
        self.assertTrue(self.other.requests.empty())

    def test_preserves_unambiguous_encoded_proxy_paths(self):
        self.launch()
        for path in ['/api/', '/api/admin/', '/%61pi/users',
                     '/api/v1.2/a%20b?next=%2Fapi%2F..%2Fx', '/api/.well-known']:
            with self.subTest(path=path):
                self.assertEqual(self.exchange(path)[0], 200)

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
        entries = self.logged_failures()
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]['reason'], 'early_eof')
        self.assertEqual(entries[0]['errno'], '0')

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
        entries = self.logged_failures()
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]['phase'], 'connect')
        self.assertEqual(entries[0]['reason'], 'connect_failed')
        self.assertEqual(int(entries[0]['errno']), errno.ECONNREFUSED)
        self.assertEqual(entries[0]['backend'], f'127.0.0.1:{port}')

    def test_concurrent_requests_and_fd_recovery(self):
        self.launch()
        initial_fds = len(os.listdir(f'/proc/{self.proc.pid}/fd'))
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            self.assertEqual(list(pool.map(lambda _: self.exchange()[2], range(40))), [b'ok'] * 40)
        wait_for(lambda: not children(self.proc.pid))
        self.assertEqual(len(os.listdir(f'/proc/{self.proc.pid}/fd')), initial_fds)

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
        entries = self.logged_failures()
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]['reason'], 'upstream_timeout')
        self.assertEqual(entries[0]['errno'], '0')
        self.assertTrue(9000 <= float(entries[0]['elapsed_ms']) < 13000)

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
        entries = self.logged_failures()
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]['reason'], 'upstream_timeout')
        self.assertEqual(entries[0]['errno'], '0')

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

    def test_client_reset_cancels_stalled_backend(self):
        import struct
        for phase in ('header', 'body'):
            with self.subTest(phase=phase):
                waiting = threading.Event()
                closed = threading.Event()

                def stalled(peer):
                    header = b''
                    while not header.endswith(b'\r\n\r\n'):
                        header += peer.recv(1)
                    if phase == 'body':
                        peer.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n')
                    waiting.set()
                    if peer.recv(1) == b'':
                        closed.set()

                self.backend.handler = stalled
                if not self.proc:
                    self.launch()
                with connect(self.port) as peer:
                    peer.sendall(b'GET /api HTTP/1.1\r\nHost: example.com\r\n\r\n')
                    self.assertTrue(waiting.wait(2))
                    if phase == 'body':
                        response = http.client.HTTPResponse(peer)
                        response.begin()
                        self.assertEqual(response.status, 200)
                        response.close()
                    peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                self.assertTrue(closed.wait(2), 'backend must close before its 10s timeout')
                wait_for(lambda: not children(self.proc.pid), seconds=2)
        self.shutdown()
        self.assertIn('phase=response_header status=0', self.logs)
        self.assertIn('phase=response_body status=0', self.logs)
        entries = self.logged_failures()
        self.assertEqual(len(entries), 2)
        for entry in entries:
            self.assertEqual(entry['reason'], 'client_cancelled')
            self.assertEqual(entry['errno'], '0')

    def test_write_half_close_keeps_delayed_backend_response(self):
        def delayed(peer):
            header = b''
            while not header.endswith(b'\r\n\r\n'):
                header += peer.recv(1)
            time.sleep(.1)
            peer.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok')
        self.backend.handler = delayed
        self.launch()
        with connect(self.port) as peer:
            peer.sendall(b'GET /api HTTP/1.1\r\nHost: example.com\r\n\r\n')
            peer.shutdown(socket.SHUT_WR)
            result = http.client.HTTPResponse(peer)
            result.begin()
            self.assertEqual((result.status, result.read()), (200, b'ok'))

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
        self.assertEqual(self.logged_failures(), [])

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
