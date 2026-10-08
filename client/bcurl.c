/* bcurl - a BHTTP/1 client.
 *
 *   ./bcurl -v localhost:9000/index.html
 *
 * Builds a binary request frame, reads the response, writes the body to
 * stdout. -v hexdumps every frame in both directions. Exits non-zero on
 * 4xx and 5xx. Opens exactly one connection, ever.
 */

#define _POSIX_C_SOURCE 200809L

#include "wire.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int verbose;

/* The one connection. Guarded so "never open a second connection" is an
 * assertion in the code and not just a promise in the README. */
static int  conn_fd       = -1;
static int  connects_made = 0;

static int dial(const char *host, const char *port)
{
    if (connects_made > 0) {
        fprintf(stderr,
                "bcurl: refusing to open a second connection; the spec "
                "allows exactly one\n");
        exit(4);
    }

    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "bcurl: %s:%s: %s\n", host, port, gai_strerror(rc));
        exit(3);
    }

    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        fprintf(stderr, "bcurl: cannot connect to %s:%s\n", host, port);
        exit(3);
    }

    connects_made++;
    conn_fd = fd;
    return fd;
}

/* host[:port]/path  ->  three pieces */
static void split_url(const char *url, char *host, size_t hostsz,
                      char *port, size_t portsz,
                      char *path, size_t pathsz)
{
    if (!strncmp(url, "bhttp://", 8)) url += 8;
    else if (!strncmp(url, "http://", 7)) url += 7;

    const char *slash = strchr(url, '/');
    const char *hp_end = slash ? slash : url + strlen(url);

    const char *colon = memchr(url, ':', (size_t)(hp_end - url));

    size_t hlen = (size_t)((colon ? colon : hp_end) - url);
    if (hlen >= hostsz) hlen = hostsz - 1;
    memcpy(host, url, hlen);
    host[hlen] = '\0';

    if (colon) {
        size_t plen = (size_t)(hp_end - colon - 1);
        if (plen >= portsz) plen = portsz - 1;
        memcpy(port, colon + 1, plen);
        port[plen] = '\0';
    } else {
        snprintf(port, portsz, "9000");
    }

    snprintf(path, pathsz, "%s", slash ? slash : "/");
}

struct hdr_print { int printed; };

static void on_header(void *ctx, const char *n, size_t nl,
                      const char *v, size_t vl)
{
    struct hdr_print *hp = ctx;
    if (!hp->printed) {
        fprintf(stderr, "  headers:\n");
        hp->printed = 1;
    }
    fprintf(stderr, "    %.*s: %.*s\n", (int)nl, n, (int)vl, v);
}

static const char *type_name(uint8_t t)
{
    switch (t) {
    case FRAME_REQUEST:  return "REQUEST";
    case FRAME_RESPONSE: return "RESPONSE";
    case FRAME_DATA:     return "DATA";
    case FRAME_GOAWAY:   return "GOAWAY";
    default:             return "UNKNOWN";
    }
}

