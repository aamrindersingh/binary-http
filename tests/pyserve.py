#!/usr/bin/env python3
"""A BHTTP/1 server in Python, written from SPEC.md.

The slide this project answers says: "a client that only works against
your own server is an implementation, not a protocol." Two independent C
codecs prove the two sides were not copied from each other, and
rawframe.py proves a third client can talk to bserve. None of that
proves the harder direction, which is that bcurl works against a server
it has never met.

So this is that server. It shares no code with bserve, and it answers
differently on purpose, in the four ways bcurl is most likely to have
quietly assumed bserve's behaviour:

  1. the body arrives in TWO DATA frames, not one. bserve always sends
     one, so a client that treats the first DATA frame as the whole body
     passes every test against bserve and truncates here.
  2. no content-length header at all. SPEC 6 makes it advisory and
     END_MESSAGE authoritative, and this is what that promise is worth.
  3. one header name is sent as a length-prefixed literal rather than a
     static index, which exercises the other half of SPEC 4.
  4. the header block comes in a different order, with a name bserve
     never sends.

Usage: pyserve.py <port> [docroot]
"""
import os
import socket
import struct
import sys

HDR = 8

# SPEC 3
T_REQUEST, T_RESPONSE, T_DATA, T_GOAWAY = 1, 2, 3, 4
END_MESSAGE = 0x01

# SPEC 4, the same ten names in the same order
STATIC = [None, ":method", ":path", "host", "user-agent", "accept",
          "content-length", "content-type", "server", "date", "connection"]
INDEX = {n: i for i, n in enumerate(STATIC) if n}

# SPEC 7
SOFT_MAX = 16384

TYPES = {"html": "text/html", "txt": "text/plain"}


def u24(v):
    return bytes(((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF))


def rd24(b):
    return (b[0] << 16) | (b[1] << 8) | b[2]


def frame(ftype, flags, req_id, payload=b""):
    """SPEC 2: Length(24) Type(8) Flags(8) RequestID(24)."""
    return u24(len(payload)) + bytes([ftype, flags]) + u24(req_id) + payload


def entry(name, value, literal=False):
    """SPEC 4. Indexed when the name is in the table, unless forced."""
    v = value.encode()
    idx = INDEX.get(name)
    if idx and not literal:
        head = bytes([0x80 | idx])
    else:
        n = name.encode()
        assert 1 <= len(n) <= 127
        head = bytes([len(n)]) + n
    return head + struct.pack(">H", len(v)) + v


def hblock(entries):
    assert len(entries) <= 255
    return bytes([len(entries)]) + b"".join(entries)


def read_exactly(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def decode_hblock(buf):
    """Returns {name: value}, or None on any SPEC 4 violation."""
    if not buf:
        return None
    cur, end = 1, len(buf)
    out = {}
    for _ in range(buf[0]):
        if cur >= end:
            return None
        lead = buf[cur]
        cur += 1
        if lead & 0x80:
            idx = lead & 0x7F
            if idx == 0 or idx >= len(STATIC):
                return None
            name = STATIC[idx]
        else:
            nlen = lead & 0x7F
            if nlen == 0 or end - cur < nlen:
                return None
            name = buf[cur:cur + nlen].decode("latin-1").lower()
            cur += nlen
        if end - cur < 2:
            return None
        vlen = struct.unpack(">H", buf[cur:cur + 2])[0]
        cur += 2
        if end - cur < vlen:
            return None
        out[name] = buf[cur:cur + vlen].decode("latin-1")
        cur += vlen
    # SPEC 4: consumed exactly
    return out if cur == end else None


def respond(sock, req_id, status, body, ctype="text/plain"):
    """A RESPONSE, then the body split across two DATA frames.

    The split is the point. SPEC 6 says END_MESSAGE is what ends a
    message, so a correct client reassembles until it sees that flag.
    No content-length is sent at all.
    """
    headers = [
        entry("content-type", ctype),
        entry("server", "pyserve/1"),
        # Not in the static table, so it goes out as a literal (SPEC 4).
        entry("x-served-by", "python", literal=True),
    ]
    sock.sendall(frame(T_RESPONSE, 0, req_id,
                       struct.pack(">H", status) + hblock(headers)))

    if not body:
        sock.sendall(frame(T_DATA, END_MESSAGE, req_id, b""))
        return

    half = max(1, len(body) // 2)
    sock.sendall(frame(T_DATA, 0, req_id, body[:half]))
    sock.sendall(frame(T_DATA, END_MESSAGE, req_id, body[half:]))


def serve_one(sock, docroot):
    while True:
        head = read_exactly(sock, HDR)
        if head is None:
            return
        length, ftype, flags = rd24(head), head[3], head[4]
        req_id = rd24(head[5:8])

        # SPEC 7: check the declared length before allocating.
        if length > SOFT_MAX:
            sock.sendall(frame(T_GOAWAY, 0, 0, u24(req_id) + bytes([2])))
            return

        payload = read_exactly(sock, length) if length else b""
        if payload is None:
            return

        if ftype == T_GOAWAY:
            return

        if ftype != T_REQUEST:
            # SPEC 8: read and discard exactly Length bytes, carry on.
            # Already read above, so there is nothing left to do.
            continue

        if req_id == 0:
            respond(sock, 1, 400, b"400 request id 0\n")
            continue
        if not payload:
            respond(sock, req_id, 400, b"400 empty request\n")
            continue

        method, hdrs = payload[0], decode_hblock(payload[1:])
        if hdrs is None:
            respond(sock, req_id, 400, b"400 bad header block\n")
            continue
        if method != 1:
            respond(sock, req_id, 405, b"405 method not allowed\n")
            continue

        path = hdrs.get(":path")
        if not path or not path.startswith("/"):
            respond(sock, req_id, 400, b"400 missing path\n")
            continue

        full = os.path.realpath(os.path.join(docroot, path.lstrip("/")))
        root = os.path.realpath(docroot)
        if full != root and not full.startswith(root + os.sep):
            respond(sock, req_id, 403, b"403 forbidden\n")
            continue
        if os.path.isdir(full):
            full = os.path.join(full, "index.html")
        if not os.path.isfile(full):
            respond(sock, req_id, 404, b"404 not found\n")
            continue

        with open(full, "rb") as fh:
            body = fh.read()
        ext = full.rsplit(".", 1)[-1]
        respond(sock, req_id, 200, body,
                TYPES.get(ext, "application/octet-stream"))


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9415
    docroot = sys.argv[2] if len(sys.argv) > 2 else "./www"

    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", port))
    ls.listen(16)
    print("pyserve on 127.0.0.1:%d, root %s" % (port, docroot), flush=True)

    while True:
        conn, _ = ls.accept()
        try:
            serve_one(conn, docroot)
        except (OSError, struct.error):
            pass
        finally:
            conn.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
