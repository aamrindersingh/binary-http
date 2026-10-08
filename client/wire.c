/* wire.c - BHTTP/1 wire format, client side. See wire.h. */

#define _POSIX_C_SOURCE 200809L

#include "wire.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

const char *const static_table[STATIC_TABLE_LEN + 1] = {
    NULL,
    ":method", ":path", "host", "user-agent", "accept",
    "content-length", "content-type", "server", "date", "connection"
};

void wr_u16(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v >> 8);
    dst[1] = (uint8_t)(v & 0xFF);
}

void wr_u24(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)((v >> 16) & 0xFF);
    dst[1] = (uint8_t)((v >> 8) & 0xFF);
    dst[2] = (uint8_t)(v & 0xFF);
}

uint16_t rd_u16(const uint8_t *src)
{
    return (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
}

uint32_t rd_u24(const uint8_t *src)
{
    return ((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | src[2];
}

void hdr_write(uint8_t dst[WIRE_HDR_SIZE], const frame_hdr *h)
{
    wr_u24(&dst[0], h->payload_len);
    dst[3] = h->frame_type;
    dst[4] = h->frame_flags;
    wr_u24(&dst[5], h->req_id);
}

void hdr_read(const uint8_t src[WIRE_HDR_SIZE], frame_hdr *h)
{
    h->payload_len = rd_u24(&src[0]);
    h->frame_type  = src[3];
    h->frame_flags = src[4];
    h->req_id      = rd_u24(&src[5]);
}

static int table_lookup(const char *name)
{
    for (int i = 1; i <= STATIC_TABLE_LEN; i++)
        if (strcasecmp(name, static_table[i]) == 0)
            return i;
    return 0;
}

int hblock_build(uint8_t *dst, size_t cap, const kv *items, int n)
{
    if (n < 0 || n > 255 || cap < 1)
        return -1;

    size_t pos = 0;
    dst[pos++] = (uint8_t)n;

    for (int i = 0; i < n; i++) {
        size_t vlen = strlen(items[i].value);
        if (vlen > 0xFFFF)
            return -1;

        int idx = table_lookup(items[i].name);
        if (idx > 0) {
            if (pos + 1 > cap) return -1;
            dst[pos++] = (uint8_t)(0x80u | (unsigned)idx);
        } else {
            size_t nlen = strlen(items[i].name);
            if (nlen == 0 || nlen > 127) return -1;
            if (pos + 1 + nlen > cap) return -1;
            dst[pos++] = (uint8_t)nlen;
            memcpy(&dst[pos], items[i].name, nlen);
            pos += nlen;
        }

        if (pos + 2 + vlen > cap) return -1;
        wr_u16(&dst[pos], (uint16_t)vlen);
        pos += 2;
        memcpy(&dst[pos], items[i].value, vlen);
        pos += vlen;
    }
    return (int)pos;
}

int hblock_walk(const uint8_t *src, size_t len, hblock_cb cb, void *ctx)
{
    if (len < 1)
        return -1;

    const uint8_t *cur = src;
    const uint8_t *end = src + len;

    unsigned remaining = *cur++;

    while (remaining--) {
        if (cur >= end)
            return -1; /* fewer bytes than Count promised, SPEC 4 */

        uint8_t lead = *cur++;
        const char *nm;
        size_t      nlen;

        if (lead & 0x80u) {
            unsigned idx = lead & 0x7Fu;
            if (idx == 0 || idx > STATIC_TABLE_LEN)
                return -1; /* SPEC 4: unknown index is not skippable */
            nm   = static_table[idx];
            nlen = strlen(nm);
        } else {
            nlen = lead & 0x7Fu;
            if (nlen == 0)
                return -1; /* SPEC 4 */
            if ((size_t)(end - cur) < nlen)
                return -1;
            nm = (const char *)cur;
            cur += nlen;
        }

        if ((size_t)(end - cur) < 2)
            return -1;
        uint16_t vlen = rd_u16(cur);
        cur += 2;

        if ((size_t)(end - cur) < vlen)
            return -1;

        if (cb)
            cb(ctx, nm, nlen, (const char *)cur, vlen);
        cur += vlen;
    }

    /* SPEC 4: consumed exactly, no trailing bytes. */
    return (cur == end) ? 0 : -1;
}

bool io_read_n(int fd, void *dst, size_t n, bool *eof)
{
    uint8_t *p    = dst;
    size_t   have = 0;

    if (eof) *eof = false;

    while (have < n) {
        ssize_t got = read(fd, p + have, n - have);
        if (got == 0) {
            if (eof && have == 0) *eof = true;
            return false;
        }
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        have += (size_t)got;
    }
    return true;
}

bool io_write_n(int fd, const void *src, size_t n)
{
    const uint8_t *p    = src;
    size_t         sent = 0;

    while (sent < n) {
        ssize_t w = write(fd, p + sent, n - sent);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return false;
        }
        sent += (size_t)w;
    }
    return true;
}

void hexdump(const char *label, const uint8_t *buf, size_t len)
{
    fprintf(stderr, "%s (%zu bytes)\n", label, len);

    for (size_t off = 0; off < len; off += 16) {
        fprintf(stderr, "  %04zx  ", off);

        for (size_t i = 0; i < 16; i++) {
            if (off + i < len) fprintf(stderr, "%02x ", buf[off + i]);
            else               fputs("   ", stderr);
            if (i == 7) fputc(' ', stderr);
        }

        fputs(" |", stderr);
        for (size_t i = 0; i < 16 && off + i < len; i++) {
            int c = buf[off + i];
            fputc(isprint(c) ? c : '.', stderr);
        }
        fputs("|\n", stderr);
    }
}
