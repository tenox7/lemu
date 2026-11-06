/*
 * Serial ports for MCP clients (see qapi/mcp.json)
 *
 * Every chardev frontend of a device gets a tap once the machine is up,
 * so what the guest writes to its serial ports can be read back without
 * changing the command line, even behind mon:stdio.  Input goes straight
 * to the device's own read handlers, past any mux.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "chardev/char-tap.h"
#include "chardev/char.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/qdev.h"
#include "monitor/mcp-common.h"
#include "monitor/mcp.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-mcp.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "system/mcp-serial.h"
#include "system/runstate.h"
#include "system/system.h"
#include "ui/input.h"

#define SERIAL_TAP_SIZE     (1024 * 1024)
#define SERIAL_READ_MAX     16384
#define SERIAL_STALL_MS     10000
#define SERIAL_SCAN_MS      20      /* let output gather between scans */
#define SERIAL_RUN_LINES    200
#define SERIAL_RUN_BYTES    32768
#define SERIAL_LINE_LIMIT   255
#define SERIAL_QUIET_MS     100     /* before serial-run types */
#define SERIAL_QUIET_MAX_MS 2000

typedef struct McpSerialPort {
    CharFrontend *fe;
    CharTap *tap;
    uint64_t mark;              /* where the last read or expect stopped */
    bool running;               /* serial-run is typing or waiting */
    char *pending;              /* end of a command serial-run left running */
    uint64_t pending_from;      /* where to look for it */
    Notifier io;                /* sees the guest's output */
    int64_t last_io_ms;         /* last input or output */
} McpSerialPort;

static GPtrArray *serial_ports;
static Notifier serial_init_done;

static gint mcp_serial_cmp(gconstpointer a, gconstpointer b)
{
    const McpSerialPort *pa = *(McpSerialPort * const *)a;
    const McpSerialPort *pb = *(McpSerialPort * const *)b;

    return g_strcmp0(qemu_chr_tap_name(pa->tap), qemu_chr_tap_name(pb->tap));
}

static void mcp_serial_io(Notifier *n, void *data)
{
    McpSerialPort *port = container_of(n, McpSerialPort, io);

    port->last_io_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
}

static void mcp_serial_add(CharFrontend *fe)
{
    g_autofree char *name = NULL;
    McpSerialPort *port;
    int i;

    for (i = 0; serial_hd(i); i++) {
        if (serial_hd(i) == fe->chr) {
            name = g_strdup_printf("serial%d", i);
            break;
        }
    }
    if (!name) {
        name = g_strdup(fe->chr->label);
    }
    port = g_new0(McpSerialPort, 1);
    port->fe = fe;
    port->tap = qemu_chr_tap_new(name, SERIAL_TAP_SIZE);
    port->io.notify = mcp_serial_io;
    qemu_chr_tap_add_notifier(port->tap, &port->io);
    qemu_chr_fe_set_tap(fe, port->tap);
    g_ptr_array_add(serial_ports, port);
}

static int mcp_serial_scan(Object *obj, void *opaque)
{
    ObjectClass *oc;

    if (!object_dynamic_cast(obj, TYPE_DEVICE)) {
        return 0;
    }
    for (oc = object_get_class(obj);
         oc && object_class_dynamic_cast(oc, TYPE_DEVICE);
         oc = object_class_get_parent(oc)) {
        DeviceClass *dc = DEVICE_CLASS(oc);
        int i;

        for (i = 0; i < dc->props_count_; i++) {
            const Property *prop = &dc->props_[i];
            CharFrontend *fe;

            if (prop->info != &qdev_prop_chr) {
                continue;
            }
            fe = object_field_prop_ptr(obj, prop);
            if (fe->chr && !fe->tap) {
                mcp_serial_add(fe);
            }
        }
    }
    return 0;
}

static void mcp_serial_machine_done(Notifier *n, void *data)
{
    object_child_foreach_recursive(object_get_root(), mcp_serial_scan, NULL);
    g_ptr_array_sort(serial_ports, mcp_serial_cmp);
}

void mcp_serial_init(void)
{
    serial_ports = g_ptr_array_new();
    serial_init_done.notify = mcp_serial_machine_done;
    qemu_add_machine_init_done_notifier(&serial_init_done);
}

