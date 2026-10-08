#!/usr/bin/env python3
"""Throw malformed and random frames at the server and check it survives.

Not a real fuzzer, but it covers the shapes hand-written cases miss:
random payloads, lengths that disagree with what is actually sent, every
frame type including undefined ones, and truncated connections.

Starts and owns its own ./bserve, and reads that server's stderr at the
end, so "no sanitizer reports" is something this script checks rather
than something the author remembered to look at. An earlier version
expected a server to already be listening and printed
"stopped accepting after 0 cases" when there wasn't one, which reads
exactly like a crash. A test that cannot tell a missing server from a
dead one is not worth much.

Usage: fuzz.py [port] [cases]   (build with `make asan` first)
"""
import os, random, signal, socket, subprocess, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9413
N    = int(sys.argv[2]) if len(sys.argv) > 2 else 400
random.seed(20261008)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG  = os.path.join(ROOT, "tests", "fuzz-server.log")

if not os.access(os.path.join(ROOT, "bserve"), os.X_OK):
    sys.exit("build first: make asan")

log = open(LOG, "w")
srv = subprocess.Popen(["./bserve", "./www", str(PORT), "-v"],
                       cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)

for _ in range(50):                      # wait for the listener
    try:
        socket.create_connection(("127.0.0.1", PORT), timeout=0.5).close()
        break
    except OSError:
        if srv.poll() is not None:
            sys.exit("server exited before fuzzing started, see " + LOG)
        time.sleep(0.1)
else:
    srv.kill()
    sys.exit("server never started listening on port %d" % PORT)


def finish(code, msg):
    print(msg)
    if srv.poll() is None:
        srv.send_signal(signal.SIGTERM)
        try:
            srv.wait(timeout=3)
        except subprocess.TimeoutExpired:
            srv.kill()
    log.flush()
    log.close()
    sys.exit(code)


def u24(v): return bytes(((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF))

sent = refused = 0
for i in range(N):
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=2)
    except OSError:
        why = ("the server process exited (code %s)" % srv.returncode
               if srv.poll() is not None else "it stopped accepting")
        finish(1, "FAIL after %d cases: %s. See %s" % (i, why, LOG))

    mode = i % 5
    if mode == 0:                                   # pure noise
        blob = bytes(random.getrandbits(8) for _ in range(random.randint(1, 64)))
    elif mode == 1:                                 # length lies, too long
        body = bytes(random.getrandbits(8) for _ in range(8))
        blob = u24(5000) + bytes([1, 1]) + u24(1) + body
    elif mode == 2:                                 # random type, honest length
        body = bytes(random.getrandbits(8) for _ in range(random.randint(0, 40)))
        blob = u24(len(body)) + bytes([random.getrandbits(8), random.getrandbits(8)]) + u24(1) + body
    elif mode == 3:                                 # header cut in half
        blob = (u24(10) + bytes([1, 1]) + u24(1))[:random.randint(1, 7)]
    else:                                           # valid-looking REQUEST, junk block
        body = bytes([1]) + bytes([random.randint(1, 8)]) + bytes(
            random.getrandbits(8) for _ in range(random.randint(0, 24)))
        blob = u24(len(body)) + bytes([1, 1]) + u24(1) + body

    try:
        s.sendall(blob)
        s.settimeout(0.4)
        try: s.recv(4096)
        except socket.timeout: pass
        sent += 1
    except OSError:
        refused += 1
    finally:
        s.close()

time.sleep(0.3)
try:
    socket.create_connection(("127.0.0.1", PORT), timeout=2).close()
except OSError:
    finish(1, "FAIL: server is dead after %d cases. See %s" % (sent, LOG))

# A forked child can die to a sanitizer while the parent keeps accepting,
# so a live listener on its own proves nothing. Read the log.
log.flush()
with open(LOG, errors="replace") as fh:
    text = fh.read()

MARKERS = ("ERROR: AddressSanitizer", "ERROR: LeakSanitizer",
           "runtime error:", "SEGV", "AddressSanitizer: DEADLYSIGNAL")
hits = sorted({m for m in MARKERS if m in text})

if hits:
    finish(1, "FAIL: %d cases sent, but the server log reports %s. See %s"
              % (sent, ", ".join(hits), LOG))

finish(0, "OK: %d cases sent, %d refused mid-write, server still accepting, "
          "no sanitizer reports in %s" % (sent, refused, LOG))
