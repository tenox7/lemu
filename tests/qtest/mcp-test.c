/*
 * qtest for the MCP screen, input and serial commands
 *
 * The input test sends 5,000 keys with the "key" command and reads them
 * back from the i8042 while stepping the clock: none may be lost, the
 * order must hold, and nothing may arrive while the VM is paused.
 *
 * The screen tests draw into the VGA linear framebuffer and check that
 * "wait-screen" sees the change, and that a blinking box does not stop
 * the screen from counting as stable.
 *
 * The pointer tests play a guest that draws its own pointer and moves
 * it 0.35 or 2 pixels per mouse count, or faster for bigger moves:
 * "mouse-move" must measure that and land on the pixel.
 *
 * The serial tests play the guest on COM1: a terminal that echoes what
 * "serial-run" types, and a shell that answers it.
 *
 * The telnet test plays a telnet server: "serial-connect" must answer
 * its options and take the telnet commands out of what it sends, and
 * what is typed must have IAC doubled and a lone CR followed by NUL.
 *
 * The steps test runs several tools in one call over MCP: it must stop
 * at the first wait that is not met or the first error, and run nothing
 * when a tool name is wrong.
 *
 * The debug tests run a BIOS of two instructions under TCG, step it,
 * stop it at an address and patch it while it runs.
 *
 * The media tests make a disc and a floppy from host files, read the
 * disc the way guests do, with and without Rock Ridge, and copy the
 * floppy's files out again, also a long name the "guest" wrote.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <math.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "libqtest.h"
#include "libqos/pci-pc.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "qemu/units.h"

#define NKEYS       5000

#define I8042_DATA      0x60
#define I8042_STATUS    0x64
#define I8042_CMD       0x64
#define I8042_OBF       0x01

/* Set 1 make codes for a..z */
static const uint8_t set1[26] = {
    0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25,
    0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f,
    0x11, 0x2d, 0x15, 0x2c,
};

static int drain_kbd(QTestState *qts, uint8_t *buf, int n, int max)
{
    while (qtest_inb(qts, I8042_STATUS) & I8042_OBF) {
        g_assert_cmpint(n, <, max);
        buf[n++] = qtest_inb(qts, I8042_DATA);
    }
    return n;
}

/* A line-based MCP client on @path */
static int mcp_connect(const char *path)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    int i;

    g_assert(fd >= 0);
    g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
    for (i = 0; i < 100; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }
        g_usleep(10 * 1000);
    }
    g_assert_not_reached();
}

static void mcp_send(int fd, const char *line)
{
    size_t len = strlen(line);

    g_assert_cmpint(write(fd, line, len), ==, len);
    g_assert_cmpint(write(fd, "\n", 1), ==, 1);
}

static char *mcp_recv(int fd)
{
    GString *s = g_string_new(NULL);
    char c;

    while (read(fd, &c, 1) == 1 && c != '\n') {
        g_string_append_c(s, c);
    }
    return g_string_free(s, false);
}

