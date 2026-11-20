import concurrent.futures
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def connect(port):
    return socket.create_connection(('127.0.0.1', port), timeout=3)


def echo(port, payload):
    with connect(port) as client:
        client.sendall(payload)
        client.shutdown(socket.SHUT_WR)
        chunks = []
        while True:
            data = client.recv(65536)
            if not data:
                break
            chunks.append(data)
        assert b''.join(chunks) == payload


def children(pid):
    return Path(f'/proc/{pid}/task/{pid}/children').read_text().split()


def wait_for(predicate, seconds=3):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.02)
    raise AssertionError('condition timed out')


def start(port, limit=4, idle=2, grace=1):
    proc = subprocess.Popen(
        [str(ROOT / 'build/tcp-echo'), str(port), str(limit), str(idle), str(grace)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        start_new_session=True,
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


def main():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    for bad in ['0', '-1', '65536', 'abc', '8080x']:
        p = subprocess.run([str(ROOT / 'build/tcp-echo'), bad], capture_output=True)
        assert p.returncode != 0
    for args in [[str(port), '0'], [str(port), '4', '0'],
                 [str(port), '4', '2', '0'], [str(port), '4', '2', '1', 'extra']]:
        p = subprocess.run([str(ROOT / 'build/tcp-echo'), *args], capture_output=True)
        assert p.returncode != 0
    proc = start(port)
    try:
        busy = subprocess.run([str(ROOT / 'build/tcp-echo'), str(port)], capture_output=True)
        assert busy.returncode != 0
        echo(port, bytes(range(256)) * 4096)
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            list(pool.map(lambda i: echo(port, bytes([i]) * 32768), range(24)))
        wait_for(lambda: not children(proc.pid))
        # Hold every worker, then confirm an extra connection waits for a slot.
        holders = [connect(port) for _ in range(4)]
        wait_for(lambda: len(children(proc.pid)) == 4)
        extra = connect(port)
        extra.sendall(b'queued')
        extra.settimeout(.15)
        try:
            extra.recv(6)
            raise AssertionError('worker limit exceeded')
        except socket.timeout:
            pass
        assert len(children(proc.pid)) == 4
        holders.pop().close()
        extra.settimeout(3)
        assert extra.recv(6) == b'queued'
        extra.close()
        for client in holders:
            client.close()
        wait_for(lambda: not children(proc.pid))
        with connect(port) as idle:
            assert idle.recv(1) == b''
        # A peer reset must not kill the parent or prevent subsequent service.
        with connect(port) as reset:
            reset.sendall(b'x' * 65536)
            reset.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        echo(port, b'after reset')
        wait_for(lambda: not children(proc.pid))
        stop(proc)  # shutdown while accept readiness is idle
    finally:
        if proc.poll() is None:
            stop(proc)

    # Graceful completion, then force an active trickling client past the grace.
    proc = start(port, idle=10)
    client = connect(port)
    try:
        client.sendall(b'finish')
        assert client.recv(6) == b'finish'
        wait_for(lambda: len(children(proc.pid)) == 1)
        proc.send_signal(signal.SIGTERM)
        client.shutdown(socket.SHUT_WR)
        assert client.recv(1) == b''
        stop(proc)
    finally:
        client.close()
        if proc.poll() is None:
            stop(proc)
    proc = start(port, idle=10)
    client = connect(port)
    try:
        wait_for(lambda: len(children(proc.pid)) == 1)
        pids = children(proc.pid)
        stop(proc)
        assert all(not Path(f'/proc/{pid}').exists() for pid in pids)
    finally:
        client.close()
        if proc.poll() is None:
            stop(proc)
    proc = start(port)
    stop(proc, group=True)
    print('PASS: echo, concurrency, cap, timeout, reset, reaping, shutdown, restart')


if __name__ == '__main__':
    main()
