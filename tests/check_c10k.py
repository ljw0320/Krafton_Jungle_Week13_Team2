"""Async connection-retention test. Never reconnect; legacy check_capacity is unchanged."""
import argparse
import asyncio
from array import array
from bisect import bisect_right
from collections import Counter
from datetime import datetime
import heapq
import hashlib
import json
import os
from pathlib import Path
import resource
import socket
import subprocess
import sys
import tempfile
import time

from check_capacity import ROOT, port, socket_counts


class CapacityClock:
    """Integral of time with all admitted requests unfinished, on the loader clock."""
    def __init__(self, limit, started):
        self.limit, self.active = limit, 0
        self.times = array('d', [started])
        self.totals = array('d', [0.0])

    def full_time_at(self, now):
        index = bisect_right(self.times, now) - 1
        if index < 0:
            raise ValueError('timestamp precedes capacity clock')
        return self.totals[index] + (now - self.times[index] if index % 2 else 0)

    def _change(self, delta, now):
        was_full = self.active == self.limit
        self.active += delta
        assert 0 <= self.active <= self.limit
        if was_full != (self.active == self.limit):
            total = self.full_time_at(now)
            self.times.append(now)
            self.totals.append(total)

    def admit(self, now=None):
        now = time.monotonic() if now is None else now
        self._change(1, now)
        return dict(at=now, released=False)

    def release(self, slot, now=None):
        if slot['released']:
            return
        self._change(-1, time.monotonic() if now is None else now)
        slot['released'] = True


class Client:
    def __init__(self, index, empty_body=True):
        self.index = index
        self.empty_body = empty_body
        self.reader = self.writer = None
        self.responses = 0
        self.last = None
        self.max_gap = 0.0
        self.timing = None

    async def exchange(self, host, target, stats, scheduled_at=None, capacity=None, slot=None):
        started = time.monotonic()
        if scheduled_at is None:
            scheduled_at = slot['at'] if slot is not None else started
        self.timing = dict(connection=self.index, request=self.responses + 1,
                           kind='first' if self.writer is None else 'reuse',
                           scheduled_at=scheduled_at,
                           started_at=started, send_at=None, drained_at=None,
                           response_completed_at=None)
        self.timing['admitted_at'] = slot['at'] if slot is not None else started
        try:
            return await self._exchange(host, target, stats)
        finally:
            if capacity is not None:
                capacity.release(slot)
            timing = self.timing
            extra = []
            if capacity is not None and timing['send_at'] is not None:
                due, admitted = timing['scheduled_at'], timing['admitted_at']
                full = capacity.full_time_at(admitted) - capacity.full_time_at(due)
                # Tiny rounding error in cumulative clock differences is clamped.
                full = min(admitted - due, max(0.0, full))
                timing['slot_full_wait_seconds'] = full
                timing['slot_available_wait_seconds'] = admitted - due - full
                timing['admission_to_send_seconds'] = timing['send_at'] - admitted
                extra = [(name, timing[name + '_seconds']) for name in
                         ('slot_full_wait', 'slot_available_wait', 'admission_to_send')]
            durations = []
            for name, end, begin in (
                ('schedule_to_start', 'started_at', 'scheduled_at'),
                ('schedule_to_send', 'send_at', 'scheduled_at'),
                ('write_buffer_wait', 'drained_at', 'send_at'),
                ('send_to_response', 'response_completed_at', 'send_at')):
                if timing[end] is None: continue
                durations.append((name, timing[end] - timing[begin]))
            for name, value in durations + extra:
                key = timing['kind'] + '_' + name + '_seconds'
                metric = stats.setdefault('request_timing', {}).setdefault(
                    key, dict(count=0, total=0.0, maximum=0.0, max_request=None))
                metric['count'] += 1
                metric['total'] += value
                if metric['max_request'] is None or value > metric['maximum']:
                    metric['maximum'] = value
                    metric['max_request'] = dict(timing)

    async def _exchange(self, host, target, stats):
        first = self.writer is None
        if first:
            self.reader, self.writer = await asyncio.open_connection(host, target)
            stats['tcp_opens'] += 1
        body = b'' if self.empty_body else ('%d:%d' % (self.index, self.responses)).encode()
        request_started = time.monotonic()
        self.timing['send_at'] = request_started
        self.writer.write(b'POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: ' +
                          str(len(body)).encode() + b'\r\n\r\n' + body)
        await self.writer.drain()
        self.timing['drained_at'] = time.monotonic()
        header = await self.reader.readuntil(b'\r\n\r\n')
        header_at = time.monotonic()
        if len(header) > 32768:
            raise AssertionError('response header too large')
        lines = header[:-4].split(b'\r\n')
        assert lines[0] == b'HTTP/1.1 200 OK', lines[0]
        fields = {}
        for line in lines[1:]:
            key, value = line.split(b':', 1)
            key = key.lower()
            assert key not in fields, ('duplicate header', key)
            fields[key] = value.strip().lower()
        assert fields.get(b'content-length') == str(len(body)).encode(), fields
        assert fields.get(b'connection') == b'keep-alive', fields
        assert fields.get(b'via') == b'1.1 jungle-proxy', fields
        assert b'transfer-encoding' not in fields
        assert await self.reader.readexactly(len(body)) == body
        now = time.monotonic()
        self.timing['response_completed_at'] = now
        kind = 'first' if first else 'reuse'
        stats[kind + '_latency_max'] = max(stats.get(kind + '_latency_max', 0), now - request_started)
        stats[kind + '_body_wait_max'] = max(stats.get(kind + '_body_wait_max', 0), now - header_at)
        if self.last is not None:
            self.max_gap = max(self.max_gap, now - self.last)
        self.last = now
        self.responses += 1
        stats['requests'] += 1
        if first: stats['initialized'] += 1
        return self

    def close(self):
        if self.writer: self.writer.close()


