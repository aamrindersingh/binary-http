/* bframe.c - BHTTP/1 wire format, server side. See bframe.h. */

#include "bframe.h"

#include <errno.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

const char *const bf_static_table[BF_STATIC_COUNT + 1] = {
    NULL,              /* index 0 reserved, SPEC 4 */
    ":method",         /* 1  */
    ":path",           /* 2  */
    "host",            /* 3  */
    "user-agent",      /* 4  */
    "accept",          /* 5  */
    "content-length",  /* 6  */
    "content-type",    /* 7  */
    "server",          /* 8  */
    "date",            /* 9  */
    "connection"       /* 10 */
};

void bf_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

void bf_put_u24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v);
}

uint16_t bf_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] << 8 | p[1]);
}

uint32_t bf_get_u24(const uint8_t *p)
{
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

/* SPEC 2 layout: Length(24) Type(8) Flags(8) RequestID(24). */
void bf_encode_header(uint8_t out[BF_HEADER_LEN], const struct bf_header *h)
{
    bf_put_u24(out, h->length);
    out[3] = h->type;
    out[4] = h->flags;
    bf_put_u24(out + 5, h->request_id);
}

void bf_decode_header(const uint8_t in[BF_HEADER_LEN], struct bf_header *h)
{
    h->length     = bf_get_u24(in);
    h->type       = in[3];
    h->flags      = in[4];
    h->request_id = bf_get_u24(in + 5);
}

int bf_decode_header_block(const uint8_t *buf, size_t len,
                           struct bf_hdr *out, int max_out)
{
    if (len < 1)
        return -1;

    size_t off   = 0;
    int    count = buf[off++];

    if (count > max_out)
        return -1;

    for (int i = 0; i < count; i++) {
        if (off >= len)
            return -1; /* ran out before Count entries, SPEC 4 */

        uint8_t first = buf[off++];

        if (first & 0x80) {
            /* indexed name: 1iiiiiii */
            uint8_t idx = (uint8_t)(first & 0x7F);
            if (idx == 0 || idx > BF_STATIC_COUNT)
                return -1; /* reserved or unknown index, SPEC 4 */
            out[i].name     = bf_static_table[idx];
            out[i].name_len = strlen(bf_static_table[idx]);
        } else {
            /* literal name: 0lllllll */
            uint8_t nlen = (uint8_t)(first & 0x7F);
            if (nlen == 0)
                return -1; /* zero-length literal name, SPEC 4 */
            if (off + nlen > len)
                return -1;
            out[i].name     = (const char *)(buf + off);
            out[i].name_len = nlen;
            off += nlen;
        }

        if (off + 2 > len)
            return -1;
        uint16_t vlen = bf_get_u16(buf + off);
        off += 2;

        if (off + vlen > len)
            return -1;
        out[i].value     = (const char *)(buf + off);
        out[i].value_len = vlen;
        off += vlen;
    }

    /* SPEC 4: the block MUST be consumed exactly. Trailing bytes mean the
     * sender and I disagree about the encoding, and guessing which of us
     * is right is how parsers become attack surface. */
    if (off != len)
        return -1;

    return count;
}

int bf_encode_header_entry(uint8_t *buf, size_t cap,
                           const char *name, const char *value)
{
    size_t vlen = strlen(value);
    if (vlen > 0xFFFF)
        return -1;

    int idx = 0;
    for (int i = 1; i <= BF_STATIC_COUNT; i++) {
        if (strcasecmp(name, bf_static_table[i]) == 0) {
            idx = i;
            break;
        }
    }

    size_t need = (idx ? 1u : 1u + strlen(name)) + 2u + vlen;
    if (need > cap)
        return -1;

    size_t off = 0;
    if (idx) {
        buf[off++] = (uint8_t)(0x80 | idx);
    } else {
        size_t nlen = strlen(name);
        if (nlen == 0 || nlen > 127)
            return -1;
        buf[off++] = (uint8_t)nlen;
        memcpy(buf + off, name, nlen);
        off += nlen;
    }

    bf_put_u16(buf + off, (uint16_t)vlen);
    off += 2;
    memcpy(buf + off, value, vlen);
    off += vlen;

    return (int)off;
}

int bf_read_exact(int fd, void *buf, size_t n)
{
    uint8_t *p    = buf;
    size_t   done = 0;

    while (done < n) {
        ssize_t r = read(fd, p + done, n - done);
        if (r == 0)
            return done == 0 ? 0 : -1; /* EOF mid-frame is an error */
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)r;
    }
    return 1;
}

int bf_write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p    = buf;
    size_t         done = 0;

    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)w;
    }
    return 0;
}
