# Why these widths

Commentary on [SPEC.md](../SPEC.md). Nothing here is normative. The spec says what a receiver must
do; this file says why the fields are the sizes they are, and what HTTP/2 does differently.

HTTP/2 chose **24 / 8 / 8 / 1+31** for Length, Type, Flags and Stream ID, which is a 9-byte header.
BHTTP/1 chose **24 / 8 / 8 / 24**, which is 8. Three of the four match, and the one that differs is
the interesting one.

---

## Length: 24 bits, because a width is a security control

This is the only field where the width is not a matter of taste.

A receiver reads `Length` and then decides how much memory to allocate. The number comes from a peer
that has proven nothing yet. At 32 bits a hostile client sends `0xFFFFFFFF` and politely asks me to
reserve 4 GiB on the strength of eight bytes it made up. One socket, four bytes of effort, and the
server is gone. The arithmetic is even worse on a forking server like `bserve`, where the attacker
picks how many copies of that allocation exist.

An implementation can of course refuse a silly length. The point is that it should not have to be
clever. If the ceiling lives in the field width then **every** conforming implementation has it,
including the careless one written next year by someone who never thought about this. 24 bits caps a
frame at 16,777,215 bytes no matter who writes the parser. That is a property of the format, not a
property of my `if` statement.

16 bits would be safer still and is the wrong trade. A 64 KiB ceiling turns a 2 MB image into 32
frames, and every extra frame is another header, another read, another partial-read case and another
opportunity to get the state machine wrong. Fragmentation bugs are more likely than exhaustion bugs
once the ceiling is already finite.

So 24 bits: small enough that the worst case is absorbable, large enough that an ordinary file is
one frame. HTTP/2 reaches the same number and then goes further, defaulting `SETTINGS_MAX_FRAME_SIZE`
to 16 KiB and making anything larger opt-in. SPEC §7 copies that, because it is the better idea:

- the **field width** is the hard limit, which no peer can exceed, ever
- the **default** is the soft limit, which an honest peer will not reach
- raising the soft limit later needs agreement, not a new wire format

The practical consequence shows up in the conformance suite. `tests/rawframe.py` sends a header
declaring 100,000 bytes and then sends nothing at all. A server that reads the payload it was
promised before checking the limit waits forever. Checking the declared length **before** reading or
allocating is the only order that works, which is why the spec states the order and not just the
number.

---

## Type: 8 bits

The smallest unit a parser can read without shifting and masking. 256 possible types against the
four defined, which is room for a decade of extensions.

Four bits would be enough for the types and would have to share a byte with something else. Every
implementation in every language would then carry shift-and-mask code on the hottest path in the
parser, to save nothing: the byte it was sharing with would still be there. Nibble packing pays off
when it removes a byte, and here it does not.

---

## Flags: 8 bits, and separate from Type

The important word is **separate**.

A flag is a modifier on a type, not a type. "`DATA`, and this is the last one" is one type with one
bit set, not a second type called `DATA_FINAL`. Merge the two fields and the type table multiplies
by two for every boolean anyone ever adds, which is how a frame table reaches thirty entries that are
really six.

This is exactly what `END_STREAM` buys HTTP/2 for one bit. `END_MESSAGE` here is the same bit doing
the same job.

Keeping all eight is deliberate even though only one is defined. Seven spare bits cost nothing in a
header that is already a power of two, and SPEC §3 requires receivers to ignore the unassigned ones
rather than reject them, so version 2 can add a flag without breaking version 1. A flags field that
receivers validate strictly is a flags field that can never be extended.

---

## Request ID: 24 bits, where I part company with HTTP/2

HTTP/2 spends a reserved bit plus 31 bits of Stream ID, which is four bytes. I spend three, and the
whole header is a byte shorter for it. The question is what that byte is buying, and the answer is
that HTTP/2 is solving a harder problem with it.

RFC 9113 §5.1.1 sets three rules I do not have to live with:

- **"Stream identifiers cannot be reused."** Not "not while outstanding", not reusable after the
  response: never, for the life of the connection. So the width has to cover everything a connection
  will ever do, not everything it is doing at once.
- **The space is split**, odd for client-initiated streams and even for server-initiated ones, so
  server push costs half the range before anyone sends a request.
- **The remedy for running out is a new connection.** The same section says a client that cannot get
  a new identifier "can establish a new connection", and a server in that position sends `GOAWAY` to
  push the client into one.

That last rule is the one that does not transfer. This protocol is built around a single connection
that stays open, so "open another one" is not an escape I am allowed to use. Exhaustion has to be
handled on the connection or not at all, which is why SPEC §1 states a wrap rule outright: after
`0xFFFFFF` the next ID is 1, never 0, and an ID in use is not reused until its response arrives.

