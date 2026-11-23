import concurrent.futures
import http.client
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/web-server'


def connect(port):
    return socket.create_connection(('127.0.0.1', port), timeout=4)


def children(pid):
    return Path(f'/proc/{pid}/task/{pid}/children').read_text().split()


def wait_for(predicate, seconds=4):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.02)
    raise AssertionError('condition timed out')


def start(port, root, limit=4, idle=2, grace=1, cwd=None):
    proc = subprocess.Popen(
        [str(BINARY), str(port), str(root), str(limit), str(idle), str(grace)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        start_new_session=True,
        cwd=cwd,
    )
    assert proc.stdout.readline().startswith('READY'), proc.stderr.read()
    return proc


def stop(proc, group=False):
    if proc.poll() is None:
        if group:
            os.killpg(proc.pid, signal.SIGINT)
        else:
            proc.send_signal(signal.SIGTERM)
    out, err = proc.communicate(timeout=6)
    assert proc.returncode == 0, (out, err)
    assert not err, err


def response(client):
    result = http.client.HTTPResponse(client)
    result.begin()
    body = result.read()
    return result.status, dict(result.getheaders()), body, result.version


def receive_exact(client, length):
    data = b''
    while len(data) < length:
        piece = client.recv(length - len(data))
        assert piece, 'connection ended before the expected bytes arrived'
        data += piece
    return data


def request(port, target='/', expected=200):
    with connect(port) as client:
        client.sendall(f'GET {target} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'.encode())
        status, headers, body, version = response(client)
        assert status == expected, (target, status, body)
        assert int(headers['Content-Length']) == len(body)
        assert headers['Connection'] == 'close' and version == 11
        assert client.recv(1) == b''
        return headers, body


def bad_request(port, payload, expected):
    with connect(port) as client:
        client.sendall(payload)
        client.shutdown(socket.SHUT_WR)
        status, headers, _, _ = response(client)
        assert status == expected, (payload[:100], status)
        assert headers['Connection'] == 'close'
        try:
            assert client.recv(1) == b''
        except ConnectionResetError:
            pass


def request_deadline(port):
    with connect(port) as client:
        client.settimeout(35)
        client.sendall(b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\nx')
        started = time.monotonic()
        # Receiving progress must not restart the total request timer.
        time.sleep(16)
        client.sendall(b'y')
        assert response(client)[0] == 408
        assert 28 <= time.monotonic() - started < 34


def stalled_response(port, pid):
    with connect(port) as client:
        client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        client.sendall(b'GET /large.bin HTTP/1.1\r\nHost: localhost\r\n\r\n')
        wait_for(lambda: len(children(pid)) == 1)
        started = time.monotonic()
        wait_for(lambda: not children(pid), seconds=16)
        assert time.monotonic() - started >= 9


def main():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary) / 'public'
        root.mkdir()
        (root / 'index.html').write_bytes(b'<h1>Hello</h1>\n')
        (root / 'style.css').write_bytes(b'body { color: navy; }')
        binary = bytes(range(256)) * 4096
        (root / 'image.png').write_bytes(binary)
        mime_cases = [('file.PNG', 'image/png'), ('file.jpg', 'image/jpeg'),
                      ('file.js', 'text/javascript; charset=utf-8'),
                      ('file.unknown', 'application/octet-stream')]
        for name, _ in mime_cases:
            (root / name).write_bytes(b'mime fixture')
        (root / 'space name.txt').write_bytes('안녕'.encode())
        (root / 'percent%20.txt').write_bytes(b'decode once')
        (root / '.env').write_text('SECRET')
        (root / 'docs').mkdir()
        (root / 'docs/index.html').write_bytes(b'docs')
        (root / 'empty').mkdir()
        (root / '.hidden').mkdir()
        (root / '.hidden/index.html').write_bytes(b'hidden')
        (Path(temporary) / 'secret.txt').write_bytes(b'secret')
        (root / 'outside').symlink_to(Path(temporary), target_is_directory=True)
        (root / 'inside').symlink_to(root / 'docs', target_is_directory=True)
        (root / 'link.txt').symlink_to(root / 'index.html')
        (root / 'linked-index').mkdir()
        (root / 'linked-index/index.html').symlink_to(Path(temporary) / 'secret.txt')
        (root / 'fifo-index').mkdir()
        os.mkfifo(root / 'fifo-index/index.html')
        os.mkfifo(root / 'pipe')
        with (root / 'large.bin').open('wb') as large:
            large.truncate(64 * 1024 * 1024)

        for args in [['0'], ['-1'], ['65536'], ['abc'], [str(port), str(root), '0'],
                     [str(port), str(root), '4', '0'], [str(port), str(root), '4', '2', '0'],
                     [str(port), str(root), '4', '2', '1', 'extra'],
                     [str(port), str(root / 'missing')], [str(port), str(root / 'inside')]]:
            assert subprocess.run([str(BINARY), *args], capture_output=True).returncode != 0
        proc = start(port, root)
        try:
            assert subprocess.run([str(BINARY), str(port), str(root)], capture_output=True).returncode != 0
            assert request(port)[1] == b'<h1>Hello</h1>\n'
            assert request(port, '/style.css?v=2')[0]['Content-Type'].startswith('text/css')
            assert request(port, '/image.png')[1] == binary
            for name, content_type in mime_cases:
                headers, body = request(port, '/' + name)
                assert headers['Content-Type'] == content_type and body == b'mime fixture'
            assert request(port, '/space%20name.txt')[1] == '안녕'.encode()
            assert request(port, '/percent%2520.txt')[1] == b'decode once'
            assert request(port, '/docs/')[1] == b'docs'
            assert request(port, '/docs')[1] == b'docs'
            for target in ['/empty/', '/.env', '/.hidden/', '/outside/secret.txt', '/inside/',
                           '/link.txt', '/../secret.txt', '/%2e%2e/secret.txt',
                           '/missing/../index.html', '/pipe', '/index.html/',
                           '/%2Eenv', '/%2e%2e%2fsecret.txt', '/docs/%2e%2e/index.html',
                           '//outside//secret.txt', '/./index.html', '/linked-index/', '/fifo-index/']:
                request(port, target, 403)
            request(port, '/missing', 404)
            for target in ['/%', '/%xx', '/%00', '/%5csecret', '/index.html#part',
                           '/%0d%0aInjected:yes', '/%7f']:
                request(port, target, 400)

            header = b'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'
            for split in range(1, len(header)):
                with connect(port) as client:
                    client.sendall(header[:split])
                    time.sleep(.002)
                    client.sendall(header[split:])
                    assert response(client)[0] == 200

            with connect(port) as client:
                for piece in [b'GET / HT', b'TP/1.1\r\nHost: local', b'host\r\n\r', b'\n']:
                    client.sendall(piece)
                    time.sleep(.01)
                assert response(client)[0] == 200
                for _ in range(105):
                    client.sendall(b'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n')
                    assert response(client)[0] == 200
                wait_for(lambda: len(children(proc.pid)) == 1)
                worker = children(proc.pid)[0]
                # stdin/stdout/stderr, the borrowed root, and the client remain open.
                wait_for(lambda: len(list(Path(f'/proc/{worker}/fd').iterdir())) == 5)
                client.sendall(b'GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive, close\r\n\r\n')
                assert response(client)[1]['Connection'] == 'close'
                assert client.recv(1) == b''

            with connect(port) as client:
                client.sendall(b'GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n')
                status, headers, _, version = response(client)
                assert status == 200 and version == 10 and headers['Connection'] == 'close'
                assert client.recv(1) == b''

            with connect(port) as client:
                client.sendall(b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\n\x00abc'
                               b'GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
                # A shared reader preserves read-ahead bytes of the next response.
                stream = client.makefile('rb')
                for expected in [501, 200]:
                    assert int(stream.readline().split()[1]) == expected
                    headers = {}
                    while (line := stream.readline()) != b'\r\n':
                        key, value = line.decode().split(':', 1)
                        headers[key] = value.strip()
                    assert len(stream.read(int(headers['Content-Length']))) == int(headers['Content-Length'])
                stream.close()

            with connect(port) as client:
                client.sendall(b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\n')
                assert receive_exact(client, 25) == b'HTTP/1.1 100 Continue\r\n\r\n'
                client.sendall(b'abc')
                assert response(client)[0] == 501

            with connect(port) as client:
                client.sendall(b'HEAD / HTTP/1.1\r\nHost: x\r\n\r\n')
                result = http.client.HTTPResponse(client, method='HEAD')
                result.begin()
                assert result.status == 405 and result.getheader('Allow') == 'GET, POST'
                assert result.read() == b'' and client.recv(1) == b''

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
                bad_request(port, base + headers + b'\r\n', status)
            bad_request(port, base + b'Content-Length: 3\r\n\r\na', 400)
            bad_request(port, b'GET / HTTP/1.1\r\n\r\n', 400)
            bad_request(port, b'GET / HTTP/2.0\r\n\r\n', 505)
            bad_request(port, b'GET / HTTP/no\r\n\r\n', 400)
            bad_request(port, b'GET  / HTTP/1.1\r\nHost: x\r\n\r\n', 400)
            bad_request(port, b'GET / HTTP/1.1\r\nHost: x\r\n\r\n'[:10], 400)
            bad_request(port, b'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n'
                              b'Content-Length: 0\r\n\r\n'
                              b'GET / HTTP/1.1\r\nHost: x\r\n\r\n', 400)
            bad_request(port, b'GET /' + b'a' * 8192 + b' HTTP/1.1\r\nHost: x\r\n\r\n', 414)
            bad_request(port, b'GET / HTTP/1.1\r\nHost: x\r\nX: ' + b'a' * 32768 + b'\r\n\r\n', 431)

            # The limits themselves are accepted; only values above them are rejected.
            request(port, '/' + 'a' * 8191, 403)
            header = b'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\nX: '
            bad_request(port, header + b'a' * (32768 - len(header) - 4) + b'\r\n\r\n', 200)
            with connect(port) as client:
                client.sendall(base + b'Content-Length: 1048576\r\nConnection: close\r\n\r\n')
                client.sendall(binary)
                assert response(client)[0] == 501

            with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
                results = list(pool.map(lambda _: request(port, '/image.png'), range(24)))
                assert all(body == binary for _, body in results)
            wait_for(lambda: not children(proc.pid))
            holders = [connect(port) for _ in range(4)]
            wait_for(lambda: len(children(proc.pid)) == 4)
            with connect(port) as queued:
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
            wait_for(lambda: not children(proc.pid))
            with connect(port) as idle:
                started = time.monotonic()
                assert idle.recv(1) == b''
                assert 1.8 <= time.monotonic() - started < 4
            with connect(port) as reset:
                reset.sendall(b'GET /large.bin HTTP/1.1\r\nHost: x\r\n\r\n')
                reset.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            request(port)
            wait_for(lambda: not children(proc.pid))
            stalled_response(port, proc.pid)
            request_deadline(port)
        finally:
            stop(proc)

        proc = start(port, root, idle=10)
        with connect(port) as client:
            client.sendall(b'GET / HTTP/1.1\r\nHost: x\r\n\r\n')
            assert response(client)[0] == 200
            pids = children(proc.pid)
            stop(proc)
            assert all(not Path(f'/proc/{pid}').exists() for pid in pids)
        proc = start(port, root)
        stop(proc, group=True)
    print('PASS: HTTP, binary files, paths, framing, keep-alive, limits, deadlines, concurrency, shutdown')


if __name__ == '__main__':
    main()