int main(int argc, char **argv)
{
    const char *url          = NULL;
    int         send_unknown = 0;
    int         repeat       = 1; /* requests to send on the one connection */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        /* Sends a type 0x7E frame before the request, to prove the peer
         * honours SPEC 8. Used by the conformance suite. */
        else if (!strcmp(argv[i], "--send-unknown-frame")) send_unknown = 1;
        /* -n N sends N requests down the SAME connection. Proves the
         * server keeps it open, and proves we never dial twice. */
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) repeat = atoi(argv[++i]);
        else url = argv[i];
    }

    if (!url) {
        fprintf(stderr,
                "usage: bcurl [-v] [-n count] [--send-unknown-frame] "
                "host:port/path\n");
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);

    char host[256], port[16], path[1100];
    split_url(url, host, sizeof host, port, sizeof port, path, sizeof path);

    int fd = dial(host, port);

    char hostport[300];
    snprintf(hostport, sizeof hostport, "%s:%s", host, port);

    /* ---- optional: an unknown frame type, SPEC 8 ---- */
    if (send_unknown) {
        uint8_t   junk[]  = { 0xDE, 0xAD, 0xBE, 0xEF };
        uint8_t   hb[WIRE_HDR_SIZE];
        frame_hdr uh = { .payload_len = sizeof junk, .frame_type = 0x7E,
                         .frame_flags = 0, .req_id = 0 };
        hdr_write(hb, &uh);
        if (verbose) {
            fprintf(stderr, "> frame type 0x7E (undefined), expecting the "
                            "server to skip it per SPEC 8\n");
            hexdump(">  header", hb, sizeof hb);
            hexdump(">  payload", junk, sizeof junk);
        }
        io_write_n(fd, hb, sizeof hb);
        io_write_n(fd, junk, sizeof junk);
    }

    int exit_code = 0;

    /* One iteration per request, all down the same fd. SPEC 1: IDs start
     * at 1 and increase monotonically. */
    for (uint32_t req_id = 1; req_id <= (uint32_t)repeat; req_id++) {

    /* ---- build the REQUEST, SPEC 5 ---- */
    uint8_t payload[WIRE_HBLOCK_MAX + 1];
    payload[0] = METHOD_GET;

    kv items[] = {
        { ":path",      path     },
        { "host",       hostport },
        { "user-agent", "bcurl/1" },
        { "accept",     "*/*"    },
    };
    int hb_len = hblock_build(payload + 1, sizeof payload - 1,
                              items, (int)(sizeof items / sizeof items[0]));
    if (hb_len < 0) {
        fprintf(stderr, "bcurl: request headers too large\n");
        return 2;
    }
    uint32_t plen = (uint32_t)hb_len + 1u;

    uint8_t   reqhdr[WIRE_HDR_SIZE];
    frame_hdr rh = { .payload_len = plen, .frame_type = FRAME_REQUEST,
                     .frame_flags = FLAG_END_MESSAGE, .req_id = req_id };
    hdr_write(reqhdr, &rh);

    if (verbose) {
        fprintf(stderr, "> REQUEST id=%u END_MESSAGE  GET %s\n",
                req_id, path);
        hexdump(">  header", reqhdr, sizeof reqhdr);
        hexdump(">  payload", payload, plen);
    }

    if (!io_write_n(fd, reqhdr, sizeof reqhdr) ||
        !io_write_n(fd, payload, plen)) {
        fprintf(stderr, "bcurl: write failed\n");
        return 3;
    }

    /* ---- read frames until END_MESSAGE for our id ---- */
    uint16_t status = 0;
    int      done   = 0;

    while (!done) {
        uint8_t hbuf[WIRE_HDR_SIZE];
        bool    eof = false;
        if (!io_read_n(fd, hbuf, sizeof hbuf, &eof)) {
            if (eof) fprintf(stderr, "bcurl: server closed the connection\n");
            else     fprintf(stderr, "bcurl: short read on frame header\n");
            return 3;
        }

        frame_hdr h;
        hdr_read(hbuf, &h);

        if (verbose) {
            fprintf(stderr, "< %s id=%u len=%u flags=0x%02x\n",
                    type_name(h.frame_type), h.req_id,
                    h.payload_len, h.frame_flags);
            hexdump("<  header", hbuf, sizeof hbuf);
        }

        /* SPEC 7: bound the allocation before making it. */
        if (h.payload_len > WIRE_SOFT_MAX) {
            fprintf(stderr,
                    "bcurl: frame of %u bytes exceeds the %u limit\n",
                    h.payload_len, WIRE_SOFT_MAX);
            return 3;
        }

        uint8_t *pl = NULL;
        if (h.payload_len) {
            pl = malloc(h.payload_len);
            if (!pl) return 3;
            if (!io_read_n(fd, pl, h.payload_len, NULL)) {
                free(pl);
                fprintf(stderr, "bcurl: truncated payload\n");
                return 3;
            }
            if (verbose) hexdump("<  payload", pl, h.payload_len);
        }

        switch (h.frame_type) {
        case FRAME_RESPONSE: {
            if (h.payload_len < 2) {
                fprintf(stderr, "bcurl: RESPONSE shorter than its status\n");
                free(pl);
                return 3;
            }
            status = rd_u16(pl);
            if (verbose) fprintf(stderr, "  status: %u\n", status);

            struct hdr_print hp = { 0 };
            if (hblock_walk(pl + 2, h.payload_len - 2u,
                            verbose ? on_header : NULL, &hp) < 0) {
                fprintf(stderr, "bcurl: malformed header block\n");
                free(pl);
                return 3;
            }

            /* SPEC 6: 2xx succeeded, 4xx our fault, 5xx theirs. Judge by
             * the leading digit so an unknown code still routes right. */
            if (status / 100 == 4 || status / 100 == 5)
                exit_code = 1;

            if (h.frame_flags & FLAG_END_MESSAGE) done = 1;
            break;
        }

        case FRAME_DATA:
            if (pl) fwrite(pl, 1, h.payload_len, stdout);
            /* SPEC 6: END_MESSAGE is authoritative, content-length is not. */
            if (h.frame_flags & FLAG_END_MESSAGE) done = 1;
            break;

        case FRAME_GOAWAY:
            if (verbose && h.payload_len >= 4)
                fprintf(stderr, "  last id=%u reason=%u\n",
                        rd_u24(pl), pl[3]);
            fprintf(stderr, "bcurl: server sent GOAWAY\n");
            free(pl);
            return 3;

        default:
            /* ----------------------------------------------------------
             * SPEC 8. The payload was read above and is freed below, so
             * exactly Length bytes have been discarded. No error, no
             * close, no reply: we keep reading the next frame.
             * ---------------------------------------------------------- */
            if (verbose)
                fprintf(stderr,
                        "  unknown type 0x%02X, skipped %u bytes per SPEC 8\n",
                        h.frame_type, h.payload_len);
            break;
        }

        free(pl);
    }
    fflush(stdout);

    } /* end per-request loop */

    close(fd);
    if (verbose)
        fprintf(stderr,
                "bcurl: %d request(s) over %d connection\n",
                repeat, connects_made);

    if (exit_code && verbose)
        fprintf(stderr, "bcurl: exiting %d, a response carried 4xx or 5xx\n",
                exit_code);
    return exit_code;
}
