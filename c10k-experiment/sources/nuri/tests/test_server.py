import concurrent.futures
import os
from pathlib import Path
import socket
import statistics
import struct
import subprocess
import time
import unittest

from support import (BINARY, ServerTestCase, StrictHTTPResponse, bad_request,
                     children, connect, pipeline_response, receive_exact,
                     request, response, response_headers, stop, wait_for)


class ServerTests(ServerTestCase):
    def setUp(self):
        super().setUp()
        (self.root / 'index.html').write_bytes(b'<h1>Hello</h1>\n')
        self.proc = self.start_server()

    def write_binary_fixture(self):
        self.binary = bytes(range(256)) * 4096
        (self.root / 'image.png').write_bytes(self.binary)

    def write_large_file(self):
        with (self.root / 'large.bin').open('wb') as large:
            large.truncate(64 * 1024 * 1024)

    def prepare_unsafe_paths(self):
        (self.root / '.env').write_text('SECRET')
        (self.root / 'docs').mkdir()
        (self.root / 'docs/index.html').write_bytes(b'docs')
        (self.root / 'empty').mkdir()
        (self.root / '.hidden').mkdir()
        (self.root / '.hidden/index.html').write_bytes(b'hidden')
        (self.work / 'secret.txt').write_bytes(b'secret')
        (self.root / 'outside').symlink_to(self.work, target_is_directory=True)
        (self.root / 'inside').symlink_to(self.root / 'docs', target_is_directory=True)
        (self.root / 'link.txt').symlink_to(self.root / 'index.html')
        (self.root / 'linked-index').mkdir()
        (self.root / 'linked-index/index.html').symlink_to(self.work / 'secret.txt')
        (self.root / 'fifo-index').mkdir()
        os.mkfifo(self.root / 'fifo-index/index.html')
        os.mkfifo(self.root / 'pipe')

    def test_rejects_an_occupied_port(self):
        assert subprocess.run([str(BINARY), str(self.port), str(self.root)], capture_output=True).returncode != 0

    def test_serves_index_html(self):
        assert request(self.port)[1] == b'<h1>Hello</h1>\n'

    def test_ignores_query_for_static_file(self):
        (self.root / 'style.css').write_bytes(b'body { color: navy; }')
        assert request(self.port, '/style.css?v=2')[0]['Content-Type'].startswith('text/css')

    def test_serves_binary_bytes(self):
        self.write_binary_fixture()
        assert request(self.port, '/image.png')[1] == self.binary

    def test_maps_mime_types(self):
        mime_cases = [('file.PNG', 'image/png'), ('file.jpg', 'image/jpeg'),
                      ('file.js', 'text/javascript; charset=utf-8'),
                      ('file.unknown', 'application/octet-stream')]
        for name, content_type in mime_cases:
            with self.subTest(case=(name, content_type)):
                (self.root / name).write_bytes(b'mime fixture')
                headers, body = request(self.port, '/' + name)
                assert headers['Content-Type'] == content_type and body == b'mime fixture'

    def test_decodes_path_once(self):
        (self.root / 'space name.txt').write_bytes('안녕'.encode())
        (self.root / 'percent%20.txt').write_bytes(b'decode once')
        assert request(self.port, '/space%20name.txt')[1] == '안녕'.encode()
        assert request(self.port, '/percent%2520.txt')[1] == b'decode once'

    def test_serves_directory_index(self):
        (self.root / 'docs').mkdir()
        (self.root / 'docs/index.html').write_bytes(b'docs')
        assert request(self.port, '/docs/')[1] == b'docs'
        assert request(self.port, '/docs')[1] == b'docs'

    def test_denies_unsafe_paths(self):
        self.prepare_unsafe_paths()
        for target in ['/empty/', '/.env', '/.hidden/', '/outside/secret.txt', '/inside/',
                       '/link.txt', '/../secret.txt', '/%2e%2e/secret.txt',
                       '/missing/../index.html', '/pipe', '/index.html/',
                       '/%2Eenv', '/%2e%2e%2fsecret.txt', '/docs/%2e%2e/index.html',
                       '//outside//secret.txt', '/./index.html', '/linked-index/', '/fifo-index/']:
            with self.subTest(case=target):
                request(self.port, target, 403)

    def test_returns_404_for_missing_file(self):
        request(self.port, '/missing', 404)

    def test_rejects_invalid_encoded_paths(self):
        for target in ['/%', '/%xx', '/%00', '/%5csecret', '/index.html#part',
                       '/%0d%0aInjected:yes', '/%7f']:
            with self.subTest(case=target):
                request(self.port, target, 400)

    def test_receives_every_header_split(self):
        header = b'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'
        for split in range(1, len(header)):
            with self.subTest(case=split):
                with connect(self.port) as client:
                    client.sendall(header[:split])
                    time.sleep(.002)
                    client.sendall(header[split:])
                    status, _, body, _ = response(client)
                    assert status == 200 and body == b'<h1>Hello</h1>\n'

    def test_receives_fragmented_request(self):
        with connect(self.port) as client:
            for piece in [b'GET / HT', b'TP/1.1\r\nHost: local', b'host\r\n\r', b'\n']:
                client.sendall(piece)
                time.sleep(.01)
            assert response(client)[0] == 200

    def test_reuses_connection_without_fd_leaks(self):
        with connect(self.port) as client:
            for _ in range(105):
                client.sendall(b'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n')
                assert response(client)[0] == 200
            wait_for(lambda: len(children(self.proc.pid)) == 1)
            worker = children(self.proc.pid)[0]
            # stdin/stdout/stderr, the borrowed root, and the client remain open.
            wait_for(lambda: len(list(Path(f'/proc/{worker}/fd').iterdir())) == 5)

    def test_small_keepalive_responses_avoid_delayed_ack_wait(self):
        import http.client
        payload = b'x' * 1024
        (self.root / 'small.txt').write_bytes(payload)
        client = http.client.HTTPConnection('127.0.0.1', self.port, timeout=3)
        self.addCleanup(client.close)
        samples = []
        for i in range(14):
            start = time.monotonic()
            client.request('GET', '/small.txt')
            result = client.getresponse()
            self.assertEqual((result.status, result.read()), (200, payload))
            if i >= 2:
                samples.append(time.monotonic() - start)
        median = statistics.median(samples)
        self.assertLess(median, .02, f'keep-alive median {median * 1000:.1f}ms')

    def test_close_token_overrides_keep_alive(self):
        with connect(self.port) as client:
            client.sendall(b'GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive, close\r\n\r\n')
            assert response(client)[1]['Connection'] == 'close'
            assert client.recv(1) == b''

    def test_closes_http10_connection(self):
        with connect(self.port) as client:
            client.sendall(b'GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n')
            status, headers, _, version = response(client)
            assert status == 200 and version == 10 and headers['Connection'] == 'close'
            assert client.recv(1) == b''

    def test_preserves_request_after_post_body(self):
        with connect(self.port) as client:
            client.sendall(b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\n\x00abc'
                           b'GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
            # A shared reader preserves read-ahead bytes of the next response.
            stream = client.makefile('rb')
            for expected, expected_body, connection in [
                (501, b'501 Not Implemented\n', 'keep-alive'),
                (200, b'<h1>Hello</h1>\n', 'close'),
            ]:
                status, headers, body = pipeline_response(stream)
                assert status == expected and body == expected_body
                assert headers['Connection'] == connection
            assert stream.read(1) == b''
            stream.close()

    def test_preserves_request_after_get_body(self):
        # GET bodies must also be consumed without eating the next request.
        (self.root / 'docs').mkdir()
        (self.root / 'docs/index.html').write_bytes(b'docs')
        with connect(self.port) as client:
            client.sendall(b'GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\n\x00abc'
                           b'GET /docs/ HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')
            with client.makefile('rb') as stream:
                for expected, connection in [(b'<h1>Hello</h1>\n', 'keep-alive'),
                                             (b'docs', 'close')]:
                    status, headers, body = pipeline_response(stream)
                    assert status == 200 and body == expected
                    assert headers['Connection'] == connection
                assert stream.read(1) == b''

    def test_sends_continue_before_receiving_body(self):
        with connect(self.port) as client:
            client.sendall(b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\n')
            assert receive_exact(client, 25) == b'HTTP/1.1 100 Continue\r\n\r\n'
            client.sendall(b'abc')
            assert response(client)[0] == 501

    def test_head_error_has_no_body(self):
        with connect(self.port) as client:
            client.sendall(b'HEAD / HTTP/1.1\r\nHost: x\r\n\r\n')
            result = StrictHTTPResponse(client, method='HEAD')
            result.begin()
            response_headers(result.getheaders())
            assert result.status == 405 and result.getheader('Allow') == 'GET, POST'
            assert result.read() == b'' and client.recv(1) == b''

    def test_rejects_invalid_headers(self):
        base = b'POST / HTTP/1.1\r\nHost: localhost\r\n'
        for headers, status in [
            (b'Content-Length: -1\r\n', 400), (b'Content-Length: x\r\n', 400),
            (b'Content-Length: 99999999999999999999999999\r\n', 400),
            (b'Content-Length: 0\r\nContent-Length: 0\r\n', 400),
            (b'Transfer-Encoding: chunked\r\nContent-Length: 0\r\n', 400),
            (b'Transfer-Encoding: chunked\r\n', 501),
            (b'Content-Length: 1048577\r\n', 413),
            (b'Host: second\r\n', 400), (b'Bad Header: value\r\n', 400),
            (b'X: \x00\r\n', 400), (b'Expect: unknown\r\n', 417),
            (b'Content-Length : 0\r\n', 400),
            (b'Content-Length:\t+1\r\n', 400),
            (b'Content-Length: 0\r\ncontent-length: 0\r\n', 400),
            (b'Transfer-Encoding: chunked\r\n transfer-encoding: identity\r\n', 400),
            (b'Connection: close,\r\n', 400),
            (b'X: a\rb\r\n', 400),
        ]:
            with self.subTest(case=(headers, status)):
                bad_request(self.port, base + headers + b'\r\n', status)

    def test_rejects_incomplete_body(self):
        base = b'POST / HTTP/1.1\r\nHost: localhost\r\n'
        bad_request(self.port, base + b'Content-Length: 3\r\n\r\na', 400)

    def test_requires_host_for_http11(self):
        bad_request(self.port, b'GET / HTTP/1.1\r\n\r\n', 400)

    def test_rejects_invalid_or_unsupported_version(self):
        for version, status in [('2.0', 505), ('no', 400)]:
            with self.subTest(version=version):
                bad_request(self.port, f'GET / HTTP/{version}\r\n\r\n'.encode(), status)

    def test_rejects_malformed_request_line(self):
        for name, payload in [
            ('double_space', b'GET  / HTTP/1.1\r\nHost: x\r\n\r\n'),
            ('truncated', b'GET / HTTP/1.1\r\nHost: x\r\n\r\n'[:10]),
        ]:
            with self.subTest(case=name):
                bad_request(self.port, payload, 400)

    def test_does_not_process_request_after_ambiguous_length(self):
        bad_request(self.port, b'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n'
                          b'Content-Length: 0\r\n\r\n'
                          b'GET / HTTP/1.1\r\nHost: x\r\n\r\n', 400)

    def test_rejects_uri_over_limit(self):
        bad_request(self.port, b'GET /' + b'a' * 8192 + b' HTTP/1.1\r\nHost: x\r\n\r\n', 414)

    def test_rejects_header_over_limit(self):
        bad_request(self.port, b'GET / HTTP/1.1\r\nHost: x\r\nX: ' + b'a' * 32768 + b'\r\n\r\n', 431)

    def test_accepts_uri_limit(self):
        # The limits themselves are accepted; only values above them are rejected.
        request(self.port, '/' + 'a' * 8191, 403)

    def test_accepts_header_limit(self):
        header = b'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\nX: '
        bad_request(self.port, header + b'a' * (32768 - len(header) - 4) + b'\r\n\r\n', 200)

    def test_accepts_body_limit(self):
        self.write_binary_fixture()
        base = b'POST / HTTP/1.1\r\nHost: localhost\r\n'
        with connect(self.port) as client:
            client.sendall(base + b'Content-Length: 1048576\r\nConnection: close\r\n\r\n')
            client.sendall(self.binary)
            assert response(client)[0] == 501

    def test_serves_concurrent_binary_requests(self):
        self.write_binary_fixture()
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            results = list(pool.map(lambda _: request(self.port, '/image.png'), range(24)))
            assert all(body == self.binary for _, body in results)
        wait_for(lambda: not children(self.proc.pid))

    def test_limits_active_workers(self):
        holders = [connect(self.port) for _ in range(4)]
        for client in holders:
            self.addCleanup(client.close)
        wait_for(lambda: len(children(self.proc.pid)) == 4)
        with connect(self.port) as queued:
            queued.sendall(b'GET / HTTP/1.0\r\n\r\n')
            queued.settimeout(.15)
            try:
                queued.recv(1)
                raise AssertionError('worker limit exceeded')
            except socket.timeout:
                pass
            holders.pop().close()
            queued.settimeout(4)
            assert response(queued)[0] == 200
        for client in holders:
            client.close()
        wait_for(lambda: not children(self.proc.pid))

    def test_closes_idle_connection_on_deadline(self):
        with connect(self.port) as idle:
            started = time.monotonic()
            assert idle.recv(1) == b''
            assert 1.8 <= time.monotonic() - started < 4

    def test_survives_connection_reset(self):
        self.write_large_file()
        with connect(self.port) as reset:
            reset.sendall(b'GET /large.bin HTTP/1.1\r\nHost: x\r\n\r\n')
            reset.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        request(self.port)
        wait_for(lambda: not children(self.proc.pid))

    def test_abandons_stalled_response(self):
        self.write_large_file()
        with connect(self.port) as client:
            client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            client.sendall(b'GET /large.bin HTTP/1.1\r\nHost: localhost\r\n\r\n')
            wait_for(lambda: len(children(self.proc.pid)) == 1)
            started = time.monotonic()
            wait_for(lambda: not children(self.proc.pid), seconds=16)
            assert time.monotonic() - started >= 9

    def test_request_progress_does_not_extend_deadline(self):
        with connect(self.port) as client:
            client.settimeout(35)
            client.sendall(b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\nx')
            started = time.monotonic()
            # Receiving progress must not restart the total request timer.
            time.sleep(16)
            client.sendall(b'y')
            assert response(client)[0] == 408
            assert 28 <= time.monotonic() - started < 34


class CliTests(ServerTestCase):
    def test_rejects_invalid_arguments(self):
        # No listener is running: bind failure must not mask CLI validation.
        (self.root / 'inside').symlink_to(self.root, target_is_directory=True)
        for args in [['0'], ['-1'], ['65536'], ['abc'], [str(self.port), str(self.root), '0'],
                     [str(self.port), str(self.root), '4', '0'], [str(self.port), str(self.root), '4', '2', '0'],
                     [str(self.port), str(self.root), '4', '2', '1', 'extra'],
                     [str(self.port), str(self.root / 'missing')], [str(self.port), str(self.root / 'inside')]]:
            with self.subTest(arguments=args):
                result = subprocess.run([str(BINARY), *args], capture_output=True, timeout=4)
                self.assertNotEqual(result.returncode, 0)


class ShutdownTests(ServerTestCase):
    def setUp(self):
        super().setUp()
        (self.root / 'index.html').write_bytes(b'<h1>Hello</h1>\n')

    def test_reaps_workers_after_parent_shutdown(self):
        self.proc = self.start_server(idle=10)
        with connect(self.port) as client:
            client.sendall(b'GET / HTTP/1.1\r\nHost: x\r\n\r\n')
            assert response(client)[0] == 200
            pids = children(self.proc.pid)
            stop(self.proc)
            assert all(not Path(f'/proc/{pid}').exists() for pid in pids)

    def test_handles_process_group_interrupt(self):
        self.proc = self.start_server()
        stop(self.proc, group=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