static void test_keys(void)
{
    g_autofree char *dir = g_dir_make_tmp("mcp-test-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "mcp.sock", NULL);
    g_autoptr(GString) cmd = g_string_new(NULL);
    int max = NKEYS * 2 + 16;
    g_autofree uint8_t *got = g_new0(uint8_t, max);
    g_autofree char *line = NULL;
    QTestState *qts;
    int fd, i, n = 0, before, steps;

    /*
     * The keys go over MCP, so that QMP stays free for stop and cont
     * while the "key" command runs.
     */
    qts = qtest_initf("-machine pc -display none "
                      "-mcp unix:%s,server=on,wait=off", path);
    fd = mcp_connect(path);
    mcp_send(fd, "{\"jsonrpc\": \"2.0\", \"id\": 1, \"method\": "
             "\"initialize\", \"params\": {\"protocolVersion\": "
             "\"2025-11-25\", \"capabilities\": {}, \"clientInfo\": "
             "{\"name\": \"mcp-test\", \"version\": \"1\"}}}");
    g_free(mcp_recv(fd));

    /* scancode translation on, so we read set 1 */
    qtest_outb(qts, I8042_CMD, 0x60);
    qtest_outb(qts, I8042_DATA, 0x45);

    g_string_append(cmd, "{\"jsonrpc\": \"2.0\", \"id\": 2, \"method\": "
                    "\"tools/call\", \"params\": {\"name\": \"key\", "
                    "\"arguments\": {\"hold-ms\": 1, \"delay-ms\": 1, "
                    "\"keys\": [");
    for (i = 0; i < NKEYS; i++) {
        g_string_append_printf(cmd, "%s\"%c\"", i ? ", " : "", 'a' + i % 26);
    }
    g_string_append(cmd, "]}}}");
    mcp_send(fd, cmd->str);

    /* step part of the way, then pause: nothing may arrive while paused */
    for (steps = 0; steps < NKEYS; steps++) {
        qtest_clock_step(qts, SCALE_MS);
        n = drain_kbd(qts, got, n, max);
    }
    g_assert_cmpint(n, >, 0);
    g_assert_cmpint(n, <, NKEYS * 2);

    qtest_qmp_assert_success(qts, "{'execute': 'stop'}");
    before = n = drain_kbd(qts, got, n, max);
    for (i = 0; i < 200; i++) {
        qtest_clock_step(qts, SCALE_MS);
        n = drain_kbd(qts, got, n, max);
    }
    g_assert_cmpint(n, ==, before);
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");

    for (steps = 0; steps < NKEYS * 4 && n < NKEYS * 2; steps++) {
        qtest_clock_step(qts, SCALE_MS);
        n = drain_kbd(qts, got, n, max);
    }

    /* the call answers after the pause that follows the last key */
    for (i = 0; i < 10; i++) {
        qtest_clock_step(qts, SCALE_MS);
    }
    line = mcp_recv(fd);
    g_assert(strstr(line, "\"id\": 2"));
    g_assert(strstr(line, "\"isError\": false"));

    g_assert_cmpint(n, ==, NKEYS * 2);
    for (i = 0; i < NKEYS; i++) {
        g_assert_cmphex(got[2 * i], ==, set1[i % 26]);
        g_assert_cmphex(got[2 * i + 1], ==, set1[i % 26] | 0x80);
    }
    close(fd);
    qtest_quit(qts);
    unlink(path);
    rmdir(dir);
}

/* Screen */

#define VBE_INDEX   0x1ce
#define VBE_DATA    0x1cf
#define SCR_W       640
#define SCR_H       480

typedef struct Screen {
    QTestState *qts;
    QPCIBus *bus;
    QPCIDevice *dev;
    QPCIBar lfb;
} Screen;

static void vbe_write(QTestState *qts, uint16_t index, uint16_t val)
{
    qtest_outw(qts, VBE_INDEX, index);
    qtest_outw(qts, VBE_DATA, val);
}

static void screen_init_machine(Screen *s, const char *machine)
{
    s->qts = qtest_initf("-machine %s -display none -vga std", machine);
    s->bus = qpci_new_pc(s->qts, NULL);
    s->dev = qpci_device_find(s->bus, QPCI_DEVFN(2, 0));
    g_assert(s->dev);
    qpci_device_enable(s->dev);
    s->lfb = qpci_iomap(s->dev, 0, NULL);

    vbe_write(s->qts, 1, SCR_W);        /* xres */
    vbe_write(s->qts, 2, SCR_H);        /* yres */
    vbe_write(s->qts, 3, 32);           /* bpp */
    vbe_write(s->qts, 4, 0x41);         /* enabled, linear framebuffer */
    /* attribute controller: palette address source, so not blanked */
    qtest_inb(s->qts, 0x3da);
    qtest_outb(s->qts, 0x3c0, 0x20);
}

static void screen_init(Screen *s)
{
    screen_init_machine(s, "pc");
}

static void screen_free(Screen *s)
{
    g_free(s->dev);
    qpci_free_pc(s->bus);
    qtest_quit(s->qts);
}

/*
 * Draw a box on the black screen in one write of whole rows, so that it
 * appears at once.  Drawn row by row, a screen check that runs between
 * two rows sees only part of the box, which made the tests flaky on a
 * busy host.  The rest of the rows stays black.
 */
static void screen_fill(Screen *s, int x, int y, int w, int h, uint32_t rgb)
{
    g_autofree uint32_t *rows = g_new0(uint32_t, SCR_W * h);
    int i, j;

    for (i = 0; i < h; i++) {
        for (j = 0; j < w; j++) {
            rows[i * SCR_W + x + j] = cpu_to_le32(rgb);
        }
    }
    qpci_memwrite(s->dev, s->lfb, y * SCR_W * 4, rows, SCR_W * h * 4);
}

/* The reply to a command sent with qtest_qmp_send(), skipping events */
static QDict *wait_reply(QTestState *qts)
{
    QDict *rsp;

    for (;;) {
        rsp = qtest_qmp_receive(qts);
        if (!qdict_haskey(rsp, "event")) {
            return rsp;
        }
        qobject_unref(rsp);
    }
}

static QDict *screen_wait_reply(Screen *s)
{
    return wait_reply(s->qts);
}

static void test_screen_change(void)
{
    Screen s;
    QDict *rsp, *ret, *wait, *box;

    screen_init(&s);
    /* let the screen settle in the new mode */
    qtest_qmp_assert_success(s.qts, "{'execute': 'wait-screen', 'arguments':"
                             " {'until': 'stable', 'stable-ms': 300,"
                             " 'timeout-ms': 5000, 'screenshot': false}}");

    qtest_qmp_send(s.qts, "{'execute': 'wait-screen', 'arguments':"
                   " {'until': 'change', 'timeout-ms': 5000,"
                   " 'screenshot': false}}");
    g_usleep(300 * 1000);
    screen_fill(&s, 100, 50, 40, 20, 0xffffff);

    rsp = screen_wait_reply(&s);
    ret = qdict_get_qdict(rsp, "return");
    g_assert(ret);
    wait = qdict_get_qdict(ret, "wait");
    g_assert(qdict_get_bool(wait, "matched"));
    g_assert_cmpint(qdict_get_int(wait, "changed-pixels"), ==, 40 * 20);
    box = qdict_get_qdict(wait, "box");
    g_assert_cmpint(qdict_get_int(box, "x"), ==, 100);
    g_assert_cmpint(qdict_get_int(box, "y"), ==, 50);
    g_assert_cmpint(qdict_get_int(box, "w"), ==, 40);
    g_assert_cmpint(qdict_get_int(box, "h"), ==, 20);
    qobject_unref(rsp);

    /* a change outside the region does not count */
    qtest_qmp_send(s.qts, "{'execute': 'wait-screen', 'arguments':"
                   " {'until': 'change', 'timeout-ms': 1000,"
                   " 'region': {'x': 0, 'y': 300, 'w': 640, 'h': 100},"
                   " 'screenshot': false}}");
    g_usleep(300 * 1000);
    screen_fill(&s, 100, 50, 40, 20, 0x000000);
    rsp = screen_wait_reply(&s);
    ret = qdict_get_qdict(rsp, "return");
    g_assert(!qdict_get_bool(qdict_get_qdict(ret, "wait"), "matched"));
    qobject_unref(rsp);

    screen_free(&s);
}

/* Toggle a text-cursor-sized box every 150 ms during a "stable" wait */
static bool blink_wait(Screen *s, bool ignore_blink)
{
    QDict *rsp, *ret;
    bool matched;
    int i;

    qtest_qmp_send(s->qts, "{'execute': 'wait-screen', 'arguments':"
                   " {'until': 'stable', 'stable-ms': 1000,"
                   " 'timeout-ms': 3000, 'ignore-blink': %i,"
                   " 'screenshot': false}}", ignore_blink);
    for (i = 0; i < 22; i++) {
        screen_fill(s, 200, 200, 8, 16, i & 1 ? 0xc0c0c0 : 0x000000);
        g_usleep(150 * 1000);
    }
    rsp = screen_wait_reply(s);
    ret = qdict_get_qdict(rsp, "return");
    g_assert(ret);
    matched = qdict_get_bool(qdict_get_qdict(ret, "wait"), "matched");
    qobject_unref(rsp);
    return matched;
}

static void test_screen_blink(void)
{
    Screen s;

    screen_init(&s);
    qtest_qmp_assert_success(s.qts, "{'execute': 'wait-screen', 'arguments':"
                             " {'until': 'stable', 'stable-ms': 300,"
                             " 'timeout-ms': 5000, 'screenshot': false}}");
    g_assert(!blink_wait(&s, false));
    g_assert(blink_wait(&s, true));
    screen_free(&s);
}

/*
 * Quit while a command waits for its input to go out.  Ending the wait
 * lets the command finish at once; nothing may touch what it left on
 * its stack after that (QEMU used to abort here).
 */
static void test_input_quit(gconstpointer cmd)
{
    QTestState *qts = qtest_init("-machine pc,vmport=off -display none"
                                 " -vga std");

    qtest_qmp_send_raw(qts, "%s", (const char *)cmd);
    /* the clock does not move, so the input stays queued */
    g_usleep(300 * 1000);
    qtest_quit(qts);
}

/*
 * Pointer: the test plays a guest with a PS/2 mouse that has no hardware
 * cursor.  It reads the mouse packets from the i8042, moves its pointer
 * @gain pixels per count (twice as far for a packet of @accel counts or
 * more, as X does), and draws the pointer, a square with the hotspot in
 * the middle.  mouse-move must find out how the guest moves the pointer
 * and land on the pixel.
 */

#define I8042_AUX_OBF   0x20
#define PTR_SIZE        9
#define PTR_HOT         4
#define CARET_X         600     /* a text cursor, far from the pointer */
#define CARET_Y         40
#define CARET_H         16

typedef struct FakeGuest {
    Screen s;
    double gain;
    int accel;
    bool caret;                 /* it blinks with every mouse packet */
    bool caret_on;
    double fx, fy;              /* the hotspot */
    int x, y;                   /* the hotspot as drawn */
    uint8_t pkt[3];
    int npkt;
    QDict *reply;
} FakeGuest;

static void fake_blink(FakeGuest *g)
{
    uint32_t c = cpu_to_le32(g->caret_on ? 0 : 0xffffff);
    uint32_t px[2] = { c, c };
    int i;

    g->caret_on = !g->caret_on;
    for (i = 0; i < CARET_H; i++) {
        qpci_memwrite(g->s.dev, g->s.lfb, ((CARET_Y + i) * SCR_W + CARET_X) * 4,
                      px, sizeof(px));
    }
}

/* Move the pointer to @nx, @ny in one write, as screen_fill() does */
static void fake_draw(FakeGuest *g, int nx, int ny)
{
    int top = MAX(0, MIN(g->y, ny) - PTR_HOT);
    int bottom = MIN(SCR_H, MAX(g->y, ny) - PTR_HOT + PTR_SIZE);
    g_autofree uint32_t *rows = g_new0(uint32_t, SCR_W * (bottom - top));
    int i, j;

    for (i = 0; i < PTR_SIZE; i++) {
        int y = ny - PTR_HOT + i;

        for (j = 0; j < PTR_SIZE; j++) {
            int x = nx - PTR_HOT + j;

            if (x >= 0 && x < SCR_W && y >= top && y < bottom) {
                rows[(y - top) * SCR_W + x] = cpu_to_le32(0xffffff);
            }
        }
    }
    qpci_memwrite(g->s.dev, g->s.lfb, top * SCR_W * 4, rows,
                  SCR_W * (bottom - top) * 4);
    g->x = nx;
    g->y = ny;
}

static void fake_packet(FakeGuest *g, const uint8_t *p)
{
    int dx = p[1] - (p[0] & 0x10 ? 256 : 0);
    int dy = p[2] - (p[0] & 0x20 ? 256 : 0);
    double k = g->gain * (g->accel && abs(dx) + abs(dy) >= g->accel ? 2 : 1);

    g->fx = MAX(0, MIN(SCR_W - 1, g->fx + dx * k));
    g->fy = MAX(0, MIN(SCR_H - 1, g->fy - dy * k));     /* PS/2 y is up */
    if (lround(g->fx) != g->x || lround(g->fy) != g->y) {
        fake_draw(g, lround(g->fx), lround(g->fy));
    }
    if (g->caret) {
        fake_blink(g);
    }
}

/* Take what the mouse sent; true if there was something */
static bool fake_poll(FakeGuest *g)
{
    bool any = false;
    uint8_t st;

    while ((st = qtest_inb(g->s.qts, I8042_STATUS)) & I8042_OBF) {
        uint8_t b = qtest_inb(g->s.qts, I8042_DATA);

        any = true;
        if (!(st & I8042_AUX_OBF) || (!g->npkt && !(b & 0x08))) {
            continue;           /* a key, or not the start of a packet */
        }
        g->pkt[g->npkt++] = b;
        if (g->npkt == 3) {
            fake_packet(g, g->pkt);
            g->npkt = 0;
        }
    }
    return any;
}

static void fake_init(FakeGuest *g, double gain, int accel)
{
    uint8_t st;

    memset(g, 0, sizeof(*g));
    g->gain = gain;
    g->accel = accel;
    screen_init_machine(&g->s, "pc,vmport=off");
    g->x = g->y = -PTR_SIZE;
    g->fx = SCR_W / 2.0;
    g->fy = SCR_H / 2.0;
    fake_draw(g, SCR_W / 2, SCR_H / 2);

    /* enable the mouse port, then the mouse, and take its ACK */
    qtest_outb(g->s.qts, I8042_CMD, 0xa8);
    qtest_outb(g->s.qts, I8042_CMD, 0xd4);
    qtest_outb(g->s.qts, I8042_DATA, 0xf4);
    for (;;) {
        st = qtest_inb(g->s.qts, I8042_STATUS);
        if (st & I8042_OBF) {
            uint8_t b = qtest_inb(g->s.qts, I8042_DATA);

            if (st & I8042_AUX_OBF) {
                g_assert_cmphex(b, ==, 0xfa);
                break;
            }
        }
    }
}

static gpointer fake_reply(gpointer opaque)
{
    FakeGuest *g = opaque;

    g_atomic_pointer_set(&g->reply, wait_reply(g->s.qts));
    return NULL;
}

/*
 * Send @cmd and play the guest until the reply comes; it is read on
 * another thread, from the QMP socket, while this one talks qtest.
 * lemu paces input on the guest's clock, which moves only when the
 * test steps it.
 */
static QDict *fake_run(FakeGuest *g, const char *cmd)
{
    GThread *t;
    QDict *rsp;

    qtest_qmp_send_raw(g->s.qts, "%s", cmd);
    t = g_thread_new("mcp-reply", fake_reply, g);
    while (!g_atomic_pointer_get(&g->reply)) {
        if (!fake_poll(g)) {
            qtest_clock_step(g->s.qts, SCALE_MS);
        }
    }
    g_thread_join(t);
    rsp = g->reply;
    g->reply = NULL;
    fake_poll(g);
    return rsp;
}

/* mouse-move to @x, @y must land within @slack px; @want in the summary */
static void fake_move(FakeGuest *g, int x, int y, int slack, const char *want)
{
    g_autofree char *cmd = g_strdup_printf("{'execute': 'mouse-move',"
                                           " 'arguments': {'x': %d, 'y': %d}}",
                                           x, y);
    QDict *rsp = fake_run(g, cmd);
    QDict *ret = qdict_get_qdict(rsp, "return");
    const char *summary;

    g_assert(ret);
    summary = qdict_get_str(ret, "summary");
    g_test_message("to %d,%d, the guest has %d,%d: %s", x, y, g->x, g->y,
                   summary);
    g_assert(!want || strstr(summary, want));
    g_assert_cmpint(abs(g->x - x), <=, slack);
    g_assert_cmpint(abs(g->y - y), <=, slack);
    qobject_unref(rsp);
}

/*
 * A guest that scales mouse motion down, with a blinking text cursor
 * that must not be taken for part of the pointer
 */
static void test_pointer_slow(void)
{
    FakeGuest g;
    QDict *rsp, *ret;

    fake_init(&g, 0.35, 0);
    g.caret = true;

    /* keys say once that lemu has not placed the pointer */
    rsp = fake_run(&g, "{'execute': 'key', 'arguments': {'keys': ['a']}}");
    ret = qdict_get_qdict(rsp, "return");
    g_assert(qdict_haskey(ret, "warning"));
    g_assert(strstr(qdict_get_str(ret, "warning"), "not placed by lemu"));
    qobject_unref(rsp);
    rsp = fake_run(&g, "{'execute': 'key', 'arguments': {'keys': ['a']}}");
    g_assert(!qdict_haskey(qdict_get_qdict(rsp, "return"), "warning"));
    qobject_unref(rsp);

    fake_move(&g, 300, 200, 1, "a count moves 0.35 px");
    fake_move(&g, 500, 420, 2, NULL);
    fake_move(&g, 40, 30, 2, NULL);
    screen_free(&g.s);
}

/* A guest that moves the pointer 2 px per count */
static void test_pointer_fast(void)
{
    FakeGuest g;

    fake_init(&g, 2, 0);
    fake_move(&g, 301, 201, 1, "a count moves 2.00 px");
    fake_move(&g, 100, 400, 1, NULL);
    screen_free(&g.s);
}

/* A guest that speeds up moves of 4 counts or more, as X does */
static void test_pointer_accel(void)
{
    FakeGuest g;

    fake_init(&g, 1, 4);
    fake_move(&g, 150, 100, 0, "steps of 1 count, and a count moves 1.00");
    fake_move(&g, 90, 160, 0, NULL);
    screen_free(&g.s);
}

/* Serial */

#define COM1            0x3f8
#define UART_LSR        5
#define UART_LSR_DR     0x01

static QTestState *serial_init(void)
{
    return qtest_init("-machine pc -display none -serial null");
}

static void uart_puts(QTestState *qts, const char *s)
{
    for (; *s; s++) {
        qtest_outb(qts, COM1, *s);
    }
}

/* Read a line typed into COM1, echoing it as a terminal does */
static char *uart_gets(QTestState *qts)
{
    GString *line = g_string_new(NULL);
    gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    char c;

    for (;;) {
        if (!(qtest_inb(qts, COM1 + UART_LSR) & UART_LSR_DR)) {
            g_assert_cmpint(g_get_monotonic_time(), <, deadline);
            g_usleep(1000);
            continue;
        }
        c = qtest_inb(qts, COM1);
        if (c == '\r') {
            uart_puts(qts, "\r\n");
            return g_string_free(line, false);
        }
        qtest_outb(qts, COM1, c);
        g_string_append_c(line, c);
    }
}

/*
 * Be the shell for one serial-run: echo the line it types, then print
 * @answer, in which each "@" stands for the start of the end marker.
 * Returns the line.
 */
static char *shell_answer(QTestState *qts, const char *answer)
{
    char *line = uart_gets(qts);
    const char *m = strstr(line, "echo __lemu");
    g_autoptr(GString) out = g_string_new(NULL);
    g_autofree char *marker = NULL;
    const char *p;

    if (m) {
        m += strlen("echo ");
        marker = g_strndup(m, strchr(m, '$') - m);
    }
    for (p = answer; *p; p++) {
        if (*p == '@') {
            g_assert(marker);
            g_string_append(out, marker);
        } else {
            g_string_append_c(out, *p);
        }
    }
    uart_puts(qts, out->str);
    return line;
}

/* serial-run with @args, the shell answering @answer; the line to @line */
static QDict *run(QTestState *qts, const char *args, const char *answer,
                  char **line)
{
    QDict *rsp;
    char *typed;

    qtest_qmp_send(qts, "{'execute': 'serial-run', 'arguments': %p}",
                   qobject_from_json(args, &error_abort));
    typed = shell_answer(qts, answer);
    if (line) {
        *line = typed;
    } else {
        g_free(typed);
    }
    rsp = wait_reply(qts);
    g_assert(qdict_haskey(rsp, "return"));
    return rsp;
}

/* vm-stats measures for the time asked, at least 100 ms, every vCPU */
static void test_vm_stats(void)
{
    QTestState *qts = qtest_init("-machine pc -display none -smp 2");
    QDict *rsp, *ret;

    rsp = qtest_qmp(qts, "{'execute': 'vm-stats', 'arguments':"
                    " {'interval-ms': 300}}");
    ret = qdict_get_qdict(rsp, "return");
    g_assert(ret);
    g_assert_cmpint(qdict_get_int(ret, "interval-ms"), >=, 300);
    g_assert_cmpint(qdict_get_int(ret, "interval-ms"), <, 5000);
    g_assert_cmpfloat(qdict_get_double(ret, "cpu-percent"), >=, 0);
    g_assert_cmpint(qdict_get_int(ret, "paused-percent"), ==, 0);
    g_assert_cmpint(qlist_size(qdict_get_qlist(ret, "vcpus")), ==, 2);
    g_assert(strstr(qdict_get_str(ret, "summary"), "host CPU"));
    qobject_unref(rsp);

    /* a paused VM runs no CPU */
    qtest_qmp_assert_success(qts, "{'execute': 'stop'}");
    rsp = qtest_qmp(qts, "{'execute': 'vm-stats', 'arguments':"
                    " {'interval-ms': 1}}");
    ret = qdict_get_qdict(rsp, "return");
    g_assert(ret);
    g_assert_cmpint(qdict_get_int(ret, "interval-ms"), >=, 100);
    g_assert_cmpint(qdict_get_int(ret, "paused-percent"), ==, 100);
    g_assert(strstr(qdict_get_str(ret, "summary"), "paused"));
    qobject_unref(rsp);
    qtest_quit(qts);
}

/* serial-expect waits for text, and its cursor moves past the match */
static void test_serial_expect(void)
{
    QTestState *qts = serial_init();
    QDict *rsp, *ret;

    uart_puts(qts, "boot...\r\nlogin: ");
    rsp = qtest_qmp(qts, "{'execute': 'serial-expect', 'arguments':"
                    " {'pattern': '(?i)LOGIN: $', 'timeout-ms': 5000}}");
    ret = qdict_get_qdict(rsp, "return");
    g_assert(ret);
    g_assert(qdict_get_bool(ret, "matched"));
    g_assert_cmpstr(qdict_get_str(ret, "match"), ==, "login: ");
    g_assert_cmpint(qdict_get_int(ret, "cursor"), ==, 16);
    qobject_unref(rsp);

    rsp = qtest_qmp(qts, "{'execute': 'serial-expect', 'arguments':"
                    " {'pattern': 'login', 'timeout-ms': 200}}");
    ret = qdict_get_qdict(rsp, "return");
    g_assert(ret);
    g_assert(!qdict_get_bool(ret, "matched"));
    qobject_unref(rsp);

    /*
     * A failed serial-expect leaves the cursor before the prompt, but the
     * answer typed after it moves the cursor: the next serial-expect
     * waits for the next prompt, not the old one.
     */
    uart_puts(qts, "Erase? [y/n]: ");
    rsp = qtest_qmp(qts, "{'execute': 'serial-expect', 'arguments':"
                    " {'pattern': 'Password:', 'timeout-ms': 200}}");
    g_assert(!qdict_get_bool(qdict_get_qdict(rsp, "return"), "matched"));
    qobject_unref(rsp);
    qtest_qmp_assert_success(qts, "{'execute': 'serial-write', 'arguments':"
                             " {'text': 'n'}}");
    g_assert_cmpint(qtest_inb(qts, COM1), ==, 'n');
    uart_puts(qts, "n\r\nKeep? [y/n]: ");
    rsp = qtest_qmp(qts, "{'execute': 'serial-expect', 'arguments':"
                    " {'pattern': '[A-Za-z]+\\\\? \\\\[y/n\\\\]',"
                    " 'timeout-ms': 5000}}");
    ret = qdict_get_qdict(rsp, "return");
    g_assert(qdict_get_bool(ret, "matched"));
    g_assert_cmpstr(qdict_get_str(ret, "match"), ==, "Keep? [y/n]");
    qobject_unref(rsp);
    qtest_quit(qts);
}

static void test_serial_run(void)
{
    QTestState *qts = serial_init();
    g_autofree char *too_long = g_strnfill(250, 'x');
    QDict *rsp, *ret;
    gint64 t0;
    char *line;

    /* a command that fails: its output without the echo, and its status */
    rsp = run(qts, "{'command': 'ls /nope'}",
              "ls: /nope: No such file\r\n@2__\r\n$ ", &line);
    g_assert(g_str_has_prefix(line, "ls /nope; echo __lemu"));
    g_assert(g_str_has_suffix(line, "_$?__"));
    g_free(line);
    ret = qdict_get_qdict(rsp, "return");
    g_assert(qdict_get_bool(ret, "done"));
    g_assert_cmpint(qdict_get_int(ret, "exit-status"), ==, 2);
    g_assert_cmpint(qdict_get_int(ret, "lines"), ==, 1);
    g_assert(g_str_has_suffix(qdict_get_str(ret, "summary"),
                              ", 1 line:\nls: /nope: No such file"));
    qobject_unref(rsp);

    /* output with no newline at its end */
    rsp = run(qts, "{'command': 'printf foo'}", "foo@0__\r\n$ ", NULL);
    ret = qdict_get_qdict(rsp, "return");
    g_assert_cmpint(qdict_get_int(ret, "exit-status"), ==, 0);
    g_assert(g_str_has_suffix(qdict_get_str(ret, "summary"), ":\nfoo"));
    qobject_unref(rsp);

    /* only the last lines */
    rsp = run(qts, "{'command': 'seq 5', 'max-lines': 2, 'tail': true}",
              "1\r\n2\r\n3\r\n4\r\n5\r\n@0__\r\n$ ", NULL);
    ret = qdict_get_qdict(rsp, "return");
    g_assert_cmpint(qdict_get_int(ret, "lines"), ==, 5);
    g_assert(strstr(qdict_get_str(ret, "summary"), ", showing the last 2 "));
    g_assert(g_str_has_suffix(qdict_get_str(ret, "summary"), ":\n4\n5"));
    qobject_unref(rsp);

    /* csh; after "&" comes no ";" */
    rsp = run(qts, "{'command': 'sleep 9 &', 'shell': 'csh'}",
              "@0__\r\n% ", &line);
    g_assert(g_str_has_prefix(line, "sleep 9 & echo __lemu"));
    g_assert(g_str_has_suffix(line, "_${status}__"));
    g_free(line);
    ret = qdict_get_qdict(rsp, "return");
    g_assert_cmpint(qdict_get_int(ret, "exit-status"), ==, 0);
    g_assert_cmpint(qdict_get_int(ret, "lines"), ==, 0);
    qobject_unref(rsp);

    /* csh reads "$?__" as "is __ set?"; the agent is told to say csh */
    rsp = run(qts, "{'command': 'true'}", "@0\r\n% ", NULL);
    ret = qdict_get_qdict(rsp, "return");
    g_assert(qdict_get_bool(ret, "done"));
    g_assert(!qdict_haskey(ret, "exit-status"));
    g_assert(strstr(qdict_get_str(ret, "summary"), "use shell \"csh\""));
    qobject_unref(rsp);

    /* not a Unix shell: the prompt ends the output */
    rsp = run(qts, "{'command': 'help', 'shell': 'none', 'prompt': '^ok $'}",
              "one\r\ntwo\r\nok ", &line);
    g_assert_cmpstr(line, ==, "help");
    g_free(line);
    ret = qdict_get_qdict(rsp, "return");
    g_assert(qdict_get_bool(ret, "done"));
    g_assert(!qdict_haskey(ret, "exit-status"));
    g_assert(g_str_has_suffix(qdict_get_str(ret, "summary"),
                              ", 2 lines:\none\ntwo"));
    qobject_unref(rsp);

    /* after typing by hand, it lets the port go quiet before it types */
    qtest_qmp_assert_success(qts, "{'execute': 'serial-write',"
                             " 'arguments': {'text': 'q'}}");
    g_assert_cmpint(qtest_inb(qts, COM1), ==, 'q');
    t0 = g_get_monotonic_time();
    qtest_qmp_send(qts, "{'execute': 'serial-run', 'arguments':"
                   " {'command': 'true'}}");
    while (!(qtest_inb(qts, COM1 + UART_LSR) & UART_LSR_DR)) {
        g_assert_cmpint(g_get_monotonic_time() - t0, <, 10 * G_USEC_PER_SEC);
        g_usleep(1000);
    }
    g_assert_cmpint(g_get_monotonic_time() - t0, >=, 80 * 1000);
    g_free(shell_answer(qts, "@0__\r\n$ "));
    qobject_unref(wait_reply(qts));

    /* a command that is still running at the timeout */
    rsp = run(qts, "{'command': 'sleep 99', 'timeout-ms': 300}", "", NULL);
    ret = qdict_get_qdict(rsp, "return");
    g_assert(!qdict_get_bool(ret, "done"));
    g_assert(qdict_haskey(ret, "end-pattern"));
    qobject_unref(rsp);

    /* a line longer than the guest takes is not typed at all */
    rsp = qtest_qmp(qts, "{'execute': 'serial-run', 'arguments':"
                    " {'command': %s}}", too_long);
    g_assert(qdict_haskey(rsp, "error"));
    qobject_unref(rsp);
    g_assert(!(qtest_inb(qts, COM1 + UART_LSR) & UART_LSR_DR));

    qtest_quit(qts);
}

/*
 * A serial-run cancelled while it types still types the whole line: half
 * a line would be glued to the next command.  The next serial-run waits
 * for its turn, and for the end of the command left running.
 */
static void test_serial_run_cancel(void)
{
    g_autofree char *dir = g_dir_make_tmp("mcp-test-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "mcp.sock", NULL);
    const char *typed = "make world; echo ";
    gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    g_autofree char *answer = NULL;
    char *line, *marker;
    QTestState *qts;
    QDict *rsp, *ret;
    int fd, i;

    qts = qtest_initf("-machine pc -display none -serial null "
                      "-mcp unix:%s,server=on,wait=off", path);
    fd = mcp_connect(path);
    mcp_send(fd, "{\"jsonrpc\": \"2.0\", \"id\": 1, \"method\": "
             "\"initialize\", \"params\": {\"protocolVersion\": "
             "\"2025-11-25\", \"capabilities\": {}, \"clientInfo\": "
             "{\"name\": \"mcp-test\", \"version\": \"1\"}}}");
    g_free(mcp_recv(fd));
    mcp_send(fd, "{\"jsonrpc\": \"2.0\", \"id\": 2, \"method\": "
             "\"tools/call\", \"params\": {\"name\": \"serial-run\", "
             "\"arguments\": {\"command\": \"make world\"}}}");
    mcp_send(fd, "{\"jsonrpc\": \"2.0\", \"method\": "
             "\"notifications/cancelled\", \"params\": {\"requestId\": 2}}");

    /* once it types, start a second call */
    while (!(qtest_inb(qts, COM1 + UART_LSR) & UART_LSR_DR)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_qmp_send(qts, "{'execute': 'serial-run', 'arguments':"
                   " {'command': 'true'}}");

    line = uart_gets(qts);
    g_assert(g_str_has_prefix(line, typed));
    g_assert(g_str_has_suffix(line, "_$?__"));
    marker = g_strndup(line + strlen(typed),
                       strlen(line) - strlen(typed) - strlen("$?__"));
    g_free(line);

    /* the second call types nothing while "make world" runs */
    for (i = 0; i < 100; i++) {
        g_assert(!(qtest_inb(qts, COM1 + UART_LSR) & UART_LSR_DR));
        g_usleep(1000);
    }
    answer = g_strdup_printf("%s0__\r\n$ ", marker);
    g_free(marker);
    uart_puts(qts, answer);

    line = shell_answer(qts, "@0__\r\n$ ");
    g_assert(g_str_has_prefix(line, "true; echo __lemu"));
    g_free(line);
    rsp = wait_reply(qts);
    ret = qdict_get_qdict(rsp, "return");
    g_assert(ret);
    g_assert_cmpint(qdict_get_int(ret, "exit-status"), ==, 0);
    g_assert_cmpint(qdict_get_int(ret, "lines"), ==, 0);
    qobject_unref(rsp);

    close(fd);
    qtest_quit(qts);
    unlink(path);
    rmdir(dir);
}

#define TN_IAC     "\xff"
#define TN_DONT    "\xfe"
#define TN_DO      "\xfd"
#define TN_WONT    "\xfc"
#define TN_WILL    "\xfb"
#define TN_SB      "\xfa"
#define TN_NOP     "\xf1"
#define TN_SE      "\xf0"
#define TN_ECHO    "\x01"
#define TN_SGA     "\x03"
#define TN_TTYPE   "\x18"
#define TN_NAWS    "\x1f"
#define TN_NEWENV  "\x27"

typedef struct TelnetServer {
    int fd;                     /* listens */
    int port;
    GThread *thread;
    char *failed;               /* what went wrong in the thread */
} TelnetServer;

static int telnet_listen(int *port, bool keep)
{
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    socklen_t len = sizeof(a);
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    g_assert(fd >= 0);
    g_assert(!bind(fd, (struct sockaddr *)&a, sizeof(a)));
    g_assert(!getsockname(fd, (struct sockaddr *)&a, &len));
    *port = ntohs(a.sin_port);
    if (!keep) {
        close(fd);              /* a port where nothing listens */
        return -1;
    }
    g_assert(!listen(fd, 1));
    return fd;
}

static void telnet_send(int fd, const char *data, size_t len)
{
    g_assert_cmpint(send(fd, data, len, 0), ==, len);
}

/* Read exactly what the client should send next */
static bool telnet_expect(TelnetServer *ts, int fd, const char *what,
                          const char *want, size_t len)
{
    g_autofree char *buf = g_malloc0(len + 1);
    size_t got = 0;

    while (got < len) {
        ssize_t n = recv(fd, buf + got, len - got, 0);

        if (n <= 0) {
            break;
        }
        got += n;
    }
    if (got != len || memcmp(buf, want, len)) {
        ts->failed = g_strdup_printf("%s: %zu of %zu bytes%s", what, got, len,
                                     got == len ? ", not the ones wanted"
                                                : "");
        return false;
    }
    return true;
}

static int telnet_accept(int lfd)
{
    struct timeval tv = { .tv_sec = 10 };
    int fd = accept(lfd, NULL, NULL);

    g_assert(fd >= 0);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

/* String literals, with the NULs in them */
#define TN_SEND(fd, s) telnet_send(fd, s, sizeof(s) - 1)
#define TN_EXPECT(ts, fd, what, s) telnet_expect(ts, fd, what, s, sizeof(s) - 1)

static gpointer telnet_server(gpointer opaque)
{
    TelnetServer *ts = opaque;
    char c;
    int fd = telnet_accept(ts->fd);

    /* what a BSD telnetd asks first, and an option nobody knows */
    TN_SEND(fd, TN_IAC TN_DO TN_TTYPE TN_IAC TN_WILL TN_ECHO
                TN_IAC TN_WILL TN_SGA TN_IAC TN_DO TN_NAWS
                TN_IAC TN_DO TN_NEWENV TN_IAC TN_WILL "\x63");
    if (!TN_EXPECT(ts, fd, "option answers",
                   TN_IAC TN_WILL TN_TTYPE TN_IAC TN_DO TN_ECHO
                   TN_IAC TN_DO TN_SGA TN_IAC TN_WONT TN_NAWS
                   TN_IAC TN_WONT TN_NEWENV TN_IAC TN_DONT "\x63")) {
        goto out;
    }
    /* asked again: no answer, or the two ends could loop */
    TN_SEND(fd, TN_IAC TN_WILL TN_ECHO
                TN_IAC TN_SB TN_TTYPE "\x01" TN_IAC TN_SE);
    if (!TN_EXPECT(ts, fd, "terminal type",
                   TN_IAC TN_SB TN_TTYPE "\x00" "VT100" TN_IAC TN_SE)) {
        goto out;
    }
    /* a CR NUL, a doubled IAC and a NOP in the text */
    TN_SEND(fd, "line1\r\na\r\0b" TN_IAC TN_IAC "c" TN_IAC TN_NOP
                "\r\nlogin: ");
    if (!TN_EXPECT(ts, fd, "a lone CR", "root\r\0") ||
        !TN_EXPECT(ts, fd, "CR LF and IAC", "a\r\nb" TN_IAC TN_IAC) ||
        !TN_EXPECT(ts, fd, "one byte at a time", "x\r\ny\r\0")) {
        goto out;
    }
    TN_SEND(fd, "bye\r\n");
    close(fd);

    /* the second session ends from the client's side */
    fd = telnet_accept(ts->fd);
    TN_SEND(fd, "again\r\nlogin: ");
    if (recv(fd, &c, 1, 0) != 0) {
        ts->failed = g_strdup("serial-connect close left the session open");
    }
out:
    close(fd);
    return NULL;
}

static QDict *telnet_call(QTestState *qts, const char *cmd, QDict *args,
                          char **error)
{
    QDict *rsp = qtest_qmp(qts, "{'execute': %s, 'arguments': %p}", cmd,
                           args);
    QDict *ret = qdict_get_qdict(rsp, "return");
    QDict *err = qdict_get_qdict(rsp, "error");

    *error = err ? g_strdup(qdict_get_str(err, "desc")) : NULL;
    if (ret) {
        g_test_message("%s: %s", cmd, qdict_get_str(ret, "summary"));
        qobject_ref(ret);
    }
    qobject_unref(rsp);
    return ret;
}

static QDict *telnet_ok(QTestState *qts, const char *cmd, QDict *args)
{
    g_autofree char *err = NULL;
    QDict *ret = telnet_call(qts, cmd, args, &err);

    if (err) {
        g_test_message("%s: %s", cmd, err);
        g_assert_not_reached();
    }
    return ret;
}

static void telnet_error(QTestState *qts, const char *cmd, QDict *args,
                         const char *want)
{
    g_autofree char *err = NULL;

    g_assert(!telnet_call(qts, cmd, args, &err));
    if (!strstr(err, want)) {
        g_test_message("%s: '%s' lacks '%s'", cmd, err, want);
        g_assert_not_reached();
    }
}

static QDict *telnet_args(const char *key, const char *val, const char *key2,
                          const char *val2)
{
    QDict *args = qdict_new();

    qdict_put_str(args, key, val);
    if (key2) {
        qdict_put_str(args, key2, val2);
    }
    return args;
}

static void telnet_summary(QDict *ret, const char *want)
{
    const char *sum = qdict_get_str(ret, "summary");

    if (!strstr(sum, want)) {
        g_test_message("'%s' lacks '%s'", sum, want);
        g_assert_not_reached();
    }
    qobject_unref(ret);
}

/* vm-info's list of serial ports, one per line */
static char *telnet_ports(QTestState *qts)
{
    QDict *rsp = qtest_qmp(qts, "{'execute': 'vm-info'}");
    QList *list = qdict_get_qlist(qdict_get_qdict(rsp, "return"), "serial");
    GString *s = g_string_new(NULL);
    QListEntry *e;

    QLIST_FOREACH_ENTRY(list, e) {
        g_string_append_printf(s, "%s\n",
                               qstring_get_str(qobject_to(QString,
                                                          e->value)));
    }
    qobject_unref(rsp);
    return g_string_free(s, false);
}

static void test_serial_telnet(void)
{
    TelnetServer ts = { 0 };
    QTestState *qts = qtest_init("-machine none -display none");
    g_autofree char *addr = NULL;
    g_autofree char *gone1 = NULL;
    g_autofree char *gone2 = NULL;
    g_autofree char *ports = NULL;
    g_autofree char *want = NULL;
    QDict *args, *ret;
    gint64 start;
    int port1, port2;

    ts.fd = telnet_listen(&ts.port, true);
    ts.thread = g_thread_new("telnet", telnet_server, &ts);
    addr = g_strdup_printf("%d", ts.port);

    ret = telnet_ok(qts, "serial-connect",
                    telnet_args("address", addr, NULL, NULL));
    g_assert_cmpstr(qdict_get_str(ret, "port"), ==, "telnet0");
    g_assert_cmpint(qdict_get_int(ret, "cursor"), ==, 0);
    telnet_summary(ret, "login: ");
    ret = telnet_ok(qts, "serial-expect",
                    telnet_args("port", "telnet0", "pattern", "login: $"));
    g_assert(qdict_get_bool(ret, "matched"));
    qobject_unref(ret);

    qobject_unref(telnet_ok(qts, "serial-write",
                            telnet_args("port", "telnet0", "text",
                                        "root\\r")));
    qobject_unref(telnet_ok(qts, "serial-write",
                            telnet_args("port", "telnet0", "text",
                                        "a\\r\\nb\\xff")));
    args = telnet_args("port", "telnet0", "text", "x\\r\\ny\\r");
    qdict_put_int(args, "char-delay-ms", 1);
    qobject_unref(telnet_ok(qts, "serial-write", args));

    /* the server says bye and hangs up: the wait ends at once */
    start = g_get_monotonic_time();
    args = telnet_args("port", "telnet0", "pattern", "never");
    qdict_put_int(args, "timeout-ms", 20000);
    ret = telnet_ok(qts, "serial-expect", args);
    g_assert(!qdict_get_bool(ret, "matched"));
    g_assert_cmpint(g_get_monotonic_time() - start, <, 10 * G_USEC_PER_SEC);
    telnet_summary(ret, "closed (the other end closed it)");
    telnet_error(qts, "serial-write",
                 telnet_args("port", "telnet0", "text", "ls\\r"),
                 "the telnet session on telnet0 is closed");

    /* telnet commands taken out, CR NUL made a lone CR, IAC IAC kept */
    args = telnet_args("port", "telnet0", NULL, NULL);
    qdict_put_int(args, "cursor", 0);
    qdict_put_bool(args, "strip-ansi", false);
    ret = telnet_ok(qts, "serial-read", args);
    telnet_summary(ret, "line1\r\na\rb\xef\xbf\xbd" "c\r\nlogin: bye\r\n");

    /* a new session on the same port, then closed from here */
    ret = telnet_ok(qts, "serial-connect",
                    telnet_args("address", addr, NULL, NULL));
    g_assert_cmpstr(qdict_get_str(ret, "port"), ==, "telnet0");
    telnet_summary(ret, "again");
    args = telnet_args("address", addr, NULL, NULL);
    qdict_put_bool(args, "close", true);
    telnet_summary(telnet_ok(qts, "serial-connect", args),
                   "closed the session");
    g_thread_join(ts.thread);
    close(ts.fd);
    if (ts.failed) {
        g_test_message("telnet server: %s", ts.failed);
        g_assert_not_reached();
    }

    /* nothing listens: a hint, and failed ports do not pile up */
    telnet_listen(&port1, false);
    gone1 = g_strdup_printf("127.0.0.1:%d", port1);
    telnet_error(qts, "serial-connect",
                 telnet_args("address", gone1, NULL, NULL), "hostfwd_add");
    telnet_listen(&port2, false);
    gone2 = g_strdup_printf("localhost:%d", port2);
    telnet_error(qts, "serial-connect",
                 telnet_args("address", gone2, NULL, NULL), "cannot connect");
    ports = telnet_ports(qts);
    want = g_strdup_printf("telnet0: telnet to 127.0.0.1:%d, closed "
                           "(serial-connect closed it)\ntelnet1: telnet to "
                           "localhost:%d, closed (", ts.port, port2);
    if (!strstr(ports, want) || strstr(ports, "telnet2")) {
        g_test_message("ports: %s", ports);
        g_assert_not_reached();
    }

    args = telnet_args("address", gone1, NULL, NULL);
    qdict_put_bool(args, "close", true);
    telnet_error(qts, "serial-connect", args, "no telnet session");
    telnet_error(qts, "serial-connect", telnet_args("address", "", NULL, NULL),
                 "is not host:port");
    qtest_quit(qts);
}

/* A VM with an MCP socket of its own, and a client that said hello */
typedef struct StepsVm {
    QTestState *qts;
    char *dir;
    char *path;
    int fd;
} StepsVm;

static void steps_vm_start(StepsVm *v, const char *args)
{
    v->dir = g_dir_make_tmp("mcp-test-XXXXXX", NULL);
    v->path = g_build_filename(v->dir, "mcp.sock", NULL);
    v->qts = qtest_initf("%s -display none -serial null "
                         "-mcp unix:%s,server=on,wait=off", args, v->path);
    v->fd = mcp_connect(v->path);
    mcp_send(v->fd, "{\"jsonrpc\": \"2.0\", \"id\": 1, \"method\": "
             "\"initialize\", \"params\": {\"protocolVersion\": "
             "\"2025-11-25\", \"capabilities\": {}, \"clientInfo\": "
             "{\"name\": \"mcp-test\", \"version\": \"1\"}}}");
    g_free(mcp_recv(v->fd));
}

static void steps_vm_stop(StepsVm *v)
{
    close(v->fd);
    qtest_quit(v->qts);
    unlink(v->path);
    rmdir(v->dir);
    g_free(v->path);
    g_free(v->dir);
}

/* Call "steps" with @steps (JSON) over MCP; the text and *@json blocks */
static char *steps_call(int fd, int id, const char *steps, bool *is_error,
                        bool *image, QDict **json)
{
    g_autofree char *req = g_strdup_printf(
        "{\"jsonrpc\": \"2.0\", \"id\": %d, \"method\": \"tools/call\", "
        "\"params\": {\"name\": \"steps\", \"arguments\": {\"steps\": %s}}}",
        id, steps);
    g_autofree char *line = NULL;
    QDict *rsp, *result;
    QList *content;
    QListEntry *e;
    char *text = NULL;

    mcp_send(fd, req);
    line = mcp_recv(fd);
    rsp = qobject_to(QDict, qobject_from_json(line, &error_abort));
    g_assert_cmpint(qdict_get_int(rsp, "id"), ==, id);
    result = qdict_get_qdict(rsp, "result");
    *is_error = qdict_get_bool(result, "isError");
    *image = false;
    *json = NULL;
    content = qdict_get_qlist(result, "content");
    QLIST_FOREACH_ENTRY(content, e) {
        QDict *b = qobject_to(QDict, qlist_entry_obj(e));
        const char *type = qdict_get_str(b, "type");

        if (!strcmp(type, "image")) {
            *image = true;
        } else if (!text) {
            text = g_strdup(qdict_get_str(b, "text"));
        } else if (!*json) {
            *json = qobject_to(QDict,
                               qobject_from_json(qdict_get_str(b, "text"),
                                                 &error_abort));
        }
    }
    g_test_message("steps: %s", text);
    qobject_unref(rsp);
    return text;
}

static void steps_has(const char *text, const char *want, bool has)
{
    if (!strstr(text, want) != !has) {
        g_test_message("'%s' %s '%s'", text, has ? "lacks" : "has", want);
        g_assert_not_reached();
    }
}

static bool uart_ready(QTestState *qts)
{
    return qtest_inb(qts, COM1 + UART_LSR) & UART_LSR_DR;
}

/*
 * "steps" runs tools in order in one call and stops at the first that
 * fails or whose wait is not met.  A bad tool name, or a step for
 * another VM, stops it before anything runs.
 */
static void test_steps(void)
{
    g_autofree char *mine = NULL;
    StepsVm v;
    QDict *json;
    bool is_error, image;
    char *text;

    steps_vm_start(&v, "-machine pc -name mine");

    /* the wait that is not met stops it: the hmp step does not run */
    text = steps_call(v.fd, 2, "[{\"tool\": \"vm-info\"}, "
                      "{\"tool\": \"serial-write\", \"args\": "
                      "{\"text\": \"x\", \"vm\": \"mine\"}}, "
                      "{\"tool\": \"serial-expect\", \"args\": "
                      "{\"pattern\": \"never\", \"timeout-ms\": 100}}, "
                      "{\"tool\": \"hmp\", \"args\": "
                      "{\"command\": \"info status\"}}]",
                      &is_error, &image, &json);
    g_assert(!is_error);
    g_assert(g_str_has_prefix(text, "stopped after step 3 of 4: no match, "
                              "so the rest did not run\n1. vm-info: "));
    steps_has(text, "\n2. serial-write: sent 1 bytes", true);
    steps_has(text, "\n3. serial-expect: serial0: no match", true);
    steps_has(text, "4. hmp", false);
    g_assert_cmpint(qdict_get_int(json, "steps-done"), ==, 3);
    g_assert_cmpint(qdict_get_int(json, "steps"), ==, 4);
    g_assert(!qdict_get_bool(qdict_get_qdict(json, "last"), "matched"));
    g_assert(uart_ready(v.qts));
    g_assert_cmpint(qtest_inb(v.qts, COM1), ==, 'x');
    qobject_unref(json);
    g_free(text);

    /* all of them: hmp text, and the screenshot's image */
    text = steps_call(v.fd, 3, "[{\"tool\": \"hmp\", \"args\": "
                      "{\"command\": \"info status\"}}, "
                      "{\"tool\": \"screenshot\"}]",
                      &is_error, &image, &json);
    g_assert(!is_error);
    g_assert(image);
    g_assert(g_str_has_prefix(text, "ran all 2 steps\n1. hmp: VM status: "
                              "running\n2. screenshot: "));
    g_assert_cmpint(qdict_get_int(json, "steps-done"), ==, 2);
    qobject_unref(json);
    g_free(text);

    /* the last step's wait is not met: it says so */
    text = steps_call(v.fd, 4, "[{\"tool\": \"serial-expect\", \"args\": "
                      "{\"pattern\": \"never\", \"timeout-ms\": 100}}]",
                      &is_error, &image, &json);
    g_assert(!is_error);
    g_assert(g_str_has_prefix(text, "step 1 of 1: no match\n"));
    qobject_unref(json);
    g_free(text);

    /* a typo: nothing runs */
    text = steps_call(v.fd, 5, "[{\"tool\": \"serial-write\", \"args\": "
                      "{\"text\": \"y\"}}, {\"tool\": \"kye\"}]",
                      &is_error, &image, &json);
    g_assert(is_error);
    g_assert_cmpstr(text, ==, "step 2: unknown tool 'kye'");
    g_assert(!uart_ready(v.qts));
    g_free(text);

    /* a step that fails stops the rest */
    text = steps_call(v.fd, 6, "[{\"tool\": \"serial-write\", \"args\": "
                      "{\"text\": \"z\"}}, {\"tool\": \"serial-write\"}, "
                      "{\"tool\": \"serial-write\", \"args\": "
                      "{\"text\": \"w\"}}]",
                      &is_error, &image, &json);
    g_assert(is_error);
    g_assert(g_str_has_prefix(text, "step 2 of 3 failed, so the rest did "
                              "not run\n1. serial-write: sent 1 bytes"));
    steps_has(text, "\n2. serial-write failed: Parameter 'text' is missing",
              true);
    g_assert_cmpint(qdict_get_int(json, "steps-done"), ==, 1);
    g_assert(!qdict_haskey(json, "last"));
    g_assert(uart_ready(v.qts));
    g_assert_cmpint(qtest_inb(v.qts, COM1), ==, 'z');
    g_assert(!uart_ready(v.qts));
    qobject_unref(json);
    g_free(text);

    /* a step for another VM: nothing runs; this VM's PID is fine */
    text = steps_call(v.fd, 7, "[{\"tool\": \"serial-write\", \"args\": "
                      "{\"text\": \"p\"}}, {\"tool\": \"serial-write\", "
                      "\"args\": {\"text\": \"q\", \"vm\": \"other\"}}]",
                      &is_error, &image, &json);
    g_assert(is_error);
    steps_has(text, "step 2: its vm is not this VM; all steps run on mine "
              "(pid ", true);
    g_assert(!uart_ready(v.qts));
    g_free(text);
    mine = g_strdup_printf("[{\"tool\": \"serial-write\", \"args\": "
                           "{\"text\": \"p\", \"vm\": %d}}]",
                           (int)qtest_pid(v.qts));
    text = steps_call(v.fd, 8, mine, &is_error, &image, &json);
    g_assert(!is_error);
    g_assert(uart_ready(v.qts));
    g_assert_cmpint(qtest_inb(v.qts, COM1), ==, 'p');
    qobject_unref(json);
    g_free(text);

    text = steps_call(v.fd, 9, "[{\"tool\": \"steps\"}]", &is_error, &image,
                      &json);
    g_assert(is_error);
    steps_has(text, "steps cannot hold steps", true);
    g_free(text);
    text = steps_call(v.fd, 10, "[{\"tool\": \"vm-wait\"}]", &is_error,
                      &image, &json);
    steps_has(text, "the bridge's own tools cannot be steps", true);
    g_free(text);
    text = steps_call(v.fd, 11, "[]", &is_error, &image, &json);
    g_assert(is_error);
    steps_has(text, "at least one step", true);
    g_free(text);

    steps_vm_stop(&v);
}

/*
 * A BIOS of three bytes at the reset vector: inc ax at 0xfffffff0,
 * then jmp short back to it at 0xfffffff1.  Halts everywhere else.
 */
static char *debug_bios(void)
{
    g_autofree uint8_t *rom = g_malloc(64 * KiB);
    char *path = NULL;
    int fd = g_file_open_tmp("mcp-bios-XXXXXX", &path, NULL);

    g_assert(fd >= 0);
    memset(rom, 0xf4, 64 * KiB);
    rom[0xfff0] = 0x40;
    rom[0xfff1] = 0xeb;
    rom[0xfff2] = 0xfd;
    g_assert_cmpint(write(fd, rom, 64 * KiB), ==, 64 * KiB);
    close(fd);
    return path;
}

static QDict *debug_ok(QTestState *qts, const char *cmd, const char *args)
{
    QDict *rsp = qtest_qmp(qts, "{'execute': %s, 'arguments': %p}", cmd,
                           qobject_from_json(args, &error_abort));
    QDict *ret = qdict_get_qdict(rsp, "return");

    if (!ret) {
        g_test_message("%s: %s", cmd,
                       qdict_get_str(qdict_get_qdict(rsp, "error"), "desc"));
    }
    g_assert(ret);
    qobject_ref(ret);
    qobject_unref(rsp);
    return ret;
}

static char *debug_error(QTestState *qts, const char *cmd, const char *args)
{
    QDict *rsp = qtest_qmp(qts, "{'execute': %s, 'arguments': %p}", cmd,
                           qobject_from_json(args, &error_abort));
    QDict *err = qdict_get_qdict(rsp, "error");
    char *desc;

    g_assert(err);
    desc = g_strdup(qdict_get_str(err, "desc"));
    qobject_unref(rsp);
    return desc;
}

/* A register from the summary: "EAX=0000002a" or "RAX=00000000..." */
static uint64_t debug_reg(QDict *ret, const char *name)
{
    const char *summary = qdict_get_str(ret, "summary");
    g_autofree char *key = g_strdup_printf("%s=", name);
    const char *p = strstr(summary, key);

    g_assert(p);
    return g_ascii_strtoull(p + strlen(key), NULL, 16);
}

static void debug_check_error(QTestState *qts, const char *cmd,
                              const char *args, const char *want)
{
    g_autofree char *desc = debug_error(qts, cmd, args);

    if (!strstr(desc, want)) {
        g_test_message("%s: '%s' lacks '%s'", cmd, desc, want);
        g_assert_not_reached();
    }
}

/* debug-step and debug-run walk the loop, from the first instruction */
static void test_debug_run(void)
{
    g_autofree char *bios = debug_bios();
    QTestState *qts;
    QDict *ret;

    qts = qtest_initf("-accel tcg -machine pc -nodefaults -vga none "
                      "-bios %s -S", bios);

    ret = debug_ok(qts, "debug-step", "{}");
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, "0xfffffff1");
    g_assert_cmpint(qdict_get_int(ret, "steps"), ==, 1);
    g_assert(qdict_get_bool(ret, "reached"));
    g_assert(!qdict_get_bool(ret, "running"));
    g_assert_cmpint(debug_reg(ret, "AX") & 0xffff, ==, 1);
    g_assert(strstr(qdict_get_str(ret, "summary"), "inc"));
    qobject_unref(ret);

    /* jmp, inc, jmp, inc */
    ret = debug_ok(qts, "debug-step", "{'count': 4}");
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, "0xfffffff1");
    g_assert_cmpint(qdict_get_int(ret, "steps"), ==, 4);
    g_assert_cmpint(debug_reg(ret, "AX") & 0xffff, ==, 3);
    qobject_unref(ret);

    /* the CPU stands at the address: it goes on to the next arrivals */
    ret = debug_ok(qts, "debug-run", "{'address': '0xfffffff1', 'hits': 3,"
                   " 'memory': [{'address': '0x1000', 'words': 2,"
                   " 'physical': true}]}");
    g_assert(qdict_get_bool(ret, "reached"));
    g_assert(!qdict_get_bool(ret, "running"));
    g_assert_cmpint(qdict_get_int(ret, "hits"), ==, 3);
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, "0xfffffff1");
    g_assert_cmpint(debug_reg(ret, "AX") & 0xffff, ==, 6);
    g_assert(strstr(qdict_get_str(ret, "summary"),
                    "Memory at physical 0x1000"));
    qobject_unref(ret);

    /* nothing gets there: the guest goes on */
    ret = debug_ok(qts, "debug-run", "{'address': '0x12345678',"
                   " 'timeout-ms': 200}");
    g_assert(!qdict_get_bool(ret, "reached"));
    g_assert(qdict_get_bool(ret, "running"));
    qobject_unref(ret);
    ret = debug_ok(qts, "query-status", "{}");
    g_assert(qdict_get_bool(ret, "running"));
    qobject_unref(ret);

    /* a running VM stops with the CPU at the address */
    ret = debug_ok(qts, "debug-run", "{'address': '0xfffffff0'}");
    g_assert(qdict_get_bool(ret, "reached"));
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, "0xfffffff0");
    g_assert_cmpint(qdict_get_int(ret, "cpu"), ==, 0);
    qobject_unref(ret);

    debug_check_error(qts, "debug-run", "{'address': 'fffffff0'}",
                      "not a number");
    debug_check_error(qts, "debug-step", "{'count': 0}", "count");
    debug_check_error(qts, "debug-step", "{'cpu': 5}", "no CPU 5");
    qtest_quit(qts);
    unlink(bios);
}

