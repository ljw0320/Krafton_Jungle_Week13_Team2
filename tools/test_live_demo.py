"""Exercise the browser-facing demo without touching the default ports."""
from http.client import HTTPConnection
import json
from pathlib import Path
import socket
import subprocess
import time
import unittest

from live_demo_bots import Client
from chat_bot import check_upgrade, handshake


ROOT = Path(__file__).resolve().parents[1]
PATH = "/api/live-test"


class BrowserSocket(Client):
    def __init__(self, address, origin):
        self.buf = bytearray()
        self.sock = socket.create_connection(address, timeout=2)
        try:
            request, accept = handshake(address, "/ws/live")
            self.send_raw(request[:-2] + f"Origin: {origin}\r\n\r\n".encode())
            while b"\r\n\r\n" not in self.buf:
                self._read()
            head, _, self.buf = self.buf.partition(b"\r\n\r\n")
            check_upgrade(bytes(head), accept)
        except BaseException:
            self.close()
            raise


class DemoTest(unittest.TestCase):
    def test_fixed_comparison(self):
        with socket.socket() as spare:
            spare.bind(("127.0.0.1", 0))
            port = spare.getsockname()[1]
        origin = f"http://127.0.0.1:{port}"

        def request(method, origin_header=None, fetch_site=None):
            with_headers = {} if origin_header is None else {"Origin": origin_header}
            if fetch_site is not None:
                with_headers["Sec-Fetch-Site"] = fetch_site
            connection = HTTPConnection("127.0.0.1", port, timeout=2)
            try:
                connection.request(method, PATH, headers=with_headers)
                response = connection.getresponse()
                return response.status, json.loads(response.read())
            finally:
                connection.close()

        launcher = subprocess.Popen(["bash", "tools/run_live_demo.sh", str(port)], cwd=ROOT)
        owned = []
        try:
            for _ in range(100):
                self.assertIsNone(launcher.poll(), "demo launcher exited before serving")
                try:
                    code, state = request("GET")
                    break
                except ConnectionRefusedError:
                    time.sleep(.05)
            else:
                self.fail("demo did not start")
            self.assertEqual((code, state["status"]), (200, "idle"))
            self.assertEqual(request("GET", fetch_site="same-origin")[0], 200)
            self.assertEqual(request("GET", "http://localhost:1")[0], 403)
            self.assertEqual(request("GET", fetch_site="cross-site")[0], 403)
            self.assertEqual(request("GET", fetch_site="same-site")[0], 403)
            for path, media in (("/live.html", "text/html"), ("/live.js", "text/javascript"),
                                ("/style.css", "text/css"), ("/live.css", "text/css")):
                connection = HTTPConnection("127.0.0.1", port, timeout=2)
                try:
                    connection.request("GET", path)
                    response = connection.getresponse()
                    self.assertEqual(response.status, 200)
                    self.assertEqual(response.headers.get_content_type(), media)
                    self.assertTrue(response.read())
                finally:
                    connection.close()
            address = "127.0.0.1", port
            with self.assertRaisesRegex(ValueError, "403"):
                BrowserSocket(address, "http://localhost:1")
            first = BrowserSocket(address, origin)
            self.addCleanup(first.close)
            second = BrowserSocket(address, origin)
            self.addCleanup(second.close)
            self.assertEqual(json.loads(first.recv(2)[1]), {"backend": "A"})
            self.assertEqual(json.loads(second.recv(2)[1]), {"backend": "A"})
            self.assertEqual(request("POST", "http://localhost:1")[0], 403)
            self.assertEqual(request("POST", origin)[0], 202)
            self.assertEqual(request("POST", origin)[0], 409)
            manual = "시험 중 화면에서 보낸 메시지"
            first.send_text(manual)
            for client in (first, second):
                self.assertEqual(client.recv(2), (1, manual.encode()))
            for _ in range(240):
                state = request("GET")[1]
                if state["status"] != "running":
                    break
                time.sleep(.2)
            self.assertEqual(state["status"], "completed", state)
            for name, assignment in (("1", (10, 0)), ("2", (5, 5))):
                result = state["results"][name]
                self.assertEqual(tuple(len(result["assignments"][side]) for side in "AB"),
                                 assignment)
                self.assertEqual((result["received"], result["missing"], result["duplicates"],
                                  result["out_of_order"]), (300, 0, 0, 0))
                self.assertLessEqual(0, result["p50_ms"])
                self.assertLessEqual(result["p50_ms"], result["p95_ms"])
            self.assertEqual(state["results"]["2"]["cross_backend"],
                             {"A_to_B": 75, "B_to_A": 75})
            self.assertEqual(request("POST", origin)[0], 202)
            for _ in range(100):
                owned = subprocess.check_output(["ps", "-o", "pid=", "--ppid",
                                                 str(launcher.pid)], text=True).split()
                if len(owned) >= 6:
                    break
                time.sleep(.02)
            self.assertEqual(len(owned), 6, owned)
        finally:
            if launcher.poll() is None:
                launcher.terminate()
            launcher.wait(timeout=10)
        self.assertEqual(launcher.returncode, 0)
        for pid in owned:
            self.assertFalse(Path(f"/proc/{pid}").exists(), f"demo process {pid} was left running")
        with socket.socket() as probe:
            self.assertNotEqual(probe.connect_ex(("127.0.0.1", port)), 0)

    def test_occupied_port_stays_open(self):
        with socket.socket() as occupied:
            occupied.bind(("127.0.0.1", 0))
            occupied.listen()
            port = occupied.getsockname()[1]
            process = subprocess.run(["bash", "tools/run_live_demo.sh", str(port)], cwd=ROOT,
                                     capture_output=True, timeout=5)
            self.assertEqual(process.returncode, 1, process.stderr)
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                pass


if __name__ == "__main__":
    unittest.main()
