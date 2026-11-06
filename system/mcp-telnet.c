/*
 * Telnet sessions for MCP clients (see serial-connect in qapi/mcp.json)
 *
 * A small telnet client.  It answers the server's option requests: it
 * sends terminal type VT100 and lets the server echo and drop go-aheads;
 * everything else it turns down, so the server needs no reply it waits
 * for.  What the server sends goes into a CharTap with the telnet
 * commands taken out, like what a guest writes to a serial port.  What
 * is typed gets IAC doubled and a lone CR followed by NUL (RFC 854).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "io/channel-socket.h"
#include "qapi/error.h"
#include "system/mcp-telnet.h"

#define TELNET_OUT_MAX  65536   /* bytes queued for the socket */
#define TELNET_SB_MAX   64      /* longest subnegotiation kept */

enum {
    T_SE = 240, T_SB = 250, T_WILL = 251, T_WONT = 252, T_DO = 253,
    T_DONT = 254, T_IAC = 255,
};

enum { OPT_ECHO = 1, OPT_SGA = 3, OPT_TTYPE = 24 };
enum { TTYPE_IS = 0, TTYPE_SEND = 1 };

typedef enum McpTelnetRx {
    RX_DATA, RX_CR, RX_IAC, RX_OPT, RX_SB, RX_SB_IAC,
} McpTelnetRx;

struct McpTelnet {
    char *host, *port, *address;
    CharTap *tap;
    McpTelnetState state;
    char *reason;
    QIOChannelSocket *sioc;
    guint rd_watch, wr_watch;
    GByteArray *out;            /* waits for the socket */
    uint64_t received;
    McpTelnetRx rx;
    uint8_t cmd;                /* WILL, WONT, DO or DONT before an option */
    uint8_t sb[TELNET_SB_MAX];
    size_t sb_len;
    bool us[256];               /* options we do */
    bool him[256];              /* options the server does */
};

void mcp_telnet_set_address(McpTelnet *t, const char *host, const char *port)
{
    assert(t->state == MCP_TELNET_CLOSED);
    g_free(t->host);
    g_free(t->port);
    g_free(t->address);
    t->host = g_strdup(host);
    t->port = g_strdup(port);
    t->address = g_strdup_printf(strchr(host, ':') ? "[%s]:%s" : "%s:%s",
                                 host, port);
    g_clear_pointer(&t->reason, g_free);
}

McpTelnet *mcp_telnet_new(const char *host, const char *port, CharTap *tap)
{
    McpTelnet *t = g_new0(McpTelnet, 1);

    mcp_telnet_set_address(t, host, port);
    t->tap = tap;
    t->out = g_byte_array_new();
    return t;
}

const char *mcp_telnet_address(McpTelnet *t)
{
    return t->address;
}

bool mcp_telnet_matches(McpTelnet *t, const char *host, const char *port)
{
    return !strcmp(t->host, host) && !strcmp(t->port, port);
}

McpTelnetState mcp_telnet_state(McpTelnet *t)
{
    return t->state;
}

const char *mcp_telnet_reason(McpTelnet *t)
{
    return t->reason;
}

uint64_t mcp_telnet_received(McpTelnet *t)
{
    return t->received;
}

static gboolean mcp_telnet_writable(QIOChannel *ioc, GIOCondition cond,
                                    gpointer opaque);

static void mcp_telnet_flush(McpTelnet *t)
{
    while (t->state == MCP_TELNET_OPEN && t->out->len) {
        Error *err = NULL;
        ssize_t n = qio_channel_write(QIO_CHANNEL(t->sioc),
                                      (char *)t->out->data, t->out->len,
                                      &err);

        if (n == QIO_CHANNEL_ERR_BLOCK) {
            if (!t->wr_watch) {
                t->wr_watch = qio_channel_add_watch(QIO_CHANNEL(t->sioc),
                                                    G_IO_OUT,
                                                    mcp_telnet_writable, t,
                                                    NULL);
            }
            return;
        }
        if (n < 0) {
            g_autofree char *why = g_strdup_printf("sending failed: %s",
                                                   error_get_pretty(err));

            error_free(err);
            mcp_telnet_close(t, why);
            return;
        }
        g_byte_array_remove_range(t->out, 0, n);
    }
}

static gboolean mcp_telnet_writable(QIOChannel *ioc, GIOCondition cond,
                                    gpointer opaque)
{
    McpTelnet *t = opaque;

    t->wr_watch = 0;
    mcp_telnet_flush(t);
    return G_SOURCE_REMOVE;
}

