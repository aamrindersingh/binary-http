/* bserve - a BHTTP/1 file server.
 *
 *   ./bserve ./www 9000
 *
 * Accepts a TCP connection, reads binary request frames, maps the path to
 * a file under the document root, and replies. Keeps the connection open
 * until the peer goes away.
 */

#define _POSIX_C_SOURCE 200809L
/* INADDR_LOOPBACK lives outside strict POSIX on BSD and macOS. */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#endif

#include "bframe.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *g_root;      /* resolved absolute document root */
static int         g_verbose;
static int         g_inject_unknown; /* emit a 0x7E frame before each RESPONSE */

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---- content types ---------------------------------------------------- */

static const char *guess_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return "application/octet-stream";
    if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm")) return "text/html";
    if (!strcasecmp(dot, ".txt"))  return "text/plain";
    if (!strcasecmp(dot, ".css"))  return "text/css";
    if (!strcasecmp(dot, ".js"))   return "application/javascript";
    if (!strcasecmp(dot, ".json")) return "application/json";
    if (!strcasecmp(dot, ".png"))  return "image/png";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, ".svg"))  return "image/svg+xml";
    return "application/octet-stream";
}

/* ---- sending ---------------------------------------------------------- */

static int send_frame(int fd, uint8_t type, uint8_t flags, uint32_t id,
                      const uint8_t *payload, uint32_t len)
{
    uint8_t          hdr[BF_HEADER_LEN];
    struct bf_header h = { .length = len, .type = type,
                           .flags = flags, .request_id = id };

    bf_encode_header(hdr, &h);
    if (bf_write_all(fd, hdr, sizeof hdr) < 0)
        return -1;
    if (len && bf_write_all(fd, payload, len) < 0)
        return -1;
    return 0;
}

static int send_goaway(int fd, uint32_t last_id, uint8_t reason)
{
    uint8_t p[4];
    bf_put_u24(p, last_id);
    p[3] = reason;
    /* SPEC 3: the header Request ID of a GOAWAY MUST be 0. */
    return send_frame(fd, BF_T_GOAWAY, 0, 0, p, sizeof p);
}

/* Build and send a RESPONSE. body may be NULL. */
static int send_response(int fd, uint32_t id, uint16_t status,
                         const char *ctype, const uint8_t *body, size_t blen)
{
    uint8_t buf[BF_MAX_HEADER_BLOCK];
    size_t  off = 0;

    buf[off++] = 0; /* count, filled in once known */
    int count  = 0;

    char clen[32];
    snprintf(clen, sizeof clen, "%zu", blen);

    char date[64];
    time_t    now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &tm);

    struct { const char *n, *v; } hs[] = {
        { "content-type",   ctype ? ctype : "text/plain" },
        { "content-length", clen },
        { "server",         "bserve/1" },
        { "date",           date },
    };

    for (size_t i = 0; i < sizeof hs / sizeof hs[0]; i++) {
        int n = bf_encode_header_entry(buf + off, sizeof buf - off,
                                       hs[i].n, hs[i].v);
        if (n < 0)
            return -1;
        off += (size_t)n;
        count++;
    }
    buf[0] = (uint8_t)count;

    /* SPEC 6: Status(16) then the header block. */
    uint8_t payload[BF_MAX_HEADER_BLOCK + 2];
    bf_put_u16(payload, status);
    memcpy(payload + 2, buf, off);

    /* SPEC 8, exercised from this side: emit a frame type the client
     * cannot know about and carry on as if nothing happened. A conforming
     * client discards it and still sees a correct response. */
    if (g_inject_unknown) {
        static const uint8_t junk[] = { 0xC0, 0xFF, 0xEE };
        if (g_verbose)
            logf_("  -> type 0x7E (undefined), client must skip it");
        if (send_frame(fd, 0x7E, 0, id, junk, sizeof junk) < 0)
            return -1;
    }

    /* SPEC 6: a RESPONSE with no body sets END_MESSAGE on itself. */
    uint8_t flags = blen ? 0 : BF_FLAG_END_MESSAGE;
    if (send_frame(fd, BF_T_RESPONSE, flags, id, payload,
                   (uint32_t)(off + 2)) < 0)
        return -1;

    /* Body in DATA frames, chunked to the soft limit from SPEC 7. */
    size_t sent = 0;
    while (sent < blen) {
        size_t  chunk = blen - sent;
        if (chunk > BF_MAX_PAYLOAD_DEFAULT)
            chunk = BF_MAX_PAYLOAD_DEFAULT;
        uint8_t f = (sent + chunk >= blen) ? BF_FLAG_END_MESSAGE : 0;
        if (send_frame(fd, BF_T_DATA, f, id, body + sent, (uint32_t)chunk) < 0)
            return -1;
        sent += chunk;
    }
    return 0;
}

