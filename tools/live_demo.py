"""Loopback browser entry point and fixed live-room comparison."""
import argparse
import base64
import binascii
from contextlib import contextmanager, ExitStack
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
from select import select
import signal
import socket
import subprocess
import sys
import threading
import time
import traceback

from live_demo_bots import compare


ROOT = Path(__file__).resolve().parents[1]
LOOPBACK = "127.0.0.1"
API = "/api/live-test"
ASSETS = {"/live.html": "text/html", "/live.js": "text/javascript",
          "/style.css": "text/css", "/live.css": "text/css"}


def valid_key(keys):
    if len(keys) != 1:
        return False
    try:
        return len(base64.b64decode(keys[0], validate=True)) == 16
    except (binascii.Error, ValueError):
        return False


def listening(process, port):
    # Verify socket ownership without connecting or advancing round robin.
    inodes = {row.split()[9] for row in Path("/proc/net/tcp").read_text().splitlines()[1:]
              if row.split()[1] == f"0100007F:{port:04X}" and row.split()[3] == "0A"}
    for fd in Path(f"/proc/{process.pid}/fd").iterdir():
        try:
            target = str(fd.readlink())
        except FileNotFoundError:
            continue
        if target.startswith("socket:[") and target[8:-1] in inodes:
            return True
    return False


def launch(stack, program, stop, *args):
    with socket.socket() as spare:
        spare.bind((LOOPBACK, 0))
        port = spare.getsockname()[1]
    process = subprocess.Popen([str(ROOT / program), str(port), *args], cwd=ROOT)

    def cleanup():
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
        process.wait()

    stack.callback(cleanup)
    deadline = time.monotonic() + 5
    while not stop.is_set() and time.monotonic() < deadline:
        code = process.poll()
        if code is not None:
            raise RuntimeError(f"{program} exited during startup ({code})")
        if listening(process, port):
            return port, process
        stop.wait(.02)
    raise RuntimeError(f"{program} did not start")


@contextmanager
def room(backends, stop):
    with ExitStack() as stack:
        hub, hub_process = launch(stack, "livehub", stop)
        processes = {"livehub": hub_process}
        routes = []
        for label in "AB"[:backends]:
            port, process = launch(stack, "chatd", stop, "--live", str(hub), label)
            processes[f"chatd {label}"] = process
            routes.append(f"/ws/={LOOPBACK}:{port}")
        port, processes["server"] = launch(stack, "server", stop, "www", "--loopback", *routes)
        yield port, processes


class Trial:
    def __init__(self, stop):
        self.stop = stop
        self.lock = threading.Lock()
        self.worker = None
        self.state = {"status": "idle", "round": None, "error": None, "results": {}}

    def snapshot(self):
        with self.lock:
            return dict(self.state, results=dict(self.state["results"]))

    def start(self):
        with self.lock:
            if self.state["status"] == "running" or self.stop.is_set():
                return False
            self.state = {"status": "running", "round": 1, "error": None, "results": {}}
            self.worker = threading.Thread(target=self.run)
            self.worker.start()
            return True

    def run(self):
        try:
            for backends in (1, 2):
                with self.lock:
                    self.state["round"] = backends
                with room(backends, self.stop) as (port, processes):
                    result, error = compare((LOOPBACK, port), processes, backends, self.stop)
                with self.lock:
                    self.state["results"][str(backends)] = result
                if error:
                    raise RuntimeError(error)
        except (OSError, RuntimeError, ValueError) as error:
            with self.lock:
                self.state.update(status="failed", error=str(error))
        except Exception:
            traceback.print_exc()
            with self.lock:
                self.state.update(status="failed", error="시험 실행 중 오류가 발생했습니다.")
        else:
            with self.lock:
                self.state.update(status="completed", round=None)