async def snapshot_counts(pid, frontend, backend):
    # /proc parsing must not contend for the loader's Python GIL during heartbeats.
    process = await asyncio.create_subprocess_exec(sys.executable, str(Path(__file__).resolve()),
        '--snapshot', str(pid), str(frontend), str(backend),
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
    try:
        output, error = await process.communicate()
        if process.returncode: raise RuntimeError(error.decode())
        return json.loads(output.decode())
    finally:
        if process.returncode is None:
            process.kill(); await process.wait()


async def retain(args, frontend, proxy_pid, backend, observe):
    clients, idle, pending = [], [], set()
    stats = dict(tcp_opens=0, initialized=0, requests=0)
    started = time.monotonic()
    capacity = CapacityClock(args.concurrency, started)
    full_at = None
    snapshot = None
    snapshot_started = None
    next_snapshot = 0.0
    records = []
    sequence = 0
    try:
        while True:
            now = time.monotonic()
            assert now - started < args.total_timeout, 'test total timeout'
            for task in list(pending):
                if task.done():
                    pending.remove(task)
                    client = task.result() # EOF, timeout or bad data is fatal; no reconnect.
                    assert client.max_gap < args.max_gap, ('heartbeat gap exceeded', client.index, client.max_gap)
                    sequence += 1
                    heapq.heappush(idle, (client.last + args.heartbeat, sequence, client))
            if idle:
                oldest = idle[0][2]
                assert now - oldest.last < args.max_gap, ('loader cannot keep heartbeat deadline', oldest.index, now - oldest.last)
            if stats['initialized'] == args.connections and full_at is None:
                full_at = now
                next_snapshot = now
                observe['ramp_seconds'] = round(now - started, 6)
                print('[full] %d initialized, holding %.1fs' % (args.connections, args.hold), flush=True)
            if snapshot is not None and snapshot.done():
                counts = snapshot.result()
                record = dict(seconds_at_start=round(snapshot_started - started, 6), counts=counts)
                records.append(record)
                observe['snapshots'] = records
                assert counts == {'frontend': args.connections, 'backend': args.connections}, counts
                snapshot = None
                if snapshot_started >= full_at + args.hold:
                    break
                next_snapshot = min(full_at + args.hold, time.monotonic() + min(2, args.hold))
            if full_at is not None and snapshot is None and now >= next_snapshot:
                snapshot_started = now
                snapshot = asyncio.ensure_future(snapshot_counts(proxy_pid, frontend, backend))
            while len(pending) < args.concurrency:
                if idle and idle[0][0] <= now:
                    scheduled_at, _, client = heapq.heappop(idle)
                elif len(clients) < args.connections:
                    client = Client(len(clients), empty_body=not args.nonempty_body)
                    clients.append(client)
                    scheduled_at = now
                else:
                    break
                slot = capacity.admit()
                task = asyncio.ensure_future(asyncio.wait_for(
                    client.exchange('127.0.0.1', frontend, stats, scheduled_at,
                                    capacity, slot), args.request_timeout))
                # Also releases admissions cancelled before exchange ever starts.
                task.add_done_callback(lambda done, slot=slot: capacity.release(slot))
                pending.add(task)
            stats['max_in_flight_requests'] = max(stats.get('max_in_flight_requests', 0), len(pending))
            observe.update(stats)
            # Wait for completion or the next scheduled heartbeat/snapshot, not a busy loop.
            timeout = max(0.001, args.total_timeout - (time.monotonic() - started))
            if len(pending) < args.concurrency and idle:
                timeout = min(timeout, max(0.001, idle[0][0] - time.monotonic()))
            if full_at is not None and snapshot is None:
                timeout = min(timeout, max(0.001, next_snapshot - time.monotonic()))
            waiting = set(pending)
            if snapshot is not None: waiting.add(snapshot)
            if waiting:
                await asyncio.wait(waiting, timeout=timeout, return_when=asyncio.FIRST_COMPLETED)
            else:
                await asyncio.sleep(timeout)
        # Finish already issued requests before closing; do not start more work.
        if pending:
            await asyncio.gather(*pending)
        assert min(c.responses for c in clients) >= 2, 'some connections were not reused'
        assert max(c.max_gap for c in clients) < args.max_gap, 'final heartbeat gap exceeded'
        observe.update(stats)
        observe.update(hold_seconds=round(time.monotonic() - full_at, 6),
                       min_responses_per_connection=min(c.responses for c in clients),
                       max_responses_per_connection=max(c.responses for c in clients),
                       max_heartbeat_gap_seconds=round(max(c.max_gap for c in clients), 6),
                       elapsed_seconds=round(time.monotonic() - started, 6))
    finally:
        observed_at = time.monotonic()
        observe['timing_at_stop'] = dict(
            observed_at=observed_at,
            unfinished_requests=[dict(c.timing) for c in clients
                                 if c.timing and c.timing['response_completed_at'] is None],
            oldest_idle_request=(dict(connection=idle[0][2].index,
                                      scheduled_at=idle[0][0],
                                      previous_response_at=idle[0][2].last,
                                      last_request=idle[0][2].timing) if idle else None))
        observe['capacity_timing'] = dict(
            full_seconds=capacity.full_time_at(observed_at),
            elapsed_seconds=observed_at - started,
            transition_count=len(capacity.times) - 1,
            history_bytes=(len(capacity.times) + len(capacity.totals)) * capacity.times.itemsize,
            scope='admission to exchange finally; completed tasks awaiting scheduler reaping are free; slot waits end at admission')
        for task in pending:
            if not task.done(): task.cancel()
        if pending: await asyncio.gather(*pending, return_exceptions=True)
        if snapshot is not None:
            await snapshot
        observe.update(stats)
        observe['elapsed_seconds'] = round(time.monotonic() - started, 6)
        observe['max_completed_heartbeat_gap_seconds'] = round(max((c.max_gap for c in clients), default=0), 6)
        for client in clients: client.close()
        await asyncio.sleep(0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--connections', type=int, default=1000)
    parser.add_argument('--contexts', type=int, default=8)
    parser.add_argument('--concurrency', type=int, default=4)
    parser.add_argument('--heartbeat', type=float, default=2.0)
    parser.add_argument('--max-gap', type=float, default=3.5)
    parser.add_argument('--hold', type=float, default=6.0)
    parser.add_argument('--total-timeout', type=float, default=60.0)
    parser.add_argument('--request-timeout', type=float, default=3.0)
    parser.add_argument('--profile', action='store_true')
    parser.add_argument('--nonempty-body', action='store_true', help='Use identifying echo bodies instead of legacy empty-POST workload')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if not 1 <= args.connections <= 10000: parser.error('connections must be 1..10000')
    if not 1 <= args.concurrency <= args.contexts <= 4096: parser.error('require 1 <= concurrency <= contexts <= 4096')
    if not 0 < args.heartbeat < args.max_gap < 4: parser.error('require 0 < heartbeat < max-gap < backend idle 4s')
    if not 5 < args.hold < args.total_timeout <= 120: parser.error('require 5 < hold < total-timeout <= 120')
    if not 0 < args.request_timeout < 10: parser.error('request-timeout must be 0..10s exclusive')
    output = args.output or ROOT / 'artifacts' / ('c10k-retain-%d-%s.json' %
        (args.connections, datetime.now().strftime('%Y%m%d-%H%M%S-%f')))
    output.parent.mkdir(parents=True, exist_ok=True)
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    wanted = min(65536, hard) if hard != resource.RLIM_INFINITY else 65536
    resource.setrlimit(resource.RLIMIT_NOFILE, (max(soft, wanted), hard))
    capacity = max(16, args.connections + max(28, args.connections // 10))
    backend, frontend = port(), port()
    while frontend == backend: frontend = port()
    env = dict(os.environ, JUNGLE_CONNECTIONS=str(capacity), JUNGLE_CONTEXTS=str(args.contexts),
               PROXY_CONNECTIONS=str(capacity), PROXY_CONTEXTS=str(args.contexts),
               JUNGLE_PROFILE='1' if args.profile else '0', PROXY_PROFILE='1' if args.profile else '0')
    observe, processes, logs = {}, [], []
    result = dict(status='failed', connections=args.connections, configured_connections=capacity,
        contexts_each=args.contexts, concurrency=args.concurrency, empty_body=not args.nonempty_body, heartbeat_seconds=args.heartbeat,
        allowed_max_gap_seconds=args.max_gap, required_hold_seconds=args.hold, profile=args.profile,
        fd_soft_before=soft, fd_soft_test=max(soft, wanted),
        binary_sha256={name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                       for name in ('webserver', 'reverse_proxy')},
        timing_clock='time.monotonic seconds; same loader clock, arbitrary origin',
        timing_send_boundary='immediately before StreamWriter.write; drain is local buffer backpressure, neither proves wire send or server receipt',
        timing_schedule='reuse: previous completion + heartbeat; first: admission time (no preassigned ramp deadline)',
        snapshot_method='separate subprocess; no loader GIL sharing',
        scope='connection retention/reuse with bounded concurrent heartbeats; not throughput benchmark',
        observation=observe)
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        with tempfile.TemporaryDirectory(prefix='jungle-retain-') as directory:
            try:
                for command, target in [(['./webserver', str(backend), directory], backend),
                    (['./reverse_proxy', str(frontend), '127.0.0.1', str(backend)], frontend)]:
                    log = tempfile.TemporaryFile(mode='w+')
                    logs.append(log)
                    process = subprocess.Popen(command, cwd=str(ROOT), env=env, stdout=log, stderr=log)
                    processes.append(process)
                    for _ in range(200):
                        if process.poll() is not None: raise AssertionError('server exited early')
                        try:
                            with socket.create_connection(('127.0.0.1', target), .1): break
                        except OSError: time.sleep(.01)
                    else: raise AssertionError('listener startup timeout')
                loop.run_until_complete(retain(args, frontend, processes[1].pid, backend, observe))
                for _ in range(150):
                    counts = socket_counts(processes[1].pid, frontend, backend)
                    if counts == {'frontend': 0, 'backend': 0}: break
                    time.sleep(.02)
                else: raise AssertionError('connections not reclaimed')
                result['final_established'] = counts
                result['status'] = 'passed'
            finally:
                for process in reversed(processes):
                    if process.poll() is None:
                        process.terminate()
                        try: process.wait(timeout=3)
                        except subprocess.TimeoutExpired: process.kill(); process.wait()
                for index, log in enumerate(logs):
                    log.seek(0)
                    lines = log.read().splitlines()
                    key = 'server_profile' if index == 0 else 'proxy_profile'
                    for line in lines:
                        if line.startswith('[' + key + '] '): result[key] = json.loads(line.split('] ', 1)[1])
                    if index == 0:
                        result['backend_accepts_excluding_probe'] = sum('[accept fd=' in x for x in lines) - 1
                        result['backend_close_reasons'] = dict(Counter(x.split('] ', 1)[-1] for x in lines if '[close fd=' in x))
                result['server_exit_codes'] = [p.returncode for p in processes]
                if result['status'] == 'passed':
                    assert all(p.returncode == 0 for p in processes), 'server shutdown failed'
                    assert result['backend_accepts_excluding_probe'] == args.connections, 'backend reconnected'
                    assert observe['tcp_opens'] == args.connections, 'frontend reconnected'
    except Exception as error:
        result['status'] = 'failed'
        result.update(error_type=type(error).__name__, error=str(error))
    finally:
        loop.run_until_complete(loop.shutdown_asyncgens())
        loop.close()
        for log in logs: log.close()
        output.write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
    if result['status'] != 'passed': raise SystemExit(1)


if __name__ == '__main__':
    if len(sys.argv) == 5 and sys.argv[1] == '--snapshot':
        print(json.dumps(socket_counts(*map(int, sys.argv[2:]))))
    else:
        main()