static void mcp_telnet_send(McpTelnet *t, const uint8_t *buf, size_t len)
{
    g_byte_array_append(t->out, buf, len);
    mcp_telnet_flush(t);
}

static void mcp_telnet_reply(McpTelnet *t, uint8_t cmd, uint8_t opt)
{
    uint8_t r[3] = { T_IAC, cmd, opt };

    mcp_telnet_send(t, r, sizeof(r));
}

/*
 * Answer a request only when it changes something, so the two ends
 * cannot keep answering each other (RFC 854).
 */
static void mcp_telnet_option(McpTelnet *t, uint8_t cmd, uint8_t opt)
{
    switch (cmd) {
    case T_DO:
        if (!t->us[opt]) {
            t->us[opt] = opt == OPT_TTYPE || opt == OPT_SGA;
            mcp_telnet_reply(t, t->us[opt] ? T_WILL : T_WONT, opt);
        }
        break;
    case T_DONT:
        if (t->us[opt]) {
            t->us[opt] = false;
            mcp_telnet_reply(t, T_WONT, opt);
        }
        break;
    case T_WILL:
        if (!t->him[opt]) {
            t->him[opt] = opt == OPT_ECHO || opt == OPT_SGA;
            mcp_telnet_reply(t, t->him[opt] ? T_DO : T_DONT, opt);
        }
        break;
    case T_WONT:
        if (t->him[opt]) {
            t->him[opt] = false;
            mcp_telnet_reply(t, T_DONT, opt);
        }
        break;
    }
}

static void mcp_telnet_sub(McpTelnet *t)
{
    static const uint8_t ttype[] = {
        T_IAC, T_SB, OPT_TTYPE, TTYPE_IS, 'V', 'T', '1', '0', '0', T_IAC, T_SE
    };

    if (t->sb_len >= 2 && t->sb[0] == OPT_TTYPE && t->sb[1] == TTYPE_SEND &&
        t->us[OPT_TTYPE]) {
        mcp_telnet_send(t, ttype, sizeof(ttype));
    }
}

/* Take the telnet commands out of what came; the rest goes to the tap */
static void mcp_telnet_recv(McpTelnet *t, const uint8_t *buf, size_t len)
{
    g_autofree uint8_t *text = g_malloc(len);
    size_t n = 0, i;

    for (i = 0; i < len; i++) {
        uint8_t c = buf[i];

        switch (t->rx) {
        case RX_CR:
            t->rx = RX_DATA;
            if (c == 0) {
                break;          /* CR NUL is a lone CR */
            }
            /* fall through */
        case RX_DATA:
            if (c == T_IAC) {
                t->rx = RX_IAC;
                break;
            }
            text[n++] = c;
            if (c == '\r') {
                t->rx = RX_CR;
            }
            break;
        case RX_IAC:
            t->rx = RX_DATA;
            if (c == T_IAC) {
                text[n++] = c;
            } else if (c >= T_WILL) {
                t->cmd = c;
                t->rx = RX_OPT;
            } else if (c == T_SB) {
                t->sb_len = 0;
                t->rx = RX_SB;
            }
            /* NOP, GA, DM, AYT and the like need nothing */
            break;
        case RX_OPT:
            t->rx = RX_DATA;
            mcp_telnet_option(t, t->cmd, c);
            break;
        case RX_SB:
            if (c == T_IAC) {
                t->rx = RX_SB_IAC;
            } else if (t->sb_len < TELNET_SB_MAX) {
                t->sb[t->sb_len++] = c;
            }
            break;
        case RX_SB_IAC:
            if (c == T_IAC) {
                if (t->sb_len < TELNET_SB_MAX) {
                    t->sb[t->sb_len++] = c;
                }
                t->rx = RX_SB;
                break;
            }
            t->rx = RX_DATA;
            if (c == T_SE) {
                mcp_telnet_sub(t);
            }
            break;
        }
    }
    if (n) {
        qemu_chr_tap_append(t->tap, text, n);
    }
}

