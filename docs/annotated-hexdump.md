# Annotated hexdump: one complete request and response

Captured off the wire by [`tests/capture.py`](../tests/capture.py), which acts as a third client
written from the spec alone. Every byte below is labelled with the field it belongs to and the
section of [SPEC.md](../SPEC.md) that defines it.

Regenerate with:

```bash
make && ./bserve ./www 9414 &
python3 tests/capture.py 9414 /index.html
```

Offsets restart at 0 for each frame. The `Length` in a frame header counts the payload only, so a
frame occupies `8 + Length` bytes on the stream.

---

```text
## Request: client to server

C->S frame header  (SPEC 2, always 8 bytes)
  0000  00 00 21                   Length = 33        payload bytes, 24 bits
  0003  01                         Type = 1           REQUEST
  0004  01                         Flags = 0x01        END_MESSAGE
  0005  00 00 01                   Request ID = 1     correlates the reply

C->S REQUEST payload  (SPEC 5)
  0008  01                         Method = 1          GET
  0009  02                         :path and host, count = 2
  000a  82                           0x82  indexed name 2 = :path
  000b  00 0b                        value length = 11  (u16)
  000d  2f 69 6e 64 65 78 2e 68 74 6d 6c   value = "/index.html"
  0018  83                           0x83  indexed name 3 = host
  0019  00 0e                        value length = 14  (u16)
  001b  6c 6f 63 61 6c 68 6f 73 74 3a 39 34 31 34   value = "localhost:9414"

## Response: server to client

S->C frame header  (SPEC 2, always 8 bytes)
  0000  00 00 3f                   Length = 63        payload bytes, 24 bits
  0003  02                         Type = 2           RESPONSE
  0004  00                         Flags = 0x00        none
  0005  00 00 01                   Request ID = 1     correlates the reply

S->C RESPONSE payload  (SPEC 6)
  0008  00 c8                      Status = 200       u16, not a string (SPEC 6)
  000a  04                         response header count = 4
  000b  87                           0x87  indexed name 7 = content-type
  000c  00 09                        value length = 9  (u16)
  000e  74 65 78 74 2f 68 74 6d 6c   value = "text/html"
  0017  86                           0x86  indexed name 6 = content-length
  0018  00 02                        value length = 2  (u16)
  001a  39 34                        value = "94"
  001c  88                           0x88  indexed name 8 = server
  001d  00 08                        value length = 8  (u16)
  001f  62 73 65 72 76 65 2f 31      value = "bserve/1"
  0027  89                           0x89  indexed name 9 = date
  0028  00 1d                        value length = 29  (u16)
  002a  54 68 75 2c 20 30 38 20 4f 63 74 20 32 30 32 36 20 31 34 3a 30 39 3a 35 36 20 47 4d 54   value = "Thu, 08 Oct 2026 14:09:56 GMT"

S->C frame header  (SPEC 2, always 8 bytes)
  0000  00 00 5e                   Length = 94        payload bytes, 24 bits
  0003  03                         Type = 3           DATA
  0004  01                         Flags = 0x01        END_MESSAGE
  0005  00 00 01                   Request ID = 1     correlates the reply

S->C DATA payload  (SPEC 3, opaque bytes)
  0008  3c 21 64 6f 63 74 79 70 65 20 68 74 6d 6c 3e 0a 3c 74 69 74 6c 65 3e 62 73 65 72 76 65 3c 2f 74 69 74 6c 65 3e 0a 3c 68 31 3e 49 74 20 77 6f 72 the file, first 48 of 94 bytes

  decoded: '<!doctype html>\n<title>bserve</title>\n<h1>It works</h1>\n<p>S' ...
```

---

## What to notice

**The 8-byte header is identical in all three frames.** Only the Type, Flags and Length differ.
That uniformity is what makes SPEC 8 possible: a receiver finds `Length` at the same offset every
time, so it can step over a frame whose Type means nothing to it.

**`82` and `83` are one byte each and carry a whole header name.** `0x82` is `1` in the top bit plus
index `2`, which is `:path`. Spelling `:path` out as a literal would have cost 6 bytes instead of 1.
Across the four response headers the static table saves 52 bytes on a 63-byte payload.

**The status is `00 c8`, not `"200"`.** Two bytes and an integer comparison, where HTTP/2 sends
three ASCII characters through HPACK because it has to stay translatable to HTTP/1.1.

**`content-length` is `"94"` and the DATA frame is 94 bytes**, but the client does not rely on that
agreement. SPEC 6 makes `END_MESSAGE` authoritative and `content-length` advisory, which is why
flags on the DATA frame are `0x01`.

**The request is 41 bytes on the wire** (8 header + 33 payload) to express what
`GET /index.html HTTP/1.1\r\nHost: localhost:9414\r\n\r\n` needs 52 bytes for, without any
compression beyond the ten-entry static table.
