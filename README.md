# BHTTP/1: HTTP, in binary

**Amrinder Singh · 24BCS10596**

A binary request/response protocol, specified from scratch, with a server and a client that were
written independently against the specification.

| Deliverable | Where |
|---|---|
| **The spec** | [SPEC.md](SPEC.md) |
| **The programs** | [`server/`](server), [`client/`](client) |
| **Annotated hexdump of one complete exchange** | [docs/annotated-hexdump.md](docs/annotated-hexdump.md) |
| The width argument, long form | [docs/why-these-widths.md](docs/why-these-widths.md) |

SPEC.md is mostly normative: what a receiver MUST do, in three and a half A4 pages at 10.5pt. The
exception is §2, which carries a short defence of each field width, because defending them is part
of what the spec was asked to do. The long form of that argument, and everything else that explains
a choice rather than stating a rule, is in `docs/why-these-widths.md`. The brief said two pages. I
measured, got three and a half, and stopped cutting when the next thing to go would have been a rule
or the worked exchange, because "enough that a stranger could implement it" is the half of that
brief that actually matters.

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

- **Length is 24 bits because the width is a security control.** The receiver allocates on this
  number and the sender is untrusted. 32 bits lets a hostile peer ask for 4 GiB before it has proven
  anything, and 24 caps a frame at 16 MiB. 16 bits would be safer and turns a 2 MB file into 32
  frames, which trades an exhaustion bug for a fragmentation bug.
- **Type 8, Flags 8, kept separate** so a boolean modifier does not double the frame table.
- **Request ID 24, not HTTP/2's 31.** HTTP/2 never reuses a stream identifier, and splits the space
  between odd client IDs and even server-pushed ones, so its width has to cover a whole connection's
  history with half of it unusable. Its remedy when that runs out is to open a new connection
  (RFC 9113 §5.1.1), which this protocol forbids, so SPEC §1 wraps instead. The width then sets how
  often a wrap happens rather than when the connection has to be abandoned. Those bits are not free
  either: a reserved bit plus 31 is four bytes where this ID is three, and that one byte is the
  difference between a 9-byte header and an 8-byte one.

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

Measured rather than asserted, by [`tests/independence.py`](tests/independence.py), which strips
comments and blank lines first and then compares the remaining code whole and in order:

```
$ make independence

  server codec: server/bframe.c server/bframe.h 213 code lines
  client codec: client/wire.c client/wire.h  214 code lines

  identical filenames across the two dirs:  none
  either including from the other:          none
  longest common run:                       3 lines
                                              #include <string.h>
                                              #include <strings.h>
                                              #include <unistd.h>
  overall line similarity:                  14.5%
```

The longest thing the two codecs have in common is three consecutive `#include` lines. The script
fails if a common run ever passes 8 lines or similarity passes 40%, and `make test` runs it, so the
separation cannot quietly rot. The `Makefile` also compiles them as two programs with no shared
object file and no common `-I` path, so it cannot rot by accident either.

There is also a third implementation of the wire format in
[`tests/rawframe.py`](tests/rawframe.py), written from the spec in Python, used to generate the
malformed frames a well-behaved client cannot produce. A bug common to both C codecs would still be
caught by it.

---

## Running it

```bash
make                       # both binaries, -Wall -Wextra -Wpedantic -Wconversion, clean
make test                  # 24 conformance checks
make asan                  # rebuild under AddressSanitizer + UBSan
make fuzz                  # 400 malformed cases, starts its own server
make independence          # proves the two codecs share no code
```

Server:

```
./bserve <docroot> <port> [-v] [--inject-unknown]
```

`--inject-unknown` emits a type `0x7E` frame before every response, to test that a peer honours
SPEC 8. Binds to loopback only.

Client:

```
./bcurl [-v] [-n count] [--start-id n] [--send-unknown-frame] host:port/path
```

`-v` hexdumps every frame in both directions. `-n` sends that many requests **down the same
connection**, which is how keep-alive is demonstrated. Exits 1 on 4xx or 5xx.

`--start-id` begins the request counter somewhere other than 1, which SPEC §1 says a client MUST NOT
do. It is a test affordance like the server's `--inject-unknown`, and it exists because §1 wraps IDs
after 16,777,215 and a test should not have to send 16.7 million requests to watch that happen:

```
$ ./bcurl -v --start-id 16777214 -n 4 localhost:9000/hello.txt >/dev/null
> REQUEST id=16777214 END_MESSAGE  GET /hello.txt
>  header (8 bytes)
  0000  00 00 30 01 01 ff ff fe                           |..0.....|
> REQUEST id=16777215 END_MESSAGE  GET /hello.txt
>  header (8 bytes)
  0000  00 00 30 01 01 ff ff ff                           |..0.....|
> REQUEST id=1 END_MESSAGE  GET /hello.txt
>  header (8 bytes)
  0000  00 00 30 01 01 00 00 01                           |..0.....|
> REQUEST id=2 END_MESSAGE  GET /hello.txt
>  header (8 bytes)
  0000  00 00 30 01 01 00 00 02                           |..0.....|
```

The last three bytes of each header are the Request ID. It goes `ff ff ff` and then `00 00 01`,
never `00 00 00`, because ID 0 is reserved for `GOAWAY`.

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

`make test` runs 24 checks: 23 traceable to a MUST in the spec, plus the independence check below.

```
  24 passed, 0 failed
```

| Group | Covers |
|---|---|
| interop | body returned, exit codes, 5 requests over 1 connection, IDs increase from 1, IDs wrap to 1 and never to 0 |
| status codes | 200, 403 on traversal, 404, 405, 400 on a request with ID 0 |
| header decoding | truncated block, trailing bytes, unknown static index, zero-length literal name |
| limits | an oversized frame gets `GOAWAY` reason 2 |
| forward compat | unknown frame type skipped, both directions |
| ID exhaustion | IDs wrap from `0xFFFFFF` to 1 and never to 0, and the server accepts a wrapped ID |
| independence | the two codecs still share no code, by `tests/independence.py` |
| pipelining | three requests in flight, IDs preserved |

Additionally:

- The whole suite passes again under **AddressSanitizer and UBSan** with zero reports.
- [`tests/fuzz.py`](tests/fuzz.py) throws **400 malformed cases** at the sanitized server: random
  noise, lengths that lie, truncated headers, every frame type. It keeps serving, with zero
  sanitizer reports.

```
$ make asan && make test && make fuzz
  24 passed, 0 failed
OK: 400 cases sent, 0 refused mid-write, server still accepting, no sanitizer reports in
tests/fuzz-server.log
```

Three bugs the tests caught while I was writing them:

**A length that lies.** The oversized case sends only a header claiming 100,000 bytes and never
sends the body. A server that waits for the payload it was promised before checking the limit
hangs. Checking the declared length *before* reading or allocating is the only order that works,
and it is the practical reason the 24-bit cap exists at all.

**My own test was order-dependent.** The pipelining check read a fixed six frames, which was right
until the suite started running it against a server in `--inject-unknown` mode, where each reply is
three frames instead of two. It reported the implementation as broken when the implementation was
fine. It now counts responses instead of frames. A conformance test that assumes how many frames a
peer sends is testing its own arithmetic.

**The fuzz script could not tell a dead server from a missing one.** It expected a server to already
be listening, so running it without one printed `stopped accepting after 0 cases`, which reads
exactly like a crash and cost me a few minutes of hunting a bug that was not there. It now starts
and owns its own server. While fixing that I noticed the weaker half of the same problem: the server
forks per connection, so a child can die to a sanitizer while the parent carries on accepting, and a
live listener at the end proves less than it looks. The script now also greps the server's own log
for sanitizer output, so "no reports" is a thing it checks rather than a thing I remembered to look
at.

---

## Layout

```
SPEC.md                     the deliverable
docs/annotated-hexdump.md   every byte of one exchange, labelled
docs/why-these-widths.md    why 24/8/8/24, and where I disagree with HTTP/2
server/
  bserve.c                  accept, route, serve files
  bframe.c  bframe.h        the server's codec
client/
  bcurl.c                   one connection, build request, hexdump
  wire.c    wire.h          the client's codec, written separately
tests/
  conformance.sh            24 checks against the spec
  rawframe.py               a third codec, for malformed frames
  capture.py                generates the annotated hexdump
  fuzz.py                   400 malformed cases, owns its server
  independence.py           measures how separate the two codecs are
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
