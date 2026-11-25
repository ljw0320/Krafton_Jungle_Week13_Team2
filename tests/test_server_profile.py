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
        assert worker['request_turn']['calls'] >= 4
        assert worker['queue_take']['units'] == worker['request_turn']['calls']
        assert worker['publish_result']['calls'] == worker['request_turn']['calls']
        assert worker['notify_main']['calls'] == worker['request_turn']['calls']
        assert profile['main']['poll']['calls'] > 0
        assert profile['main']['prepare_scan']['units'] >= 16
        return responses

assert run(False) == run(True)
print('server profile: disabled silence, enabled JSON, worker aggregation, HTTP parity passed')
