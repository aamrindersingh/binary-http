#!/usr/bin/env python3
"""Capture one real exchange and emit a byte-by-byte annotation.

Sits on the wire as a client, writes exactly what the spec says, and
labels every byte that comes back with its field name and the section of
SPEC.md that defines it. If a byte cannot be labelled, the spec has a
hole in it.
"""
import socket, struct, sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9414
PATH = sys.argv[2] if len(sys.argv) > 2 else "/index.html"
HOST = "localhost:%d" % PORT

STATIC = [None, ":method", ":path", "host", "user-agent", "accept",
          "content-length", "content-type", "server", "date", "connection"]
TYPES  = {1: "REQUEST", 2: "RESPONSE", 3: "DATA", 4: "GOAWAY"}

def u24(v): return bytes(((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF))
def hx(b):  return " ".join("%02x" % c for c in b)

out = []
def emit(off, raw, label):
    """One annotated line: offset, raw bytes, what they mean."""
    out.append("  %04x  %-26s %s" % (off, hx(raw), label))

def annotate_header(buf, base, direction):
    h = {"len": (buf[0] << 16) | (buf[1] << 8) | buf[2],
         "type": buf[3], "flags": buf[4],
         "id": (buf[5] << 16) | (buf[6] << 8) | buf[7]}
    tn = TYPES.get(h["type"], "UNKNOWN 0x%02X" % h["type"])
    out.append("%s frame header  (SPEC 2, always 8 bytes)" % direction)
    emit(base + 0, buf[0:3], "Length = %d        payload bytes, 24 bits" % h["len"])
    emit(base + 3, buf[3:4], "Type = %d           %s" % (h["type"], tn))
    fl = "END_MESSAGE" if h["flags"] & 1 else "none"
    emit(base + 4, buf[4:5], "Flags = 0x%02x        %s" % (h["flags"], fl))
    emit(base + 5, buf[5:8], "Request ID = %d     correlates the reply" % h["id"])
    return h

def annotate_block(buf, base, what):
    """Walk a header block the way SPEC 4 says to, labelling as we go."""
    off = 0
    n = buf[off]
    emit(base + off, buf[off:off+1], "%s count = %d" % (what, n))
    off += 1
    for _ in range(n):
        lead = buf[off]
        if lead & 0x80:
            idx = lead & 0x7F
            emit(base + off, buf[off:off+1],
                 "  0x%02x  indexed name %d = %s" % (lead, idx, STATIC[idx]))
            off += 1
        else:
            nl = lead & 0x7F
            emit(base + off, buf[off:off+1], "  0x%02x  literal name, %d bytes" % (lead, nl))
            off += 1
            emit(base + off, buf[off:off+nl], '  name = "%s"' % buf[off:off+nl].decode())
            off += nl
        vl = struct.unpack(">H", buf[off:off+2])[0]
        emit(base + off, buf[off:off+2], "  value length = %d  (u16)" % vl)
        off += 2
        v = buf[off:off+vl].decode("utf-8", "replace")
        emit(base + off, buf[off:off+vl], '  value = "%s"' % v)
        off += vl
    return off

# ---- build the request exactly as the spec describes -------------------
block = bytes([2])
for idx, val in ((2, PATH), (3, HOST)):
    v = val.encode()
    block += bytes([0x80 | idx]) + struct.pack(">H", len(v)) + v
payload = bytes([1]) + block                       # method GET
req = u24(len(payload)) + bytes([1, 1]) + u24(1) + payload

out.append("## Request: client to server\n")
annotate_header(req[:8], 0, "C->S")
out.append("")
out.append("C->S REQUEST payload  (SPEC 5)")
emit(8, req[8:9], "Method = 1          GET")
annotate_block(req[9:], 9, ":path and host,")
out.append("")

s = socket.create_connection(("127.0.0.1", PORT), timeout=3)
s.sendall(req)

def recv_exact(n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: break
        b += c
    return b

# ---- RESPONSE ----------------------------------------------------------
rh = recv_exact(8)
out.append("## Response: server to client\n")
h = annotate_header(rh, 0, "S->C")
body = recv_exact(h["len"])
out.append("")
out.append("S->C RESPONSE payload  (SPEC 6)")
st = struct.unpack(">H", body[:2])[0]
emit(8, body[:2], "Status = %d       u16, not a string (SPEC 6)" % st)
annotate_block(body[2:], 10, "response header")
out.append("")

# ---- DATA --------------------------------------------------------------
dh = recv_exact(8)
h2 = annotate_header(dh, 0, "S->C")
data = recv_exact(h2["len"])
out.append("")
out.append("S->C DATA payload  (SPEC 3, opaque bytes)")
shown = data[:48]
emit(8, shown, "the file, first %d of %d bytes" % (len(shown), len(data)))
out.append("")
out.append("  decoded: %r%s" % (data[:60].decode("utf-8", "replace"),
                                " ..." if len(data) > 60 else ""))
s.close()
print("\n".join(out))
