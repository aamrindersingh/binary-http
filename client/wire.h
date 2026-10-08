/* wire.h - BHTTP/1 wire format, client side.
 *
 * Written from SPEC.md, not from the server's bframe.h. The naming,
 * structure and decoding strategy differ on purpose: the server decodes
 * a header block into an array up front, this one walks it with a cursor
 * and a callback. If both sides had been copied from one file the
 * exercise would prove nothing.
 *
 * Nothing here is shared with ../server/. There is no common include
 * path and no shared object file; see the Makefile.
 */
#ifndef WIRE_H
#define WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* SPEC 2 */
#define WIRE_HDR_SIZE 8

/* SPEC 7 */
#define WIRE_SOFT_MAX   16384u
#define WIRE_HARD_MAX   0xFFFFFFu
#define WIRE_HBLOCK_MAX 8192u

/* SPEC 3 */
#define FRAME_REQUEST  0x01
#define FRAME_RESPONSE 0x02
#define FRAME_DATA     0x03
#define FRAME_GOAWAY   0x04

#define FLAG_END_MESSAGE 0x01

/* SPEC 1: IDs run 1..WIRE_MAX_REQ_ID and then wrap to 1. 0 is reserved
 * for connection-level frames, so it is never a request ID. */
#define WIRE_MAX_REQ_ID 0xFFFFFFu

/* SPEC 5 */
#define METHOD_GET 1

/* SPEC 4: the same ten names, in the same order, because the table is
 * part of the wire format rather than an implementation detail. */
#define STATIC_TABLE_LEN 10
extern const char *const static_table[STATIC_TABLE_LEN + 1];

typedef struct {
    uint32_t payload_len; /* 24 bits */
    uint8_t  frame_type;
    uint8_t  frame_flags;
    uint32_t req_id;      /* 24 bits */
} frame_hdr;

/* Big-endian accessors. */
void     wr_u16(uint8_t *dst, uint16_t v);
void     wr_u24(uint8_t *dst, uint32_t v);
uint16_t rd_u16(const uint8_t *src);
uint32_t rd_u24(const uint8_t *src);

void hdr_write(uint8_t dst[WIRE_HDR_SIZE], const frame_hdr *h);
void hdr_read(const uint8_t src[WIRE_HDR_SIZE], frame_hdr *h);

/* Build a header block into dst. Returns total bytes, or -1. */
typedef struct { const char *name; const char *value; } kv;
int hblock_build(uint8_t *dst, size_t cap, const kv *items, int n);

/* Walk a header block, invoking cb per entry. Returns 0 ok, -1 on any
 * violation from SPEC 4. The cursor style means a malformed block stops
 * at the exact byte that broke, which makes -v output more useful. */
typedef void (*hblock_cb)(void *ctx,
                          const char *name, size_t nlen,
                          const char *val,  size_t vlen);
int hblock_walk(const uint8_t *src, size_t len, hblock_cb cb, void *ctx);

/* Exact-length socket IO. */
bool io_read_n(int fd, void *dst, size_t n, bool *eof);
bool io_write_n(int fd, const void *src, size_t n);

/* -v output. Hexdump with an offset column and an ASCII gutter. */
void hexdump(const char *label, const uint8_t *buf, size_t len);

#endif
