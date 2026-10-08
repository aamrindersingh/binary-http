# BHTTP/1 — a binary request/response protocol

**Version:** 1 · **Author:** Amrinder Singh (24BCS10596) · implemented by `bserve` and `bcurl`

A minimal binary protocol for fetching files over one TCP connection. Everything a second
implementer needs is here; the reference code is not required reading.

All multi-byte integers are **unsigned, big-endian**. There is no padding anywhere.

---

## 1. Connection

A client opens **one** TCP connection and keeps it open. There is no handshake and no version byte,
for the reason in §8. Either side ends the connection with `GOAWAY`.

Requests carry a **Request ID**; responses echo it. Correlation is by ID, not by order, so a server
MAY answer out of order. Client-generated IDs **start at 1 and increase monotonically**. An ID MUST
NOT be reused while a response for it is outstanding. **ID 0 is reserved for connection-level
frames** (`GOAWAY`) and MUST NOT appear on a `REQUEST`.

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

| Field | Bits | Meaning |
|---|---|---|
| Length | 24 | payload bytes, **not** counting this header |
| Type | 8 | §3 |
| Flags | 8 | §3 |
| Request ID | 24 | correlates a response to its request |

### Defending the widths

**Length is 24 bits because the width is a security control, not an aesthetic one.** A receiver
reads this field and then decides how much memory to allocate, and the sender is an untrusted peer.
At 32 bits a hostile client declares `0xFFFFFFFF` and asks me to reserve 4 GiB before it has proven
anything. The cap has to live in the protocol, not in each implementation's good judgement. 24 bits
caps a frame at 16 MiB, small enough to absorb and large enough that an ordinary file is one frame.
16 bits is too small: a 64 KiB ceiling turns a 2 MB image into 32 frames, and every extra frame is
another header, another read and another chance to get the state machine wrong. HTTP/2 reasons the
same way, and goes further by defaulting the usable maximum to 16 KiB and making larger opt-in; §7
adopts that.

**Type is 8 bits**, the smallest unit a parser reads without masking. 256 types against the four
defined here. A 4-bit type would share a byte with something else and every implementation would
carry shift-and-mask code to save nothing.

**Flags are 8 bits and separate from Type**, so a boolean modifier does not multiply the type space.
"`DATA`, and this is the last one" is one type with a flag, not a second type. That is why
`END_STREAM` costs HTTP/2 one bit instead of doubling its frame table.

**Request ID is 24 bits.** 16 bits (65,535) is reachable on a reused connection; 24 bits (16.7
million) is not. HTTP/2 spends 31 bits plus a reserved bit because its stream IDs carry priority and
server-push semantics this protocol does not have, and because it splits the space between
client- and server-initiated streams. Those 7 extra bits are not free: they are what makes the
HTTP/2 header 9 bytes.

**The header is 8 bytes, not 9**, and the 24-bit Request ID is what buys the power of two. A
fixed 8-byte header means header fields come out of one machine word and buffer arithmetic stays in
powers of two. (It does **not** keep payloads aligned on the stream: after an 8-byte header and a
97-byte payload the next frame starts at offset 105. Alignment does not survive the first
odd-length payload, and claiming otherwise would be wrong.)

---

## 3. Frame types and flags

| Type | Name | Direction | Payload |
|---|---|---|---|
| `0x01` | `REQUEST` | client → server | §5 |
| `0x02` | `RESPONSE` | server → client | §6 |
| `0x03` | `DATA` | either | opaque bytes |
| `0x04` | `GOAWAY` | either | §7, header Request ID MUST be 0 and MUST be ignored |
| other | unknown | — | **MUST be skipped, §8** |

| Flag | Value | Applies to | Meaning |
|---|---|---|---|
| `END_MESSAGE` | `0x01` | `REQUEST`, `RESPONSE`, `DATA` | no further frames for this Request ID |

Unassigned flag bits MUST be sent as 0 and **ignored** on receipt. Ignoring rather than rejecting is
what lets version 2 add a flag without breaking version 1.

---

## 4. Header encoding

HPACK's first two mechanisms and nothing more: a **static table** of the ten names this protocol
actually sends, and **length-prefixed literals** for the rest.

| # | Name | | # | Name |
|---|---|---|---|---|
| 1 | `:method` | | 6 | `content-length` |
| 2 | `:path` | | 7 | `content-type` |
| 3 | `host` | | 8 | `server` |
| 4 | `user-agent` | | 9 | `date` |
| 5 | `accept` | | 10 | `connection` |

A header block is a count byte followed by that many entries:

```
indexed name:   1 i i i i i i i      i = table index, 1-127
                <u16 value length> <value bytes>

literal name:   0 l l l l l l l      l = name length, 1-127
                <name bytes> <u16 value length> <value bytes>
```

Values are at most 65,535 bytes; literal names at most 127, which is ample for names under 30
characters.

**The block MUST be consumed exactly.** A decoder reads exactly `Count` entries and MUST finish on
the final byte of the block. Running out of bytes before `Count` entries, or having bytes left after
them, is a `400` — a decoder that simply trusts `Count` and reads until it stops is wrong.

