import concurrent.futures
import os
from pathlib import Path
import random
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import time

from test_server import (ROOT, children, connect, pipeline_response, receive_exact,
                         response, start, stop, wait_for)


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


def exchange(port, data, expected=200, expected_body=None, half_close=False,
             expected_type='text/plain; charset=utf-8'):
    with connect(port) as client:
        client.settimeout(8)
        client.sendall(data)
        if half_close:
            client.shutdown(socket.SHUT_WR)
        status, headers, body, version = response(client)
        assert status == expected, (status, expected, body)
        assert int(headers['Content-Length']) == len(body)
        assert headers['Content-Type'] == expected_type, headers
        assert version == (10 if data.split(b'\r\n', 1)[0].endswith(b'HTTP/1.0') else 11)
        assert headers['Connection'] == 'close'
        assert client.recv(1) == b''
        if expected_body is not None:
            assert body == expected_body, (body[:80], expected_body[:80])
        return headers, body, version


FIXTURE = '''#!/usr/bin/python3
import os, signal, sys, time
mode = open('mode').read()
valid = b'Content-Type: text/plain\\r\\n\\r\\n'
if mode == 'timeout':
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    time.sleep(60)
elif mode == 'eof-alive':
    os.close(1)
    time.sleep(60)
elif mode == 'trickle':
    os.write(1, valid)
    while True:
        os.write(1, b'x')
        time.sleep(.1)
elif mode == 'overflow':
    while True: os.write(1, b'x' * 8192)
elif mode == 'limit':
    os.write(1, valid + b'x' * (65536 - len(valid)))
elif mode == 'nonzero':
    os.write(1, valid + b'false success')
    sys.exit(127)
elif mode == 'signal':
    os.kill(os.getpid(), signal.SIGKILL)
elif mode == 'invalid':
    os.write(1, b'not a CGI response')
elif mode == 'duplicate':
    os.write(1, valid.replace(b'\\r\\n\\r\\n', b'\\r\\nContent-Type: text/html\\r\\n\\r\\n'))
elif mode == 'injection':
    os.write(1, b'Content-Type: text/plain\\rInjected: yes\\n\\nsecret')
elif mode == 'nul':
    os.write(1, b'Content-Type: text/plain\\x00\\n\\nsecret')
elif mode == 'header-overflow':
    os.write(1, b'Content-Type: ' + b'x' * 8200 + b'\\n\\n')
elif mode == 'status':
    os.write(1, b'Status: 400 Bad Request\\nContent-Type: text/plain\\n\\ninvalid')
elif mode == 'binary':
    os.write(1, b'Content-Type: application/octet-stream\\r\\n\\r\\n\\x00\\xff')
elif mode == 'custom':
    data = open('output', 'rb').read()
    while data:
        data = data[os.write(1, data):]
elif mode == 'descendant':
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    child = os.fork()
    if child == 0:
        os.close(0)
        os.close(1)
        time.sleep(60)
        os._exit(0)
    with open('descendant.pid', 'w') as file:
        file.write(str(child))
    os.write(1, valid + b'clean')
elif mode == 'duplex':
    data = valid + b'x' * 60000
    while data:
        data = data[os.write(1, data):]
    incoming = sys.stdin.buffer.read()
    assert len(incoming) == int(os.environ['CONTENT_LENGTH'])
    os.write(1, incoming)
elif mode == 'duplex-pressure':
    import fcntl
    assert fcntl.fcntl(0, fcntl.F_GETPIPE_SZ) < int(os.environ['CONTENT_LENGTH'])
    fcntl.fcntl(1, fcntl.F_SETPIPE_SZ, 4096)
    assert fcntl.fcntl(1, fcntl.F_GETPIPE_SZ) < 60000
    data = valid + b'x' * 60000
    while data:
        data = data[os.write(1, data):]
    incoming = sys.stdin.buffer.read()
    assert incoming == b'x' * int(os.environ['CONTENT_LENGTH'])
    os.write(1, b'drained')
elif mode == 'fd-check':
    assert not any(os.readlink('/proc/self/fd/' + f).startswith('socket:')
                   for f in os.listdir('/proc/self/fd') if f.isdigit() and
                   os.path.exists('/proc/self/fd/' + f))
    assert 'PATH' not in os.environ
    os.write(1, valid + b'clean')
'''


def running(pid):
    try:
        state = Path(f'/proc/{pid}/status').read_text().split('State:', 1)[1].split()[0]
        return state != 'Z'
    except FileNotFoundError:
        return False