void mcp_serial_list(strList **tail)
{
    int i;

    for (i = 0; serial_ports && i < serial_ports->len; i++) {
        McpSerialPort *port = g_ptr_array_index(serial_ports, i);
        Chardev *chr = port->fe->chr;
        g_autofree char *backend = NULL;

        backend = chr ? g_strdup_printf("%s (%s)", chr->label,
                                        object_get_typename(OBJECT(chr)))
                      : g_strdup("none");
        QAPI_LIST_APPEND(tail, g_strdup_printf("%s: %s",
                                               qemu_chr_tap_name(port->tap),
                                               backend));
    }
}

static McpSerialPort *mcp_serial_find(const char *name, Error **errp)
{
    g_autoptr(GString) names = g_string_new(NULL);
    int i;

    name = name ? name : "serial0";
    for (i = 0; serial_ports && i < serial_ports->len; i++) {
        McpSerialPort *port = g_ptr_array_index(serial_ports, i);

        if (!strcmp(qemu_chr_tap_name(port->tap), name)) {
            return port;
        }
        g_string_append_printf(names, "%s%s", i ? ", " : "",
                               qemu_chr_tap_name(port->tap));
    }
    error_setg(errp, "no serial port '%s'; ports: %s", name,
               names->len ? names->str : "none");
    return NULL;
}

/*
 * Turn raw serial bytes into UTF-8 text.  @map[i] is the raw offset
 * (from @raw) just after text byte i, so a match can be turned back
 * into a cursor.
 */
static GString *mcp_serial_text(const uint8_t *raw, size_t len, bool strip,
                                GArray *map)
{
    GString *s = g_string_sized_new(len);
    size_t i = 0;

#define EMIT(c, next) do {                                          \
        uint32_t end_ = (next);                                     \
        g_string_append_c(s, (c));                                  \
        if (map) {                                                  \
            g_array_append_val(map, end_);                          \
        }                                                           \
    } while (0)

    while (i < len) {
        uint8_t c = raw[i];

        if (strip && c == 0x1b) {
            /* CSI: ESC [ params final; OSC: ESC ] ... BEL or ESC \ */
            size_t j = i + 1;

            if (j < len && raw[j] == '[') {
                for (j++; j < len && (raw[j] < 0x40 || raw[j] > 0x7e); j++) {
                    /* parameters */
                }
                i = MIN(j + 1, len);
            } else if (j < len && raw[j] == ']') {
                for (j++; j < len && raw[j] != 0x07 &&
                     !(raw[j] == 0x1b && j + 1 < len && raw[j + 1] == '\\');
                     j++) {
                    /* text */
                }
                i = MIN(j + (j < len && raw[j] == 0x1b ? 2 : 1), len);
            } else {
                i = MIN(j + 1, len);
            }
            continue;
        }
        if (strip && (c == '\r' || c == 0 || c == 0x7f ||
                      (c < 0x20 && c != '\n' && c != '\t' && c != '\b'))) {
            i++;
            continue;
        }
        if (c < 0x80) {
            EMIT(c, i + 1);
            i++;
        } else {
            const gchar *end;
            size_t n = MIN(len - i, 4);

            /* keep a valid UTF-8 sequence, else show U+FFFD */
            g_utf8_validate((const gchar *)raw + i, n, &end);
            n = (const uint8_t *)end - (raw + i);
            /* invalid input gives (gunichar)-1 or -2, above U+10FFFF */
            if (n && g_utf8_get_char_validated((const gchar *)raw + i, n)
                     <= 0x10ffff) {
                size_t k;

                for (k = 0; k < n; k++) {
                    EMIT(raw[i + k], i + n);
                }
                i += n;
            } else {
                EMIT(0xef, i + 1);
                EMIT(0xbf, i + 1);
                EMIT(0xbd, i + 1);
                i++;
            }
        }
    }
#undef EMIT
    return s;
}

McpSerialResult *qmp_serial_read(const char *port_name, bool has_cursor,
                                 int64_t cursor, bool has_max_bytes,
                                 int64_t max_bytes, bool has_strip_ansi,
                                 bool strip_ansi, Error **errp)
{
    McpSerialPort *port = mcp_serial_find(port_name, errp);
    g_autofree uint8_t *buf = NULL;
    g_autoptr(GString) text = NULL;
    McpSerialResult *res;
    uint64_t from, actual, start, end;
    size_t n, max;

    if (!port) {
        return NULL;
    }
    max = has_max_bytes ? MIN(MAX(max_bytes, 1), SERIAL_READ_MAX) : 4096;
    from = has_cursor ? MAX(cursor, 0) : port->mark;
    qemu_chr_tap_range(port->tap, &start, &end);
    buf = g_malloc(max);
    n = qemu_chr_tap_read(port->tap, from, buf, max, &actual);
    text = mcp_serial_text(buf, n, has_strip_ansi ? strip_ansi : true, NULL);
    port->mark = actual + n;

    res = g_new0(McpSerialResult, 1);
    res->port = g_strdup(qemu_chr_tap_name(port->tap));
    res->start = actual;
    res->cursor = actual + n;
    if (actual > from) {
        res->has_lost = true;
        res->lost = actual - from;
    }
    res->summary = g_strdup_printf("%s: %zu bytes from %" PRIu64
                                   "%s, cursor %" PRIu64 "%s%s",
                                   res->port, n, actual,
                                   actual + n < end ? " (more to read)" : "",
                                   actual + n,
                                   n ? ":\n" : "", text->str);
    return res;
}