A **literal name length of 0** (byte `0x00`) is invalid: `400`.

An **unknown static index** (say 11, added by a later version) is `400`. This is the one place the
protocol is deliberately unforgiving, because unlike a frame type an unknown index cannot be
skipped: the decoder does not learn the name, so it cannot tell whether the header mattered.

Names are lowercase ASCII and MUST be compared case-insensitively.

---

## 5. REQUEST

```
+---------------+
| Method (8)    |   1 = GET. Anything else: 405.
+---------------+
| Header block  |   §4, MUST include :path
+---------------+
```

`:path` is an absolute path beginning with `/`, already percent-decoded by the sender. A `REQUEST`
sets `END_MESSAGE` unless a body follows in `DATA` frames.

The method is a number because there are nine of them and they never change. `"GET"` as a
length-prefixed string costs five bytes to carry three bits.

---

## 6. RESPONSE

```
+-------------------------------+
| Status (16)                   |
+-------------------------------+
| Header block                  |   §4
+-------------------------------+
```

The body follows in zero or more `DATA` frames with the same Request ID, the last setting
`END_MESSAGE`. A `RESPONSE` with no body sets `END_MESSAGE` on itself.

**`END_MESSAGE` is authoritative for framing; `content-length` is advisory.** If they disagree the
receiver MUST believe `END_MESSAGE` and MAY report the mismatch. A length header that the framing
does not enforce is exactly the ambiguity that produced HTTP/1.1 request smuggling, so only one of
them is allowed to be the truth.

**Status is a 16-bit integer, not a string.** HTTP/2 carries `:status` as ASCII because it had to
stay mechanically translatable to HTTP/1.1; this protocol has no such obligation, so a status is two
bytes and a comparison. A receiver that does not recognise a code MUST use the leading digit: `2xx`
succeeded, `4xx` the sender was wrong, `5xx` the receiver was wrong. That is what lets version 2 add
`418` without breaking anyone.

| Status | When |
|---|---|
| 200 | file returned |
| 400 | malformed: short header, truncated payload, bad header block, zero-length literal name, unknown static index, `REQUEST` with ID 0, missing `:path` |
| 403 | path resolved outside the document root |
| 404 | no such file |
| 405 | method other than GET |
| 500 | server failed, e.g. could not read a file it had just stat'ed |

---

## 7. GOAWAY and limits

```
+-------------------------------+
| Last Request ID (24)          |   highest ID this sender processed
+-------------------------------+
| Reason (8)                    |   0 none, 1 protocol error, 2 too large
+-------------------------------+
```

The header Request ID of a `GOAWAY` MUST be 0. After sending one a sender MUST NOT send further
frames and SHOULD close once in-flight responses are written. A receiver MUST NOT send new requests
after seeing one.

| Limit a receiver MUST enforce | Default | Exceeded |
|---|---|---|
| frame payload | 16,384 | `GOAWAY` reason 2, close |
| absolute maximum, by field width | 16,777,215 | unrepresentable |
| header block | 8,192 | `400` |
| `:path` | 1,024 | `400` |

The 16 KiB default sits far below the 24-bit ceiling on purpose: the field width is the hard limit
an attacker cannot exceed, the default is the soft limit an ordinary peer will not. A later version
can raise the default by negotiation with no wire change.

---

## 8. Forward compatibility

> **A receiver meeting a frame `Type` it does not recognise MUST read and discard exactly `Length`
> bytes of payload and continue. It MUST NOT close the connection, MUST NOT reply with an error, and
> MUST NOT treat the frame as malformed.**

This is why there is no version number on the wire. A version 2 sender adds a frame type and a
version 1 receiver steps over it, which is cheaper and more honest than a version byte every
implementation must agree on the meaning of.

The rule is only implementable because of §2: `Length` sits at a **fixed offset in a fixed-size
header**, ahead of anything type-specific. A receiver can always find where a frame ends without
understanding it. A format that put a variable-length field before the length, or derived the length
from the type, could not offer this and would be stuck at version 1 forever.

The same principle covers unassigned flag bits (§3). It deliberately does **not** cover unknown
static indices (§4), where skipping is impossible.

---

## 9. A complete exchange

```
C→S  REQUEST   id=1  END_MESSAGE    method=GET
                                    :path = "/index.html"      index 2
                                    host  = "localhost:9000"   index 3

S→C  RESPONSE  id=1  —              status=200
                                    content-type   = "text/html"  index 7
                                    content-length = "97"         index 6
                                    server         = "bserve/1"   index 8

S→C  DATA      id=1  END_MESSAGE    <97 bytes>
```

Byte-level annotation of exactly this exchange, captured off the wire:
[docs/annotated-hexdump.md](docs/annotated-hexdump.md).

The conformance suite in [tests/](tests) exercises every MUST above, including §8 in **both
directions** by sending a type `0x7E` frame neither implementation defines and asserting the peer
carries on.
