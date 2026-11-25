"""Actual TCP pressure beyond the buffer count, including half-close and abort."""
import http.client
import socket
import struct
import threading
import unittest
import test_proxy as support


class BufferWaitTests(unittest.TestCase):
    start = support.ProxyTests.start
    stop = support.ProxyTests.stop
    mock = support.ProxyTests.mock

    def held_backend(self):
        release = threading.Event()
        def reply(peer, request):
            if request.startswith(b'GET /hold '):
                release.wait(6)
            peer.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
        port, backend, process = self.mock(reply, contexts=1)
        self.addCleanup(release.set)
        holder = self.send(port, '/hold')
        self.assertIn(b'/hold', backend.seen.get(timeout=2))
        return port, backend, process, release, holder

    def send(self, port, path):
        peer = socket.create_connection(('127.0.0.1', port), 2)
        self.addCleanup(peer.close)
        peer.sendall(('GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' % path).encode())
        return peer

    def waiting(self, peer):
        peer.settimeout(.08)
        with self.assertRaises(socket.timeout):
            peer.recv(1)  # Previously this returned 503 immediately.

    def response(self, peer):
        peer.settimeout(3)
        response = http.client.HTTPResponse(peer)
        response.begin()
        self.assertEqual((response.status, response.read()), (200, b''))

    def test_fifo_resume_and_half_closed_request(self):
        port, backend, _, release, holder = self.held_backend()
        first = self.send(port, '/first')
        self.waiting(first)
        second = self.send(port, '/second')
        second.shutdown(socket.SHUT_WR)  # FIN after a request still permits its response.
        self.waiting(second)
        self.assertTrue(backend.seen.empty())
        release.set()
        self.response(holder)
        self.response(first)
        self.response(second)
        paths = [backend.seen.get(timeout=2).split(b' ', 2)[1] for _ in range(2)]
        self.assertEqual(paths, [b'/first', b'/second'])

    def test_reset_waiter_removed_and_next_request_recovers(self):
        port, backend, _, release, holder = self.held_backend()
        aborted = self.send(port, '/aborted')
        self.waiting(aborted)
        aborted.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        aborted.close()
        healthy = self.send(port, '/healthy')
        self.waiting(healthy)
        release.set()
        self.response(holder)
        self.response(healthy)
        self.assertEqual(backend.seen.get(timeout=2).split(b' ', 2)[1], b'/healthy')
        self.assertTrue(backend.seen.empty())

    def test_shutdown_with_waiters(self):
        port, _, process, _, _ = self.held_backend()
        waiting = self.send(port, '/waiting')
        self.waiting(waiting)
        process.terminate()
        self.assertEqual(process.wait(timeout=2), 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
