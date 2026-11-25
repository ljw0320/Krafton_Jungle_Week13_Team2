"""Verify opt-in profiling, real worker aggregation and unchanged HTTP results."""
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
ROOT = Path(__file__).resolve().parents[1]

def run(enabled):
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0)); port = reservation.getsockname()[1]
    env = dict(os.environ, JUNGLE_PROFILE='1' if enabled else '0')
    for key in ('JUNGLE_CONNECTIONS', 'JUNGLE_CONTEXTS'): env.pop(key, None)
    with tempfile.TemporaryDirectory() as root:
        process = subprocess.Popen(['./webserver', str(port), root], cwd=str(ROOT), env=env,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        responses = []
        try:
            for _ in range(200):
                if process.poll() is not None: raise AssertionError('server exited early')
                try:
                    with socket.create_connection(('127.0.0.1', port), .1): break
                except OSError: time.sleep(.01)
            else: raise AssertionError('listener did not start')
            client = http.client.HTTPConnection('127.0.0.1', port, timeout=3)
            try:
                for body in (b'a', b'binary\x00\xff', b'c', b'd'):
                    client.request('POST', '/echo', body)
                    response = client.getresponse()
                    responses.append((response.status, response.read(), response.getheader('Connection')))
                    assert responses[-1] == (200, body, 'keep-alive')
            finally: client.close()
        finally:
            process.terminate()
            try: _, stderr = process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill(); process.communicate(); raise AssertionError('shutdown hung')
        assert process.returncode == 0
        lines = [line for line in stderr.decode().splitlines() if line.startswith('[server_profile] ')]
        if not enabled:
            assert not lines
            return responses
        assert len(lines) == 1
        profile = json.loads(lines[0][len('[server_profile] '):])
        assert set(profile) == {'main', 'workers'}
        for metrics in profile.values():
            for value in metrics.values():
                assert all(v >= 0 for v in value.values())
        worker = profile['workers']
        assert worker['nonterminal_turn']['calls'] >= 4
        assert worker['nonterminal_pending_wall_only']['calls'] == worker['nonterminal_turn']['calls']
        assert worker['nonterminal_queue_wall_only']['calls'] == worker['nonterminal_turn']['calls']
        assert worker['nonterminal_turn']['calls'] <= worker['request_turn']['calls']
        assert worker['request_turn']['max_wall_seconds'] > 0
        assert worker['request_turn']['max_cpu_seconds'] > 0
        assert worker['queue_residence_wall_only']['calls'] == worker['queue_take']['units']
        pending = profile['main']['pending_residence_wall_only']
        assert pending['calls'] >= 4
        assert pending['max_wall_seconds'] > 0
        assert pending['cpu_seconds'] == pending['max_cpu_seconds'] == 0
        assert worker['request_turn']['calls'] >= 4
        assert worker['queue_take']['units'] == worker['request_turn']['calls']
        assert worker['publish_result']['calls'] == worker['request_turn']['calls']
        assert worker['notify_main']['calls'] == worker['request_turn']['calls']
        assert profile['main']['epoll_wait']['calls'] > 0
        assert profile['main']['prepare_changed']['units'] >= 4
        return responses

assert run(False) == run(True)
pool_env = dict(os.environ, JUNGLE_PROFILE='1')
for key in ('JUNGLE_CONTEXTS', 'JUNGLE_CONNECTIONS'): pool_env.pop(key, None)
subprocess.run(['make', 'test_context_pool'], cwd=str(ROOT), check=True,
               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
pool = subprocess.run(['./test_context_pool'], cwd=str(ROOT), env=pool_env,
                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
pool_json = [line.split('] ', 1)[1] for line in pool.stderr.decode().splitlines()
             if line.startswith('[server_profile] ')]
assert len(pool_json) == 1
pool_profile = json.loads(pool_json[0])
assert pool_profile['main']['context_wait_started']['calls'] == 5
assert pool_profile['main']['context_wait_started']['max_units'] == 2
assert pool_profile['main']['context_wait_acquired_wall_only']['calls'] == 3
assert pool_profile['main']['context_wait_cancelled_wall_only']['calls'] == 2
assert pool_profile['workers']['context_mutex_wait']['calls'] > 0
print('server profile: disabled silence, enabled JSON, worker aggregation, HTTP parity passed')
