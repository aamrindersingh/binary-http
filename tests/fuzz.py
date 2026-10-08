#!/usr/bin/env python3
"""Throw malformed and random frames at the server and check it survives.

Not a real fuzzer, but it covers the shapes hand-written cases miss:
random payloads, lengths that disagree with what is actually sent, every
frame type including undefined ones, and truncated connections.
"""
import random, socket, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9413
N    = int(sys.argv[2]) if len(sys.argv) > 2 else 400
random.seed(20261008)

def u24(v): return bytes(((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF))

sent = refused = 0
for i in range(N):
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=2)
    except OSError:
        print("FAIL: server stopped accepting after %d cases" % i)
        sys.exit(1)

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
    s = socket.create_connection(("127.0.0.1", PORT), timeout=2); s.close()
    print("OK: %d cases sent, server still accepting connections" % sent)
except OSError:
    print("FAIL: server is dead after fuzzing")
    sys.exit(1)
