# BHTTP/1: a binary request/response protocol

**Version 1.** Amrinder Singh, 24BCS10596. Reference implementations `bserve` and `bcurl`.

Everything a second implementer needs is in this document; the reference code is not required
reading. Section 2 also defends each field width, briefly, since the widths are the part of a binary
format most worth questioning. The long form of that argument, and anything else that explains a
choice rather than imposing a rule, is in [docs/why-these-widths.md](docs/why-these-widths.md),
which is commentary and binds nobody.

All multi-byte integers are unsigned and big-endian. There is no padding anywhere.

---

## 1. Connection

A client opens **one** TCP connection and keeps it open for every request it makes. There is no
handshake and no version byte, for the reason in §8. Either side ends the connection with `GOAWAY`.

Every `REQUEST` carries a **Request ID** and its response echoes it, so correlation is by ID and not
by arrival order: a server MAY answer out of order and a client MAY hold several requests in flight.

Client IDs MUST start at 1 and increase by one per request. After `0xFFFFFF` they **wrap back to 1**,
never to 0. An ID MUST NOT be reused while a response for it is outstanding, so a client reaching an
ID it still has unanswered MUST wait for that response before sending. Since the connection cannot
be replaced, wrapping is the only way this protocol survives running out of IDs, and §2 is where
that width is argued.

**ID 0 is reserved for connection-level frames** (`GOAWAY`); a `REQUEST` carrying ID 0 is a `400`.

---

## 2. Frame header

Every frame begins with the same **8-byte** header.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Length (24)                |     Type (8)  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|    Flags (8)  |                Request ID (24)                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      Payload (Length bytes)                 ...
+---------------------------------------------------------------+
```

| Field | Bits | Meaning | Width chosen because |
|---|---|---|---|
| Length | 24 | payload bytes, **not** counting this header | a receiver allocates on it and the sender is untrusted |
| Type | 8 | §3 | smallest unit read without shift and mask |
| Flags | 8 | §3 | separate, so a modifier cannot double the type table |
| Request ID | 24 | correlates a response to its request | wrapping is legal, so this sets how often, not whether |

### Why these widths

HTTP/2 chose 24 / 8 / 8 / 1+31 and got a 9-byte header. Three of those four are right, and the one
that differs pays for features this protocol does not have.

**Length is 24 bits because a width is a security control.** A receiver reads this field and then
decides how much memory to allocate, on the word of a peer that has proven nothing yet. At 32 bits a
hostile client declares `0xFFFFFFFF` and asks the receiver to reserve 4 GiB for eight bytes of
effort. Put the ceiling in the field and every conforming implementation has it, including the
careless one written next year; leave it to each parser and only the careful parsers have it. 16 bits
would be safer still and is the wrong trade: a 64 KiB ceiling turns a 2 MB file into 32 frames, and
every extra frame is another header, another read and another partial-read case to get wrong.

**Request ID is 24 bits rather than 31** because HTTP/2 is solving a harder problem with those bits.
It forbids reusing a stream identifier for the life of a connection and splits the space between
odd client-initiated and even server-initiated IDs (RFC 9113 §5.1.1), so its width has to cover a
whole connection's history with half of it unusable, and its stated remedy for exhaustion is to open
a new connection. This protocol has no server push, so the space is not split, and §1 permits
wrapping, so the width decides only how often a wrap happens rather than when the connection has to
be abandoned. What it costs is the longest a single request may stay outstanding before it blocks a
reuse of its ID, and at 24 bits that is hours of sustained traffic. The extra bits are not free
either: a reserved bit plus 31 is four bytes where this ID is three, and that byte is the difference
between a 9-byte header and an 8-byte one.

Type, Flags, the 8-byte total and two smaller choices made the same way:
[docs/why-these-widths.md](docs/why-these-widths.md).

---

## 3. Frame types and flags

| Type | Name | Direction | Payload |
|---|---|---|---|
| `0x01` | `REQUEST` | client to server | §5 |
| `0x02` | `RESPONSE` | server to client | §6 |
| `0x03` | `DATA` | either | opaque bytes |
| `0x04` | `GOAWAY` | either | §7, header Request ID MUST be 0 and MUST be ignored |
| any other | unknown | either | **MUST be skipped, §8** |

| Flag | Value | Applies to | Meaning |
|---|---|---|---|
| `END_MESSAGE` | `0x01` | `REQUEST`, `RESPONSE`, `DATA` | no further frames for this Request ID |

Unassigned flag bits MUST be sent as 0 and MUST be **ignored** on receipt. Ignoring rather than
rejecting is what lets version 2 add a flag without breaking version 1.

---

## 4. Header encoding

HPACK's first two mechanisms and nothing more: a **static table** of the ten names this protocol
actually sends, and **length-prefixed literals** for everything else. No dynamic table, no Huffman.

| Index | Name | Sent by |
|---|---|---|
| 1 | `:method` | client |
| 2 | `:path` | client |
| 3 | `host` | client |
| 4 | `user-agent` | client |
| 5 | `accept` | client |
| 6 | `content-length` | server |
| 7 | `content-type` | server |
| 8 | `server` | server |
| 9 | `date` | server |
| 10 | `connection` | either |

Index 0 does not exist. These numbers are part of the wire format, so an implementation MUST use
exactly this order.

A header block is a count byte followed by exactly that many entries:

```
indexed name:   1 i i i i i i i      i = table index, 1 to 127
                <u16 value length> <value bytes>

