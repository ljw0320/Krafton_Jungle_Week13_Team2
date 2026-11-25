"""Linux/WSL only. No third-party dependencies. See README.md for limitations."""
import argparse
import asyncio
import csv
import difflib
import io
import json
import math
import os
from pathlib import Path
import platform
import resource
import signal
import socket
import subprocess
import tarfile
import time

BASE = Path(__file__).resolve().parent
REPO = BASE.parent / 'Krafton_Jungle_Week13_Team2'
BODY = b'c' * 1024
REQUEST = b'GET /payload.txt HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n'


def percentile(values, fraction):
    return sorted(values)[max(0, math.ceil(len(values) * fraction) - 1)] if values else None


async def read_response(reader):
    header = await reader.readuntil(b'\r\n\r\n')
    lines = header.decode('ascii').split('\r\n')
    status = int(lines[0].split()[1])
    fields = dict(line.split(':', 1) for line in lines[1:] if ':' in line)
    fields = {k.lower(): v.strip() for k, v in fields.items()}
    if 'content-length' not in fields:
        raise ValueError('response has no Content-Length')
    return status, await reader.readexactly(int(fields['content-length']))


def run(command, **kwargs):
    return subprocess.check_output(command, **kwargs)


async def wait_process(process, timeout):
    deadline = time.monotonic() + timeout
    while process.poll() is None:
        if time.monotonic() >= deadline:
            return False
        await asyncio.sleep(.05)
    return True