/* The input context: the MCP call's, or @local for a QMP caller */
static QemuInputCtx *mcp_serial_ctx(QemuInputCtx *local)
{
    QemuInputCtx *ctx = mcp_current_input_ctx();

    if (ctx) {
        return ctx;
    }
    qemu_input_ctx_init(local, NULL);
    return local;
}

static void coroutine_fn mcp_serial_sleep(QemuInputCtx *ctx, int64_t ms)
{
    qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME, ms * SCALE_MS);
}

/* Only a running guest reads what is typed */
static bool mcp_serial_running(Error **errp)
{
    if (!runstate_is_running()) {
        error_setg(errp, "VM is paused (state %s); resume it first",
                   RunState_str(runstate_get()));
        return false;
    }
    return true;
}

/*
 * Type @data into @port as fast as the device takes it, or @delay ms
 * apart.  With @finish, a cancel does not stop it halfway, which would
 * leave part of a command line in the guest.
 */
static bool coroutine_fn mcp_serial_send(McpSerialPort *port,
                                         QemuInputCtx *ctx, GString *data,
                                         int64_t delay, bool finish,
                                         Error **errp)
{
    int64_t last_progress = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    size_t pos = 0;

    while (pos < data->len) {
        CharFrontend *fe = port->fe;
        int n = fe->chr_can_read ? fe->chr_can_read(fe->opaque) : 0;

        if (ctx->cancelled && !finish) {
            error_setg(errp, "cancelled after %zu of %zu bytes", pos,
                       data->len);
            return false;
        }
        if (n <= 0) {
            if (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - last_progress >
                SERIAL_STALL_MS) {
                error_setg(errp, "%s took only %zu of %zu bytes; the guest "
                           "is not reading the port",
                           qemu_chr_tap_name(port->tap), pos, data->len);
                return false;
            }
            mcp_serial_sleep(ctx, 5);
            continue;
        }
        n = delay ? 1 : MIN(n, data->len - pos);
        fe->chr_read(fe->opaque, (uint8_t *)data->str + pos, n);
        pos += n;
        last_progress = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        port->last_io_ms = last_progress;
        if (delay && pos < data->len) {
            mcp_serial_sleep(ctx, delay);
        }
    }
    return true;
}

McpResult *coroutine_fn qmp_serial_write(const char *port_name,
                                         const char *text, bool has_escapes,
                                         bool escapes, bool has_char_delay_ms,
                                         int64_t char_delay_ms, Error **errp)
{
    McpSerialPort *port = mcp_serial_find(port_name, errp);
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_serial_ctx(&local);
    g_autoptr(GString) data = NULL;
    McpResult *res;

    if (!port || !mcp_serial_running(errp)) {
        return NULL;
    }
    /* typing by hand: serial-run no longer waits for its last command */
    g_clear_pointer(&port->pending, g_free);
    data = (has_escapes && !escapes) ? g_string_new(text)
                                     : mcp_unescape(text);
    if (!mcp_serial_send(port, ctx, data,
                         has_char_delay_ms ? MAX(char_delay_ms, 0) : 0,
                         false, errp)) {
        return NULL;
    }

    res = g_new0(McpResult, 1);
    res->summary = g_strdup_printf("sent %zu bytes to %s", data->len,
                                   qemu_chr_tap_name(port->tap));
    return res;
}

typedef struct McpSerialWaiter {
    Notifier notifier;
    QemuInputCtx *ctx;
} McpSerialWaiter;

static void mcp_serial_wake(Notifier *n, void *data)
{
    McpSerialWaiter *w = container_of(n, McpSerialWaiter, notifier);

    qemu_co_sleep_wake(&w->ctx->sleep);
}