/*
 * memory-write changes RAM and ROM but no device, and code made from
 * the old bytes goes away; debug-profile finds the loop
 */
/*
 * A debug-run step that does not get to its address stops the steps,
 * and a debug-step that does all its steps lets them go on
 */
static void test_steps_debug(void)
{
    g_autofree char *bios = debug_bios();
    g_autofree char *args = g_strdup_printf("-accel tcg -machine pc -bios %s",
                                            bios);
    StepsVm v;
    QDict *json;
    bool is_error, image;
    char *text;

    steps_vm_start(&v, args);
    text = steps_call(v.fd, 2, "[{\"tool\": \"debug-run\", \"args\": "
                      "{\"address\": \"0x12345678\", \"timeout-ms\": 200}}, "
                      "{\"tool\": \"serial-write\", \"args\": "
                      "{\"text\": \"q\"}}]", &is_error, &image, &json);
    g_assert(!is_error);
    g_assert(g_str_has_prefix(text, "stopped after step 1 of 2: the CPU did "
                              "not get there, so the rest did not run\n"));
    g_assert(!qdict_get_bool(qdict_get_qdict(json, "last"), "reached"));
    g_assert(!uart_ready(v.qts));
    qobject_unref(json);
    g_free(text);

    text = steps_call(v.fd, 3, "[{\"tool\": \"debug-step\"}, "
                      "{\"tool\": \"vm-power\", \"args\": "
                      "{\"action\": \"resume\"}}, "
                      "{\"tool\": \"serial-write\", \"args\": "
                      "{\"text\": \"r\"}}]", &is_error, &image, &json);
    g_assert(!is_error);
    g_assert(g_str_has_prefix(text, "ran all 3 steps\n1. debug-step: "));
    g_assert(uart_ready(v.qts));
    g_assert_cmpint(qtest_inb(v.qts, COM1), ==, 'r');
    qobject_unref(json);
    g_free(text);

    steps_vm_stop(&v);
    unlink(bios);
}

