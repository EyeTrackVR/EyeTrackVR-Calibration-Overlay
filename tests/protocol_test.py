#!/usr/bin/env python3
"""End-to-end UDP protocol test for EyeTrackVR-Overlay (simulator backend).

Drives the real executable the way the EyeTrackVR app does (see
EyeTrackApp/data_collection.py and osc_calibrate_filter.py) and checks the
signal sequence, acks and process lifecycle. No VR runtime needed.

    python3 tests/protocol_test.py <path/to/EyeTrackVR-Overlay[.exe]>
"""

import os
import random
import signal
import socket
import struct
import subprocess
import sys
import time

EXE = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else None
SIG_PORT = 22112 + random.randint(0, 400) * 2
CMD_PORT = SIG_PORT + 1
FAILS = []


def check(cond, msg):
    if not cond:
        FAILS.append(msg)
        print("  FAIL:", msg, flush=True)
    return cond


def launch(*args, **kw):
    cmd = [EXE, *args, "--backend=sim", f"--signal-port={SIG_PORT}", f"--cmd-port={CMD_PORT}"]
    return subprocess.Popen(cmd, cwd=os.path.dirname(EXE), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **kw)


class Link:
    def __init__(self):
        self.rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.rx.bind(("127.0.0.1", SIG_PORT))
        self.tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def close(self):
        self.rx.close()
        self.tx.close()

    def send(self, code, payload=b""):
        self.tx.sendto(struct.pack(">i", code) + payload, ("127.0.0.1", CMD_PORT))

    def send_xy(self, code, x, y):
        self.tx.sendto(struct.pack(">iff", code, x, y), ("127.0.0.1", CMD_PORT))

    def wait(self, want, timeout):
        """Wait for signal `want`; return (ok, signals seen before it)."""
        seen = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.rx.settimeout(max(0.01, deadline - time.time()))
            try:
                data, _ = self.rx.recvfrom(64)
            except TimeoutError:
                break
            if len(data) < 4:
                continue
            v = struct.unpack(">i", data[:4])[0]
            if v == want:
                return True, seen
            seen.append(v)
        return False, seen

    def drain(self):
        self.rx.setblocking(False)
        try:
            while True:
                self.rx.recvfrom(64)
        except (BlockingIOError, OSError):
            pass
        self.rx.setblocking(True)


def exited(proc, timeout):
    try:
        return proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        return None


def kill(proc):
    if proc.poll() is None:
        proc.kill()
        proc.wait()


def test_interactive():
    print("interactive session", flush=True)
    link = Link()
    proc = launch("interactive")
    try:
        ok, _ = link.wait(255, 15)
        if not check(ok, "no ready (255) signal"):
            return
        link.send(50, "Close your eyes\nlook around — résumé".encode())
        time.sleep(0.3)
        link.send(98)
        link.drain()
        link.send(100)
        ok, seen = link.wait(10, 20)
        check(ok and seen == list(range(9)), f"pass sweep signals {seen}")
        for _ in range(4):
            x, y = random.uniform(-0.6, 0.6), random.uniform(-0.6, 0.6)
            t0 = time.time()
            link.send_xy(111, x, y)
            ok, seen = link.wait(20, 8)
            dt = time.time() - t0
            check(ok and not seen, f"jitter capture 20 (seen {seen})")
            check(0.5 < dt < 1.5, f"jitter point took {dt:.2f}s (expected ~0.65)")
        link.send(119)
        ok, _ = link.wait(19, 5)
        check(ok, "end of pass 19")
        link.send(50, b"Blink repeatedly\nlook at the dot")
        for bx, by in [(-0.45, 0.35), (0.45, 0.35), (0.45, -0.35), (-0.45, -0.35)]:
            link.send_xy(113, bx, by)
            ok, _ = link.wait(21, 2)
            check(ok, "pin ack 21")
            time.sleep(0.4)
        link.send(114)
        link.send(98)
        for code in (130, 131, 132):
            link.send(code)
            ok, _ = link.wait(22, 2.5)
            check(ok, f"backdrop ack 22 for {code}")
        link.send(120)
        time.sleep(0.5)
        link.send(121)
        ok, _ = link.wait(19, 5)
        check(ok, "headset-shift end 19")
        # Garbage must not upset it.
        link.tx.sendto(b"\x00", ("127.0.0.1", CMD_PORT))
        link.tx.sendto(struct.pack(">i", 777), ("127.0.0.1", CMD_PORT))
        link.tx.sendto(struct.pack(">iff", 111, float("nan"), 0.0), ("127.0.0.1", CMD_PORT))
        link.tx.sendto(os.urandom(1500), ("127.0.0.1", CMD_PORT))
        time.sleep(0.3)
        check(proc.poll() is None, "overlay died on garbage input")
        link.send(200)
        code = exited(proc, 5)
        check(code == 0, f"exit after 200: {code}")
    finally:
        kill(proc)
        link.close()