class Handler(BaseHTTPRequestHandler):
    def setup(self):
        self.request.settimeout(5)
        super().setup()

    def log_message(self, *_):
        pass

    def respond(self, code, body, media="application/json"):
        if media == "application/json":
            body = json.dumps(body, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", media + "; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.end_headers()
        self.wfile.write(body)

    def host(self):
        hosts = self.headers.get_all("Host", [])
        port = self.server.server_port
        if len(hosts) == 1 and hosts[0] in (f"{LOOPBACK}:{port}", f"localhost:{port}"):
            return hosts[0]
        return None

    def same_origin(self):
        host = self.host()
        return host and self.headers.get_all("Origin") == [f"http://{host}"]

    def readable_status(self):
        return (self.headers.get_all("Sec-Fetch-Site") in (None, ["same-origin"], ["none"])
                and (self.headers.get_all("Origin") is None or self.same_origin()))

    def no_body(self):
        return (self.headers.get_all("Content-Length") in (None, ["0"])
                and not self.headers.get_all("Transfer-Encoding"))

    def do_POST(self):
        if self.path != API:
            self.respond(404, {"error": "경로를 찾을 수 없습니다."})
        elif not self.same_origin():
            self.respond(403, {"error": "데모 페이지에서 요청해야 합니다."})
        elif not self.no_body():
            self.respond(400, {"error": "시험 요청에는 본문이 없어야 합니다."})
        else:
            code = 202 if self.server.trial.start() else 409
            self.respond(code, self.server.trial.snapshot())

    def do_GET(self):
        if not self.host():
            self.respond(403, {"error": "루프백 주소에서 열어야 합니다."})
        elif self.path == API:
            if self.readable_status():
                self.respond(200, self.server.trial.snapshot())
            else:
                self.respond(403, {"error": "데모 페이지에서 조회해야 합니다."})
        elif self.path == "/ws/live":
            self.websocket()
        elif self.path == "/":
            self.send_response(302)
            self.send_header("Location", "/live.html")
            self.end_headers()
        elif self.path in ASSETS:
            try:
                body = (ROOT / "www" / self.path[1:]).read_bytes()
            except OSError:
                self.respond(404, {"error": "파일을 찾을 수 없습니다."})
            else:
                self.respond(200, body, ASSETS[self.path])
        else:
            self.respond(404, {"error": "경로를 찾을 수 없습니다."})

    def websocket(self):
        if not self.same_origin():
            self.respond(403, {"error": "데모 페이지에서 연결해야 합니다."})
            return
        key = self.headers.get_all("Sec-WebSocket-Key", [])
        connection = self.headers.get("Connection", "").lower().split(",")
        if (not self.no_body() or self.headers.get("Upgrade", "").lower() != "websocket"
                or "upgrade" not in [part.strip() for part in connection]
                or self.headers.get_all("Sec-WebSocket-Version") != ["13"]
                or not valid_key(key)):
            self.respond(400, {"error": "WebSocket 연결 정보를 확인할 수 없습니다."})
            return
        port = self.server.manual_port
        try:
            upstream = socket.create_connection((LOOPBACK, port), timeout=3)
        except OSError:
            self.respond(502, {"error": "공용 채팅 서버에 연결하지 못했습니다."})
            return
        request = (f"GET /ws/live HTTP/1.1\r\nHost: {LOOPBACK}:{port}\r\n"
                   "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                   f"Sec-WebSocket-Key: {key[0]}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        with upstream:
            try:
                upstream.sendall(request.encode("ascii"))
                peer = {self.connection: upstream, upstream: self.connection}
                while not self.server.trial.stop.is_set():
                    ready, _, _ = select(peer, [], [], 1)
                    for source in ready:
                        chunk = source.recv(65536)
                        if not chunk:
                            return
                        peer[source].sendall(chunk)
            except OSError:
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", type=int, nargs="?", default=8081)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be in 1..65535")
    stop = threading.Event()
    trial = Trial(stop)
    interrupted = False

    def interrupt(_signum, _frame):
        nonlocal interrupted
        interrupted = True

    signal.signal(signal.SIGINT, interrupt)
    signal.signal(signal.SIGTERM, interrupt)
    try:
        with ThreadingHTTPServer((LOOPBACK, args.port), Handler) as gateway:
            gateway.timeout = .25
            with room(1, stop) as (gateway.manual_port, _):
                gateway.trial = trial
                print(f"http://{LOOPBACK}:{args.port}/live.html", flush=True)
                try:
                    while not interrupted:
                        gateway.handle_request()
                finally:
                    stop.set()
                    with trial.lock:
                        worker = trial.worker
                    if worker:
                        worker.join()
    except (OSError, RuntimeError) as error:
        if interrupted:
            return 0
        print(f"live demo: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