static void test_debug_memory(void)
{
    g_autofree char *bios = debug_bios();
    QTestState *qts;
    QDict *ret, *top;
    uint64_t ax, cx;

    qts = qtest_initf("-accel tcg -machine pc -nodefaults -vga none "
                      "-bios %s", bios);

    ret = debug_ok(qts, "memory-write", "{'address': '0x1000',"
                   " 'bytes': 'de ad be ef', 'physical': true}");
    g_assert_cmpstr(qdict_get_str(ret, "before"), ==, "00000000");
    g_assert_cmpstr(qdict_get_str(ret, "after"), ==, "deadbeef");
    g_assert_cmphex(qtest_readl(qts, 0x1000), ==, 0xefbeadde);
    qobject_unref(ret);

    /* paging is off, so the virtual address is the physical one */
    ret = debug_ok(qts, "memory-write", "{'address': '8192',"
                   " 'bytes': '0x01020304'}");
    g_assert_cmpstr(qdict_get_str(ret, "after"), ==, "01020304");
    g_assert_cmphex(qtest_readl(qts, 0x2000), ==, 0x04030201);
    qobject_unref(ret);

    debug_check_error(qts, "memory-write", "{'address': '0xfec00000',"
                      " 'bytes': '00', 'physical': true}", "not RAM or ROM");
    debug_check_error(qts, "memory-write", "{'address': '0x1000',"
                      " 'bytes': 'abc'}", "whole bytes");
    debug_check_error(qts, "memory-write", "{'address': '0x1000',"
                      " 'bytes': 'zz'}", "not hex");

    /* the loop runs in one block at its start */
    ret = debug_ok(qts, "debug-profile", "{'interval-ms': 300}");
    g_assert_cmpint(qdict_get_int(ret, "samples"), >, 10);
    g_assert_cmpint(qdict_get_int(ret, "halted-percent"), ==, 0);
    top = qobject_to(QDict, qlist_peek(qdict_get_qlist(ret, "top")));
    g_assert(top);
    g_assert_cmpstr(qdict_get_str(top, "pc"), ==, "0xfffffff0");
    g_assert_cmpfloat(qdict_get_double(top, "percent"), >, 50);
    g_assert(strstr(qdict_get_str(top, "instruction"), "inc"));
    qobject_unref(ret);

    /* inc cx into the running loop: AX stops, CX counts */
    ret = debug_ok(qts, "memory-write", "{'address': '0xfffffff0',"
                   " 'bytes': '41', 'physical': true}");
    g_assert_cmpstr(qdict_get_str(ret, "before"), ==, "40");
    qobject_unref(ret);
    g_usleep(50 * 1000);
    ret = debug_ok(qts, "debug-run", "{'address': '0xfffffff0'}");
    ax = debug_reg(ret, "AX") & 0xffff;
    cx = debug_reg(ret, "CX") & 0xffff;
    qobject_unref(ret);
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    g_usleep(50 * 1000);
    ret = debug_ok(qts, "debug-run", "{'address': '0xfffffff0'}");
    g_assert_cmpint(debug_reg(ret, "AX") & 0xffff, ==, ax);
    g_assert_cmpint(debug_reg(ret, "CX") & 0xffff, !=, cx);
    qobject_unref(ret);

    /* a step into hlt halts; a halted CPU does not step */
    ret = debug_ok(qts, "memory-write", "{'address': '0xfffffff0',"
                   " 'bytes': 'f4', 'physical': true}");
    qobject_unref(ret);
    ret = debug_ok(qts, "debug-step", "{}");
    g_assert_cmpint(qdict_get_int(ret, "steps"), ==, 1);
    g_assert(strstr(qdict_get_str(ret, "summary"), "it halted"));
    qobject_unref(ret);
    ret = debug_ok(qts, "debug-step", "{}");
    g_assert_cmpint(qdict_get_int(ret, "steps"), ==, 0);
    g_assert(!qdict_get_bool(ret, "reached"));
    qobject_unref(ret);

    /* a paused VM runs no CPU */
    ret = debug_ok(qts, "debug-profile", "{'interval-ms': 100}");
    g_assert_cmpint(qdict_get_int(ret, "paused-percent"), ==, 100);
    g_assert_cmpint(qdict_get_int(ret, "samples"), ==, 0);
    qobject_unref(ret);
    qtest_quit(qts);
    unlink(bios);
}