def prepare():
    configs = {}
    for name, ref in [('nuri', 'origin/nuri'), ('ljw', 'origin/ljw'), ('shw', 'origin/shw')]:
        commit = run(['git', '-C', str(REPO), 'rev-parse', ref], text=True).strip()
        root = BASE / 'sources' / name
        root.mkdir(parents=True, exist_ok=True)
        if any(root.iterdir()):
            raise RuntimeError(f'{root} already exists; preserve results and use another directory')
        archive = run(['git', '-C', str(REPO), 'archive', commit])
        with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
            tar.extractall(root, filter='data')
        changes = []
        replacements = {
            'nuri': [('main.c', 'parse_positive_integer(argv[3], 4096)', 'parse_positive_integer(argv[3], 12000)')],
            'ljw': [('src/server.h', 'MAX_CONNECTIONS = 16', 'MAX_CONNECTIONS = 12000'),
                    ('src/server.h', 'KEEP_ALIVE_TIMEOUT_MS = 5000', 'KEEP_ALIVE_TIMEOUT_MS = 600000')],
            'shw': [('src/conn.h', '#define IDLE_TIMEOUT_SEC 10', '#define IDLE_TIMEOUT_SEC 600')],
        }
        for filename, old, new in replacements[name]:
            path = root / filename
            before = path.read_text()
            if before.count(old) != 1:
                raise ValueError(f'patch mismatch {name}/{filename}: {old}')
            after = before.replace(old, new)
            path.write_text(after)
            changes.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True), fromfile=filename, tofile=filename))
        (BASE / 'results' / f'{name}-experiment.patch').write_text(''.join(changes))
        with (BASE / 'results' / f'{name}-build.log').open('w') as log:
            subprocess.run(['make', 'CFLAGS=-std=gnu11 -Wall -Wextra -O2'], cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
        configs[name] = {'commit': commit, 'root': str(root)}
    (BASE / 'results' / 'commits.json').write_text(json.dumps(configs, indent=2))


def snapshot(group):
    pids, threads, pss, ticks = 0, 0, 0, 0
    for path in Path('/proc').iterdir():
        if not path.name.isdigit():
            continue
        try:
            stat = (path / 'stat').read_text().rsplit(')', 1)[1].split()
            if int(stat[2]) != group:
                continue
            pids += 1
            # Include reaped connection children, whose CPU is charged to the parent.
            ticks += sum(int(stat[i]) for i in (11, 12, 13, 14))
            threads += int(stat[17])
            for line in (path / 'smaps_rollup').read_text().splitlines():
                if line.startswith('Pss:'):
                    pss += int(line.split()[1])
        except (OSError, ValueError):
            continue
    return {'processes': pids, 'threads': threads, 'pss_kib': pss, 'ticks': ticks}


def alive(writers):
    count = 0
    for writer in writers:
        sock = writer.get_extra_info('socket')
        try:
            # asyncio owns reads; EOF is published to its protocol/reader.
            if sock.fileno() >= 0 and not writer.is_closing() and not writer._protocol._stream_reader.at_eof():
                count += 1
        except (OSError, AttributeError):
            pass
    return count


async def connect(port):
    reader, writer = await asyncio.wait_for(asyncio.open_connection('127.0.0.1', port), 2)
    try:
        writer.write(REQUEST)
        await writer.drain()
        status, body = await asyncio.wait_for(read_response(reader), 2)
        if status != 200 or body != BODY:
            raise ValueError(f'invalid response {status}, {len(body)} bytes')
        return reader, writer
    except BaseException:
        writer.close()
        raise


async def measure(name, config, target, repeat, duration, rate, process_cap=1100):
    port = 18081
    root = Path(config['root'])
    document_root = BASE / 'public'
    commands = {'nuri': ['./build/web-server', str(port), str(document_root), '12000', '600', '1'],
                'ljw': ['./webserver', str(port), str(document_root)],
                'shw': ['./server', str(port), str(document_root)]}
    row = dict(branch=name, repeat=repeat, target=target, status='ok', setup_failures=0)
    writers, errors = [], {}
    log = (BASE / 'results' / f'{name}-{target}-r{repeat}.log').open('w')
    def limits():
        # Bound the process-per-connection experiment independently of host pid_max.
        resource.setrlimit(resource.RLIMIT_NPROC, (process_cap, process_cap))
    server = subprocess.Popen(commands[name], cwd=root, stdout=log, stderr=subprocess.STDOUT,
                              start_new_session=True, preexec_fn=limits)
    try:
        for attempt in range(100):
            if server.poll() is not None:
                raise RuntimeError(f'server exited {server.returncode}')
            try:
                _, w = await connect(port)
                w.close()
                await w.wait_closed()
                break
            except (OSError, asyncio.TimeoutError):
                await asyncio.sleep(.05)
        else:
            raise RuntimeError('server startup timed out')
        setup_start = time.monotonic()
        # Moderate batches prevent the client's connection burst from testing backlog.
        for offset in range(0, target, 25):
            batch = await asyncio.gather(*(connect(port) for _ in range(min(25, target-offset))), return_exceptions=True)
            for result in batch:
                if isinstance(result, BaseException):
                    row['setup_failures'] += 1
                    key = type(result).__name__
                    errors[key] = errors.get(key, 0) + 1
                else:
                    writers.append(result[1])
            if row['setup_failures'] >= 25:
                break
            if offset % 500 == 0:
                if int(Path('/proc/meminfo').read_text().split('MemAvailable:')[1].split()[0]) < 1024*1024:
                    row['status'] = 'memory_safety_stop'
                    break
        row['setup_seconds'] = round(time.monotonic()-setup_start, 3)
        await asyncio.sleep(.2)
        row['held_before'] = alive(writers)
        before = snapshot(server.pid)
        client_before = resource.getrusage(resource.RUSAGE_SELF)
        start = time.monotonic()
        latencies = []
        async def probe(index):
            await asyncio.sleep(max(0, start + index/rate - time.monotonic()))
            sent = time.monotonic()
            try:
                _, writer = await connect(port)
                latencies.append((time.monotonic()-sent)*1000)
                writer.close()
                try:
                    await writer.wait_closed()
                except OSError:
                    pass
            except (OSError, ValueError, asyncio.TimeoutError, asyncio.IncompleteReadError) as exc:
                key = type(exc).__name__
                errors[key] = errors.get(key, 0) + 1
        await asyncio.gather(*(probe(i) for i in range(round(duration*rate))))
        await asyncio.sleep(max(0, start+duration-time.monotonic()))
        elapsed = time.monotonic()-start
        client_after = resource.getrusage(resource.RUSAGE_SELF)
        after = snapshot(server.pid)
        row.update(held_after=alive(writers), requests=round(duration*rate), successes=len(latencies),
                   failures=round(duration*rate)-len(latencies), median_ms=percentile(latencies,.5),
                   p95_ms=percentile(latencies,.95), elapsed_seconds=elapsed,
                   server_cpu_percent=100*(after['ticks']-before['ticks'])/os.sysconf('SC_CLK_TCK')/elapsed,
                   client_cpu_percent=100*((client_after.ru_utime+client_after.ru_stime)-(client_before.ru_utime+client_before.ru_stime))/elapsed,
                   client_peak_rss_kib=client_after.ru_maxrss, processes=before['processes'], threads=before['threads'],
                   pss_kib=before['pss_kib'], pss_after_kib=after['pss_kib'], errors=errors)
        if row['held_before'] != target or row['held_after'] != target:
            row['status'] = 'target_not_maintained'
    finally:
        for writer in writers:
            writer.close()
        try:
            os.killpg(server.pid, signal.SIGTERM)
            await wait_process(server, 3)
            # Also clean descendants if the parent exited first.
            try:
                os.killpg(server.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            server.wait()
        except ProcessLookupError:
            server.wait()
        log.close()
        await asyncio.sleep(.3)
    return row


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--prepare', action='store_true')
    parser.add_argument('--levels', default='0,100,500,1000,3000,10000')
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--duration', type=float, default=8)
    parser.add_argument('--rate', type=int, default=10)
    parser.add_argument('--output', default='measurements.jsonl')
    parser.add_argument('--branches', default='nuri,ljw,shw')
    parser.add_argument('--nuri-max', type=int, default=1000)
    parser.add_argument('--process-cap', type=int, default=1100)
    parser.add_argument('--resume', action='store_true')
    args = parser.parse_args()
    (BASE/'results').mkdir(exist_ok=True)
    (BASE/'public').mkdir(exist_ok=True)
    (BASE/'public'/'payload.txt').write_bytes(BODY)
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (min(65536, hard), hard))
    if args.prepare:
        prepare()
    env = {'platform': platform.platform(), 'cpu': Path('/proc/cpuinfo').read_text(),
           'memory': Path('/proc/meminfo').read_text(), 'limits': Path('/proc/self/limits').read_text(),
           'arguments': vars(args), 'time_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
    env_suffix = f'.resume-{time.time_ns()}' if args.resume else ''
    (BASE/'results'/f'{args.output}{env_suffix}.environment.json').write_text(json.dumps(env, indent=2))
    configs = json.loads((BASE/'results'/'commits.json').read_text())
    output = BASE/'results'/args.output
    if output.exists() and not args.resume:
        raise RuntimeError(f'refusing to overwrite {output}')
    completed = set()
    if args.resume and output.exists():
        completed = {(r['branch'], r['target'], r['repeat']) for r in map(json.loads, output.read_text().splitlines())}
    with output.open('a' if args.resume else 'w') as file:
        for repeat in range(1, args.repeats+1):
            for target in map(int, args.levels.split(',')):
                # Rotate branch order between repeats to reduce consistent ordering bias.
                names = args.branches.split(',')
                names = names[repeat % 3:] + names[:repeat % 3]
                for name in names:
                    if (name, target, repeat) in completed:
                        continue
                    if name == 'nuri' and target > args.nuri_max:
                        row = dict(branch=name, repeat=repeat, target=target, status=f'not_run_process_safety_cap_{args.process_cap}')
                    else:
                        row = asyncio.run(measure(name, configs[name], target, repeat, args.duration, args.rate, args.process_cap))
                    file.write(json.dumps(row)+'\n')
                    file.flush()
                    print(json.dumps(row), flush=True)
    rows = [json.loads(line) for line in output.read_text().splitlines()]
    fields = list(dict.fromkeys(key for row in rows for key in row))
    with output.with_suffix('.csv').open('w', newline='') as file:
        writer = csv.DictWriter(file, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


if __name__ == '__main__':
    main()