/* The last @max bytes of @s, cut at a UTF-8 character boundary */
static const char *mcp_serial_tail(GString *s, size_t max)
{
    const char *p = s->str;

    if (s->len > max) {
        p = g_utf8_find_next_char(s->str + s->len - max - 1, NULL);
    }
    return p ? p : "";
}

/* A port's output from some offset on, as text (see mcp_serial_text) */
typedef struct McpSerialScan {
    uint8_t *buf;
    GString *text;
    GArray *map;
    uint64_t start;             /* offset of the first byte read */
    size_t len;                 /* bytes read */
} McpSerialScan;

static void mcp_serial_scan_init(McpSerialScan *s)
{
    s->buf = g_malloc(SERIAL_TAP_SIZE);
    s->text = g_string_new(NULL);
    s->map = g_array_new(false, false, sizeof(uint32_t));
    s->start = 0;
    s->len = 0;
}

static void mcp_serial_scan_free(McpSerialScan *s)
{
    g_free(s->buf);
    g_string_free(s->text, true);
    g_array_free(s->map, true);
}

/* Read everything from @from on, again */
static void mcp_serial_scan_read(McpSerialPort *port, uint64_t from,
                                 McpSerialScan *s)
{
    s->len = qemu_chr_tap_read(port->tap, from, s->buf, SERIAL_TAP_SIZE,
                               &s->start);
    g_array_set_size(s->map, 0);
    g_string_free(s->text, true);
    s->text = mcp_serial_text(s->buf, s->len, true, s->map);
}

/* The offset where text byte @pos starts */
static uint64_t mcp_serial_scan_offset(McpSerialScan *s, size_t pos)
{
    return s->start + (pos ? g_array_index(s->map, uint32_t, pos - 1) : 0);
}

typedef bool McpSerialFound(McpSerialScan *s, void *opaque);

/*
 * Scan @port's output from @from on until @found sees what it waits for,
 * the deadline passes or the call is cancelled.  True if @found saw it.
 */
static bool coroutine_fn mcp_serial_wait(McpSerialPort *port,
                                         QemuInputCtx *ctx, uint64_t from,
                                         int64_t deadline, McpSerialScan *s,
                                         McpSerialFound *found, void *opaque)
{
    McpSerialWaiter w = { .notifier.notify = mcp_serial_wake, .ctx = ctx };
    bool ret = false;

    qemu_chr_tap_add_notifier(port->tap, &w.notifier);
    for (;;) {
        int64_t now;

        mcp_serial_scan_read(port, from, s);
        if (found(s, opaque)) {
            ret = true;
            break;
        }
        now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        if (ctx->cancelled || now >= deadline) {
            break;
        }
        mcp_serial_sleep(ctx, deadline - now);
        /* more output: let it gather before scanning it all again */
        now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        if (!ctx->cancelled && now < deadline) {
            qemu_co_sleep_ns(QEMU_CLOCK_REALTIME,
                             MIN(SERIAL_SCAN_MS, deadline - now) * SCALE_MS);
        }
    }
    qemu_chr_tap_remove_notifier(&w.notifier);
    return ret;
}

typedef struct McpSerialExpect {
    GRegex *re;
    int me;                     /* where the match ends */
    char *match;
} McpSerialExpect;

static bool mcp_serial_expect_found(McpSerialScan *s, void *opaque)
{
    McpSerialExpect *e = opaque;
    g_autoptr(GMatchInfo) mi = NULL;

    if (!g_regex_match(e->re, s->text->str, 0, &mi)) {
        return false;
    }
    g_match_info_fetch_pos(mi, 0, NULL, &e->me);
    e->match = g_match_info_fetch(mi, 0);
    return true;
}