#define MEDIA_ARGS  "-machine pc -display none -nodefaults" \
                    " -drive if=ide,index=2,media=cdrom" \
                    " -drive if=floppy,index=0"
#define ISO_SECTOR  2048

static const char *const media_names[] = {
    "readme.txt", "Long File Name.data", "run.sh", "docs", NULL
};

static void media_put(const char *dir, const char *name, const void *data,
                      size_t len)
{
    g_autofree char *path = g_build_filename(dir, name, NULL);

    g_assert(g_file_set_contents(path, data, len, NULL));
}

static uint8_t media_byte(int i)
{
    return i * 7 + 3;
}

/* A host folder with media_names in it */
static char *media_tree(void)
{
    char *dir = g_dir_make_tmp("mcp-media-XXXXXX", NULL);
    g_autofree char *docs = g_build_filename(dir, "docs", NULL);
    g_autofree char *deep = g_build_filename(docs, "deep", NULL);
    g_autofree char *run = g_build_filename(dir, "run.sh", NULL);
    uint8_t big[5000];
    int i;

    g_assert(dir);
    for (i = 0; i < sizeof(big); i++) {
        big[i] = media_byte(i);
    }
    g_assert(g_mkdir_with_parents(deep, 0755) == 0);
    media_put(dir, "readme.txt", "hello\n", 6);
    media_put(dir, "Long File Name.data", big, sizeof(big));
    media_put(dir, "run.sh", "#!/bin/sh\n", 10);
    g_assert(chmod(run, 0755) == 0);
    media_put(docs, "a.txt", "a\n", 2);
    media_put(docs, "empty", "", 0);
    media_put(deep, "x.c", "int x;\n", 7);
    return dir;
}