literal name:   0 l l l l l l l      l = name length, 1 to 127
                <name bytes> <u16 value length> <value bytes>
```

Values are at most 65,535 bytes and literal names at most 127. Names are lowercase ASCII and MUST be
compared case-insensitively.

**The block MUST be consumed exactly.** A decoder reads exactly `Count` entries and MUST finish on
the final byte of the block. Fewer bytes than `Count` promised is a `400`; bytes left over after
them is also a `400`. So is a **literal name length of 0**, and so is an **unknown static index**
such as 11 added by a later version. An unknown index is the one thing here a receiver may not skip
over, because it never learns the name and so cannot tell whether the header mattered.

---

## 5. REQUEST

```
+---------------+
| Method (8)    |   1 = GET. Anything else: 405.
+---------------+
| Header block  |   §4, MUST include :path
+---------------+
```

`:path` is an absolute path beginning with `/`, already percent-decoded by the sender. A missing
`:path` is a `400`. A `REQUEST` sets `END_MESSAGE` unless a body follows in `DATA` frames.

---

## 6. RESPONSE

```
+-------------------------------+
| Status (16)                   |
+-------------------------------+
| Header block                  |   §4
+-------------------------------+
```

The body follows in zero or more `DATA` frames carrying the same Request ID, the last of them
setting `END_MESSAGE`. A `RESPONSE` with no body sets `END_MESSAGE` on itself.

**`END_MESSAGE` is authoritative for framing and `content-length` is advisory.** If the two
disagree a receiver MUST believe `END_MESSAGE` and MAY report the mismatch.

**Status is a 16-bit integer, not a string.** A receiver that does not recognise a code MUST act on
its leading digit: `2xx` succeeded, `4xx` the sender was wrong, `5xx` the receiver was wrong.

| Status | When |
|---|---|
| 200 | file returned |
| 400 | malformed: short header, truncated payload, bad header block, zero-length literal name, unknown static index, `REQUEST` with ID 0, missing `:path` |
| 403 | path resolved outside the document root |
| 404 | no such file |
| 405 | method other than GET |
| 500 | the receiver failed, for instance could not read a file it had just stat'ed |

---

## 7. GOAWAY and limits

```
+-------------------------------+
| Last Request ID (24)          |   highest ID this sender processed
+-------------------------------+
| Reason (8)                    |   0 none, 1 protocol error, 2 too large
+-------------------------------+
```

The header Request ID of a `GOAWAY` MUST be 0. After sending one, a sender MUST NOT send further
frames and SHOULD close once in-flight responses are written. A receiver MUST NOT send new requests
after seeing one.

| Limit a receiver MUST enforce | Default | When exceeded |
|---|---|---|
| frame payload | 16,384 | `GOAWAY` reason 2, then close |
| absolute maximum, by field width | 16,777,215 | unrepresentable |
| header block | 8,192 | `400` |
| `:path` | 1,024 | `400` |

A receiver MUST check the declared `Length` **before** reading or allocating the payload. The 16 KiB
default sits far below the 24-bit ceiling on purpose: the width is the hard limit an attacker cannot
exceed, the default is the soft limit an honest peer will not reach.

---

## 8. Forward compatibility

> **A receiver meeting a frame `Type` it does not recognise MUST read and discard exactly `Length`
> bytes of payload and continue. It MUST NOT close the connection, MUST NOT reply with an error,
> and MUST NOT treat the frame as malformed.**

This is why there is no version byte on the wire: a version 2 sender adds a frame type and a version
1 receiver steps over it. It is only implementable because of §2. `Length` is at a fixed offset in a
fixed-size header, ahead of anything type-specific, so a receiver can always find where a frame ends
without understanding it.

The same principle covers unassigned flag bits (§3). It deliberately does **not** cover unknown
static indices (§4), where skipping is impossible.

---

## 9. A complete exchange

```
C->S  REQUEST   id=1  END_MESSAGE    method=GET                    payload 33 bytes
                                     :path = "/index.html"         index 2
                                     host  = "localhost:9000"      index 3

S->C  RESPONSE  id=1  (no flags)     status=200                    payload 63 bytes
                                     content-type   = "text/html"  index 7
                                     content-length = "94"         index 6
                                     server         = "bserve/1"   index 8

S->C  DATA      id=1  END_MESSAGE    <94 bytes of file>            payload 94 bytes
```

Three frames, 24 bytes of header, 190 bytes of payload.

Every byte of exactly this exchange, captured off the wire and annotated:
[docs/annotated-hexdump.md](docs/annotated-hexdump.md).