static int send_status(int fd, uint32_t id, uint16_t status)
{
    const char *msg;
    switch (status) {
    case BF_ST_BAD_REQUEST: msg = "400 bad request\n";  break;
    case BF_ST_FORBIDDEN:   msg = "403 forbidden\n";    break;
    case BF_ST_NOT_FOUND:   msg = "404 not found\n";    break;
    case BF_ST_BAD_METHOD:  msg = "405 method not allowed\n"; break;
    default:                msg = "500 server error\n"; break;
    }
    return send_response(fd, id, status, "text/plain",
                         (const uint8_t *)msg, strlen(msg));
}

/* ---- path safety ------------------------------------------------------ */

/* Resolve :path under the root and refuse anything that escapes it.
 * realpath() is the check that matters: string inspection for ".." is
 * defeated by symlinks, and by encodings the sender controls. */
static int resolve_path(const char *req_path, size_t len, char out[PATH_MAX])
{
    if (len == 0 || req_path[0] != '/')
        return -1;
    if (len > BF_MAX_PATH)
        return -1;
    if (memchr(req_path, '\0', len))
        return -1; /* embedded NUL: the C string and the frame would disagree */

    char rel[BF_MAX_PATH + 1];
    memcpy(rel, req_path, len);
    rel[len] = '\0';

    char joined[PATH_MAX];
    if (snprintf(joined, sizeof joined, "%s%s", g_root, rel) >= (int)sizeof joined)
        return -1;

    /* Directory request serves index.html, same as every other file server. */
    size_t jl = strlen(joined);
    if (jl && joined[jl - 1] == '/') {
        if (snprintf(joined + jl, sizeof joined - jl, "index.html")
            >= (int)(sizeof joined - jl))
            return -1;
    }

    char resolved[PATH_MAX];
    if (!realpath(joined, resolved))
        return -2; /* does not exist -> 404, distinct from -1 malformed */

    size_t rootlen = strlen(g_root);
    if (strncmp(resolved, g_root, rootlen) != 0 ||
        (resolved[rootlen] != '/' && resolved[rootlen] != '\0'))
        return -3; /* escaped the root -> 403 */

    memcpy(out, resolved, strlen(resolved) + 1);
    return 0;
}

/* ---- request handling -------------------------------------------------- */

static int handle_request(int fd, const struct bf_header *h,
                          const uint8_t *payload)
{
    if (h->request_id == 0) {
        /* SPEC 1: ID 0 is reserved for connection-level frames. */
        return send_status(fd, 0, BF_ST_BAD_REQUEST);
    }
    if (h->length < 1)
        return send_status(fd, h->request_id, BF_ST_BAD_REQUEST);

    uint8_t method = payload[0];

    struct bf_hdr hdrs[BF_MAX_HEADERS];
    int n = bf_decode_header_block(payload + 1, h->length - 1,
                                   hdrs, BF_MAX_HEADERS);
    if (n < 0)
        return send_status(fd, h->request_id, BF_ST_BAD_REQUEST);

    if (method != BF_METHOD_GET)
        return send_status(fd, h->request_id, BF_ST_BAD_METHOD);

    const char *path     = NULL;
    size_t      path_len = 0;
    for (int i = 0; i < n; i++) {
        if (hdrs[i].name_len == 5 && !strncasecmp(hdrs[i].name, ":path", 5)) {
            path     = hdrs[i].value;
            path_len = hdrs[i].value_len;
            break;
        }
    }
    if (!path)
        return send_status(fd, h->request_id, BF_ST_BAD_REQUEST);

    char full[PATH_MAX];
    int  rc = resolve_path(path, path_len, full);
    if (rc == -1) return send_status(fd, h->request_id, BF_ST_BAD_REQUEST);
    if (rc == -2) return send_status(fd, h->request_id, BF_ST_NOT_FOUND);
    if (rc == -3) return send_status(fd, h->request_id, BF_ST_FORBIDDEN);

    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
        return send_status(fd, h->request_id, BF_ST_NOT_FOUND);

    FILE *f = fopen(full, "rb");
    if (!f)
        return send_status(fd, h->request_id, BF_ST_SERVER_ERR);

    uint8_t *body = malloc((size_t)st.st_size ? (size_t)st.st_size : 1);
    if (!body) {
        fclose(f);
        return send_status(fd, h->request_id, BF_ST_SERVER_ERR);
    }
    size_t got = fread(body, 1, (size_t)st.st_size, f);
    fclose(f);

    if (got != (size_t)st.st_size) {
        free(body);
        return send_status(fd, h->request_id, BF_ST_SERVER_ERR);
    }

    if (g_verbose)
        logf_("  200 %.*s (%zu bytes)", (int)path_len, path, got);

    int r = send_response(fd, h->request_id, BF_ST_OK,
                          guess_type(full), body, got);
    free(body);
    return r;
}