static void media_rmtree(const char *path)
{
    GDir *d = g_dir_open(path, 0, NULL);
    const char *e;

    if (!d) {
        g_assert(unlink(path) == 0);
        return;
    }
    while ((e = g_dir_read_name(d))) {
        g_autofree char *p = g_build_filename(path, e, NULL);

        media_rmtree(p);
    }
    g_dir_close(d);
    g_assert(rmdir(path) == 0);
}

/* @name is in @dir as it is, even on a host that ignores case */
static void media_check(const char *dir, const char *name, const void *want,
                        size_t len)
{
    g_autofree char *path = g_build_filename(dir, name, NULL);
    g_autofree char *base = g_path_get_basename(path);
    g_autofree char *up = g_path_get_dirname(path);
    g_autofree char *data = NULL;
    GDir *d = g_dir_open(up, 0, NULL);
    const char *e;
    bool found = false;
    gsize n;

    g_assert(d);
    while ((e = g_dir_read_name(d))) {
        found |= !strcmp(e, base);
    }
    g_dir_close(d);
    if (!found || !g_file_get_contents(path, &data, &n, NULL)) {
        g_test_message("copy-out did not make %s", path);
        g_assert_not_reached();
    }
    g_assert_cmpint(n, ==, len);
    g_assert(!memcmp(data, want, len));
}

