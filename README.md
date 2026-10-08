# BHTTP/1 — HTTP, in binary

**Amrinder Singh · 24BCS10596**

A binary request/response protocol, specified from scratch, with a server and a client that were
written independently against the specification.

| Deliverable | Where |
|---|---|
| **The spec** | [SPEC.md](SPEC.md) |
| **The programs** | [`server/`](server), [`client/`](client) |
| **Annotated hexdump of one complete exchange** | [docs/annotated-hexdump.md](docs/annotated-hexdump.md) |

```
$ make
$ ./bserve ./www 9000 &
$ ./bcurl -v localhost:9000/index.html
```

---

## The part that is actually the project

A fixed 8-byte frame header, and every width defended in [SPEC §2](SPEC.md#2-frame-header):

```
 0                   1                   2                   3
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Length (24)                |     Type (8)  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|    Flags (8)  |                Request ID (24)                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

The short version of the defence, with the full argument in the spec:

- **Length is 24 bits because the width is a security control.** The receiver allocates based on
  this number and the sender is untrusted. 32 bits lets a hostile peer ask for 4 GiB before proving
  anything; 24 caps a frame at 16 MiB. 16 bits would be safe but turns a 2 MB file into 32 frames.
- **Type 8, Flags 8, kept separate** so a boolean modifier does not double the frame table.
- **Request ID 24, not HTTP/2's 31.** 65 k IDs is reachable on a reused connection, 16.7 M is not,
  and HTTP/2's extra bits pay for multiplexing and server push that this protocol does not have.
  Those 7 bits are exactly what makes the HTTP/2 header 9 bytes instead of 8.

Headers use HPACK's first two mechanisms and nothing else: a ten-entry static table for the names
actually sent, and length-prefixed literals for the rest. In the captured exchange the table saves
52 bytes on a 63-byte response payload.

### The rule that may not be skipped

> A receiver meeting a frame type it does not recognise MUST read and discard exactly `Length`
> bytes and continue.

This is why `Length` lives at a fixed offset in a fixed-size header, ahead of anything
type-specific: a receiver can always find the end of a frame it does not understand. It is also why
there is no version byte on the wire.

Both programs implement it, and the conformance suite proves it **in both directions** by sending a
type `0x7E` frame that neither side defines:

```
PASS  server skips unknown type          200       SPEC 8
PASS  server skips it from bcurl too     0         SPEC 8
PASS  client skips unknown type          3         SPEC 8
PASS  and still gets a correct body      0         SPEC 8
```

---

## "A client that only works against your own server is an implementation, not a protocol"

The server and the client do not share a line of wire-format code. Not a header, not a source file,
not an include path.

| | server | client |
|---|---|---|
| codec | `server/bframe.c` / `.h` | `client/wire.c` / `.h` |
| naming | `bf_` prefix, `struct bf_header` | no prefix, `frame_hdr` |
| header block decoding | decodes into a fixed array up front | walks with a cursor and a callback |
| IO | `int` returns, `1/0/-1` | `bool` returns, separate `eof` out-param |

Measured rather than asserted:

```
identical filenames across the two dirs:  none
either including from the other:          none
longest common run between the codecs:    4 lines (all #include <...>)
overall line similarity:                  31.5%
```

The `Makefile` compiles them as two separate programs with no shared object file and no common
`-I` path, so the separation cannot quietly rot.

There is also a third implementation of the wire format in
[`tests/rawframe.py`](tests/rawframe.py), written from the spec in Python, used to generate the
malformed frames a well-behaved client cannot produce. A bug common to both C codecs would still be
caught by it.

---

## Running it

```bash
make                       # both binaries, -Wall -Wextra -Wpedantic -Wconversion, clean
make test                  # 21 conformance checks
make asan                  # rebuild under AddressSanitizer + UBSan
```

Server:

```
./bserve <docroot> <port> [-v] [--inject-unknown]
```

`--inject-unknown` emits a type `0x7E` frame before every response, to test that a peer honours
SPEC 8. Binds to loopback only.

Client:

```
./bcurl [-v] [-n count] [--send-unknown-frame] host:port/path
```

`-v` hexdumps every frame in both directions. `-n` sends that many requests **down the same
connection**, which is how keep-alive is demonstrated. Exits 1 on 4xx or 5xx.

Opening a second connection is a guarded error, not a convention:

```c
if (connects_made > 0) {
    fprintf(stderr, "bcurl: refusing to open a second connection; "
                    "the spec allows exactly one\n");
    exit(4);
}
```

---

## Testing

`make test` runs 21 checks, each traceable to a MUST in the spec.

```
  21 passed, 0 failed
```

| Group | Covers |
|---|---|
| interop | body returned, exit codes, 5 requests over 1 connection, IDs increase from 1 |
| status codes | 200, 403 on traversal, 404, 405, 400 on a request with ID 0 |
| header decoding | truncated block, trailing bytes, unknown static index, zero-length literal name |
| limits | an oversized frame gets `GOAWAY` reason 2 |
| forward compat | unknown frame type skipped, both directions |
| pipelining | three requests in flight, IDs preserved |

Additionally:

- The whole suite passes again under **AddressSanitizer and UBSan** with zero reports.
- [`tests/fuzz.py`](tests/fuzz.py) throws **400 malformed cases** at the sanitized server — random
  noise, lengths that lie, truncated headers, every frame type — and it keeps serving. Zero
  sanitizer reports.

Two bugs the tests caught while I was writing them:

**A length that lies.** The oversized case sends only a header claiming 100,000 bytes and never
sends the body. A server that waits for the payload it was promised before checking the limit
hangs. Checking the declared length *before* reading or allocating is the only order that works,
and it is the practical reason the 24-bit cap exists at all.

**My own test was order-dependent.** The pipelining check read a fixed six frames, which was right
until the suite started running it against a server in `--inject-unknown` mode, where each reply is
three frames instead of two. It reported the implementation as broken when the implementation was
fine. It now counts responses instead of frames. A conformance test that assumes how many frames a
peer sends is testing its own arithmetic.

---

## Layout

```
SPEC.md                     the deliverable
docs/annotated-hexdump.md   every byte of one exchange, labelled
server/
  bserve.c                  accept, route, serve files
  bframe.c  bframe.h        the server's codec
client/
  bcurl.c                   one connection, build request, hexdump
  wire.c    wire.h          the client's codec, written separately
tests/
  conformance.sh            21 checks against the spec
  rawframe.py               a third codec, for malformed frames
  capture.py                generates the annotated hexdump
  fuzz.py                   400 malformed cases
www/                        document root
```

---

## Known limits

Honest about what this is not:

- **GET only.** Method is a one-byte field with room for the rest; nothing else is implemented.
- **No TLS, no compression.** Out of scope, and the frame layout does not preclude either.
- **The server forks per connection.** Fine for a file server on loopback, wrong at scale. An event
  loop would be the next change, and nothing in the wire format depends on the process model.
- **`content-length` is advisory.** Deliberate, see SPEC §6: `END_MESSAGE` is the single source of
  truth for framing, because two sources of truth is what produced HTTP/1.1 request smuggling.
- **Responses are buffered whole in memory** before sending. A 16 MiB file means a 16 MiB
  allocation. Streaming straight from the file into DATA frames is the obvious fix and the frame
  layout already supports it.