def main():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        (work / 'build').mkdir()
        public = work / 'public'
        public.mkdir()
        (public / 'index.html').write_bytes(b'static')
        # Even files in public/cgi-bin must never be executed or served as CGI.
        (public / 'cgi-bin').mkdir()
        (public / 'cgi-bin/other').write_bytes(b'private program')
        executable = work / 'build/adder'
        shutil.copy2(ROOT / 'build/adder', executable)
        proc = start(port, public, cwd=work)
        try:
            exchange(port, packet(), expected_body=b'30\n')
            exchange(port, packet('/cgi-bin/adder', b'a=10&b=20'), expected_body=b'30\n')
            exchange(port, packet('/cgi-bin/adder?a=999&b=999', b'a=2&b=3'),
                     expected_body=b'5\n')
            exchange(port, packet('/cgi-bin/%61dder?%61=%2B2&b=-3'), expected_body=b'-1\n')
            assert exchange(port, packet(version='1.0'), expected_body=b'30\n')[2] == 10
            exchange(port, packet(), expected_body=b'30\n', half_close=True)
            # Python arithmetic supplies an oracle independent of the C adder.
            random_values = random.Random(20261123)
            for _ in range(40):
                a, b = [random_values.randrange(-(2 ** 62), 2 ** 62) for _ in range(2)]
                form = f'a={a}&b={b}'
                expected = f'{a + b}\n'.encode()
                exchange(port, packet('/cgi-bin/adder?' + form), expected_body=expected)
                exchange(port, packet('/cgi-bin/adder', form.encode()), expected_body=expected)
            boundary_form = b'a=' + b'0' * (4096 - len(b'a=1&b=2')) + b'1&b=2'
            assert len(boundary_form) == 4096
            exchange(port, packet('/cgi-bin/adder', boundary_form), expected_body=b'3\n')
            for form, result in [
                ('a=-9223372036854775808&b=0', b'-9223372036854775808\n'),
                ('b=9223372036854775807&a=0', b'9223372036854775807\n'),
                ('a=0002&b=-2', b'0\n'),
            ]:
                exchange(port, packet('/cgi-bin/adder?' + form), expected_body=result)
            bad_forms = ['', 'a=1', 'b=2', 'a=1&a=2&b=3', 'a=1&b=2&b=3',
                         'a=1&b=2&x=3', 'a=1&b=2&',
                         'a=&b=2', 'a=+1&b=2', 'a=%201&b=2', 'a=1x&b=2',
                         'a=%00&b=2', 'a=%xx&b=2', 'a=%&b=2', 'a=%0a1&b=2',
                         'a=1%262&b=3', 'a=9223372036854775808&b=0',
                         'a=-9223372036854775809&b=0',
                         'a=9223372036854775807&b=1', 'a=-9223372036854775808&b=-1']
            for form in bad_forms:
                exchange(port, packet('/cgi-bin/adder?' + form), 400)
                exchange(port, packet('/cgi-bin/adder', form.encode()), 400)
            exchange(port, packet('/cgi-bin/adder', b'a=1\0&b=2'), 400)
            exchange(port, packet('/cgi-bin/other'), 404)
            exchange(port, packet('//cgi-bin/other'), 404)
            exchange(port, packet('/cgi-bin/adder/extra'), 404)
            exchange(port, packet('/cgi-bin/adder/', b'a=1&b=2'), 404)
            exchange(port, packet('/cgi-bin/adder', b'x' * 4096), 400)
            # Reject before waiting for the body or sending 100 Continue.
            for header, expected in [
                (b'Content-Length: 4097\r\nContent-Type: application/x-www-form-urlencoded\r\n', 413),
                (b'Content-Length: 7\r\n', 415),
                (b'Content-Length: 7\r\nContent-Type: application/json\r\n', 415),
                (b'Content-Type: x\r\nContent-Type: y\r\n', 400),
            ]:
                exchange(port, b'POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\n' + header +
                         b'Expect: 100-continue\r\n\r\n', expected)
            with connect(port) as client:
                client.sendall(packet('/cgi-bin/adder', b'a=2&b=3', close=False,
                                      headers=b'Expect: 100-continue\r\n')[:-7])
                assert receive_exact(client, 25) == b'HTTP/1.1 100 Continue\r\n\r\n'
                for piece in [b'a=', b'2&', b'b=3']:
                    client.sendall(piece)
                assert response(client)[2] == b'5\n'
                wait_for(lambda: len(children(proc.pid)) == 1)
                worker = children(proc.pid)[0]
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
            # Pipeline POST body and subsequent static/CGI requests on one connection.
            with connect(port) as client:
                client.sendall(packet('/cgi-bin/adder', b'a=2&b=3', close=False) +
                               packet('/', close=False) + packet())
                with client.makefile('rb') as stream:
                    for expected, connection in [(b'5\n', 'keep-alive'),
                                                 (b'static', 'keep-alive'), (b'30\n', 'close')]:
                        status, headers, body = pipeline_response(stream)
                        assert status == 200 and body == expected
                        assert headers['Connection'] == connection
                    assert stream.read(1) == b''
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                list(pool.map(lambda i: exchange(
                    port, packet(f'/cgi-bin/adder?a={i}&b={i * i}'),
                    expected_body=f'{i + i * i}\n'.encode()), range(16)))
            wait_for(lambda: not children(proc.pid))

            # Missing executable, permission failure and exec format failure are server errors.
            executable.unlink()
            exchange(port, packet(), 500)
            executable.write_text('not executable')
            executable.chmod(0o644)
            exchange(port, packet(), 500)
            executable.chmod(0o755)
            exchange(port, packet(), 500)

            executable.write_text(FIXTURE)
            executable.chmod(0o755)
            mode_file = work / 'mode'
            mode_file.write_text('custom')
            output_file = work / 'output'
            plain_header = b'Content-Type: text/plain\r\n\r\n'
            for status in [200, 400, 403, 404, 405, 413, 415, 500]:
                output_file.write_bytes(f'Status: {status} Fixture\r\nContent-Type: text/plain\r\n\r\n'.encode() + b'fixture')
                exchange(port, packet(), status, b'fixture', expected_type='text/plain')
            for output in [
                b'Status: 200 OK\r\n\r\nsecret',  # Content-Type is required.
                b'Content-Type: \r\n\r\nsecret',
                b'Content-Type: text/plain\r\nContent-Length: 6\r\n\r\nsecret',
                b'Content-Type: text/plain\r\nLocation: /\r\n\r\nsecret',
                b'Status: 200 OK\r\nStatus: 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nsecret',
                b'Status: 302 Found\r\nContent-Type: text/plain\r\n\r\nsecret',
                b'Status: 20x Invalid\r\nContent-Type: text/plain\r\n\r\nsecret',
                b'Content-Type: text/plain\r\n',
                b'Content-Type: ' + b'x' * 128 + b'\r\n\r\nsecret',
                plain_header + b'x' * (65537 - len(plain_header)),
            ]:
                output_file.write_bytes(output)
                exchange(port, packet(), 502)
            output_file.write_bytes(b'Content-Type: ' + b'x' * 127 + b'\r\n\r\nbody')
            exchange(port, packet(), 200, b'body', expected_type='x' * 127)
            mode_file.write_text('descendant')
            try:
                exchange(port, packet(), 200, b'clean', expected_type='text/plain')
                descendant = int((work / 'descendant.pid').read_text())
                # Orphan zombies belong to init; the server must stop execution.
                wait_for(lambda: not running(descendant))
            finally:
                pid_file = work / 'descendant.pid'
                if pid_file.exists():
                    descendant = int(pid_file.read_text())
                    if running(descendant):
                        os.kill(descendant, signal.SIGKILL)
            for mode in ['invalid', 'nonzero', 'signal', 'duplicate', 'injection', 'nul',
                         'header-overflow', 'overflow']:
                mode_file.write_text(mode)
                exchange(port, packet(), 502)
                wait_for(lambda: not children(proc.pid))
            mode_file.write_text('status')
            exchange(port, packet(), 400, b'invalid', expected_type='text/plain')
            mode_file.write_text('binary')
            exchange(port, packet(), 200, b'\x00\xff', expected_type='application/octet-stream')
            mode_file.write_text('limit')
            exchange(port, packet(), 200, b'x' * (65536 - len(b'Content-Type: text/plain\r\n\r\n')),
                     expected_type='text/plain')
            mode_file.write_text('duplex')
            exchange(port, packet('/cgi-bin/adder', b'x' * 4096), 200, b'x' * 64096,
                     expected_type='text/plain')
            # Exercise the CGI layer directly: HTTP's 4KiB cap fits in a pipe.
            mode_file.write_text('duplex-pressure')
            reader, writer = socket.socketpair()
            with reader, writer:
                reader.settimeout(8)
                with subprocess.Popen([str(ROOT / 'build/test-cgi-io'), str(writer.fileno())],
                                      pass_fds=(writer.fileno(),), cwd=work) as helper:
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
            mode_file.write_text('fd-check')
            exchange(port, packet(), 200, b'clean', expected_type='text/plain')
            for mode in ['timeout', 'eof-alive', 'trickle']:
                mode_file.write_text(mode)
                started = time.monotonic()
                exchange(port, packet(), 504)
                assert 4.8 <= time.monotonic() - started < 7
                wait_for(lambda: not children(proc.pid))

            # Reset cancels a running CGI, including one that ignores SIGTERM.
            mode_file.write_text('timeout')
            client = connect(port)
            client.sendall(packet())
            wait_for(lambda: len(children(proc.pid)) == 1)
            worker = children(proc.pid)[0]
            wait_for(lambda: bool(children(worker)))
            cgi = children(worker)[0]
            client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            client.close()
            wait_for(lambda: not children(proc.pid))
            assert not Path(f'/proc/{cgi}').exists()

            # Parent-only shutdown must also clean up the CGI grandchild.
            client = connect(port)
            client.sendall(packet())
            wait_for(lambda: len(children(proc.pid)) == 1)
            worker = children(proc.pid)[0]
            wait_for(lambda: bool(children(worker)))
            cgi = children(worker)[0]
            stop(proc)
            assert not Path(f'/proc/{cgi}').exists()
            assert not Path(f'/proc/{worker}').exists()
            client.close()
        finally:
            if proc.poll() is None:
                stop(proc)
    print('PASS: CGI GET/POST, validation, limits, duplex pipes, failures, reaping, keep-alive, shutdown')


if __name__ == '__main__':
    main()
