"""Compare burst requests before/after the proxy buffer wait change (Linux)."""
import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'proxy/tests'))
import test_proxy as support


def start(binary, backend):
    port = support.free_port()
    env = dict(os.environ, PROXY_CONTEXTS='8', PROXY_CONNECTIONS='64')
    process = subprocess.Popen([str(binary), str(port), '127.0.0.1', str(backend)],
                               env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_listen(process, port)
    return process, port


def wait_listen(process, port):
    for _ in range(300):
        if process.poll() is not None:
            raise RuntimeError('process exited')
        try:
            with socket.create_connection(('127.0.0.1', port), .1):
                return
        except OSError:
            time.sleep(.01)
    raise RuntimeError('listen timeout')


def stop(process):
    process.terminate()
    process.wait(timeout=3)


def request(port, gate=None):
    if gate:
        gate.wait(timeout=10)
    begin = time.monotonic()
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
    try:
        connection.request('POST', '/echo', body=b'', headers={'Connection': 'close'})
        response = connection.getresponse()
        body = response.read()
        status = response.status
        if status == 200 and (body or response.getheader('Content-Length') != '0'):
            status = 'invalid'
        return status, (time.monotonic() - begin) * 1000
    except (OSError, http.client.HTTPException) as error:
        return type(error).__name__, (time.monotonic() - begin) * 1000
    finally:
        connection.close()


def sockets(port):
    # Count established front/backend sockets on each listener, excluding TIME_WAIT.
    count = 0
    for name in ('/proc/net/tcp', '/proc/net/tcp6'):
        for line in Path(name).read_text().splitlines()[1:]:
            fields = line.split()
            if int(fields[1].split(':')[1], 16) == port and fields[3] == '01':
                count += 1
    return count


def measure(binary, backend, scenario, concurrency, repeat):
    process, port = start(binary, backend)
    try:
        gate = threading.Barrier(concurrency)
        with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as executor:
            results = list(executor.map(lambda _: request(port, gate), range(concurrency)))
        statuses = {}
        for status, _ in results:
            statuses[str(status)] = statuses.get(str(status), 0) + 1
        latency = sorted(duration for _, duration in results)
        # Healthy wave needs all eight buffers again; also verify actual socket cleanup.
        recovery_gate = threading.Barrier(8)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            recovery = list(executor.map(lambda _: request(port, recovery_gate)[0], range(8)))
        until = time.monotonic() + 2
        while (sockets(port) or sockets(backend)) and time.monotonic() < until:
            time.sleep(.02)
        row = dict(version=binary.name, scenario=scenario, concurrency=concurrency,
                   repeat=repeat, statuses=statuses,
                   p95_ms=round(latency[(95 * len(latency) + 99)//100 - 1], 2),
                   max_ms=round(latency[-1], 2),
                   recovery_ok=all(status == 200 for status in recovery),
                   frontend_established=sockets(port), backend_established=sockets(backend))
        print(json.dumps(row), flush=True)
        return row
    finally:
        stop(process)


def main():
    binaries = [ROOT/'artifacts/proxy_wait_before', ROOT/'reverse_proxy']
    rows = []
    port = support.free_port()
    server = subprocess.Popen([str(ROOT/'webserver'), str(port), 'www', 'cgi-bin'],
                              cwd=str(ROOT), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_listen(server, port)
        for concurrency in (16, 32):
            for repeat in range(1, 4):
                for binary in binaries:
                    rows.append(measure(binary, port, 'real_server', concurrency, repeat))
    finally:
        stop(server)
    # Fixed service time isolates buffer saturation from server/generator scheduling.
    for delay in (.1, .8):
        def reply(peer, data):
            time.sleep(delay)
            peer.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
        support.Backend.request_queue_size = 128
        backend = support.Backend(('127.0.0.1', 0), support.Handler)
        import queue
        backend.seen = queue.Queue()
        backend.reply = reply
        threading.Thread(target=backend.serve_forever, kwargs={'poll_interval': .02}, daemon=True).start()
        try:
            for concurrency in (16, 32):
                for repeat in range(1, 4):
                    for binary in binaries:
                        rows.append(measure(binary, backend.server_address[1],
                                            'fixed_%dms' % (delay*1000), concurrency, repeat))
        finally:
            backend.shutdown()
            backend.server_close()
    (ROOT/'artifacts/proxy_buffer_wait_results.json').write_text(json.dumps(rows, indent=2))
    assert all(r['recovery_ok'] and not r['frontend_established'] and
               not r['backend_established'] for r in rows)


if __name__ == '__main__':
    main()
