/* bframe.h - BHTTP/1 wire format, server side.
 *
 * This is the SERVER's own codec. The client under ../client/ has a
 * separate one, written independently. They share no header, no source
 * file and no include path: the only thing in common is SPEC.md. That is
 * the point of the exercise, so please do not "helpfully" factor them
 * into a shared library.
 */
#ifndef BFRAME_H
#define BFRAME_H

#include <stddef.h>
#include <stdint.h>

/* SPEC 2: the header is 8 bytes, fixed, always. */
#define BF_HEADER_LEN 8

/* SPEC 7: limits a receiver MUST enforce. */
#define BF_MAX_PAYLOAD_DEFAULT 16384u    /* soft ceiling  */
#define BF_MAX_PAYLOAD_ABS     0xFFFFFFu /* 24-bit field  */
#define BF_MAX_HEADER_BLOCK    8192u
#define BF_MAX_PATH            1024u

/* SPEC 3 */
enum {
    BF_T_REQUEST  = 0x01,
    BF_T_RESPONSE = 0x02,
    BF_T_DATA     = 0x03,
    BF_T_GOAWAY   = 0x04
};

#define BF_FLAG_END_MESSAGE 0x01

/* SPEC 5 */
#define BF_METHOD_GET 1

/* SPEC 6 */
enum {
    BF_ST_OK          = 200,
    BF_ST_BAD_REQUEST = 400,
    BF_ST_FORBIDDEN   = 403,
    BF_ST_NOT_FOUND   = 404,
    BF_ST_BAD_METHOD  = 405,
    BF_ST_SERVER_ERR  = 500
};

/* SPEC 7 */
enum { BF_GO_NONE = 0, BF_GO_PROTOCOL = 1, BF_GO_TOO_LARGE = 2 };

/* SPEC 4: the ten names. Index 0 is reserved, so entry 0 is NULL and the
 * table is addressed 1..BF_STATIC_COUNT. */
#define BF_STATIC_COUNT 10
extern const char *const bf_static_table[BF_STATIC_COUNT + 1];

struct bf_header {
    uint32_t length;     /* 24 bits */
    uint8_t  type;
    uint8_t  flags;
    uint32_t request_id; /* 24 bits */
};

/* One decoded header. Names point into caller-owned memory or the static
 * table; values point into the frame payload buffer. Nothing is copied. */
struct bf_hdr {
    const char *name;
    size_t      name_len;
    const char *value;
    size_t      value_len;
};

#define BF_MAX_HEADERS 32

/* Big-endian helpers. Written out rather than using htonl so the byte
 * order is visible at the point of use, which matters in a protocol
 * where endianness is a stated rule. */
void     bf_put_u16(uint8_t *p, uint16_t v);
void     bf_put_u24(uint8_t *p, uint32_t v);
uint16_t bf_get_u16(const uint8_t *p);
uint32_t bf_get_u24(const uint8_t *p);

void bf_encode_header(uint8_t out[BF_HEADER_LEN], const struct bf_header *h);
void bf_decode_header(const uint8_t in[BF_HEADER_LEN], struct bf_header *h);

/* Decode a header block (SPEC 4). Returns the number of headers, or
 * negative on any violation: short block, trailing bytes, zero-length
 * literal name, unknown static index. */
int bf_decode_header_block(const uint8_t *buf, size_t len,
                           struct bf_hdr *out, int max_out);

/* Append one header to a block being built. Uses the static table when
 * the name is in it. Returns bytes written, or -1 if it would overflow. */
int bf_encode_header_entry(uint8_t *buf, size_t cap,
                           const char *name, const char *value);

/* Blocking read of exactly n bytes. Returns 1 ok, 0 clean EOF, -1 error.
 * read(2) is allowed to return short and a protocol parser that ignores
 * that works on loopback and fails over a real network. */
int bf_read_exact(int fd, void *buf, size_t n);
int bf_write_all(int fd, const void *buf, size_t n);

#endif
