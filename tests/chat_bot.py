"""표준 라이브러리로 구현한 WebSocket 클라이언트와 대화 상대."""
import argparse
import asyncio
import base64
from collections import deque
import hashlib
from http.client import HTTPConnection
import ipaddress
import json
import multiprocessing as mp
from multiprocessing.connection import wait
import os
from pathlib import Path
import random
import resource
import socket
import struct
import sys
import time
import traceback


def frame(op, payload, fin=True):
    mask = os.urandom(4)
    length = len(payload)
    if length < 126:
        size = bytes([128 + length])
    elif length <= 65535:
        size = b"\xfe" + struct.pack("!H", length)
    else:
        size = b"\xff" + struct.pack("!Q", length)
    key = (mask * ((length + 3) // 4))[:length]
    masked = (int.from_bytes(payload, "little") ^ int.from_bytes(key, "little")).to_bytes(length, "little")
    return bytes([(128 if fin else 0) | op]) + size + mask + masked


def handshake(addr, path):
    key = base64.b64encode(os.urandom(16))
    request = (f"GET {path} HTTP/1.1\r\nHost: {addr[0]}:{addr[1]}\r\n".encode()
               + b"Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Key: "
               + key + b"\r\nSec-WebSocket-Version: 13\r\n\r\n")
    return request, base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())


def check_upgrade(head, accept):
    headers = {}
    for line in head.splitlines()[1:]:
        if line:
            name, value = line.split(b":", 1)
            headers[name.lower()] = value.strip()
    if head.split()[:2] != [b"HTTP/1.1", b"101"] or headers.get(b"sec-websocket-accept") != accept:
        raise ValueError(f"upgrade failed: {head!r}")


class Client:
    def __init__(self, addr, path, rcvbuf=0):
        if isinstance(addr, str):
            host, port = addr.rsplit(":", 1)
            addr = host, int(port)
        self.sock = socket.socket()
        self.buf = bytearray()
        try:
            self.sock.settimeout(5)
            if rcvbuf:
                self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
            self.sock.connect(addr)
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            request, accept = handshake(addr, path)
            self.send_raw(request)
            while b"\r\n\r\n" not in self.buf:
                if len(self.buf) >= 8192:
                    raise ValueError("upgrade response header too large")
                self._read()
            head, _, self.buf = self.buf.partition(b"\r\n\r\n")
            check_upgrade(bytes(head), accept)
        except BaseException:
            self.close()
            raise

    def _read(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise EOFError("WebSocket closed")
        self.buf.extend(chunk)

    def send_raw(self, payload):
        self.sock.sendall(payload)

    def send_text(self, text):
        self.send_raw(frame(1, text.encode()))

    def send_bin(self, payload):
        self.send_raw(frame(2, payload))

    def recv(self, timeout):
        deadline = None if timeout is None else time.monotonic() + timeout

        def need(n):
            while len(self.buf) < n:
                remaining = None if deadline is None else deadline - time.monotonic()
                if remaining is not None and remaining <= 0:
                    raise TimeoutError()
                self.sock.settimeout(remaining)
                self._read()

        previous = self.sock.gettimeout()
        try:
            while True:
                need(2)
                op, size = self.buf[0] & 15, self.buf[1]
                if size & 128:
                    raise ValueError("masked server frame")
                start = 2
                if size >= 126:
                    start += 2 if size == 126 else 8
                    need(start)
                    size = int.from_bytes(self.buf[2:start], "big")
                need(start + size)
                payload = bytes(self.buf[start:start + size])
                del self.buf[:start + size]
                if op != 9:
                    return op, payload
                self.send_raw(frame(10, payload))
        finally:
            self.sock.settimeout(previous)

    def close(self):
        self.sock.close()


def echo(addr, path):
    c = Client(addr, path)
    try:
        while True:
            op, data = c.recv(None)
            if op == 8:
                return
            if op != 1:
                continue
            if data == b"l":
                c.send_text("n")
            elif data.startswith(b"m"):
                time.sleep(.3)
                c.send_text("m" + random.choice(("반가워!", "그렇구나.", "더 들려줘.")))
            elif data.startswith(b"i"):
                c.send_text("m사진 잘 봤다")
    finally:
        c.close()


IMAGE_BYTES = 2 * 1024 * 1024
DELIVER_GRACE = 5
ACK_LIMIT = 2
IMAGE_LIMIT = 5


class Bot:
    """한 연결의 발신 순번과 수신자가 독립적으로 확인한 순번."""

    def __init__(self, ident, args):
        self.ident = ident
        self.args = args
        self.stalled = args.mode == "slow" and ident >= args.users // 2
        self.drop = args.self_test_drop and ident == 0
        self.connected = asyncio.Event()
        self.changed = asyncio.Event()
        self.peer = None
        self.sent = self.acks = self.received = 0
        self.last_number = 0
        self.sequence_error = None
        self.unacked = deque()
        self.max_ack = 0.0
        self.pause_at = float("inf")
        self.ended = self.closing = False
        self.image_in = self.image_out = None
        self.image_bytes = 0
        self.writer = None

    async def open(self):
        sock = socket.socket()
        try:
            sock.setblocking(False)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            limit = 65536
            if self.stalled:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            if self.args.mode == "flood":
                # 쉬는 동안 커널과 스트림이 대신 받아 주는 양을 줄인다.
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32768)
                limit = 8192
            await asyncio.get_running_loop().sock_connect(sock, self.args.addr)
            self.port = sock.getsockname()[1]
            self.reader, self.writer = await asyncio.open_connection(sock=sock, limit=limit)
        except BaseException:
            sock.close()
            raise
        request, accept = handshake(self.args.addr, self.args.path)
        self.writer.write(request)
        check_upgrade(await self.reader.readuntil(b"\r\n\r\n"), accept)

    async def until(self, predicate):
        while not predicate():
            self.changed.clear()
            await self.changed.wait()

    async def read(self):
        try:
            await self.read_frames()
        except (asyncio.IncompleteReadError, OSError):
            # churn은 짝이 된 뒤 프레임 도중에도 끊는다.
            if self.args.mode != "churn" or not self.connected.is_set():
                raise

    async def read_frames(self):
        while True:
            if time.monotonic() >= self.pause_at:
                self.pause_at += 5
                await asyncio.sleep(1)
            try:
                header, size = await self.reader.readexactly(2)
            except (asyncio.IncompleteReadError, OSError):
                if self.closing:
                    return
                raise
            if header & 0xf0 != 0x80 or size & 0x80:
                raise ValueError("invalid server frame")
            if size >= 126:
                size = int.from_bytes(await self.reader.readexactly(2 if size == 126 else 8), "big")
            if size > 16384:
                raise ValueError(f"oversize server frame: {size}")
            data = await self.reader.readexactly(size)
            op = header & 15
            if op == 9:
                self.writer.write(frame(10, data))
            elif op == 2 and self.image_in is not None:
                self.image_in.update(data)
                self.image_bytes += size
                if self.image_bytes > IMAGE_BYTES:
                    raise ValueError("excess image data")
            elif op != 1:
                raise ValueError(f"unexpected opcode {op}, client {self.ident}")
            elif data == b"c":
                self.connected.set()
                if self.stalled:
                    self.writer.transport.pause_reading()
                    return
            elif data == b"w" or data.startswith(b"s"):
                continue
            elif data == b"a":
                self.acks += 1
                if self.acks > self.sent:
                    raise ValueError("unexpected acknowledgement")
                if self.unacked:
                    self.max_ack = max(self.max_ack, time.monotonic() - self.unacked.popleft())
            elif data.startswith(b"m"):
                self.message(data)
            elif data == b"ipng 2097152" and self.args.mode == "image" and self.image_in is None:
                self.image_in = hashlib.sha256()
            elif data == b"l" and (self.closing or self.args.mode == "churn"):
                return
            else:
                raise ValueError(f"unexpected frame {data[:64]!r}, client {self.ident}")
            self.changed.set()

    def message(self, data):
        sender, serial = data[1:].split(b":")
        sender = int(sender)
        if sender == self.ident or not 0 <= sender < self.args.users or self.peer not in (None, sender):
            raise ValueError(f"unexpected sender {sender}, client {self.ident}")
        self.peer = sender
        if self.ended:
            raise ValueError("message after end marker")
        if serial.startswith(b"end="):
            if int(serial[4:]) != self.received:
                self.sequence_error = f"sequence incomplete from {sender}: received {self.received}, marker {serial!r}"
            self.ended = True
        elif self.drop:
            self.drop = False
        else:
            number = int(serial)
            if number != self.last_number + 1 and self.sequence_error is None:
                self.sequence_error = f"sequence from {sender}: expected {self.last_number + 1}, got {number}"
            self.last_number = number
            self.received += 1

    def messages(self, count, width=0):
        block = bytearray()
        for serial in range(self.sent + 1, self.sent + count + 1):
            block.extend(frame(1, f"m{self.ident}:{serial:{width}}".encode()))
        self.sent_now(count)
        self.writer.write(block)

    def sent_now(self, count):
        self.sent += count
        if self.args.mode == "load":
            self.unacked.extend([time.monotonic()] * count)

    async def finish(self):
        # 마커가 TCP 스트림의 마지막 글을 지나야 수신자가 합계를 확정한다.
        marker = f"m{self.ident}:end={self.sent}"
        self.sent_now(1)
        self.writer.write(frame(1, marker.encode()))
        await self.writer.drain()
        await self.until(lambda: self.ended and self.acks == self.sent)

    async def traffic(self, start, duration):
        args = self.args
        deadline = start + duration
        if args.mode == "load":
            for step in range(args.rate * args.sec):
                due = start + (step + 0.9 * self.ident / args.users) / args.rate
                await asyncio.sleep(max(0, due - time.monotonic()))
                if time.monotonic() >= deadline:
                    break
                self.messages(1)
                await self.writer.drain()
        else:
            self.pause_at = start + 5 * self.ident / args.users
            while time.monotonic() < deadline:
                # 쌓이는 양은 전송 버퍼가 차면 멈추는 drain이 제한한다.
                self.messages(4, 4000)
                await self.writer.drain()
                await asyncio.sleep(0)

    async def churn(self):
        for _ in range(random.randint(1, 8)):
            self.messages(4)
            await asyncio.sleep(random.uniform(0, .02))
        self.messages(4)
        self.close()

    async def image(self):
        self.messages(1)
        await self.until(lambda: self.peer is not None)
        if self.ident < self.peer:
            self.writer.write(frame(1, f"ipng {IMAGE_BYTES}".encode()))
            src = random.Random(self.ident)
            self.image_out = hashlib.sha256()
            for _ in range(IMAGE_BYTES // 16384):
                data = src.randbytes(16384)
                self.image_out.update(data)
                self.writer.write(frame(2, data))
                await self.writer.drain()
                await asyncio.sleep(0)
        else:
            await self.until(lambda: self.image_bytes == IMAGE_BYTES)

    async def slow_send(self):
        # 읽지 않는 양쪽 모두 전송해 커널 버퍼와 상대 큐를 채운다.
        payload = b"m" + b"x" * 4095
        try:
            while True:
                self.writer.write(frame(1, payload))
                await self.writer.drain()
                await asyncio.sleep(0)
        except OSError:
            return  # 끊긴 시각은 상대편 TCP 소켓의 상태로 확인한다.

    def close(self):
        if self.writer is None or self.writer.is_closing():
            return
        if self.args.mode == "churn":
            sock = self.writer.get_extra_info("socket")
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        self.writer.transport.abort()


async def slow_round(bots, gate, tasks, args, start):
    normal = [b for b in bots if not b.stalled]
    stalled = [b for b in bots if b.stalled]
    stop = asyncio.Event()

    # 서버는 time(), chatd는 단조 시계로 재므로 하한은 더 많이 흐른 쪽으로 본다.
    wall = time.time() - (time.monotonic() - start)

    async def witness(bot):
        while not stop.is_set():
            bot.messages(1)
            await bot.writer.drain()
            await asyncio.sleep(.05)

    witnesses = [tasks.create_task(witness(b)) for b in normal]
    senders = [tasks.create_task(b.slow_send()) for b in stalled]
    ports = {b.port for b in stalled}
    while ports:
        # 클라이언트가 recv하지 않아도 FIN/RST를 보낸 서버 쪽 상태는 보인다.
        established = set()
        for row in Path("/proc/net/tcp").read_text().splitlines()[1:]:
            local, remote, state = row.split()[1:4]
            if state == "01" and int(local.split(":")[1], 16) == args.addr[1]:
                established.add(int(remote.split(":")[1], 16))
        elapsed = time.monotonic() - start
        if ports - established and max(elapsed, time.time() - wall) < 10:
            raise ValueError(f"slow clients disconnected early at {elapsed:.3f}s")
        if elapsed > 30:
            raise TimeoutError(f"slow clients still open at 30s: {ports}")
        ports &= established
        if ports:
            await asyncio.sleep(.1)
    for task in senders:
        task.cancel()
    await gate()
    stop.set()
    await asyncio.gather(*witnesses)


async def exercise(rank, args, barrier, again):
    async def gate():
        await asyncio.to_thread(barrier.wait, 60)

    totals = dict(sent=0, ack=0, received=0, rounds=0, seconds=0.0, max_ack=0.0, hashes={}, sequence_error=None)
    churn_end = None
    while True:
        bots = [Bot(i, args) for i in range(rank, args.users, args.procs)]
        readers = []
        try:
            async with asyncio.TaskGroup() as tasks:
                slots = asyncio.Semaphore(16)
                ramp_start = time.monotonic()

                async def join(bot):
                    if args.mode == "load":
                        await asyncio.sleep(max(0, ramp_start + bot.ident / 1000 - time.monotonic()))
                    async with slots:
                        await bot.open()
                    readers.append(tasks.create_task(bot.read()))
                    await bot.connected.wait()

                if args.mode == "slow":
                    for lo, hi in ((0, args.witness), (args.witness, args.users // 2),
                                   (args.users // 2, args.users)):
                        await asyncio.gather(*(join(b) for b in bots if lo <= b.ident < hi))
                        await gate()
                else:
                    await asyncio.gather(*(join(b) for b in bots))
                await gate()
                start = time.monotonic()
                if args.mode == "burst":
                    for round_no in range(1, args.rounds + 1):
                        await gate()
                        for bot in bots:
                            bot.messages(args.n)
                        target = round_no * args.n
                        await asyncio.gather(*(b.until(lambda b=b: b.acks == b.received == target) for b in bots))
                elif args.mode == "image":
                    await asyncio.gather(*(b.image() for b in bots))
                elif args.mode == "slow":
                    await slow_round(bots, gate, tasks, args, start)
                elif args.mode == "churn":
                    if churn_end is None:
                        churn_end = start + args.sec
                    await asyncio.gather(*(b.churn() for b in bots))
                else:
                    await asyncio.gather(*(b.traffic(start, args.sec) for b in bots))
                send_seconds = time.monotonic() - start
                if args.mode == "load":
                    send_seconds = max(args.sec, send_seconds)
                totals["seconds"] += send_seconds
                totals["rounds"] += 1
                normal = [b for b in bots if not b.stalled]
                marker = args.mode != "churn"
                if marker:
                    await asyncio.gather(*(b.finish() for b in normal))
                totals["start"], totals["done"] = start, time.monotonic()
                for b in normal:
                    totals["sent"] += b.sent - marker
                    totals["ack"] += b.acks - marker
                    totals["received"] += b.received
                    totals["max_ack"] = max(totals["max_ack"], b.max_ack)
                    if b.sequence_error:
                        totals["sequence_error"] = b.sequence_error
                    if args.mode == "image":
                        totals["hashes"][b.ident] = (b.peer, b.image_out is not None,
                                                      (b.image_out or b.image_in).hexdigest())
                for b in bots:
                    b.closing = True
                await gate()
                for reader in readers:
                    reader.cancel()
        finally:
            for b in bots:
                b.close()
        if args.mode != "churn":
            break
        if rank == 0:
            again.value = time.monotonic() < churn_end
        await gate()
        if not again.value:
            break
    return totals


def worker(rank, args, barrier, again, pipe):
    async def run():
        async with asyncio.timeout(args.sec + args.users / 1000 + 100):
            return await exercise(rank, args, barrier, again)
    try:
        pipe.send(asyncio.run(run()))
    except Exception:
        pipe.send(traceback.format_exc())
    finally:
        pipe.close()


def sample(addr, path, limit, peaks):
    started = time.monotonic()
    conn = HTTPConnection(*addr, timeout=2)
    try:
        conn.request("GET", path)
        res = conn.getresponse()
        if res.status != 200:
            raise ValueError(f"{path}: HTTP {res.status}")
        state = json.loads(res.read())
        elapsed = time.monotonic() - started
        rss = state["rss_kb"]
        old_rss, old_latency = peaks.get(path, (0, 0))
        peaks[path] = max(old_rss, rss), max(old_latency, elapsed)
        if elapsed > 2 or not 0 < rss <= limit * 1024:
            raise ValueError(f"{path}: {elapsed:.3f}s, RSS {rss} KiB exceeds limits")
        return state
    finally:
        conn.close()


def check_results(args, results):
    sent, ack, received = (sum(r[key] for r in results) for key in ("sent", "ack", "received"))
    seconds = max(r["seconds"] for r in results)
    rate = sent / seconds if seconds else 0
    print(f"{args.mode}: sent={sent} ack={ack} received={received} send_rate={rate:.1f}/s", flush=True)
    for result in results:
        if result["sequence_error"]:
            raise ValueError(result["sequence_error"])
    if args.mode == "churn":
        rounds = min(r["rounds"] for r in results)
        print(f"churn: rounds={rounds}", flush=True)
        if not rounds or not received:
            raise ValueError("no churn rounds or no messages delivered")
        return
    if not sent or sent != ack or sent != received:
        raise ValueError("message totals differ or no messages delivered")
    if args.mode == "load" and (sent != args.users * args.rate * args.sec or rate < args.users * args.rate):
        raise ValueError(f"load target missed: {args.users * args.rate}/s")
    delivered = max(r["done"] for r in results) - min(r["start"] for r in results)
    if args.mode == "load":
        max_ack = max(r["max_ack"] for r in results)
        print(f"load: delivered={delivered:.3f}s max_ack={max_ack:.3f}s", flush=True)
        if delivered > args.sec + DELIVER_GRACE or max_ack > ACK_LIMIT:
            raise ValueError("delivery or acknowledgement too slow")
    if args.mode == "burst" and sent != args.users * args.n * args.rounds:
        raise ValueError("incomplete burst")
    if args.mode == "image":
        print(f"image: delivered={delivered:.3f}s", flush=True)
        if delivered > IMAGE_LIMIT:
            raise ValueError("images delivered too slowly")
        hashes = {i: item for result in results for i, item in result["hashes"].items()}
        for ident, (peer, sender, digest) in hashes.items():
            if hashes[peer] != (ident, not sender, digest):
                raise ValueError(f"image SHA-256 mismatch for {ident} and {peer}")


def cpu_seconds(pid):
    # /proc/PID/stat의 14, 15번째 필드는 사용자와 시스템 CPU tick이다.
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    except FileNotFoundError:
        return 0
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def run_load(args):
    peaks = {}
    paused = 0
    baseline = sample(args.addr, "/ws/stat", 600, peaks)["users"]
    barrier = mp.Barrier(args.procs)
    again = mp.Value("b", True)
    processes, pending, results = [], set(), []
    services = {name: int(os.environ[name + "_PID"]) for name in ("SERVER", "CHATD") if name + "_PID" in os.environ}
    initial_cpu = {name: cpu_seconds(pid) for name, pid in services.items()}
    started = time.monotonic()
    try:
        for rank in range(args.procs):
            reader, writer = mp.Pipe(duplex=False)
            process = mp.Process(target=worker, args=(rank, args, barrier, again, writer))
            process.start()
            processes.append(process)
            pending.add(reader)
            writer.close()
        poll_at = 0
        while pending:
            now = time.monotonic()
            if now >= poll_at:
                sample(args.addr, "/_stat", 300, peaks)
                paused = max(paused, sample(args.addr, "/ws/stat", 600, peaks)["paused"])
                poll_at = now + 1
            for pipe in wait(pending, max(0, poll_at - time.monotonic())):
                result = pipe.recv()
                pending.remove(pipe)
                pipe.close()
                if isinstance(result, str):
                    raise ValueError(result)
                results.append(result)
        check_results(args, results)
        if args.mode == "flood":
            print(f"flood: max paused={paused}", flush=True)
            if paused < 1:
                raise ValueError("backpressure never paused a sender")
        sample(args.addr, "/_stat", 300, peaks)
        deadline = time.monotonic() + 5
        while sample(args.addr, "/ws/stat", 600, peaks)["users"] != baseline:
            if time.monotonic() >= deadline:
                raise ValueError(f"users did not return to baseline {baseline}")
            time.sleep(.05)
    finally:
        elapsed = time.monotonic() - started
        cpu = {"bot": sum(cpu_seconds(p.pid) for p in processes)}
        for name, pid in services.items():
            cpu[name.lower()] = max(0, cpu_seconds(pid) - initial_cpu[name])
        print("CPU (one core=100%):", " ".join(f"{name}={sec / elapsed:.1%}" for name, sec in cpu.items()))
        for path, (rss, latency) in peaks.items():
            print(f"{path}: peak RSS={rss} KiB max latency={latency:.3f}s")
        for proc in processes:
            if pending and proc.is_alive():
                proc.terminate()
        for proc in processes:
            proc.join()
        for pipe in pending:
            pipe.close()


def raise_fd_limit():
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    target = 65536 if hard == resource.RLIM_INFINITY else min(65536, hard)
    if soft != resource.RLIM_INFINITY and soft < target:
        resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["echo", "load", "burst", "flood", "slow", "image", "churn"])
    parser.add_argument("--addr", default="127.0.0.1:8080")
    parser.add_argument("--path", default="/ws/chat")
    parser.add_argument("--users", type=int)
    parser.add_argument("--rate", type=int, default=1)
    parser.add_argument("--sec", type=int)
    parser.add_argument("--procs", type=int, default=4)
    parser.add_argument("--n", type=int, default=20)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--witness", type=int)
    parser.add_argument("--self-test-drop", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    raise_fd_limit()
    if args.mode == "echo":
        echo(args.addr, args.path)
        return
    host, port = args.addr.rsplit(":", 1)
    host = socket.gethostbyname(host)
    if not ipaddress.ip_address(host).is_loopback:
        parser.error("부하 시험은 로컬 서버 주소를 사용합니다")
    args.addr = host, int(port)
    if args.users is None:
        args.users = {"load": 5000, "burst": 5000, "churn": 1000}.get(args.mode, 200)
    if args.sec is None:
        args.sec = 60 if args.mode == "load" else 30
    if not 2 <= args.users <= 6000 or args.users % 2:
        parser.error("--users must be an even number between 2 and 6000")
    if min(args.sec, args.rate, args.procs, args.rounds, args.n) < 1:
        parser.error("duration, rate, processes, rounds and burst size must be positive")
    args.procs = min(args.users, args.procs)
    if args.witness is None:
        args.witness = args.users // 2
    if args.mode == "slow" and (args.users % 4 or args.witness % 2 or not 0 <= args.witness <= args.users // 2):
        parser.error("slow mode requires paired groups of readers and stalled clients")
    run_load(args)


if __name__ == "__main__":
    try:
        main()
    except (OSError, EOFError, ValueError) as exc:
        sys.exit(f"FAIL: {exc}")
