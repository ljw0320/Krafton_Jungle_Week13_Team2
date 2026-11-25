"""Reproduce a six-run comparison without changing the active checkout or binaries."""
import argparse
from datetime import datetime
import hashlib
import io
import json
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
COMMITS = {'before': '9dd357f', 'after': '80b5a16'}
ORDER = ['before', 'after', 'after', 'before', 'before', 'after']


def call(args, cwd=ROOT):
    return subprocess.check_output(args, cwd=str(cwd), stderr=subprocess.STDOUT)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path,
                        help='Result directory (default: artifacts/server-queues-<timestamp>)')
    args = parser.parse_args()
    OUTPUT = (args.output_dir or
              ROOT / 'artifacts' / ('server-queues-' + datetime.now().strftime('%Y%m%d-%H%M%S-%f'))).resolve()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    if (OUTPUT / 'runs.json').exists():
        raise RuntimeError('Existing measurement results must be preserved; choose a new --output-dir')
    started = time.monotonic()
    metadata = dict(commits={k: call(['git', 'rev-parse', v]).decode().strip()
                             for k, v in COMMITS.items()}, order=ORDER,
        compiler=call(['cc', '--version']).decode().splitlines()[0],
        kernel=platform.release(), python=sys.version, warmup=False,
        condition='10000 empty POST, concurrency4, contexts8, capacity11000, both profiles on; one round; no loader changes',
        note='Alternating paired order AB/BA/AB. Expected retention failures are recorded, not suppressed as passes.')
    runs = []
    with tempfile.TemporaryDirectory(prefix='jungle-version-compare-') as directory:
        builds = {}
        for name, sha in metadata['commits'].items():
            target = Path(directory) / name
            target.mkdir()
            archive = call(['git', 'archive', '--format=zip', sha,
                            'src', 'common', 'proxy', 'Makefile', 'tests/check_capacity.py'])
            with zipfile.ZipFile(io.BytesIO(archive)) as zipped:
                zipped.extractall(str(target))
            build = call(['make', 'webserver'], target)
            (OUTPUT / (name + '-build.log')).write_bytes(build)
            builds[name] = target
        # One proxy executable and the identical runner for both server variants.
        proxy_build = call(['make', 'reverse_proxy'], builds['after'])
        (OUTPUT / 'proxy-build.log').write_bytes(proxy_build)
        proxy = (builds['after'] / 'reverse_proxy').read_bytes()
        dest = builds['before'] / 'reverse_proxy'
        dest.write_bytes(proxy)
        dest.chmod(0o755)
        assert digest(builds['before'] / 'tests/check_capacity.py') == digest(builds['after'] / 'tests/check_capacity.py')
        metadata['runner_sha256'] = digest(builds['after'] / 'tests/check_capacity.py')
        metadata['proxy_sha256'] = digest(builds['after'] / 'reverse_proxy')
        metadata['server_sha256'] = {k: digest(v / 'webserver') for k, v in builds.items()}
        (OUTPUT / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
        for index, name in enumerate(ORDER, 1):
            label = '%02d-%s' % (index, name)
            output = OUTPUT / (label + '.json')
            command = ['python3', 'tests/check_capacity.py', '--connections', '10000',
                       '--empty-body', '--rounds', '1', '--server-profile', '--proxy-profile',
                       '--output', str(output)]
            print('[start] ' + label, flush=True)
            with (OUTPUT / (label + '.log')).open('wb') as log:
                process = subprocess.run(command, cwd=str(builds[name]), stdout=log,
                                         stderr=subprocess.STDOUT, timeout=90)
            result = json.loads(output.read_text())
            request = result['timings'][0]
            main_metrics = result['server_profile']['main']
            prep = main_metrics.get('prepare_changed', main_metrics.get('prepare_scan'))
            requests = result.get('completed_requests', result.get('requests'))
            assert requests == 10000, (label, result)
            assert process.returncode in (0, 1), (label, process.returncode)
            assert process.returncode == 0 or result.get('error_type') == 'AssertionError'
            row = dict(run=index, version=name, exit_code=process.returncode, status=result['status'],
                completed_requests=requests, wall_seconds=request['wall_seconds'],
                cpu_seconds=request['cpu_seconds'], prepare_cpu_seconds=prep['cpu_seconds'],
                prepare_units=prep['units'], loop_calls=main_metrics['epoll_wait']['calls'],
                first_snapshot=result['timings'][1].get('observed_established'),
                final_observation=result.get('observed_established'), report=output.name)
            runs.append(row)
            (OUTPUT / 'runs.json').write_text(json.dumps(runs, indent=2) + '\n')
            print('[done] ' + json.dumps(row), flush=True)
    summary = {}
    for name in COMMITS:
        subset = [r for r in runs if r['version'] == name]
        metrics = {'wall_seconds': [r['wall_seconds'] for r in subset],
                   'backend_cpu_seconds': [r['cpu_seconds']['backend'] for r in subset],
                   'proxy_cpu_seconds': [r['cpu_seconds']['proxy'] for r in subset],
                   'loader_cpu_seconds': [r['cpu_seconds']['loader'] for r in subset],
                   'prepare_cpu_seconds': [r['prepare_cpu_seconds'] for r in subset]}
        summary[name] = {k: dict(median=statistics.median(v), minimum=min(v), maximum=max(v))
                         for k, v in metrics.items()}
    summary['elapsed_seconds'] = round(time.monotonic() - started, 3)
    (OUTPUT / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print('[summary] ' + json.dumps(summary), flush=True)


if __name__ == '__main__':
    main()
