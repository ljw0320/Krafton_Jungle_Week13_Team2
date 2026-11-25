#!/usr/bin/python3
"""Standalone raw-TCP checks; no production server instrumentation or CGI endpoint."""

from contextlib import ExitStack
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def wire(path="/", body=None, version="1.1", headers=b""):
    verb = "GET" if body is None else "POST"
    head = f"{verb} {path} HTTP/{version}\r\nHost: localhost\r\n".encode()
    if body is not None:
        head += f"Content-Length: {len(body)}\r\n".encode()
    return head + headers + b"\r\n" + (body or b"")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


class Peer:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=7)
        self.reader = self.socket.makefile("rb")

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.reader.close()
        self.socket.close()

    def send(self, data):
        self.socket.sendall(data)

    def response(self, expected=b"diagnostic-home", status=200):
        line = self.reader.readline(1024)
        require(line.startswith(b"HTTP/1.1 "), "응답 전에 연결이 종료되었거나 상태 줄이 잘못되었습니다.")
        actual_status = int(line.split()[1])
        headers = {}
        for _ in range(32):
            line = self.reader.readline(2048)
            if line == b"\r\n":
                break
            require(b":" in line, "응답 헤더가 잘못되었습니다.")
            name, value = line.decode().split(":", 1)
            headers[name.lower()] = value.strip()
        else:
            raise AssertionError("응답 헤더가 너무 많습니다.")
        length = int(headers.get("content-length", "0"))
        require(0 <= length <= 65536, "진단 응답 길이가 허용 범위를 벗어났습니다.")
        body = self.reader.read(length)
        require(actual_status == status, f"예상 HTTP {status}, 실제 HTTP {actual_status}")
        require(body == expected, "응답 본문이 예상값과 다릅니다.")
        return headers

    def eof(self):
        require(self.reader.read(1) == b"", "종료되어야 할 연결에서 추가 데이터가 수신되었습니다.")