/* Run "media" with @args; its return, or NULL and the error in @error */
static QDict *media(QTestState *qts, QDict *args, char **error)
{
    QDict *rsp = qtest_qmp(qts, "{'execute': 'media', 'arguments': %p}",
                           args);
    QDict *ret = qdict_get_qdict(rsp, "return");
    QDict *err = qdict_get_qdict(rsp, "error");

    if (error) {
        *error = err ? g_strdup(qdict_get_str(err, "desc")) : NULL;
    } else if (err) {
        g_test_message("media: %s", qdict_get_str(err, "desc"));
        g_assert_not_reached();
    }
    if (ret) {
        g_test_message("media: %s", qdict_get_str(ret, "summary"));
        qobject_ref(ret);
    }
    qobject_unref(rsp);
    return ret;
}

static QDict *media_args(const char *action, const char *device)
{
    QDict *args = qdict_new();

    qdict_put_str(args, "action", action);
    if (device) {
        qdict_put_str(args, "device", device);
    }
    return args;
}

static void media_error(QTestState *qts, QDict *args, const char *want)
{
    g_autofree char *err = NULL;

    g_assert(!media(qts, args, &err));
    if (!strstr(err, want)) {
        g_test_message("media: '%s' lacks '%s'", err, want);
        g_assert_not_reached();
    }
}

static void media_summary(QDict *ret, const char *want)
{
    if (!strstr(qdict_get_str(ret, "summary"), want)) {
        g_test_message("media: no '%s' in the summary", want);
        g_assert_not_reached();
    }
}

/* Insert a medium made from media_names in @dir */
static QDict *media_insert(QTestState *qts, const char *device,
                           const char *dir)
{
    QDict *args = media_args("insert", device);
    QList *files = qlist_new();
    const char *const *n;

    for (n = media_names; *n; n++) {
        g_autofree char *p = g_build_filename(dir, *n, NULL);

        qlist_append_str(files, p);
    }
    qdict_put(args, "files", files);
    return media(qts, args, NULL);
}

static QDict *media_copy_out(QTestState *qts, const char *to, char **error)
{
    QDict *args = media_args("copy-out", "floppy0");

    qdict_put_str(args, "to", to);
    return media(qts, args, error);
}

/* The image file in @device */
static char *media_file(QTestState *qts, const char *device)
{
    QDict *ret = media(qts, media_args("list", NULL), NULL);
    QListEntry *e;
    char *file = NULL;

    QLIST_FOREACH_ENTRY(qdict_get_qlist(ret, "drives"), e) {
        QDict *d = qobject_to(QDict, qlist_entry_obj(e));

        if (!strcmp(qdict_get_str(d, "device"), device)) {
            file = g_strdup(qdict_get_try_str(d, "file"));
        }
    }
    qobject_unref(ret);
    g_assert(file);
    return file;
}

/* The System Use entry @sig (Rock Ridge) of the disc record @r */
static const uint8_t *iso_su(const uint8_t *r, const char *sig)
{
    int i = 33 + r[32] + !(r[32] & 1);

    while (i + 4 <= r[0] && r[i + 2] >= 4) {
        if (!memcmp(r + i, sig, 2)) {
            return r + i;
        }
        i += r[i + 2];
    }
    return NULL;
}

/*
 * The record of @name in the folder of record @dir: by its Rock Ridge
 * name if @rr, else by its ISO 9660 name without ";1", as guests do
 */
static const uint8_t *iso_find(const uint8_t *iso, const uint8_t *dir,
                               const char *name, bool rr)
{
    uint32_t lba = ldl_le_p(dir + 2), size = ldl_le_p(dir + 10), off = 0;

    while (off < size) {
        const uint8_t *r = iso + lba * ISO_SECTOR + off;
        const uint8_t *nm = rr ? iso_su(r, "NM") : NULL;
        char id[256];
        char *end;

        if (!r[0]) {
            off = ROUND_UP(off + 1, ISO_SECTOR);
            continue;
        }
        if (rr) {
            snprintf(id, sizeof(id), "%.*s", nm ? nm[2] - 5 : 0,
                     nm ? (const char *)nm + 5 : "");
        } else {
            snprintf(id, sizeof(id), "%.*s", r[32], r + 33);
            end = strstr(id, ";1");
            if (end) {
                *end = 0;
                if (end > id && end[-1] == '.') {
                    end[-1] = 0;
                }
            }
        }
        if (!strcmp(id, name)) {
            return r;
        }
        off += r[0];
    }
    return NULL;
}

/* The disc names every guest reads, and the real names for Rock Ridge */
static void test_media_disc(void)
{
    g_autofree char *dir = media_tree();
    QTestState *qts = qtest_init(MEDIA_ARGS);
    g_autofree char *file = NULL;
    g_autofree uint8_t *iso = NULL;
    const uint8_t *pvd, *root, *r, *docs, *deep, *e, *pt;
    uint32_t off;
    gsize len;
    QDict *ret;
    int i, n;

    ret = media_insert(qts, "ide1-cd0", dir);
    media_summary(ret, "a disc lemu made from 6 files and 2 folders");
    media_summary(ret, "(read-only)");
    media_summary(ret, "LONG_FIL.DAT (Long File Name.data)");
    qobject_unref(ret);
    file = media_file(qts, "ide1-cd0");
    g_assert(g_file_get_contents(file, (char **)&iso, &len, NULL));
    g_assert_cmpint(len % ISO_SECTOR, ==, 0);

    pvd = iso + 16 * ISO_SECTOR;
    g_assert(pvd[0] == 1 && !memcmp(pvd + 1, "CD001", 5));
    g_assert_cmpint(ldl_le_p(pvd + 80), ==, len / ISO_SECTOR);
    g_assert_cmpint(ldl_be_p(pvd + 84), ==, len / ISO_SECTOR);
    g_assert_cmpint(iso[17 * ISO_SECTOR], ==, 255);
    root = pvd + 156;

    /* Rock Ridge says it is there in the "." record of the top folder */
    r = iso + ldl_le_p(root + 2) * ISO_SECTOR;
    g_assert(r[32] == 1 && r[33] == 0);
    e = iso_su(r, "SP");
    g_assert(e && e[4] == 0xbe && e[5] == 0xef);

    r = iso_find(iso, root, "readme.txt", true);
    g_assert(r && r == iso_find(iso, root, "README.TXT", false));
    g_assert_cmpint(ldl_le_p(r + 10), ==, 6);
    g_assert(!memcmp(iso + ldl_le_p(r + 2) * ISO_SECTOR, "hello\n", 6));

    r = iso_find(iso, root, "Long File Name.data", true);
    g_assert(r && r == iso_find(iso, root, "LONG_FIL.DAT", false));
    g_assert_cmpint(ldl_le_p(r + 10), ==, 5000);
    for (i = 0; i < 5000; i++) {
        g_assert(iso[ldl_le_p(r + 2) * ISO_SECTOR + i] == media_byte(i));
    }

    r = iso_find(iso, root, "run.sh", true);
    e = r ? iso_su(r, "PX") : NULL;
    g_assert(e && ldl_le_p(e + 4) == 0100755);

    docs = iso_find(iso, root, "docs", true);
    g_assert(docs && (docs[25] & 2) && docs == iso_find(iso, root, "DOCS",
                                                         false));
    deep = iso_find(iso, docs, "deep", true);
    g_assert(deep && (deep[25] & 2));
    r = iso_find(iso, deep, "x.c", true);
    g_assert(r && r == iso_find(iso, deep, "X.C", false));
    g_assert(!memcmp(iso + ldl_le_p(r + 2) * ISO_SECTOR, "int x;\n", 7));
    /* an empty file has a sector of its own, or AIX hides it */
    r = iso_find(iso, docs, "empty", true);
    g_assert(r && r == iso_find(iso, docs, "EMPTY", false));
    g_assert_cmpint(ldl_le_p(r + 10), ==, 0);
    g_assert_cmpint(ldl_le_p(r + 2), !=, 0);
    g_assert_cmpint(ldl_le_p(r + 2), !=,
                    ldl_le_p(iso_find(iso, docs, "a.txt", true) + 2));

    /* the path table has the 3 folders, the top one first */
    pt = iso + ldl_le_p(pvd + 140) * ISO_SECTOR;
    for (off = 0, n = 0; off < ldl_le_p(pvd + 132); n++) {
        off += 8 + pt[off] + (pt[off] & 1);
    }
    g_assert_cmpint(n, ==, 3);
    g_assert_cmpint(ldl_le_p(pt + 2), ==, ldl_le_p(root + 2));

    media_error(qts, media_args("insert", "ide1-cd0"), "'file' or 'files'");
    ret = media_args("insert", "ide1-cd0");
    qdict_put(ret, "files", qlist_new());
    media_error(qts, ret, "a disc needs something");
    ret = media_args("copy-out", "ide1-cd0");
    qdict_put_str(ret, "to", dir);
    media_error(qts, ret, "is a CD-ROM drive");

    /* lemu deletes the disc when the drive is emptied */
    qobject_unref(media(qts, media_args("eject", "ide1-cd0"), NULL));
    g_assert(!g_file_test(file, G_FILE_TEST_EXISTS));
    qtest_quit(qts);
    media_rmtree(dir);
}

