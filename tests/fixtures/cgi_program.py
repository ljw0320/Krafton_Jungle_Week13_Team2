#!/usr/bin/python3
import os
from pathlib import Path
import signal
import sys
import time


def write_all(data):
    while data:
        data = data[os.write(1, data):]


mode = Path('mode').read_text()
valid = b'Content-Type: text/plain\r\n\r\n'
if mode == 'timeout':
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    time.sleep(60)
elif mode == 'eof-alive':
    os.close(1)
    time.sleep(60)
elif mode == 'trickle':
    os.write(1, valid)
    while True:
        os.write(1, b'x')
        time.sleep(.1)
elif mode == 'overflow':
    while True:
        os.write(1, b'x' * 8192)
elif mode == 'limit':
    write_all(valid + b'x' * (65536 - len(valid)))
elif mode == 'nonzero':
    os.write(1, valid + b'false success')
    sys.exit(127)
elif mode == 'signal':
    os.kill(os.getpid(), signal.SIGKILL)
elif mode == 'invalid':
    os.write(1, b'not a CGI response')
elif mode == 'duplicate':
    os.write(1, valid.replace(b'\r\n\r\n', b'\r\nContent-Type: text/html\r\n\r\n'))
elif mode == 'injection':
    os.write(1, b'Content-Type: text/plain\rInjected: yes\n\nsecret')
elif mode == 'nul':
    os.write(1, b'Content-Type: text/plain\x00\n\nsecret')
elif mode == 'header-overflow':
    os.write(1, b'Content-Type: ' + b'x' * 8200 + b'\n\n')
elif mode == 'status':
    os.write(1, b'Status: 400 Bad Request\nContent-Type: text/plain\n\ninvalid')
elif mode == 'binary':
    os.write(1, b'Content-Type: application/octet-stream\r\n\r\n\x00\xff')
elif mode == 'custom':
    write_all(Path('output').read_bytes())
elif mode == 'descendant':
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    child = os.fork()
    if child == 0:
        os.close(0)
        os.close(1)
        time.sleep(60)
        os._exit(0)
    Path('descendant.pid').write_text(str(child))
    os.write(1, valid + b'clean')
elif mode == 'duplex':
    write_all(valid + b'x' * 60000)
    incoming = sys.stdin.buffer.read()
    assert len(incoming) == int(os.environ['CONTENT_LENGTH'])
    os.write(1, incoming)
elif mode == 'duplex-pressure':
    import fcntl
    assert fcntl.fcntl(0, fcntl.F_GETPIPE_SZ) < int(os.environ['CONTENT_LENGTH'])
    fcntl.fcntl(1, fcntl.F_SETPIPE_SZ, 4096)
    assert fcntl.fcntl(1, fcntl.F_GETPIPE_SZ) < 60000
    write_all(valid + b'x' * 60000)
    incoming = sys.stdin.buffer.read()
    assert incoming == b'x' * int(os.environ['CONTENT_LENGTH'])
    os.write(1, b'drained')
elif mode == 'fd-check':
    assert not any(os.readlink('/proc/self/fd/' + f).startswith('socket:')
                   for f in os.listdir('/proc/self/fd') if f.isdigit() and
                   os.path.exists('/proc/self/fd/' + f))
    assert 'PATH' not in os.environ
    os.write(1, valid + b'clean')