class Target:
    def __init__(self, directory):
        root = Path(directory)
        www, cgi = root / "www", root / "cgi"
        www.mkdir()
        cgi.mkdir()
        (www / "index.html").write_bytes(b"diagnostic-home")
        shutil.copy2(ROOT / "cgi-bin/adder", cgi / "adder")
        self.executable = str(ROOT / "webserver")
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            self.port = reservation.getsockname()[1]
        self.process = subprocess.Popen([self.executable, str(self.port), str(www), str(cgi)],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def start(self):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            require(self.process.poll() is None, "임시 테스트 서버가 시작되지 않았습니다.")
            try:
                with Peer(self.port) as peer:
                    peer.send(wire(headers=b"Connection: close\r\n"))
                    peer.response()
                    peer.socket.shutdown(socket.SHUT_WR)
                    peer.eof()
                    return
            except ConnectionRefusedError:
                time.sleep(0.01)
        raise TimeoutError("임시 테스트 서버 시작 시간이 초과되었습니다.")

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()

    def reuse(self):
        with Peer(self.port) as peer:
            responses = []
            for packet, expected in [(wire(), b"diagnostic-home"),
                                     (wire("/echo", b"hello"), b"hello"),
                                     (wire("/cgi-bin/adder?a=12&b=30"), b'{"a":12,"b":30,"sum":42}\n')]:
                peer.send(packet)
                responses.append(peer.response(expected))
            require(all(h["connection"] == "keep-alive" for h in responses), "연결 유지 응답이 아닙니다.")
            return "하나의 TCP 소켓에서 GET → POST → CGI 응답 3개 수신", {
                "responses": 3, "client_port": peer.socket.getsockname()[1],
                "server_port": peer.socket.getpeername()[1], "automatic_reconnect": False}

    def idle(self):
        with Peer(self.port) as peer:
            peer.send(wire())
            peer.response()
            started = time.monotonic()
            peer.eof()
            elapsed = time.monotonic() - started
            require(4.5 <= elapsed <= 7, f"유휴 종료 측정값 {elapsed:.3f}초가 허용 범위 밖입니다.")
            return f"유휴 {elapsed:.3f}초 후 EOF", {"idle_seconds": round(elapsed, 3)}

    def limit(self):
        with Peer(self.port) as peer:
            peer.send(b"".join(wire("/echo", str(i).encode()) for i in range(1, 102)))
            for number in range(1, 101):
                headers = peer.response(str(number).encode())
                expected = "close" if number == 100 else "keep-alive"
                require(headers["connection"] == expected, f"{number}번째 응답의 연결 정책이 잘못되었습니다.")
            peer.eof()
            return "동일 연결 1~99번째 유지, 100번째 close + EOF, 101번째 응답 없음", {"responses": 100, "last_response_index": 100, "extra_response": False}

    def http10(self):
        with Peer(self.port) as peer:
            peer.send(wire(version="1.0", headers=b"Connection: keep-alive\r\n") + wire())
            headers = peer.response()
            require(headers["connection"] == "close", "HTTP/1.0 응답이 연결을 종료하지 않습니다.")
            peer.eof()
            return "HTTP/1.0 + keep-alive 요청 → close + EOF, 후속 요청 미처리", {"connection": "close", "responses": 1}

    def explicit_close(self):
        with Peer(self.port) as peer:
            peer.send(wire())
            first = peer.response()
            peer.send(wire(headers=b"Connection: Keep-Alive, cLoSe\r\n") + wire())
            last = peer.response()
            require(last["connection"] == "close", "명시적 close가 적용되지 않았습니다.")
            require(first["connection"] == "keep-alive", "첫 응답이 연결을 유지하지 않습니다.")
            peer.eof()
            return "첫 응답 유지 → 같은 연결의 close 요청 → EOF, 후속 요청 미처리", {"responses": 2, "automatic_reconnect": False}

    def pipeline(self):
        payload = b"\x00\xff\r\nGET /fake HTTP/1.1\r\n\r\n"
        following = wire("/echo", b"split-body")
        with Peer(self.port) as peer:
            peer.send(wire() + wire("/echo", payload) + following[:8])
            headers = [peer.response(), peer.response(payload)]
            peer.send(following[8:-4])
            time.sleep(0.03)
            peer.send(following[-4:] + wire(headers=b"Connection: close\r\n"))
            headers += [peer.response(b"split-body"), peer.response()]
            peer.eof()
            require([h["connection"] for h in headers] == ["keep-alive"] * 3 + ["close"],
                    "합쳐진 요청 또는 분할 요청의 연결 정책이 잘못되었습니다.")
            return "합쳐 보낸 GET·바이너리 POST와 분할 헤더·본문을 순서대로 처리", {
                "responses": 4, "binary_body_matched": True, "automatic_reconnect": False}

    def slots(self):
        with ExitStack() as stack:
            peers, ports = [], set()
            for _ in range(16):
                peer = stack.enter_context(Peer(self.port))
                peer.send(wire())
                peer.response()
                ports.add(peer.socket.getsockname()[1])
                peers.append(peer)
            require(len(ports) == 16, "서로 다른 16개 연결을 확보하지 못했습니다.")
            with Peer(self.port) as extra:
                try:
                    extra.send(wire())
                    extra.eof()
                except (ConnectionResetError, BrokenPipeError):
                    pass
            peers[0].send(wire("/echo", b"still-working"))
            peers[0].response(b"still-working")
            return "16개 연결 응답 성공·유지, 17번째 EOF/리셋 거절, 기존 연결 추가 요청 성공", {"accepted_connections": 16, "rejected_connection": 17, "existing_connection_works": True}


CASES = {
    "reuse": ("GET·POST·CGI 연결 재사용", "재접속 없이 같은 TCP 소켓에서 GET·POST·CGI 응답 수신"),
    "idle": ("유휴 5초 종료", "응답 후 추가 요청 없이 약 5초 뒤 EOF (허용 4.5~7초)"),
    "limit": ("100번째 응답 후 종료", "100번째 close + EOF, 101번째 요청 미처리"),
    "http10": ("HTTP/1.0 종료 정책", "keep-alive를 요청해도 응답 후 close + EOF"),
    "explicit_close": ("Connection: close", "같은 연결의 명시적 종료 요청 후 EOF"),
    "pipeline": ("합쳐진 요청·분할 요청", "바이너리 본문을 보존하고 요청·응답을 순서대로 처리"),
    "slots": ("16개 슬롯·17번째 거절", "서로 다른 16개 연결 유지, 17번째 거절, 기존 연결 정상"),
}


def execute(target, key):
    label, expected = CASES[key]
    started = time.monotonic()
    try:
        actual, measurements = getattr(target, key)()
        passed = True
    except Exception as error:
        actual, measurements, passed = f"{type(error).__name__}: {error}", {}, False
    return {"key": key, "label": label, "expected": expected, "actual": actual,
            "passed": passed, "elapsed_ms": round((time.monotonic() - started) * 1000, 1),
            "measurements": measurements}


def main():
    parser = argparse.ArgumentParser(description="Check keep-alive on a temporary instance of ./webserver.")
    parser.add_argument("--case", choices=["all", *CASES], default="all")
    parser.add_argument("--output", type=Path, help="Write a JSON report for local import in the test page.")
    options = parser.parse_args()
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="jungle-keep-alive-check-") as directory:
        target = Target(directory)
        try:
            target.start()
            results = [execute(target, key) for key in (CASES if options.case == "all" else [options.case])]
        finally:
            target.close()
    report = {"format_version": 1, "scope": "isolated-executable",
              "generated_at": datetime.now(timezone.utc).isoformat(),
              "server_executable": target.executable, "target_port": target.port,
              "target_pid": target.process.pid, "target_stopped": target.process.poll() is not None,
              "results": results, "passed": all(item["passed"] for item in results),
              "duration_ms": round((time.monotonic() - started) * 1000, 1)}
    for item in results:
        print(f"{'PASS' if item['passed'] else 'FAIL'} {item['label']}: {item['actual']}")
    if options.output:
        options.output.parent.mkdir(parents=True, exist_ok=True)
        options.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"Report: {options.output.resolve()}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"ERROR: {type(error).__name__}: {error}", file=sys.stderr)
        sys.exit(2)
