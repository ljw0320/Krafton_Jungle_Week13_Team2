import concurrent.futures
import os
from pathlib import Path
import random
import shutil
import signal
import socket
import struct
import subprocess
import time
import unittest

from support import (ROOT, ServerTestCase, children, connect, exchange, pipeline_response,
                     receive_exact, response, stop, wait_for)


def packet(target='/cgi-bin/adder?a=10&b=20', body=None, headers=b'', close=True,
           version='1.1'):
    method = 'POST' if body is not None else 'GET'
    result = f'{method} {target} HTTP/{version}\r\nHost: localhost\r\n'.encode()
    if close:
        result += b'Connection: close\r\n'
    if body is not None:
        result += (f'Content-Length: {len(body)}\r\n'.encode() +
                   b'Content-Type: application/x-www-form-urlencoded\r\n')
    return result + headers + b'\r\n' + (body or b'')


def running(pid):
    try:
        state = Path(f'/proc/{pid}/status').read_text().split('State:', 1)[1].split()[0]
        return state != 'Z'
    except FileNotFoundError:
        return False


class CgiTestCase(ServerTestCase):
    cgi_program = ROOT / 'build/adder'

    def setUp(self):
        super().setUp()
        (self.work / 'build').mkdir()
        (self.root / 'index.html').write_bytes(b'static')
        (self.root / 'cgi-bin').mkdir()
        (self.root / 'cgi-bin/other').write_bytes(b'private program')
        self.executable = self.work / 'build/adder'
        shutil.copy2(self.cgi_program, self.executable)
        self.executable.chmod(0o755)
        self.mode_file = self.work / 'mode'
        self.proc = self.start_server(cwd=self.work)

    def assert_cgi_response(self, data, expected_status=200, expected_body=None, half_close=False,
                        expected_type='text/plain; charset=utf-8'):
        with connect(self.port) as client:
            client.settimeout(8)
            status, headers, body, version = exchange(client, data, half_close)
            self.assertEqual(status, expected_status, body[:80])
            self.assertEqual(headers['Content-Type'], expected_type)
            expected_version = 10 if data.split(b'\r\n', 1)[0].endswith(b'HTTP/1.0') else 11
            self.assertEqual(version, expected_version)
            self.assertEqual(headers['Connection'], 'close')
            self.assertEqual(client.recv(1), b'')
            if expected_body is not None:
                self.assertEqual(body, expected_body)
            return headers, body, version

    def assert_child_first_post_status(self, expected):
        reader, writer = socket.socketpair()
        with reader, writer:
            reader.settimeout(8)
            with subprocess.Popen(
                [str(ROOT / 'build/test-cgi-io'), str(writer.fileno()), 'child-first'],
                pass_fds=(writer.fileno(),), cwd=self.work,
            ) as helper:
                writer.close()
                try:
                    status, _, _, _ = response(reader)
                    self.assertEqual(helper.wait(timeout=8), 0)
                    self.assertEqual(status, expected)
                finally:
                    if helper.poll() is None:
                        helper.kill()