def test_quit_mid_pass():
    print("quit mid-pass", flush=True)
    link = Link()
    proc = launch("interactive")
    try:
        ok, _ = link.wait(255, 15)
        if not check(ok, "no ready (255)"):
            return
        link.send(101)
        ok, _ = link.wait(2, 10)
        check(ok, "squint pass started")
        link.send(200)
        check(exited(proc, 5) == 0, "did not exit cleanly on 200 during a pass")
    finally:
        kill(proc)
        link.close()


def test_takeover():
    print("stale instance takeover", flush=True)
    link = Link()
    a = launch("interactive", "--no-parent-watch")
    b = None
    try:
        ok, _ = link.wait(255, 15)
        if not check(ok, "first instance not ready"):
            return
        b = launch("interactive")
        ok, _ = link.wait(255, 15)
        check(ok, "second instance never became ready")
        check(exited(a, 5) is not None, "stale instance did not quit")
        link.send(200)
        check(exited(b, 5) == 0, "second instance did not exit")
    finally:
        kill(a)
        if b:
            kill(b)
        link.close()


def test_finetune():
    print("finetune", flush=True)
    link = Link()
    proc = launch("finetune", "--sim-fast")
    try:
        ok, seen = link.wait(100, 60)
        check(ok and seen == list(range(11)), f"finetune signals {seen}")
        check(exited(proc, 10) == 0, "finetune did not exit 0")
    finally:
        kill(proc)
        link.close()


def port_free(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def test_parent_watch():
    print("parent watchdog", flush=True)
    link = Link()
    try:
        # An intermediate process launches the overlay and then exits: the
        # orphaned interactive overlay must notice and close itself.
        code = (
            "import subprocess,sys,time\n"
            f"subprocess.Popen({[EXE, 'interactive', '--backend=sim', f'--signal-port={SIG_PORT}', f'--cmd-port={CMD_PORT}']!r},"
            " stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)\n"
            "time.sleep(2.5)\n"
        )
        launcher = subprocess.Popen([sys.executable, "-c", code])
        ok, _ = link.wait(255, 15)
        check(ok, "orphan-to-be never became ready")
        launcher.wait()
        deadline = time.time() + 6
        while time.time() < deadline and not port_free(CMD_PORT):
            time.sleep(0.2)
        check(port_free(CMD_PORT), "overlay kept running after its parent exited")
    finally:
        link.close()


def test_sigterm():
    if os.name == "nt":
        return
    print("SIGTERM", flush=True)
    link = Link()
    proc = launch("interactive")
    try:
        ok, _ = link.wait(255, 15)
        if not check(ok, "not ready"):
            return
        proc.send_signal(signal.SIGTERM)
        check(exited(proc, 4) == 0, "SIGTERM did not shut down cleanly")
    finally:
        kill(proc)
        link.close()


def main():
    if not EXE or not os.path.isfile(EXE):
        print("usage: protocol_test.py <overlay executable>")
        return 2
    for t in (test_interactive, test_quit_mid_pass, test_takeover, test_finetune, test_parent_watch, test_sigterm):
        t()
    log = os.path.join(os.path.dirname(EXE), "eyetrackvr_overlay.log")
    if os.path.exists(log):
        with open(log, encoding="utf-8", errors="replace") as f:
            errors = [l.strip() for l in f if "[ERROR]" in l]
        check(not errors, f"errors in last log: {errors}")
    print(f"\n{len(FAILS)} failure(s)")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