McpSerialResult *coroutine_fn qmp_serial_expect(const char *port_name,
                                                const char *pattern,
                                                bool has_cursor,
                                                int64_t cursor,
                                                bool has_timeout_ms,
                                                int64_t timeout_ms,
                                                Error **errp)
{
    McpSerialPort *port = mcp_serial_find(port_name, errp);
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_serial_ctx(&local);
    g_autoptr(GRegex) re = NULL;
    g_autoptr(GError) gerr = NULL;
    McpSerialExpect e = { 0 };
    McpSerialScan s;
    int64_t start_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    McpSerialResult *res;
    uint64_t from;
    bool matched;

    if (!port) {
        return NULL;
    }
    re = g_regex_new(pattern, G_REGEX_MULTILINE, 0, &gerr);
    if (!re) {
        error_setg(errp, "bad pattern: %s", gerr->message);
        return NULL;
    }
    e.re = re;
    timeout_ms = MIN(has_timeout_ms ? MAX(timeout_ms, 0) : 30000,
                     MCP_WAIT_MAX_MS);
    from = has_cursor ? MAX(cursor, 0) : port->mark;

    mcp_serial_scan_init(&s);
    matched = mcp_serial_wait(port, ctx, from, start_ms + timeout_ms, &s,
                              mcp_serial_expect_found, &e);
    if (ctx->cancelled) {
        g_free(e.match);
        mcp_serial_scan_free(&s);
        error_setg(errp, "cancelled");
        return NULL;
    }
    res = g_new0(McpSerialResult, 1);
    res->port = g_strdup(qemu_chr_tap_name(port->tap));
    res->start = s.start;
    if (s.start > from) {
        res->has_lost = true;
        res->lost = s.start - from;
    }
    res->has_matched = true;
    res->matched = matched;
    if (matched) {
        uint64_t end_off = mcp_serial_scan_offset(&s, e.me);

        g_string_truncate(s.text, e.me);
        port->mark = end_off;
        res->cursor = end_off;
        res->match = e.match;
        res->summary = g_strdup_printf("%s: matched after %" PRId64
                                       " ms, cursor %" PRIu64 ":\n%s",
                                       res->port,
                                       qemu_clock_get_ms(QEMU_CLOCK_REALTIME)
                                       - start_ms, end_off,
                                       mcp_serial_tail(s.text, 4096));
    } else {
        /* the cursor stays, so the next call sees the same text */
        res->cursor = from;
        res->summary = g_strdup_printf("%s: no match within %" PRId64
                                       " ms; %zu bytes since the cursor, "
                                       "ending with:\n%s",
                                       res->port, timeout_ms, s.len,
                                       mcp_serial_tail(s.text, 2048));
    }
    mcp_serial_scan_free(&s);
    return res;
}

/* serial-run */

typedef struct McpSerialRun {
    McpShell shell;
    GRegex *end;                /* the end marker, or the prompt */
    const char *echo;           /* the end marker as typed, if any */
    const char *head;           /* the start of the command */
    ssize_t out;                /* text offset where the output starts */
    int ms, me;                 /* where @end matched */
    char *status;               /* the exit status as printed, or "" */
} McpSerialRun;

/*
 * Where the output starts in the first @len bytes of @text: after the
 * echo of the typed line.  That is the line with the end marker as
 * typed; else the first line, if it shows the start of the command; else
 * (no echo) the very start.  -1 while the echo is still coming, unless
 * @final.
 */
static ssize_t mcp_serial_run_output(McpSerialRun *r, const char *text,
                                     size_t len, bool final)
{
    const char *p = r->echo ? g_strstr_len(text, len, r->echo) : NULL;
    const char *nl;

    if (p) {
        p += strlen(r->echo);
        nl = memchr(p, '\n', len - (p - text));
        return nl ? nl + 1 - text : final ? p - text : -1;
    }
    nl = memchr(text, '\n', len);
    if (!nl) {
        return final ? 0 : -1;
    }
    return g_strstr_len(text, nl - text, r->head) ? nl + 1 - text : 0;
}

static bool mcp_serial_run_found(McpSerialScan *s, void *opaque)
{
    McpSerialRun *r = opaque;
    const char *text = s->text->str;
    g_autoptr(GMatchInfo) mi = NULL;

    if (r->shell == MCP_SHELL_NONE) {
        /* a prompt counts only after the echo */
        r->out = mcp_serial_run_output(r, text, s->text->len, false);
        if (r->out < 0 ||
            !g_regex_match_full(r->end, text, s->text->len, r->out, 0, &mi,
                                NULL)) {
            return false;
        }
        g_match_info_fetch_pos(mi, 0, &r->ms, &r->me);
        return true;
    }
    /* the echo of the marker has no digits in it, so it cannot match */
    if (!g_regex_match(r->end, text, 0, &mi)) {
        return false;
    }
    g_match_info_fetch_pos(mi, 0, &r->ms, &r->me);
    r->out = mcp_serial_run_output(r, text, r->ms, true);
    r->status = g_match_info_fetch(mi, 1);
    return true;
}

static void mcp_serial_chomp(GString *s)
{
    while (s->len && g_ascii_isspace(s->str[s->len - 1])) {
        g_string_truncate(s, s->len - 1);
    }
}

