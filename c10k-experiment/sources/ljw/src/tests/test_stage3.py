"""Minimal exhaustion/resume/deadline smoke check; run from repository root."""
import socket, subprocess, time
p = subprocess.Popen(['./webserver', '18993', 'www'], stdout=subprocess.DEVNULL)
clients = []
def connect():
    s = socket.create_connection(('127.0.0.1', 18993), timeout=3)
    clients.append(s)
    return s
try:
    time.sleep(.3)
    holders = [connect() for _ in range(8)]
    for s in holders: s.sendall(b'GET / HTTP/1.1\r\n')
    time.sleep(.2)
    waiter = connect()
    waiter.sendall(b'GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
    waiter.settimeout(.2)
    try:
        waiter.recv(1)
        raise AssertionError('must wait')
    except socket.timeout: pass
    holders[0].close()
    waiter.settimeout(2)
    assert b'200' in waiter.recv(1024)
    # Restore exhaustion, then check the absolute wait deadline.
    extra = connect(); extra.sendall(b'GET / HTTP/1.1\r\n')
    time.sleep(.2)
    expired = connect(); expired.sendall(b'G')
    expired.settimeout(3)
    try: assert expired.recv(1) == b''
    except ConnectionResetError: pass
    print('stage3: exhaustion, resume, wait timeout passed')
finally:
    for s in clients: s.close()
    p.terminate(); p.wait(timeout=3)
