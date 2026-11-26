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

        def wait_for_demo(launcher):
            for _ in range(100):
                self.assertIsNone(launcher.poll(), "demo launcher exited before serving")
                try:
                    code, state = request("GET")
                except ConnectionRefusedError:
                    time.sleep(.05)
                else:
                    self.assertEqual((code, state["status"]), (200, "idle"))
                    self.assertEqual((state["run_id"], state["messages"]), (None, []))
                    return
            self.fail("demo did not start")

        launch_command = ["bash", "tools/run_live_demo.sh", str(port)]
        launcher = subprocess.Popen(launch_command, cwd=ROOT)
        owned = []
        try:
            wait_for_demo(launcher)
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
            code, started = request("POST", origin)
            run_id = started["run_id"]
            self.assertEqual((code, started["messages"]), (202, []))
            self.assertIsInstance(run_id, str)
            self.assertTrue(run_id)
            code, duplicate = request("POST", origin)
            self.assertEqual((code, duplicate["run_id"]), (409, run_id))
            manual = "시험 중 화면에서 보낸 메시지"
            first.send_text(manual)
            for client in (first, second):
                self.assertEqual(client.recv(2), (1, manual.encode()))
            messages = []
            partial_rounds = set()
            for _ in range(240):
                state = request("GET")[1]
                self.assertEqual(state["run_id"], run_id)
                current = state["messages"]
                self.assertEqual(current[:len(messages)], messages)
                messages = current
                if state["status"] != "running":
                    break
                if state["round"] == 1 and 0 < len(messages) < 30:
                    partial_rounds.add(1)
                if state["round"] == 2 and 30 < len(messages) < 60:
                    partial_rounds.add(2)
                time.sleep(.2)
            self.assertEqual(state["status"], "completed", state)
            self.assertEqual(partial_rounds, {1, 2})
            for round_number in (1, 2):
                expected = []
                for sequence in range(1, 31):
                    bot = f"bot-{(sequence - 1) % 10 + 1:02d}"
                    expected.append({"round": round_number, "sequence": sequence, "from": bot,
                                     "text": f"[{sequence:02d}] {bot}의 시험 메시지"})
                start = (round_number - 1) * 30
                self.assertCountEqual(messages[start:start + 30], expected)
            self.assertEqual(len(messages), 60)
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
            code, restarted = request("POST", origin)
            self.assertEqual((code, restarted["messages"]), (202, []))
            self.assertIsInstance(restarted["run_id"], str)
            self.assertTrue(restarted["run_id"])
            self.assertNotEqual(restarted["run_id"], run_id)
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

        relaunched = subprocess.Popen(launch_command, cwd=ROOT)
        try:
            wait_for_demo(relaunched)
            code, started_again = request("POST", origin)
            self.assertEqual(code, 202)
            self.assertIsInstance(started_again["run_id"], str)
            self.assertTrue(started_again["run_id"])
            self.assertNotIn(started_again["run_id"], (run_id, restarted["run_id"]))
        finally:
            if relaunched.poll() is None:
                relaunched.terminate()
            relaunched.wait(timeout=10)
        self.assertEqual(relaunched.returncode, 0)

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