/* The line to type: the command and, for a Unix shell, the end marker */
static GString *mcp_serial_run_line(const char *command, McpShell shell,
                                    const char *echo)
{
    GString *line = g_string_new(command);
    const char *sep = "; ";

    if (shell != MCP_SHELL_NONE) {
        /* "cmd;" or "cmd &" with "; echo" after it is a syntax error */
        mcp_serial_chomp(line);
        if (line->len && line->str[line->len - 1] == ';') {
            g_string_truncate(line, line->len - 1);
            mcp_serial_chomp(line);
        } else if (line->len && line->str[line->len - 1] == '&' &&
                   (line->len < 2 || line->str[line->len - 2] != '&')) {
            sep = " ";
        }
        g_string_append_printf(line, "%secho %s", line->len ? sep : "",
                               echo);
    }
    g_string_append_c(line, '\r');
    return line;
}

/* Lines in @s, after a newline at the very end was taken off */
static int64_t mcp_serial_run_lines(GString *s)
{
    int64_t n = 1;
    size_t i;

    if (!s->len) {
        return 0;
    }
    for (i = 0; i < s->len; i++) {
        n += s->str[i] == '\n';
    }
    return n;
}

/* Cut @s, which has @n lines, to its first @max lines, or last with @tail */
static void mcp_serial_run_clip(GString *s, int64_t n, int64_t max, bool tail)
{
    size_t from = 0, to = s->len;

    if (max <= 0) {
        g_string_truncate(s, 0);
        return;
    }
    if (n > max) {
        int64_t skip = tail ? n - max : max;
        size_t i = 0;

        while (skip > 0) {
            if (s->str[i++] == '\n') {
                skip--;
            }
        }
        if (tail) {
            from = i;
        } else {
            to = i - 1;
        }
    }
    /* not too many bytes either, cut at a character boundary */
    if (to - from > SERIAL_RUN_BYTES) {
        if (tail) {
            from = to - SERIAL_RUN_BYTES;
            while (from < to && (s->str[from] & 0xc0) == 0x80) {
                from++;
            }
        } else {
            to = from + SERIAL_RUN_BYTES;
            while (to > from && (s->str[to] & 0xc0) == 0x80) {
                to--;
            }
        }
    }
    g_string_truncate(s, to);
    g_string_erase(s, 0, from);
}

/* Remember the end of a command that serial-run leaves running */
static void mcp_serial_run_pending(McpSerialPort *port, const char *pattern,
                                   uint64_t from)
{
    g_free(port->pending);
    port->pending = g_strdup(pattern);
    port->pending_from = from;
}

typedef struct McpSerialPendingWait {
    McpSerialExpect e;
    McpSerialPort *port;
} McpSerialPendingWait;

static bool mcp_serial_pending_found(McpSerialScan *s, void *opaque)
{
    McpSerialPendingWait *w = opaque;

    /* a serial-write in the meantime ends the wait too */
    return !w->port->pending || mcp_serial_expect_found(s, &w->e);
}

/* Wait for the end of the command that an earlier serial-run left */
static bool coroutine_fn mcp_serial_run_settle(McpSerialPort *port,
                                               QemuInputCtx *ctx,
                                               int64_t deadline,
                                               Error **errp)
{
    g_autofree char *pending = g_strdup(port->pending);
    g_autoptr(GRegex) re = g_regex_new(pending, G_REGEX_MULTILINE, 0, NULL);
    McpSerialPendingWait w = { .e.re = re, .port = port };
    McpSerialScan s;
    bool ended;

    mcp_serial_scan_init(&s);
    ended = re && mcp_serial_wait(port, ctx, port->pending_from, deadline, &s,
                                  mcp_serial_pending_found, &w);
    mcp_serial_scan_free(&s);
    g_free(w.e.match);
    if (ctx->cancelled) {
        error_setg(errp, "cancelled");
        return false;
    }
    if (!ended) {
        error_setg(errp, "the command that serial-run typed before is still "
                   "running on %s; wait for its end with serial-expect "
                   "pattern \"%s\", or stop it with serial-write \"\\x03\" "
                   "(after a serial-write, serial-run no longer waits for "
                   "it)", qemu_chr_tap_name(port->tap), pending);
        return false;
    }
    g_clear_pointer(&port->pending, g_free);
    return true;
}

/*
 * Let @port go quiet: an old Unix can throw away what is typed just
 * after Ctrl-C, or while the shell still prints.
 */
