import http.client
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import unittest

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


def response_headers(pairs):
    headers = {}
    for name, value in pairs:
        name = name.lower()
        assert name not in headers, ('duplicate response header', name, pairs)
        headers[name] = value
    assert 'transfer-encoding' not in headers, headers
    assert {'content-length', 'content-type', 'connection'} <= headers.keys(), headers
    length = headers['content-length']
    assert length.isascii() and length.isdecimal(), headers
    assert headers['connection'] in ('close', 'keep-alive'), headers
    return {name.title(): value for name, value in headers.items()}


class StrictHTTPResponse(http.client.HTTPResponse):
    def _read_status(self):
        version, status, reason = super()._read_status()
        # HTTPResponse otherwise treats HTTP/1.9 as HTTP/1.1.
        assert version in ('HTTP/1.0', 'HTTP/1.1'), version
        return version, status, reason


def response(client):
    result = StrictHTTPResponse(client)
    result.begin()
    headers = response_headers(result.getheaders())
    body = result.read()
    assert len(body) == int(headers['Content-Length'])
    return result.status, headers, body, result.version


def exchange(client, data, half_close=False):
    client.sendall(data)
    if half_close:
        client.shutdown(socket.SHUT_WR)
    return response(client)


def pipeline_response(stream):
    line = stream.readline()
    version, status, _ = line.split(b' ', 2)
    assert version == b'HTTP/1.1' and line.endswith(b'\r\n'), line
    pairs = []
    while (line := stream.readline()) != b'\r\n':
        assert line and line.endswith(b'\r\n'), line
        name, value = line[:-2].decode('ascii').split(':', 1)
        pairs.append((name, value.strip()))
    headers = response_headers(pairs)
    body = stream.read(int(headers['Content-Length']))
    assert len(body) == int(headers['Content-Length'])
    return int(status), headers, body


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


class ServerTestCase(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.root = self.work / 'public'
        self.root.mkdir()
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            self.port = probe.getsockname()[1]

    def start_server(self, **options):
        proc = start(self.port, self.root, **options)
        self.addCleanup(self.stop_server, proc)
        return proc

    @staticmethod
    def stop_server(proc):
        if not proc.stdout.closed:
            stop(proc)
