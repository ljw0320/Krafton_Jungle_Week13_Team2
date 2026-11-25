"""Local smoke/throughput evidence; uses three actual C server processes."""
import concurrent.futures
import http.client
from pathlib import Path
import socket
import subprocess
import tempfile
import time

repo = Path(__file__).resolve().parents[1]
binary = repo / 'build/web-server'
procs = []


def free_port():
    with socket.socket() as peer:
        peer.bind(('127.0.0.1', 0))
        return peer.getsockname()[1]


def start(port, root, *extra):
    proc = subprocess.Popen([str(binary), str(port), str(root), '64', '2', '1', *extra],
                            cwd=repo, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    procs.append(proc)
    assert proc.stdout.readline().startswith('READY')


with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    roots = [work / name for name in ('a', 'b', 'proxy')]
    for root in roots:
        root.mkdir()
    (roots[0] / 'api').mkdir()
    (roots[0] / 'api/hello.txt').write_bytes(b'backend-a')
    (roots[1] / 'assets').mkdir()
    (roots[1] / 'assets/hello.txt').write_bytes(b'backend-b')
    (roots[2] / 'index.html').write_bytes(b'local')
    ports = [free_port() for _ in roots]
    config = work / 'routes.conf'
    config.write_text(f'/api 127.0.0.1 {ports[0]}\n/assets 127.0.0.1 {ports[1]}\n')
    try:
        start(ports[0], roots[0])
        start(ports[1], roots[1])
        start(ports[2], roots[2], '--routes', str(config))
        for path, expected in [('/api/hello.txt?q=1', b'backend-a'),
                               ('/assets/hello.txt', b'backend-b'), ('/', b'local')]:
            result = subprocess.run(['curl', '--fail', '--silent', '--show-error',
                                     '-H', 'Host: public.example',
                                     f'http://127.0.0.1:{ports[2]}{path}'],
                                    capture_output=True, check=True)
            assert result.stdout == expected, result
        print('curl: backend A / backend B / local fallback passed')

        def exchange(_):
            conn = http.client.HTTPConnection('127.0.0.1', ports[2], timeout=10)
            try:
                conn.request('GET', '/api/hello.txt', headers={'Host': 'public.example'})
                result = conn.getresponse()
                assert result.status == 200 and result.read() == b'backend-a'
            finally:
                conn.close()
        began = time.monotonic()
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            list(pool.map(exchange, range(256)))
        elapsed = time.monotonic() - began
        print(f'load: 256/256 successful, concurrency=16, body=9 bytes, '
              f'new client/upstream connection per request, elapsed={elapsed:.3f}s, '
              f'throughput={256 / elapsed:.1f} requests/s')
        print(subprocess.check_output(['cc', '--version'], text=True).splitlines()[0])
        print(subprocess.check_output(['uname', '-sr'], text=True).strip())
        print('build: -std=c11 -Wall -Wextra -Wpedantic -Werror -O2; loopback IPv4')
    finally:
        for proc in reversed(procs):
            proc.terminate()
            _, error = proc.communicate(timeout=5)
            assert proc.returncode == 0 and not error, (proc.returncode, error)