static void coroutine_fn mcp_serial_quiet(McpSerialPort *port,
                                          QemuInputCtx *ctx, int64_t deadline)
{
    int64_t start = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    for (;;) {
        int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        int64_t idle = now - port->last_io_ms;

        if (idle >= SERIAL_QUIET_MS || now - start >= SERIAL_QUIET_MAX_MS ||
            now >= deadline || ctx->cancelled) {
            return;
        }
        qemu_co_sleep_ns(QEMU_CLOCK_REALTIME,
                         (SERIAL_QUIET_MS - idle) * SCALE_MS);
    }
}

/*
 * Wait for our turn on @port: until no other serial-run types or waits
 * there, a command that one left running has ended, and the port is
 * quiet.  Then the port is ours until we clear port->running.
 */
static bool coroutine_fn mcp_serial_run_turn(McpSerialPort *port,
                                             QemuInputCtx *ctx,
                                             int64_t deadline, Error **errp)
{
    while (port->running) {
        if (ctx->cancelled) {
            error_setg(errp, "cancelled");
            return false;
        }
        if (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) >= deadline) {
            error_setg(errp, "another serial-run is still busy on %s",
                       qemu_chr_tap_name(port->tap));
            return false;
        }
        mcp_serial_sleep(ctx, 10);
    }
    if (!mcp_serial_running(errp)) {
        return false;
    }
    port->running = true;
    if (port->pending && !mcp_serial_run_settle(port, ctx, deadline, errp)) {
        port->running = false;
        return false;
    }
    mcp_serial_quiet(port, ctx, deadline);
    return true;
}

