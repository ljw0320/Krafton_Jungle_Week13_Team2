#!/usr/bin/env python3
"""proxy_test.sh가 부르는 소켓 시나리오. 사용: python3 proxy_cases.py batch <서버 pid> [순서 ...] | resume | idle

batch : 한 배치에 A의 RST와 B의 이벤트가 같이 온다(서버 SIGSTOP -> 이벤트 -> SIGCONT). 규칙 /dead/=127.0.0.1:8009 필요,
        8009는 이 스크립트가 연다. 순서 글자: A=A RST, B=B 응답, F=B FIN, X=B RST, N=새 연결. 출력 "<순서> alive"
resume: 8081 서버(빈 fd 3개)에서 B만 닫혀도 멈춘 accept가 풀린다(proxy_close의 재개). 출력 "resume ok"
idle  : 8081 서버(빈 fd 3개)에서 유휴 연결 하나를 닫으면 멈춘 accept가 풀린다(conn_close의 재개). 출력 "idle ok"
"""
import os, signal, socket, struct, sys, time


def rst(s):
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()


def batch(pid, orders):
    L = socket.socket()
    L.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    L.bind(("127.0.0.1", 8009)); L.listen(16)
    R = b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"

    def stopped():
        for _ in range(100):
            if open("/proc/%d/stat" % pid).read().rsplit(")", 1)[1].split()[0] == "T": return
            time.sleep(0.01)

    def pair():
        a = socket.create_connection(("127.0.0.1", 8080)); a.settimeout(5)
        a.sendall(b"GET /dead/x HTTP/1.1\r\nHost: x\r\n\r\n")
        L.settimeout(5); b = L.accept()[0]; b.settimeout(5); buf = b""
        while b"\r\n\r\n" not in buf: buf += b.recv(4096)
        return a, b

    for o in orders:
        ps = [pair() for _ in range(5)]
        time.sleep(0.2)   # 서버가 epoll_wait에 들어가게
        os.kill(pid, signal.SIGSTOP)
        try:
            stopped()
            for a, b in ps:
                for ch in o:
                    if ch == "A": rst(a)
                    elif ch == "B": b.sendall(R)
                    elif ch == "F": b.close()
                    elif ch == "X": rst(b)
                    elif ch == "N": socket.create_connection(("127.0.0.1", 8080)).close()
        finally:
            os.kill(pid, signal.SIGCONT)   # 실패해도 서버를 멈춘 채 두지 않는다
        time.sleep(0.3)
        for a, b in ps: a.close(); b.close()
        try:
            s = socket.create_connection(("127.0.0.1", 8080)); s.settimeout(5)
            s.sendall(b"GET /index.html HTTP/1.0\r\nHost: x\r\n\r\n")
            ok = s.recv(12) == b"HTTP/1.1 200"
        except OSError:
            ok = False
        print(o, "alive" if ok else "DEAD")
        if not ok: break


def con():
    s = socket.create_connection(("127.0.0.1", 8081)); s.settimeout(8); return s


def first(s):
    t = time.time()
    try: r = s.recv(12).decode()
    except OSError as e: r = repr(e)
    return r, time.time() - t


def resume():
    a1 = con(); a1.sendall(b"GET /api/over/split HTTP/1.1\r\nHost: x\r\n\r\n")   # 0.3초 뒤 B가 끝나고 A1은 유지
    time.sleep(0.1)
    a2 = con(); time.sleep(0.05)   # 남은 fd를 채운다
    a3 = con(); a3.sendall(b"GET /api/../x HTTP/1.1\r\nHost: x\r\n\r\n")   # 대기열. 400이라 fd가 더 필요 없다
    r, t = first(a3)
    print("resume ok" if r == "HTTP/1.1 400" and t < 4 else "resume BAD %s %.1fs" % (r, t))


def idle():
    a = [con() for _ in range(3)]; time.sleep(0.2)
    b = con(); b.sendall(b"GET /api/../x HTTP/1.1\r\nHost: x\r\n\r\n"); time.sleep(0.2)
    a[0].close()
    r, t = first(b)
    print("idle ok" if r == "HTTP/1.1 400" and t < 4 else "idle BAD %s %.1fs" % (r, t))


if __name__ == "__main__":
    m = sys.argv[1]
    if m == "batch": batch(int(sys.argv[2]), sys.argv[3:] or ["AB", "BA"])
    elif m == "resume": resume()
    elif m == "idle": idle()