Once wrapping is legal the width stops being a deadline. It is worth being precise about what it
becomes, because the tempting version of this argument is wrong. 24 bits is **not** a number nobody
reaches: at a sustained 300 requests per second, 2^24 IDs are used up in about 15 and a half hours,
so a connection held open for a day does wrap, and wrapping is routine rather than exceptional.

What the width actually buys is **how long a single request may stay outstanding before it blocks
new ones**. A wrap is free unless the specific ID being reused is still unanswered, and that only
happens when one request has been in flight for an entire cycle. So the cycle time is the budget for
the slowest request on the connection:

| Width | IDs | One cycle at 300 req/s | A request can be outstanding for |
|---|---|---|---|
| 16 bits | 65,536 | about 3.6 minutes | under 3.6 minutes |
| 24 bits | 16,777,216 | about 15.5 hours | under 15.5 hours |

That is the argument against 16 bits, and it is a practical one rather than a counting exercise.
Three and a half minutes is well inside the range of things that really happen: a slow client on a
bad link, a large file, a request deliberately held open. A protocol where the server taking four
minutes to answer stalls the client's next request is a protocol with a timeout baked into its frame
header. 24 bits moves that limit to most of a day, which nothing reasonable reaches.

256 times the headroom, for bits that were spare in an 8-byte header anyway. That is the whole
trade.

One claim I removed while checking it: I had written that HTTP/2 stream IDs carry priority
relationships, and that would have been an argument for the width. It does not hold up. Priority was
a separate dependency field rather than part of the identifier, and RFC 9113 §5.3 deprecates the
RFC 7540 scheme entirely in favour of RFC 9218. The defensible reasons for 31 bits are the two above,
no reuse and the odd/even split, so those are the ones I argue against.

---

## The header is 8 bytes, not 9

This is the payoff. Length, Type and Flags are fixed by the arguments above, which spends 40 bits
and leaves 24 in a 64-bit header. The ID gets them because they were otherwise going to waste, and
24 bits is what lands the header on a power of two.

Eight bytes means the whole header is one 64-bit load, and it means buffer arithmetic stays in powers
of two: a read loop asking for 8 bytes and then `Length` bytes never computes an awkward offset.
Nine bytes is not slow, it is just permanently slightly annoying, in a struct that every
implementation of the protocol has to write.

One thing I will not claim, because it is the tempting and false version of this argument: an 8-byte
header does **not** keep payloads aligned on the stream. Take the real capture in
[annotated-hexdump.md](annotated-hexdump.md). Its three frames carry 33, 63 and 94 bytes, so they
begin at stream offsets 0, 41 and 112 and the stream ends at 214. The second frame is already
misaligned, and it was misaligned by the first payload. Alignment dies at the first payload whose
length is not a multiple of 8, and since payloads are file contents, that is immediately. The
benefit is the header struct and the arithmetic around it, nothing further out.

---

## Two smaller choices made the same way

**The method is a byte, not a string.** There are nine HTTP methods and the list has barely moved in
thirty years. `"GET"` as a length-prefixed literal costs five bytes to carry what is really three
bits of information, and forces every receiver to do a string comparison before it can route. One
byte and a `switch` is the honest encoding of a closed set. The spec keeps a full byte rather than
packing it, for the reason given under Type.

**The status is a `u16`, not ASCII.** HTTP/2 carries `:status` as a string, and it had a reason:
it had to stay mechanically translatable to and from HTTP/1.1, where the status really is text on a
line. This protocol has no such obligation. Two bytes and an integer comparison, and SPEC §6 keeps
the one property that mattered about the text form by requiring receivers to act on the leading digit
when they meet a code they do not know, so `2xx` still means success for codes that do not exist yet.

---

## What the forward-compatibility rule costs

SPEC §8 says a receiver meeting an unknown frame type must discard exactly `Length` bytes and carry
on. That rule is the reason the header is laid out the way it is, and it is worth seeing what it
rules out.

It needs `Length` to be at a **fixed offset in a fixed-size header, ahead of anything
type-specific**. A receiver that cannot parse a frame must still be able to find where it ends.

So two designs that otherwise look reasonable are unavailable:

- putting a variable-length field before the length, which means you must understand the frame to
  find the length
- deriving the length from the type, as a format with fixed-size messages per type would, which
  means an unknown type has unknown length

Either one leaves the format stuck at version 1 forever, because the only safe response to an
unrecognised frame is to close the connection. Paying three bytes at a fixed offset in every single
frame is what buys the ability to extend the protocol without renegotiating it, and that is also why
there is no version byte on the wire: §8 does the job a version byte would have done, without
anyone having to agree on what the byte means.