class AdderTests(CgiTestCase):

    def test_get_adds_values(self):
        self.assert_cgi_response(packet(), expected_body=b'30\n')

    def test_post_adds_values(self):
        self.assert_cgi_response(packet('/cgi-bin/adder', b'a=10&b=20'), expected_body=b'30\n')

    def test_post_uses_body_instead_of_query(self):
        self.assert_cgi_response(packet('/cgi-bin/adder?a=999&b=999', b'a=2&b=3'),
                 expected_body=b'5\n')

    def test_decodes_form_fields(self):
        form = '%61=%2B2&b=-3'
        for data in [packet('/cgi-bin/%61dder?' + form),
                     packet('/cgi-bin/%61dder', form.encode())]:
            with self.subTest(request=data):
                self.assert_cgi_response(data, expected_body=b'-1\n')

    def test_supports_http10(self):
        assert self.assert_cgi_response(packet(version='1.0'), expected_body=b'30\n')[2] == 10

    def test_accepts_write_half_close(self):
        self.assert_cgi_response(packet(), expected_body=b'30\n', half_close=True)

    def test_matches_python_arithmetic(self):
        # Python arithmetic supplies an oracle independent of the C adder.
        random_values = random.Random(20261123)
        for _ in range(40):
            a, b = [random_values.randrange(-(2 ** 62), 2 ** 62) for _ in range(2)]
            form = f'a={a}&b={b}'
            expected = f'{a + b}\n'.encode()
            for method, data in [('GET', packet('/cgi-bin/adder?' + form)),
                                 ('POST', packet('/cgi-bin/adder', form.encode()))]:
                with self.subTest(method=method, a=a, b=b):
                    self.assert_cgi_response(data, expected_body=expected)

    def test_accepts_exact_post_limit(self):
        boundary_form = b'a=' + b'0' * (4096 - len(b'a=1&b=2')) + b'1&b=2'
        assert len(boundary_form) == 4096
        self.assert_cgi_response(packet('/cgi-bin/adder', boundary_form), expected_body=b'3\n')

    def test_accepts_signed_integer_boundaries(self):
        for form, result in [
            ('a=-9223372036854775808&b=0', b'-9223372036854775808\n'),
            ('b=9223372036854775807&a=0', b'9223372036854775807\n'),
            ('a=0002&b=-2', b'0\n'),
        ]:
            with self.subTest(case=(form, result)):
                self.assert_cgi_response(packet('/cgi-bin/adder?' + form), expected_body=result)

    def test_rejects_invalid_forms(self):
        bad_forms = ['', 'a=1', 'b=2', 'a=1&a=2&b=3', 'a=1&b=2&b=3',
                     'a=1&b=2&x=3', 'a=1&b=2&',
                     'a=&b=2', 'a=+1&b=2', 'a=%201&b=2', 'a=1x&b=2',
                     'a=%00&b=2', 'a=%xx&b=2', 'a=%&b=2', 'a=%0a1&b=2',
                     'a=1%262&b=3', 'a=9223372036854775808&b=0',
                     'a=-9223372036854775809&b=0',
                     'a=9223372036854775807&b=1', 'a=-9223372036854775808&b=-1']
        for form in bad_forms:
            for method, data in [('GET', packet('/cgi-bin/adder?' + form)),
                                 ('POST', packet('/cgi-bin/adder', form.encode()))]:
                with self.subTest(method=method, form=form):
                    self.assert_cgi_response(data, 400)

    def test_rejects_nul_in_post_body(self):
        self.assert_cgi_response(packet('/cgi-bin/adder', b'a=1\0&b=2'), 400)

    def test_restricts_cgi_routes(self):
        for target, body in [('/cgi-bin/other', None), ('//cgi-bin/other', None),
                             ('/cgi-bin/adder/extra', None), ('/cgi-bin/adder/', b'a=1&b=2')]:
            with self.subTest(target=target, body=body):
                self.assert_cgi_response(packet(target, body), 404)

    def test_rejects_invalid_form_at_limit(self):
        self.assert_cgi_response(packet('/cgi-bin/adder', b'x' * 4096), 400)

    def test_rejects_headers_before_continue_or_body(self):
        # Reject before waiting for the body or sending 100 Continue.
        for header, expected in [
            (b'Content-Length: 4097\r\nContent-Type: application/x-www-form-urlencoded\r\n', 413),
            (b'Content-Length: 7\r\n', 415),
            (b'Content-Length: 7\r\nContent-Type: application/json\r\n', 415),
            (b'Content-Type: x\r\nContent-Type: y\r\n', 400),
        ]:
            with self.subTest(case=(header, expected)):
                self.assert_cgi_response(b'POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\n' + header +
                         b'Expect: 100-continue\r\n\r\n', expected)

    def test_continues_before_fragmented_post_body(self):
        with connect(self.port) as client:
            client.sendall(packet('/cgi-bin/adder', b'a=2&b=3', close=False,
                                  headers=b'Expect: 100-continue\r\n')[:-7])
            assert receive_exact(client, 25) == b'HTTP/1.1 100 Continue\r\n\r\n'
            for piece in [b'a=', b'2&', b'b=3']:
                client.sendall(piece)
            assert response(client)[2] == b'5\n'

    def test_reuses_connection_without_cgi_or_fd_leaks(self):
        with connect(self.port) as client:
            client.sendall(packet(close=False))
            assert response(client)[2] == b'30\n'
            wait_for(lambda: len(children(self.proc.pid)) == 1)
            worker = children(self.proc.pid)[0]
            # Sequential mixed requests must leave neither CGI children nor extra fds.
            for _ in range(20):
                client.sendall(packet(close=False))
                assert response(client)[2] == b'30\n'
                client.sendall(packet('/', close=False))
                assert response(client)[2] == b'static'
                client.sendall(packet('/cgi-bin/adder?a=bad', close=False))
                assert response(client)[0] == 400
            assert not children(worker)
            assert len(list(Path(f'/proc/{worker}/fd').iterdir())) == 5

    def test_pipelines_post_static_and_cgi_requests(self):
        # Each request must refresh its route and preserve the next request's bytes.
        with connect(self.port) as client:
            client.sendall(packet('/cgi-bin/adder', b'a=2&b=3', close=False) +
                           packet('/cgi-bin/missing', b'a=2&b=3', close=False) +
                           packet('/', b'a=2&b=3', close=False) +
                           packet('/', close=False) + packet())
            with client.makefile('rb') as stream:
                for expected_status, expected, connection in [
                    (200, b'5\n', 'keep-alive'),
                    (404, b'404 Not Found\n', 'keep-alive'),
                    (501, b'501 Not Implemented\n', 'keep-alive'),
                    (200, b'static', 'keep-alive'),
                    (200, b'30\n', 'close'),
                ]:
                    status, headers, body = pipeline_response(stream)
                    assert status == expected_status and body == expected
                    assert headers['Connection'] == connection
                assert stream.read(1) == b''

    def test_isolates_concurrent_requests(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(lambda i: self.assert_cgi_response(
                packet(f'/cgi-bin/adder?a={i}&b={i * i}'),
                expected_body=f'{i + i * i}\n'.encode()), range(16)))
        wait_for(lambda: not children(self.proc.pid))

    def test_returns_500_for_missing_executable(self):
        self.executable.unlink()
        self.assert_cgi_response(packet(), 500)

    def test_returns_500_for_non_executable_file(self):
        self.executable.write_text('not executable')
        self.executable.chmod(0o644)
        self.assert_cgi_response(packet(), 500)

    def test_returns_500_for_invalid_exec_format(self):
        self.executable.write_text('not executable')
        self.executable.chmod(0o755)
        self.assert_cgi_response(packet(), 500)

    def test_post_exec_failure_before_input_write_returns_500(self):
        for failure in ['missing', 'permission', 'format']:
            with self.subTest(failure=failure):
                if failure == 'missing':
                    self.executable.unlink()
                else:
                    self.executable.write_text('not executable')
                    self.executable.chmod(0o644 if failure == 'permission' else 0o755)
                self.assert_child_first_post_status(500)