static void serve_connection(int fd)
{
    uint32_t last_id = 0;

    for (;;) {
        uint8_t hbuf[BF_HEADER_LEN];
        int     r = bf_read_exact(fd, hbuf, sizeof hbuf);
        if (r == 0)
            break; /* peer closed cleanly between frames */
        if (r < 0) {
            logf_("  short read on frame header, closing");
            break;
        }

        struct bf_header h;
        bf_decode_header(hbuf, &h);

        /* SPEC 7: enforce the soft ceiling before allocating anything. The
         * whole point of a 24-bit length is that this check is cheap and
         * the worst case is bounded. */
        if (h.length > BF_MAX_PAYLOAD_DEFAULT) {
            logf_("  frame of %u bytes exceeds the %u limit, going away",
                  h.length, BF_MAX_PAYLOAD_DEFAULT);
            send_goaway(fd, last_id, BF_GO_TOO_LARGE);
            break;
        }

        uint8_t *payload = NULL;
        if (h.length) {
            payload = malloc(h.length);
            if (!payload)
                break;
            if (bf_read_exact(fd, payload, h.length) != 1) {
                free(payload);
                logf_("  truncated payload, closing");
                break;
            }
        }

        if (h.request_id > last_id)
            last_id = h.request_id;

        switch (h.type) {
        case BF_T_REQUEST:
            if (g_verbose)
                logf_("  <- REQUEST id=%u len=%u", h.request_id, h.length);
            if (handle_request(fd, &h, payload) < 0) {
                free(payload);
                return;
            }
            break;

        case BF_T_DATA:
            /* A request body. This server has no use for one, but reading
             * and discarding it keeps the stream in sync. */
            if (g_verbose)
                logf_("  <- DATA id=%u len=%u (ignored)", h.request_id, h.length);
            break;

        case BF_T_GOAWAY:
            if (g_verbose)
                logf_("  <- GOAWAY, closing");
            free(payload);
            return;

        default:
            /* ----------------------------------------------------------
             * SPEC 8, the rule that may not be skipped.
             *
             * The payload has already been read and will be freed below,
             * which is exactly "read and discard exactly Length bytes".
             * No error, no close, no reply. We simply carry on, and that
             * is what leaves room for a version 2.
             * ---------------------------------------------------------- */
            if (g_verbose)
                logf_("  <- unknown type 0x%02X len=%u, skipped per SPEC 8",
                      h.type, h.length);
            break;
        }

        free(payload);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <docroot> <port> [-v]\n", argv[0]);
        return 2;
    }
    for (int i = 3; i < argc; i++)
    {
        if (!strcmp(argv[i], "-v"))
            g_verbose = 1;
        else if (!strcmp(argv[i], "--inject-unknown"))
            g_inject_unknown = 1;
    }

    static char rootbuf[PATH_MAX];
    if (!realpath(argv[1], rootbuf)) {
        perror("docroot");
        return 2;
    }
    g_root = rootbuf;

    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "bad port\n");
        return 2;
    }

    /* A client that disappears mid-write would otherwise kill us. */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in a = { 0 };
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons((uint16_t)port);

    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(srv, 16) < 0) { perror("listen"); return 1; }

    logf_("bserve: root=%s port=%d", g_root, port);

    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }
        if (g_verbose)
            logf_("connection open");

        /* One process per connection. Keeps the per-connection state
         * machine simple, and the connection genuinely stays open for as
         * many requests as the client wants to send. */
        pid_t pid = fork();
        if (pid == 0) {
            close(srv);
            serve_connection(c);
            close(c);
            _exit(0);
        }
        close(c);
    }
    return 0;
}
