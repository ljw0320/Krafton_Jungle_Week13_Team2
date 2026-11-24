"""Linux/WSL capacity smoke test; bounded concurrency, not throughput benchmarking."""
import argparse
import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import resource
import socket
import subprocess
import tempfile
import time
import threading
from collections import Counter
ROOT = Path(__file__).resolve().parents[1]

def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]

def socket_counts(pid, frontend, backend):
    inodes = set()
    for fd in Path('/proc/%d/fd' % pid).iterdir():
        try:
            link = os.readlink(str(fd))
            if link.startswith('socket:['): inodes.add(link[8:-1])
        except FileNotFoundError: pass
    counts = {'frontend': 0, 'backend': 0}
    for line in Path('/proc/net/tcp').read_text().splitlines()[1:]:
        fields = line.split()
        if fields[3] != '01' or fields[9] not in inodes: continue
        local = int(fields[1].split(':')[1], 16)
        remote = int(fields[2].split(':')[1], 16)
        if local == frontend: counts['frontend'] += 1
        if remote == backend: counts['backend'] += 1
    return counts

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--connections', type=int, default=100)
    parser.add_argument('--output')
    parser.add_argument('--empty-body', action='store_true')
    parser.add_argument('--contexts', type=int, default=8)
    parser.add_argument('--rounds', type=int, default=7)
    args = parser.parse_args()
    if not 1 <= args.connections <= 10000: parser.error('connections must be 1..10000')
    if not 1 <= args.contexts <= 4096: parser.error('contexts must be 1..4096')
    if not 1 <= args.rounds <= 90: parser.error('rounds must be 1..90')
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    wanted = max(soft, 65536)
    if hard != resource.RLIM_INFINITY: wanted = min(wanted, hard)
    resource.setrlimit(resource.RLIMIT_NOFILE, (wanted, hard))
    capacity = max(16, args.connections + max(28, args.connections // 10))
    backend_port, frontend_port = port(), port()
    while frontend_port == backend_port: frontend_port = port()
    env = dict(os.environ, JUNGLE_CONNECTIONS=str(capacity), JUNGLE_CONTEXTS=str(args.contexts),
               PROXY_CONNECTIONS=str(capacity), PROXY_CONTEXTS=str(args.contexts))
    clients, processes, logs = [], [], []
    started = time.monotonic()
    progress = [0]
    failed = threading.Event()
    with tempfile.TemporaryDirectory(prefix='jungle-capacity-') as directory:
        try:
            for program, command in [('backend', ['./webserver', str(backend_port), directory]),
                  ('proxy', ['./reverse_proxy', str(frontend_port), '127.0.0.1', str(backend_port)])]:
                log = open(Path(directory)/ (program+'.log'), 'w+')
                logs.append(log)
                process = subprocess.Popen(command, cwd=str(ROOT), env=env, stdout=log, stderr=log)
                processes.append(process)
                target = backend_port if program == 'backend' else frontend_port
                for _ in range(200):
                    if process.poll() is not None:
                        log.seek(0); raise AssertionError(log.read())
                    try:
                        with socket.create_connection(('127.0.0.1', target), .1): break
                    except OSError: time.sleep(.01)
                else: raise AssertionError('listener did not start')
            # Every connection actually processes a request; empty TCP accepts do not count.
            started = time.monotonic()
            for _ in range(args.connections):
                clients.append(http.client.HTTPConnection('127.0.0.1', frontend_port, timeout=5))
            snapshots = []
            requests = 0
            def checked_exchange(item):
                if failed.is_set(): return
                try:
                    exchange(item)
                    progress[0] += 1
                except Exception:
                    failed.set()
                    raise

            def exchange(item):
                index, client = item
                body = b'' if args.empty_body else ('%d:%d' % (round_number, index)).encode()
                client.request('POST', '/echo', body)
                response = client.getresponse()
                assert response.status == 200, response.status
                assert response.read() == body
                assert response.getheader('Connection') == 'keep-alive'
                assert client.sock is not None
            for round_number in range(args.rounds):
                with concurrent.futures.ThreadPoolExecutor(max_workers=min(4, args.contexts)) as executor:
                    list(executor.map(checked_exchange, enumerate(clients)))
                requests += len(clients)
                counts = socket_counts(processes[1].pid, frontend_port, backend_port)
                assert counts == {'frontend': args.connections, 'backend': args.connections}, counts
                snapshots.append(counts)
                if round_number < args.rounds - 1: time.sleep(1)
            elapsed = time.monotonic() - started
            # One backend accept per pair proves backend reuse across all rounds.
            logs[0].flush(); logs[0].seek(0)
            accepts = logs[0].read().count('[accept fd=')
            assert accepts == args.connections + 1, accepts  # + readiness probe
            fds = [len(list(Path('/proc/%d/fd' % p.pid).iterdir())) for p in processes]
            for client in clients: client.close()
            for _ in range(100):
                if socket_counts(processes[1].pid, frontend_port, backend_port) == {'frontend': 0, 'backend': 0}: break
                time.sleep(.02)
            else: raise AssertionError('connections were not reclaimed')
            result = dict(status='passed', connections=args.connections, configured_connections=capacity,
                contexts_each=args.contexts, empty_body=args.empty_body, requests=requests, elapsed_seconds=round(elapsed, 3),
                fd_soft_before=soft, fd_soft_test=wanted, backend_accepts_excluding_probe=accepts-1,
                fd_counts_during={'backend': fds[0], 'proxy': fds[1]},
                final_established={'frontend': 0, 'backend': 0},
                scope='bounded-concurrency POST echo heartbeats; connection retention/reuse, not throughput')
            print(json.dumps(result, ensure_ascii=False, indent=2))
            if args.output: Path(args.output).write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n')
        except Exception as error:
            observed = socket_counts(processes[1].pid, frontend_port, backend_port) if len(processes) == 2 and processes[1].poll() is None else None
            accepts, reasons = 0, Counter()
            if logs:
                logs[0].seek(0)
                for line in logs[0]:
                    if '[accept fd=' in line: accepts += 1
                    if '[close fd=' in line: reasons[line.split('] ', 1)[-1].strip()] += 1
            result = dict(status='failed', connections=args.connections, configured_connections=capacity,
                contexts_each=args.contexts, empty_body=args.empty_body, completed_requests=progress[0],
                elapsed_seconds=round(time.monotonic()-started, 3), error_type=type(error).__name__,
                error=str(error), observed_established=observed, backend_accepts_including_probe=accepts,
                backend_close_reasons=dict(reasons), fd_soft_test=wanted,
                scope='bounded-concurrency connection retention test; failure does not isolate loader/server bottleneck')
            print(json.dumps(result, indent=2))
            if args.output: Path(args.output).write_text(json.dumps(result, indent=2)+'\n')
            raise
        finally:
            for client in clients: client.close()
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=3)
                    except subprocess.TimeoutExpired: process.kill(); process.wait()
            for log in logs: log.close()

if __name__ == '__main__': main()