class CgiOutputTests(CgiTestCase):
    cgi_program = ROOT / 'tests/fixtures/cgi_program.py'

    def set_output(self, output):
        self.mode_file.write_text('custom')
        (self.work / 'output').write_bytes(output)

    def test_accepts_supported_status_codes(self):
        for status in [200, 400, 403, 404, 405, 413, 415, 500]:
            with self.subTest(status=status):
                self.set_output(f'Status: {status} Fixture\r\n'
                                'Content-Type: text/plain\r\n\r\nfixture'.encode())
                self.assert_cgi_response(packet(), status, b'fixture', expected_type='text/plain')

    def test_rejects_invalid_cgi_output(self):
        for name, output in [
            ('missing_type', b'Status: 200 OK\r\n\r\nsecret'),
            ('empty_type', b'Content-Type: \r\n\r\nsecret'),
            ('cgi_content_length', b'Content-Type: text/plain\r\nContent-Length: 6\r\n\r\nsecret'),
            ('redirect_header', b'Content-Type: text/plain\r\nLocation: /\r\n\r\nsecret'),
            ('duplicate_status', b'Status: 200 OK\r\nStatus: 400 Bad Request\r\n'
                                 b'Content-Type: text/plain\r\n\r\nsecret'),
            ('unsupported_status', b'Status: 302 Found\r\nContent-Type: text/plain\r\n\r\nsecret'),
            ('invalid_status', b'Status: 20x Invalid\r\nContent-Type: text/plain\r\n\r\nsecret'),
            ('incomplete_header', b'Content-Type: text/plain\r\n'),
        ]:
            with self.subTest(case=name):
                self.set_output(output)
                self.assert_cgi_response(packet(), 502)

    def test_rejects_content_type_over_limit(self):
        self.set_output(b'Content-Type: ' + b'x' * 128 + b'\r\n\r\nsecret')
        self.assert_cgi_response(packet(), 502)

    def test_rejects_output_over_limit(self):
        header = b'Content-Type: text/plain\r\n\r\n'
        self.set_output(header + b'x' * (65537 - len(header)))
        self.assert_cgi_response(packet(), 502)

    def test_accepts_exact_content_type_limit(self):
        self.set_output(b'Content-Type: ' + b'x' * 127 + b'\r\n\r\nbody')
        self.assert_cgi_response(packet(), 200, b'body', expected_type='x' * 127)

    def test_kills_descendants_after_success(self):
        self.mode_file.write_text('descendant')
        try:
            self.assert_cgi_response(packet(), 200, b'clean', expected_type='text/plain')
            descendant = int((self.work / 'descendant.pid').read_text())
            # Orphan zombies belong to init; the server must stop execution.
            wait_for(lambda: not running(descendant))
        finally:
            pid_file = self.work / 'descendant.pid'
            if pid_file.exists():
                descendant = int(pid_file.read_text())
                if running(descendant):
                    os.kill(descendant, signal.SIGKILL)

    def test_rejects_abnormal_program_exit(self):
        for mode in ['nonzero', 'signal']:
            with self.subTest(mode=mode):
                self.mode_file.write_text(mode)
                self.assert_cgi_response(packet(), 502)
                wait_for(lambda: not children(self.proc.pid))

    def test_post_program_exit_before_input_write_returns_502(self):
        for mode in ['nonzero', 'signal']:
            with self.subTest(mode=mode):
                self.mode_file.write_text(mode)
                self.assert_child_first_post_status(502)

    def test_post_incomplete_input_is_not_success_even_with_valid_output(self):
        self.set_output(b'Content-Type: text/plain\r\n\r\nfalse success')
        self.assert_child_first_post_status(502)

    def test_rejects_malformed_program_output(self):
        for mode in ['invalid', 'duplicate', 'injection', 'nul',
                     'header-overflow', 'overflow']:
            with self.subTest(case=mode):
                self.mode_file.write_text(mode)
                self.assert_cgi_response(packet(), 502)
                wait_for(lambda: not children(self.proc.pid))

    def test_honors_lf_status_header(self):
        self.mode_file.write_text('status')
        self.assert_cgi_response(packet(), 400, b'invalid', expected_type='text/plain')

    def test_preserves_binary_output(self):
        self.mode_file.write_text('binary')
        self.assert_cgi_response(packet(), 200, b'\x00\xff', expected_type='application/octet-stream')

    def test_accepts_exact_output_limit(self):
        self.mode_file.write_text('limit')
        self.assert_cgi_response(packet(), 200, b'x' * (65536 - len(b'Content-Type: text/plain\r\n\r\n')),
                 expected_type='text/plain')

    def test_exchanges_post_body_and_large_output(self):
        self.mode_file.write_text('duplex')
        self.assert_cgi_response(packet('/cgi-bin/adder', b'x' * 4096), 200, b'x' * 64096,
                 expected_type='text/plain')

    def test_drains_both_pipes_under_backpressure(self):
        # Exercise the CGI layer directly: HTTP's 4KiB cap fits in a pipe.
        self.mode_file.write_text('duplex-pressure')
        reader, writer = socket.socketpair()
        with reader, writer:
            reader.settimeout(8)
            with subprocess.Popen([str(ROOT / 'build/test-cgi-io'), str(writer.fileno())],
                                  pass_fds=(writer.fileno(),), cwd=self.work) as helper:
                writer.close()
                try:
                    status, headers, body, _ = response(reader)
                    assert status == 200, (status, body)
                    assert headers['Content-Type'] == 'text/plain'
                    assert body == b'x' * 60000 + b'drained'
                    assert int(headers['Content-Length']) == len(body)
                    assert helper.wait(timeout=8) == 0
                finally:
                    if helper.poll() is None:
                        helper.kill()

    def test_does_not_inherit_socket_or_server_environment(self):
        self.mode_file.write_text('fd-check')
        self.assert_cgi_response(packet(), 200, b'clean', expected_type='text/plain')

    def assert_cgi_deadline(self, mode):
        self.mode_file.write_text(mode)
        started = time.monotonic()
        self.assert_cgi_response(packet(), 504)
        self.assertGreaterEqual(time.monotonic() - started, 4.8)
        self.assertLess(time.monotonic() - started, 7)
        wait_for(lambda: not children(self.proc.pid))

    def test_stops_program_that_ignores_sigterm(self):
        self.assert_cgi_deadline('timeout')

    def test_eof_does_not_end_program_deadline(self):
        self.assert_cgi_deadline('eof-alive')

    def test_output_progress_does_not_extend_deadline(self):
        self.assert_cgi_deadline('trickle')

    def test_cancels_cgi_after_connection_reset(self):
        # Reset cancels a running CGI, including one that ignores SIGTERM.
        self.mode_file.write_text('timeout')
        client = connect(self.port)
        self.addCleanup(client.close)
        client.sendall(packet())
        wait_for(lambda: len(children(self.proc.pid)) == 1)
        worker = children(self.proc.pid)[0]
        wait_for(lambda: bool(children(worker)))
        cgi = children(worker)[0]
        client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        client.close()
        wait_for(lambda: not children(self.proc.pid))
        assert not Path(f'/proc/{cgi}').exists()

    def test_parent_shutdown_reaps_worker_and_cgi(self):
        # Parent-only shutdown must also clean up the CGI grandchild.
        self.mode_file.write_text('timeout')
        client = connect(self.port)
        self.addCleanup(client.close)
        client.sendall(packet())
        wait_for(lambda: len(children(self.proc.pid)) == 1)
        worker = children(self.proc.pid)[0]
        wait_for(lambda: bool(children(worker)))
        cgi = children(worker)[0]
        stop(self.proc)
        assert not Path(f'/proc/{cgi}').exists()
        assert not Path(f'/proc/{worker}').exists()
        client.close()


if __name__ == '__main__':
    unittest.main(verbosity=2)