static gboolean mcp_telnet_readable(QIOChannel *ioc, GIOCondition cond,
                                    gpointer opaque)
{
    McpTelnet *t = opaque;
    uint8_t buf[4096];
    Error *err = NULL;
    ssize_t n = qio_channel_read(ioc, (char *)buf, sizeof(buf), &err);
    bool first;

    if (n == QIO_CHANNEL_ERR_BLOCK) {
        return G_SOURCE_CONTINUE;
    }
    if (n <= 0) {
        g_autofree char *why = n ? g_strdup_printf("the connection failed: "
                                                   "%s", error_get_pretty(err))
                                 : g_strdup("the other end closed it");

        error_free(err);
        t->rd_watch = 0;
        mcp_telnet_close(t, why);
        return G_SOURCE_REMOVE;
    }
    first = !t->received;
    t->received += n;
    mcp_telnet_recv(t, buf, n);
    if (first) {
        /* even if it was all telnet commands: the server is there */
        qemu_chr_tap_notify(t->tap);
    }
    if (t->state != MCP_TELNET_OPEN) {
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void mcp_telnet_connected(QIOTask *task, gpointer opaque)
{
    McpTelnet *t = opaque;
    QIOChannelSocket *sioc = QIO_CHANNEL_SOCKET(qio_task_get_source(task));
    Error *err = NULL;

    if (sioc != t->sioc) {
        return;                 /* given up on; the task drops the socket */
    }
    if (qio_task_propagate_error(task, &err)) {
        mcp_telnet_close(t, error_get_pretty(err));
        error_free(err);
        return;
    }
    qio_channel_set_blocking(QIO_CHANNEL(sioc), false, NULL);
    qio_channel_set_delay(QIO_CHANNEL(sioc), false);
    t->state = MCP_TELNET_OPEN;
    t->rd_watch = qio_channel_add_watch(QIO_CHANNEL(sioc),
                                        G_IO_IN | G_IO_HUP | G_IO_ERR,
                                        mcp_telnet_readable, t, NULL);
    qemu_chr_tap_notify(t->tap);
}

void mcp_telnet_open(McpTelnet *t)
{
    SocketAddress addr = {
        .type = SOCKET_ADDRESS_TYPE_INET,
        .u.inet.host = t->host,
        .u.inet.port = t->port,
    };

    if (t->state != MCP_TELNET_CLOSED) {
        return;
    }
    g_clear_pointer(&t->reason, g_free);
    t->state = MCP_TELNET_CONNECTING;
    t->received = 0;
    t->rx = RX_DATA;
    memset(t->us, 0, sizeof(t->us));
    memset(t->him, 0, sizeof(t->him));
    g_byte_array_set_size(t->out, 0);
    t->sioc = qio_channel_socket_new();
    qio_channel_set_name(QIO_CHANNEL(t->sioc), "mcp-telnet");
    qio_channel_socket_connect_async(t->sioc, &addr, mcp_telnet_connected, t,
                                     NULL, NULL);
}

void mcp_telnet_close(McpTelnet *t, const char *reason)
{
    if (t->state == MCP_TELNET_CLOSED) {
        return;
    }
    g_free(t->reason);
    t->reason = g_strdup(reason);
    if (t->rd_watch) {
        g_source_remove(t->rd_watch);
        t->rd_watch = 0;
    }
    if (t->wr_watch) {
        g_source_remove(t->wr_watch);
        t->wr_watch = 0;
    }
    if (t->state == MCP_TELNET_OPEN) {
        qio_channel_close(QIO_CHANNEL(t->sioc), NULL);
    }
    /* while connecting, the task keeps the socket until it is done */
    g_clear_pointer(&t->sioc, object_unref);
    t->state = MCP_TELNET_CLOSED;
    qemu_chr_tap_notify(t->tap);
}

int mcp_telnet_room(McpTelnet *t)
{
    switch (t->state) {
    case MCP_TELNET_OPEN:
        /* a typed byte can take two on the wire */
        return t->out->len < TELNET_OUT_MAX
            ? (TELNET_OUT_MAX - t->out->len) / 2 : 0;
    case MCP_TELNET_CONNECTING:
        return 0;
    default:
        return -1;
    }
}

void mcp_telnet_write(McpTelnet *t, const uint8_t *buf, size_t len, int next)
{
    static const uint8_t nul;
    size_t i;

    for (i = 0; i < len; i++) {
        int after = i + 1 < len ? buf[i + 1] : next;

        g_byte_array_append(t->out, &buf[i], 1);
        if (buf[i] == T_IAC) {
            g_byte_array_append(t->out, &buf[i], 1);
        } else if (buf[i] == '\r' && after != '\n') {
            g_byte_array_append(t->out, &nul, 1);
        }
    }
    mcp_telnet_flush(t);
}