McpSerialRunResult *coroutine_fn qmp_serial_run(const char *port_name,
                                                const char *command,
                                                bool has_shell,
                                                McpShell shell,
                                                const char *prompt,
                                                bool has_timeout_ms,
                                                int64_t timeout_ms,
                                                bool has_max_lines,
                                                int64_t max_lines,
                                                bool has_tail, bool tail,
                                                bool has_line_limit,
                                                int64_t line_limit,
                                                bool has_char_delay_ms,
                                                int64_t char_delay_ms,
                                                Error **errp)
{
    static unsigned run_seq;
    McpSerialPort *port = mcp_serial_find(port_name, errp);
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_serial_ctx(&local);
    g_autoptr(GRegex) end = NULL;
    g_autoptr(GError) gerr = NULL;
    g_autoptr(GString) line = NULL;
    g_autoptr(GString) out = NULL;
    g_autoptr(GString) sum = NULL;
    g_autofree char *echo = NULL;
    g_autofree char *pattern = NULL;
    g_autofree char *head = NULL;
    McpSerialRun r = { 0 };
    McpSerialScan s;
    McpSerialRunResult *res;
    int64_t start_ms, ms, lines, shown;
    uint64_t first, from;
    unsigned id = ++run_seq;
    size_t full;
    bool done;

    if (!port) {
        return NULL;
    }
    shell = has_shell ? shell : MCP_SHELL_SH;
    if (strpbrk(command, "\r\n")) {
        error_setg(errp, "the command must be one line; join commands "
                   "with ';' or '&&'");
        return NULL;
    }
    if (shell == MCP_SHELL_NONE) {
        if (!prompt) {
            error_setg(errp, "shell \"none\" needs a prompt pattern, to see "
                       "where the output ends");
            return NULL;
        }
        pattern = g_strdup(prompt);
    } else {
        echo = g_strdup_printf("__lemu%u_%s__", id,
                               shell == MCP_SHELL_CSH ? "${status}" : "$?");
        /* or what the other kind of shell makes of it */
        pattern = g_strdup_printf("__lemu%u_(?:([0-9]+)__|%s(?=\\n))", id,
                                  shell == MCP_SHELL_CSH ? "__" : "[01]");
    }
    end = g_regex_new(pattern, G_REGEX_MULTILINE, 0, &gerr);
    if (!end) {
        error_setg(errp, "bad prompt pattern: %s", gerr->message);
        return NULL;
    }
    line = mcp_serial_run_line(command, shell, echo);
    line_limit = has_line_limit ? line_limit : SERIAL_LINE_LIMIT;
    if (line->len > line_limit) {
        error_setg(errp, "the line is %zu bytes with the end marker, more "
                   "than line-limit %" PRId64 "; shorten it, or raise "
                   "line-limit if the guest takes longer lines",
                   line->len, line_limit);
        return NULL;
    }
    timeout_ms = MIN(has_timeout_ms ? MAX(timeout_ms, 0) : 30000,
                     MCP_WAIT_MAX_MS);
    start_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (!mcp_serial_run_turn(port, ctx, start_ms + timeout_ms, errp)) {
        return NULL;
    }
    head = g_strndup(command + strspn(command, " \t"), 8);
    r.shell = shell;
    r.end = end;
    r.echo = echo;
    r.head = head;

    qemu_chr_tap_range(port->tap, &first, &from);
    /* the whole line, even if cancelled: half a line breaks the next */
    if (!mcp_serial_send(port, ctx, line,
                         has_char_delay_ms ? MAX(char_delay_ms, 0) : 0,
                         true, errp)) {
        port->running = false;
        return NULL;
    }
    start_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    mcp_serial_scan_init(&s);
    done = mcp_serial_wait(port, ctx, from, start_ms + timeout_ms, &s,
                           mcp_serial_run_found, &r);
    ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - start_ms;
    if (ctx->cancelled) {
        /* the command runs on: the next serial-run waits for its end */
        mcp_serial_run_pending(port, pattern, from);
        port->running = false;
        g_free(r.status);
        mcp_serial_scan_free(&s);
        error_setg(errp, "cancelled");
        return NULL;
    }
    if (!done) {
        r.out = mcp_serial_run_output(&r, s.text->str, s.text->len, true);
        r.ms = r.me = s.text->len;
    }

    out = g_string_new_len(s.text->str + r.out, r.ms - r.out);
    if (out->len && out->str[out->len - 1] == '\n') {
        g_string_truncate(out, out->len - 1);
    }
    lines = mcp_serial_run_lines(out);
    full = out->len;
    mcp_serial_run_clip(out, lines,
                        has_max_lines ? max_lines : SERIAL_RUN_LINES,
                        has_tail && tail);
    shown = mcp_serial_run_lines(out);

    res = g_new0(McpSerialRunResult, 1);
    res->port = g_strdup(qemu_chr_tap_name(port->tap));
    res->done = done;
    res->lines = lines;
    res->start = mcp_serial_scan_offset(&s, r.out);
    if (done) {
        size_t after = r.me;

        /* past the end marker's line */
        if (shell != MCP_SHELL_NONE && s.text->str[after] == '\n') {
            after++;
        }
        res->cursor = mcp_serial_scan_offset(&s, after);
    } else {
        /* serial-expect for the end goes on from the output */
        res->cursor = res->start;
        res->end_pattern = shell == MCP_SHELL_NONE
            ? g_strdup(prompt) : g_strdup_printf("__lemu%u_[0-9]+__", id);
        mcp_serial_run_pending(port, pattern, res->start);
    }
    port->running = false;
    port->mark = res->cursor;
    if (done && r.status && *r.status) {
        res->has_exit_status = true;
        res->exit_status = g_ascii_strtoll(r.status, NULL, 10);
    }

    sum = g_string_new(NULL);
    g_string_append_printf(sum, "%s: ", res->port);
    if (res->has_exit_status) {
        g_string_append_printf(sum, "exit status %" PRId64 " after %" PRId64
                               " ms", res->exit_status, ms);
    } else if (!done) {
        g_string_append_printf(sum, "still running after %" PRId64 " ms",
                               ms);
    } else if (shell == MCP_SHELL_NONE) {
        g_string_append_printf(sum, "done after %" PRId64 " ms", ms);
    } else {
        const char *other = shell == MCP_SHELL_CSH ? "sh" : "csh";

        g_string_append_printf(sum, "done after %" PRId64 " ms, but with no "
                               "exit status: the shell looks like %s, so "
                               "use shell \"%s\"", ms, other, other);
    }
    if (lines) {
        g_string_append_printf(sum, ", %" PRId64 " line%s%s", lines,
                               lines == 1 ? "" : "s", done ? "" : " so far");
    } else {
        g_string_append(sum, done ? ", no output" : ", no output yet");
    }
    if (out->len < full) {
        g_string_append_printf(sum, ", showing the %s %" PRId64
                               " (serial-read from cursor %" PRIu64
                               " has all)", has_tail && tail ? "last"
                               : "first", shown, res->start);
    }
    if (s.start > from) {
        g_string_append_printf(sum, "; the first %" PRIu64 " bytes are "
                               "lost, more than 1 MB came", s.start - from);
    }
    if (!done) {
        g_string_append_printf(sum, "; wait for the end with serial-expect "
                               "pattern \"%s\", or stop the command with "
                               "serial-write \"\\x03\"", res->end_pattern);
    }
    if (out->len) {
        g_string_append(sum, ":\n");
        g_string_append_len(sum, out->str, out->len);
    }
    res->summary = g_string_free(g_steal_pointer(&sum), false);
    g_free(r.status);
    mcp_serial_scan_free(&s);
    return res;
}