static uint8_t media_lfn_sum(const char *name83)
{
    uint8_t s = 0;
    int i;

    for (i = 0; i < 11; i++) {
        s = ((s & 1) << 7) + (s >> 1) + name83[i];
    }
    return s;
}

/* Part @seq of the long name @name, the way Windows writes it */
static void media_lfn(uint8_t *e, int seq, bool last, const char *name,
                      uint8_t sum)
{
    static const uint8_t at[13] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
    };
    size_t len = strlen(name), i;
    int k;

    memset(e, 0, 32);
    e[0] = seq | (last ? 0x40 : 0);
    e[11] = 0x0f;
    e[13] = sum;
    for (k = 0; k < 13; k++) {
        i = (seq - 1) * 13 + k;
        stw_le_p(e + at[k], i < len ? name[i] : i == len ? 0 : 0xffff);
    }
}

/* Set FAT12 entry @n of the first FAT to @v */
static void media_fat12(int fd, uint32_t n, uint16_t v)
{
    uint8_t p[2];
    off_t off = 512 + n * 3 / 2;

    g_assert(pread(fd, p, 2, off) == 2);
    if (n & 1) {
        p[0] = (p[0] & 0x0f) | (v << 4);
        p[1] = v >> 4;
    } else {
        p[0] = v;
        p[1] = (p[1] & 0xf0) | ((v >> 8) & 0x0f);
    }
    g_assert(pwrite(fd, p, 2, off) == 2);
}

/*
 * A floppy from host files, its files back from copy-out, also a file
 * the "guest" (this test) wrote with a long name, and a damaged FAT
 */
static void test_media_floppy(void)
{
    static const char sname[] = "LONGNA~1TXT";
    g_autofree char *dir = media_tree();
    g_autofree char *out = g_dir_make_tmp("mcp-media-XXXXXX", NULL);
    g_autofree char *out2 = g_dir_make_tmp("mcp-media-XXXXXX", NULL);
    g_autofree char *out3 = g_dir_make_tmp("mcp-media-XXXXXX", NULL);
    QTestState *qts = qtest_init(MEDIA_ARGS);
    g_autofree char *file = NULL;
    g_autofree char *err = NULL;
    uint8_t b[512], top[224 * 32], big[5000], *e;
    int fd, i, docs = -1, free_slot = -1;
    QDict *ret;

    ret = media_insert(qts, "floppy0", dir);
    media_summary(ret, "a floppy lemu made from 6 files and 2 folders");
    media_summary(ret, "LONG_FIL.DAT (Long File Name.data)");
    media_summary(ret, "copy-out");
    qobject_unref(ret);
    file = media_file(qts, "floppy0");

    /* a DOS 1.44 MB floppy */
    fd = open(file, O_RDWR);
    g_assert(fd >= 0);
    g_assert(pread(fd, b, sizeof(b), 0) == sizeof(b));
    g_assert(b[510] == 0x55 && b[511] == 0xaa);
    g_assert_cmpint(lduw_le_p(b + 11), ==, 512);
    g_assert_cmpint(lduw_le_p(b + 19), ==, 2880);
    g_assert_cmpint(b[21], ==, 0xf0);

    /* copy-out gives the files back, in lower case where NT shows that */
    ret = media_copy_out(qts, out, NULL);
    media_summary(ret, "copied 6 files and 2 folders");
    qobject_unref(ret);
    for (i = 0; i < sizeof(big); i++) {
        big[i] = media_byte(i);
    }
    media_check(out, "readme.txt", "hello\n", 6);
    media_check(out, "LONG_FIL.DAT", big, sizeof(big));
    media_check(out, "run.sh", "#!/bin/sh\n", 10);
    media_check(out, "docs/a.txt", "a\n", 2);
    media_check(out, "docs/empty", "", 0);
    media_check(out, "docs/deep/x.c", "int x;\n", 7);

    /* the guest writes "A long name.txt" into cluster 2000 */
    g_assert(pread(fd, top, sizeof(top), 19 * 512) == sizeof(top));
    for (i = 0; i < 224 && free_slot < 0; i++) {
        if (!memcmp(top + i * 32, "DOCS       ", 11)) {
            docs = lduw_le_p(top + i * 32 + 26);
        }
        if (!top[i * 32]) {
            free_slot = i;
        }
    }
    g_assert(docs >= 2 && free_slot >= 0);
    e = top + free_slot * 32;
    media_lfn(e, 2, true, "A long name.txt", media_lfn_sum(sname));
    media_lfn(e + 32, 1, false, "A long name.txt", media_lfn_sum(sname));
    memset(e + 64, 0, 32);
    memcpy(e + 64, sname, 11);
    e[64 + 11] = 0x20;
    stw_le_p(e + 64 + 26, 2000);
    stl_le_p(e + 64 + 28, 5);
    g_assert(pwrite(fd, e, 96, 19 * 512 + free_slot * 32) == 96);
    g_assert(pwrite(fd, "long\n", 5, (33 + 2000 - 2) * 512) == 5);
    media_fat12(fd, 2000, 0xfff);
    qobject_unref(media_copy_out(qts, out2, NULL));
    media_check(out2, "A long name.txt", "long\n", 5);
    media_check(out2, "readme.txt", "hello\n", 6);

    /* a FAT that loops a folder onto itself stops copy-out */
    media_fat12(fd, docs, docs);
    g_assert(!media_copy_out(qts, out3, &err));
    g_assert(strstr(err, "damaged"));
    close(fd);

    media_error(qts, media_args("copy-out", "floppy0"), "'to' is needed");
    ret = media_args("insert", "floppy0");
    qdict_put_str(ret, "file", file);
    qdict_put(ret, "files", qlist_new());
    media_error(qts, ret, "not both");

    /* and at exit */
    qtest_quit(qts);
    g_assert(!g_file_test(file, G_FILE_TEST_EXISTS));
    media_rmtree(dir);
    media_rmtree(out);
    media_rmtree(out2);
    media_rmtree(out3);
}

/* Run the emulator as the bridge, with @input on stdin; returns stdout */
static char *bridge_run(const char *arg, const char *input)
{
    const char *script = "printf '%s\\n' \"$2\" | \"$0\" -mcp-bridge $1";
    char *argv[] = {
        (char *)"/bin/sh", (char *)"-c", (char *)script,
        (char *)g_getenv("QTEST_QEMU_BINARY"), (char *)arg, (char *)input,
        NULL
    };
    g_autoptr(GError) err = NULL;
    char *out = NULL;
    int status;

    g_assert(g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                          &out, NULL, &status, &err));
    g_assert_no_error(err);
    g_assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return out;
}

/*
 * Every system emulator is also the lemu-mcp bridge, so the kits need no
 * extra file.  initialize does not look for VMs, so this touches none.
 */
static void test_bridge(void)
{
    g_autofree char *usage = bridge_run("--help", "");
    g_autofree char *out = bridge_run("", "{\"jsonrpc\": \"2.0\", \"id\": 1,"
        " \"method\": \"initialize\", \"params\": {\"protocolVersion\":"
        " \"2025-06-18\", \"capabilities\": {}, \"clientInfo\": {\"name\":"
        " \"t\", \"version\": \"1\"}}}");
    g_autoptr(QDict) reply = NULL;
    QDict *result;

    g_assert(strstr(usage, " -mcp-bridge\n"));
    g_assert(strchr(out, '\n'));
    *strchr(out, '\n') = 0;
    reply = qobject_to(QDict, qobject_from_json(out, &error_abort));
    result = qdict_get_qdict(reply, "result");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(result, "serverInfo"),
                                  "name"), ==, "lemu");
    g_assert(strstr(qdict_get_str(result, "instructions"), "screenshot"));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mcp/bridge", test_bridge);

    qtest_add_func("/mcp/keys", test_keys);
    qtest_add_func("/mcp/screen/change", test_screen_change);
    qtest_add_func("/mcp/screen/blink", test_screen_blink);
    qtest_add_data_func("/mcp/input/quit-key",
                        "{\"execute\": \"key\", \"arguments\":"
                        " {\"keys\": [\"a\", \"b\", \"c\"]}}",
                        test_input_quit);
    qtest_add_data_func("/mcp/input/quit-mouse",
                        "{\"execute\": \"mouse-move\", \"arguments\":"
                        " {\"x\": 100, \"y\": 100}}",
                        test_input_quit);
    qtest_add_func("/mcp/pointer/slow", test_pointer_slow);
    qtest_add_func("/mcp/pointer/fast", test_pointer_fast);
    qtest_add_func("/mcp/pointer/accel", test_pointer_accel);
    qtest_add_func("/mcp/vm-stats", test_vm_stats);
    qtest_add_func("/mcp/serial/expect", test_serial_expect);
    qtest_add_func("/mcp/serial/run", test_serial_run);
    qtest_add_func("/mcp/serial/run-cancel", test_serial_run_cancel);
    qtest_add_func("/mcp/serial/telnet", test_serial_telnet);
    qtest_add_func("/mcp/steps", test_steps);
    qtest_add_func("/mcp/media/disc", test_media_disc);
    qtest_add_func("/mcp/media/floppy", test_media_floppy);
    if (qtest_has_accel("tcg")) {
        qtest_add_func("/mcp/debug/run", test_debug_run);
        qtest_add_func("/mcp/debug/memory", test_debug_memory);
        qtest_add_func("/mcp/steps/debug", test_steps_debug);
    }
    return g_test_run();
}
