#!/usr/bin/env python3
"""Send hand-built frames to a BHTTP/1 server and report what comes back.

Written straight from SPEC.md with no reference to either implementation,
so it is a third opinion on the wire format rather than a mirror of one
of the two. Used by conformance.sh to exercise the cases a well-behaved
client cannot produce: truncated blocks, bad indices, oversized frames.
"""
import socket
import struct
import sys

HDR = 8

# SPEC 3
T_REQUEST, T_RESPONSE, T_DATA, T_GOAWAY = 1, 2, 3, 4
END_MESSAGE = 0x01

# SPEC 4
STATIC = [None, ":method", ":path", "host", "user-agent", "accept",
          "content-length", "content-type", "server", "date", "connection"]


def u24(v):
    return bytes(((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF))


def frame(ftype, flags, req_id, payload=b""):
    """SPEC 2: Length(24) Type(8) Flags(8) RequestID(24)."""
    return u24(len(payload)) + bytes([ftype, flags]) + u24(req_id) + payload


def entry_indexed(idx, value):
    v = value.encode()
    return bytes([0x80 | idx]) + struct.pack(">H", len(v)) + v


def entry_literal(name, value):
    n, v = name.encode(), value.encode()
    return bytes([len(n)]) + n + struct.pack(">H", len(v)) + v


def hblock(entries):
    return bytes([len(entries)]) + b"".join(entries)


def get_request(path, host="localhost:9000"):
    body = bytes([1]) + hblock([entry_indexed(2, path), entry_indexed(3, host)])
    return frame(T_REQUEST, END_MESSAGE, 1, body)


def read_frame(sock):
    hdr = b""
    while len(hdr) < HDR:
        chunk = sock.recv(HDR - len(hdr))
        if not chunk:
            return None
        hdr += chunk
    length = (hdr[0] << 16) | (hdr[1] << 8) | hdr[2]
    ftype, flags = hdr[3], hdr[4]
    rid = (hdr[5] << 16) | (hdr[6] << 8) | hdr[7]
    body = b""
    while len(body) < length:
        chunk = sock.recv(length - len(body))
        if not chunk:
            break
        body += chunk
    return {"type": ftype, "flags": flags, "id": rid, "payload": body}


def status_of(f):
    if f and f["type"] == T_RESPONSE and len(f["payload"]) >= 2:
        return struct.unpack(">H", f["payload"][:2])[0]
    return None


def send_and_read(port, blob, reads=1, timeout=3.0):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.sendall(blob)
    out = []
    try:
        for _ in range(reads):
            f = read_frame(s)
            if f is None:
                break
            out.append(f)
    except socket.timeout:
        pass
    s.close()
    return out


CASES = {}


def case(name):
    def deco(fn):
        CASES[name] = fn
        return fn
    return deco


@case("ok")
def c_ok(port):
    """A well-formed request gets 200."""
    fs = send_and_read(port, get_request("/index.html"), reads=2)
    return status_of(fs[0]) if fs else None


@case("truncated-block")
def c_trunc(port):
    """Count says 2, only one entry follows. SPEC 4 -> 400."""
    body = bytes([1]) + bytes([2]) + entry_indexed(2, "/index.html")
    fs = send_and_read(port, frame(T_REQUEST, END_MESSAGE, 1, body))
    return status_of(fs[0]) if fs else None


@case("trailing-bytes")
def c_trailing(port):
    """Count says 1 but extra bytes follow. SPEC 4 -> 400."""
    body = bytes([1]) + bytes([1]) + entry_indexed(2, "/index.html") + b"\xAA\xBB"
    fs = send_and_read(port, frame(T_REQUEST, END_MESSAGE, 1, body))
    return status_of(fs[0]) if fs else None


@case("unknown-index")
def c_badidx(port):
    """Static index 11 does not exist. SPEC 4 -> 400."""
    body = bytes([1]) + hblock([entry_indexed(11, "x"),
                                entry_indexed(2, "/index.html")])
    fs = send_and_read(port, frame(T_REQUEST, END_MESSAGE, 1, body))
    return status_of(fs[0]) if fs else None


@case("zero-literal-name")
def c_zeroname(port):
    """A literal name of length 0. SPEC 4 -> 400."""
    body = bytes([1]) + bytes([1]) + bytes([0x00]) + struct.pack(">H", 1) + b"x"
    fs = send_and_read(port, frame(T_REQUEST, END_MESSAGE, 1, body))
    return status_of(fs[0]) if fs else None


@case("no-path")
def c_nopath(port):
    """No :path header. SPEC 6 -> 400."""
    body = bytes([1]) + hblock([entry_indexed(3, "localhost")])
    fs = send_and_read(port, frame(T_REQUEST, END_MESSAGE, 1, body))
    return status_of(fs[0]) if fs else None


@case("request-id-zero")
def c_idzero(port):
    """SPEC 1: ID 0 is reserved for connection-level frames -> 400."""
    body = bytes([1]) + hblock([entry_indexed(2, "/index.html")])
    fs = send_and_read(port, frame(T_REQUEST, END_MESSAGE, 0, body))
    return status_of(fs[0]) if fs else None


@case("bad-method")
def c_badmethod(port):
    """Method 9 is not GET. SPEC 5 -> 405."""
    body = bytes([9]) + hblock([entry_indexed(2, "/index.html")])
    fs = send_and_read(port, frame(T_REQUEST, END_MESSAGE, 1, body))
    return status_of(fs[0]) if fs else None


@case("oversized")
def c_oversize(port):
    """Declare 100000 bytes, over the 16384 soft cap. SPEC 7 -> GOAWAY(2).

    Only the header is sent. A server that waits for the body it was
    promised before checking the limit would hang here, which is the bug
    this case is really hunting.
    """
    hdr = u24(100000) + bytes([T_REQUEST, END_MESSAGE]) + u24(1)
    fs = send_and_read(hdr and port, hdr)
    if fs and fs[0]["type"] == T_GOAWAY and len(fs[0]["payload"]) >= 4:
        return "GOAWAY:%d" % fs[0]["payload"][3]
    return None


@case("unknown-frame-skipped")
def c_unknown(port):
    """SPEC 8: an undefined type before a real request is stepped over."""
    blob = frame(0x7E, 0, 0, b"\xDE\xAD\xBE\xEF") + get_request("/index.html")
    fs = send_and_read(port, blob, reads=2)
    return status_of(fs[0]) if fs else None


@case("traversal")
def c_traversal(port):
    """Escaping the document root. SPEC 6 -> 403."""
    fs = send_and_read(port, get_request("/../outside-docroot.txt"))
    return status_of(fs[0]) if fs else None


@case("pipelined")
def c_pipelined(port):
    """Three requests written at once, three responses, IDs preserved.

    Counts RESPONSE frames rather than reading a fixed number of frames.
    An earlier version read exactly six, which silently broke when the
    server was run with --inject-unknown and each reply became three
    frames instead of two. A conformance test that assumes how many
    frames a peer sends is testing its own arithmetic, not the protocol.
    """
    blob = b""
    for i in (1, 2, 3):
        body = bytes([1]) + hblock([entry_indexed(2, "/hello.txt")])
        blob += frame(T_REQUEST, END_MESSAGE, i, body)

    s = socket.create_connection(("127.0.0.1", port), timeout=3.0)
    s.sendall(blob)
    ids = []
    try:
        while len(ids) < 3:
            f = read_frame(s)
            if f is None:
                break
            if f["type"] == T_RESPONSE:
                ids.append(f["id"])
    except socket.timeout:
        pass
    s.close()
    return ",".join(str(i) for i in ids)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("usage: rawframe.py <port> <case>", file=sys.stderr)
        print("cases: " + " ".join(sorted(CASES)), file=sys.stderr)
        sys.exit(2)
    port, name = int(sys.argv[1]), sys.argv[2]
    if name not in CASES:
        print("no such case: " + name, file=sys.stderr)
        sys.exit(2)
    print(CASES[name](port))
