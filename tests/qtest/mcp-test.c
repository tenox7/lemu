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
 * The text test waits for SeaBIOS's words on its text screen, also for
 * a line highlighted the way menus show the chosen one.
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
    QDict *rsp, *ret, *wait;
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
    wait = qdict_get_qdict(ret, "wait");
    matched = qdict_get_bool(wait, "matched");
    if (!matched) {
        /* it says what kept changing */
        QDict *last = qdict_get_qdict(wait, "last-box");

        g_assert_cmpint(qdict_get_int(wait, "changes"), >=, 3);
        g_assert_cmpint(qdict_get_int(last, "x"), ==, 200);
        g_assert_cmpint(qdict_get_int(last, "y"), ==, 200);
        g_assert_cmpint(qdict_get_int(last, "w"), ==, 8);
        g_assert_cmpint(qdict_get_int(last, "h"), ==, 16);
        g_assert(strstr(qdict_get_str(ret, "summary"),
                        "ms ago at 200,200 8x16 (to skip that area"));
    }
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
    bool hw;                    /* an ATI card's hardware cursor instead */
    QPCIBar mmio;
    uint32_t img;               /* hw: the cursor image shown */
    double fx, fy;              /* the hotspot */
    int x, y;                   /* the hotspot as drawn */
    uint8_t pkt[3];
    int npkt;
    QDict *reply;
} FakeGuest;

/*
 * A guest with a hardware cursor on an ATI card, for the closed loop.
 * Like X, it loads another cursor image for each window and puts the
 * image at the pointer less that image's hotspot: left of the middle
 * an arrow with its hotspot at 2,1, right of it a text cursor with its
 * hotspot at 17,16, and between them a window border 8 px wide with a
 * resize cursor, hotspot 10,6.  A scrollbar 8 px wide runs down the
 * right edge of the top half, with a cursor whose hotspot is at 17,20.
 * The card reports only where the image is.
 */
#define ATI_CRTC_GEN_CNTL       0x0050
#define ATI_CUR_OFFSET          0x0260
#define ATI_CUR_HORZ_VERT_POSN  0x0264
#define ATI_CUR_HORZ_VERT_OFF   0x0268
#define ATI_CRTC_ON             0x03010600  /* 32 bpp, CRTC and cursor on */
#define ATI_IMG_ARROW           0x200000    /* the images in VRAM */
#define ATI_IMG_TEXT            0x200400
#define ATI_IMG_RESIZE          0x200800
#define ATI_IMG_SCROLL          0x200c00
#define ATI_IMG_SIZE            1024        /* 64x64, 2 bits a pixel */
#define BORDER_X                (SCR_W / 2 - 4)
#define BORDER_W                8
#define SCROLL_W                8

static void fake_hw_cursor(FakeGuest *g)
{
    int px = lround(g->fx), py = lround(g->fy);
    uint32_t img = ATI_IMG_ARROW;
    int x = px - 2, y = py - 1;
    int xoff, yoff;

    if (px >= BORDER_X && px < BORDER_X + BORDER_W) {
        img = ATI_IMG_RESIZE;
        x = px - 10;
        y = py - 6;
    } else if (px >= SCR_W - SCROLL_W && py < SCR_H / 2) {
        img = ATI_IMG_SCROLL;
        x = px - 17;
        y = py - 20;
    } else if (px >= SCR_W / 2) {
        img = ATI_IMG_TEXT;
        x = px - 17;
        y = py - 16;
    }
    xoff = MAX(0, -x);
    yoff = MAX(0, -y);

    if (img != g->img) {
        qpci_io_writel(g->s.dev, g->mmio, ATI_CUR_OFFSET, img);
        g->img = img;
    }
    qpci_io_writel(g->s.dev, g->mmio, ATI_CUR_HORZ_VERT_OFF,
                   xoff << 16 | yoff);
    qpci_io_writel(g->s.dev, g->mmio, ATI_CUR_HORZ_VERT_POSN,
                   (x + xoff) << 16 | (y + yoff));
    g->x = px;
    g->y = py;
}

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
    if (g->hw) {
        fake_hw_cursor(g);
    } else if (lround(g->fx) != g->x || lround(g->fy) != g->y) {
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

/* Enable the mouse port, then the mouse, and take its ACK */
static void fake_mouse_on(FakeGuest *g)
{
    uint8_t st;

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

static void fake_init(FakeGuest *g, double gain, int accel)
{
    memset(g, 0, sizeof(*g));
    g->gain = gain;
    g->accel = accel;
    screen_init_machine(&g->s, "pc,vmport=off");
    g->x = g->y = -PTR_SIZE;
    g->fx = SCR_W / 2.0;
    g->fy = SCR_H / 2.0;
    fake_draw(g, SCR_W / 2, SCR_H / 2);
    fake_mouse_on(g);
}

static void fake_init_hw(FakeGuest *g, double gain, int accel)
{
    uint8_t img[ATI_IMG_SIZE];
    Screen *s = &g->s;

    memset(g, 0, sizeof(*g));
    g->gain = gain;
    g->accel = accel;
    g->hw = true;
    s->qts = qtest_init("-machine pc,vmport=off -nodefaults -display none"
                        " -device ati-vga,addr=02.0");
    s->bus = qpci_new_pc(s->qts, NULL);
    s->dev = qpci_device_find(s->bus, QPCI_DEVFN(2, 0));
    g_assert(s->dev);
    qpci_device_enable(s->dev);
    s->lfb = qpci_iomap(s->dev, 0, NULL);
    g->mmio = qpci_iomap(s->dev, 2, NULL);

    /* four images that differ, then 640x480 with the cursor on */
    memset(img, 0xaa, sizeof(img));
    qpci_memwrite(s->dev, s->lfb, ATI_IMG_ARROW, img, sizeof(img));
    memset(img, 0x55, sizeof(img));
    qpci_memwrite(s->dev, s->lfb, ATI_IMG_TEXT, img, sizeof(img));
    memset(img, 0x33, sizeof(img));
    qpci_memwrite(s->dev, s->lfb, ATI_IMG_RESIZE, img, sizeof(img));
    memset(img, 0xcc, sizeof(img));
    qpci_memwrite(s->dev, s->lfb, ATI_IMG_SCROLL, img, sizeof(img));
    qpci_io_writel(s->dev, g->mmio, ATI_CRTC_GEN_CNTL, ATI_CRTC_ON);
    qtest_inb(s->qts, 0x3da);
    qtest_outb(s->qts, 0x3c0, 0x20);

    g->fx = SCR_W / 2.0;
    g->fy = SCR_H / 2.0;
    fake_hw_cursor(g);
    fake_mouse_on(g);
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
    g_assert(!strstr(summary, "could not be measured"));
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

/*
 * The closed loop on a hardware cursor whose image, and so hotspot,
 * changes with the window under the pointer.  Homing measures the
 * hotspot of the text cursor in the bottom-right corner; the arrow's
 * must be measured on the way into the left half, or every click
 * there lands 15 px off.  Moves that cross the border must not stop
 * at its resize cursor; a move onto the border measures that one.  At
 * the scrollbar on the right edge the guest stops the pointer short,
 * so a step there must not be used to measure its cursor.
 */
static void test_pointer_hw_cursor(void)
{
    FakeGuest g;

    fake_init_hw(&g, 1, 4);
    fake_move(&g, 100, 300, 0, "homed first");
    fake_move(&g, 500, 200, 0, NULL);
    fake_move(&g, 330, 400, 0, NULL);
    fake_move(&g, 300, 90, 0, NULL);
    fake_move(&g, 20, 10, 0, NULL);
    fake_move(&g, 319, 250, 0, NULL);
    fake_move(&g, 600, 450, 0, NULL);
    fake_move(&g, 311, 250, 0, NULL);
    fake_move(&g, 636, 100, 0, NULL);
    fake_move(&g, 400, 120, 0, NULL);
    fake_move(&g, 634, 60, 0, NULL);
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

/* Write @s at @row, @col of the 80-column VGA text screen, in @attr */
static void text_put(QTestState *qts, int row, int col, const char *s,
                     uint8_t attr)
{
    size_t i, n = strlen(s);
    g_autofree uint8_t *cells = g_new(uint8_t, n * 2);

    for (i = 0; i < n; i++) {
        cells[i * 2] = s[i];
        cells[i * 2 + 1] = attr;
    }
    qtest_memwrite(qts, 0xb8000 + (row * 80 + col) * 2, cells, n * 2);
}

/* A wait-screen reply: where it found its text, or NULL */
static QDict *text_found(QDict *rsp)
{
    QDict *ret = qdict_get_qdict(rsp, "return");
    QDict *wait;

    if (!ret) {
        g_autoptr(GString) json = qobject_to_json(QOBJECT(rsp));

        g_test_message("reply: %s", json->str);
        g_assert_not_reached();
    }
    wait = qdict_get_qdict(ret, "wait");
    g_test_message("wait-screen: %s", qdict_get_str(ret, "summary"));
    g_assert(qdict_get_bool(wait, "matched") ==
             qdict_haskey(wait, "found"));
    return qdict_get_qdict(wait, "found");
}

static void text_found_at(QDict *rsp, int row, int col, const char *line)
{
    QDict *found = text_found(rsp);

    g_assert(found);
    g_assert_cmpint(qdict_get_int(found, "row"), ==, row);
    g_assert_cmpint(qdict_get_int(found, "col"), ==, col);
    g_assert_cmpstr(qdict_get_str(found, "line"), ==, line);
    qobject_unref(rsp);
}

static void text_error(QDict *rsp, const char *want)
{
    const char *desc;

    g_assert(qdict_haskey(rsp, "error"));
    desc = qdict_get_str(qdict_get_qdict(rsp, "error"), "desc");
    if (!strstr(desc, want)) {
        g_test_message("error '%s' lacks '%s'", desc, want);
        g_assert_not_reached();
    }
    qobject_unref(rsp);
}

/*
 * SeaBIOS on a text screen.  The VM starts with -S, and one "steps"
 * call starts it, waits for the boot menu prompt and answers it: that
 * is how an agent catches a prompt that shows only for seconds.  Then a
 * regular expression, a wait that times out, the chosen line of a menu
 * (on blue, which a wrong colour bit once hid), and bad arguments.
 */
static void test_screen_text(void)
{
    StepsVm v;
    QDict *json, *rsp, *found;
    QListEntry *e;
    bool is_error, image, seen = false;
    char *text;

    steps_vm_start(&v, "-accel tcg -machine pc -nodefaults -vga std"
                   " -boot menu=on,splash-time=30000 -S");
    text = steps_call(v.fd, 2, "[{\"tool\": \"vm-power\", \"args\": "
                      "{\"action\": \"resume\"}}, "
                      "{\"tool\": \"wait-screen\", \"args\": "
                      "{\"until\": \"text\", \"text\": \"Press ESC for boot"
                      " menu\", \"timeout-ms\": 60000, "
                      "\"screenshot\": false}}, "
                      "{\"tool\": \"key\", \"args\": {\"keys\": [\"esc\"], "
                      "\"then\": {\"wait\": \"text\", \"text\": \"Select"
                      " boot device\", \"timeout-ms\": 30000}}}]",
                      &is_error, &image, &json);
    g_assert(!is_error);
    g_assert(g_str_has_prefix(text, "ran all 3 steps\n"));
    steps_has(text, "\n2. wait-screen: text \"Press ESC for boot menu\" "
              "found after ", true);
    steps_has(text, "; text \"Select boot device\" found after ", true);
    g_assert_cmpint(qdict_get_int(json, "steps-done"), ==, 3);
    qobject_unref(json);
    g_free(text);

    /* ^ and $ match at the ends of each row, without its trailing spaces */
    rsp = qtest_qmp(v.qts, "{'execute': 'wait-screen', 'arguments':"
                    " {'until': 'text', 'regex': true,"
                    " 'text': '^SeaBIOS [(]version [^)]+[)]$',"
                    " 'timeout-ms': 1000, 'screenshot': false}}");
    found = text_found(rsp);
    g_assert(found);
    g_assert_cmpint(qdict_get_int(found, "row"), ==, 0);
    g_assert_cmpint(qdict_get_int(found, "col"), ==, 0);
    g_assert(g_str_has_prefix(qdict_get_str(found, "line"),
                              "SeaBIOS (version "));
    qobject_unref(rsp);

    /* not there: it says so when the time is up */
    rsp = qtest_qmp(v.qts, "{'execute': 'wait-screen', 'arguments':"
                    " {'until': 'text', 'text': 'No such words',"
                    " 'timeout-ms': 300, 'screenshot': false}}");
    g_assert(!text_found(rsp));
    g_assert(strstr(qdict_get_str(qdict_get_qdict(rsp, "return"),
                                  "summary"),
                    "text \"No such words\" not found within "));
    qobject_unref(rsp);

    /* a key's "then" takes text only with wait "text" */
    text_error(qtest_qmp(v.qts, "{'execute': 'key', 'arguments':"
                         " {'keys': ['shift'], 'then': {'text': 'x'}}}"),
               "go with wait \"text\"");

    /*
     * A menu: the chosen line is a bar of white on blue, with a space
     * at each end; the other line is not highlighted.
     */
    qtest_qmp_assert_success(v.qts, "{'execute': 'stop'}");
    text_put(v.qts, 20, 4, " Chosen line ", 0x1f);
    text_put(v.qts, 21, 4, "Other line", 0x07);
    text_found_at(qtest_qmp(v.qts, "{'execute': 'wait-screen', 'arguments':"
                            " {'until': 'text', 'text': 'line',"
                            " 'highlighted': true, 'timeout-ms': 1000,"
                            " 'screenshot': false}}"),
                  20, 12, "Chosen line");
    text_found_at(qtest_qmp(v.qts, "{'execute': 'wait-screen', 'arguments':"
                            " {'until': 'text', 'text': '^Chosen line$',"
                            " 'regex': true, 'highlighted': true,"
                            " 'timeout-ms': 1000, 'screenshot': false}}"),
                  20, 5, "Chosen line");
    rsp = qtest_qmp(v.qts, "{'execute': 'wait-screen', 'arguments':"
                    " {'until': 'text', 'text': 'Other',"
                    " 'highlighted': true, 'timeout-ms': 300,"
                    " 'screenshot': false}}");
    g_assert(!text_found(rsp));
    qobject_unref(rsp);
    /* without highlighted, any text counts, and line is the whole row */
    text_found_at(qtest_qmp(v.qts, "{'execute': 'wait-screen', 'arguments':"
                            " {'until': 'text', 'text': 'Other line',"
                            " 'timeout-ms': 1000, 'screenshot': false}}"),
                  21, 4, "    Other line");

    /* screen-text lists the chosen line as highlighted, and only it */
    rsp = qtest_qmp(v.qts, "{'execute': 'screen-text'}");
    QLIST_FOREACH_ENTRY(qdict_get_qlist(qdict_get_qdict(rsp, "return"),
                                        "highlights"), e) {
        QDict *hl = qobject_to(QDict, qlist_entry_obj(e));

        g_assert_cmpint(qdict_get_int(hl, "row"), ==, 20);
        g_assert_cmpint(qdict_get_int(hl, "col"), ==, 4);
        g_assert_cmpint(qdict_get_int(hl, "len"), ==, 13);
        g_assert_cmpstr(qdict_get_str(hl, "text"), ==, "Chosen line");
        g_assert(!seen);
        seen = true;
    }
    g_assert(seen);
    qobject_unref(rsp);

    text_error(qtest_qmp(v.qts, "{'execute': 'wait-screen', 'arguments':"
                         " {'until': 'text'}}"), "needs text");
    text_error(qtest_qmp(v.qts, "{'execute': 'wait-screen', 'arguments':"
                         " {'until': 'text', 'text': '(', 'regex': true}}"),
               "not a regular expression");
    text_error(qtest_qmp(v.qts, "{'execute': 'wait-screen', 'arguments':"
                         " {'until': 'stable', 'text': 'x'}}"),
               "go with until \"text\"");

    steps_vm_stop(&v);
}

/* A card without a text mode: the wait says so at once */
static void test_screen_text_none(void)
{
    QTestState *qts = qtest_init("-machine pc -nodefaults"
                                 " -device bochs-display");
    int64_t start = g_get_monotonic_time();

    text_error(qtest_qmp(qts, "{'execute': 'wait-screen', 'arguments':"
                         " {'until': 'text', 'text': 'x'}}"),
               "has no text mode; use wait-screen until change or stable");
    g_assert_cmpint(g_get_monotonic_time() - start, <, 30 * G_USEC_PER_SEC);
    text_error(qtest_qmp(qts, "{'execute': 'screen-text'}"),
               "has no text mode; use screenshot");
    qtest_quit(qts);
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
 * A BIOS whose loop takes a fault that comes back to the instruction,
 * as a TLB miss does.  At f000:0000, after a far jump from the reset
 * vector, so that the IRET comes back to the same address:
 *
 *  0000  xor ax,ax / mov ds,ax           the divide error vector is
 *  0004  mov word [0],001e               f000:001e
 *  000a  mov word [2],f000
 *  0010  mov ss,ax / mov sp,7c00
 *  0015  xor bx,bx                       loop
 *  0017  mov ax,1
 *  001a  div bl                          faults while bl is 0
 *  001c  jmp 0015
 *  001e  inc bx / iret                   the handler: bl = 1, again
 */
#define FAULT_DIV       0xf001a
#define FAULT_HANDLER   0xf001e

/*
 * A 64 KiB BIOS file: @code at f000:0000, a far jump to it at the
 * reset vector, and hlt everywhere else
 */
static char *debug_bios_code(const uint8_t *code, size_t len)
{
    static const uint8_t reset[] = { 0xea, 0x00, 0x00, 0x00, 0xf0 };
    g_autofree uint8_t *rom = g_malloc(64 * KiB);
    char *path = NULL;
    int fd = g_file_open_tmp("mcp-bios-XXXXXX", &path, NULL);

    g_assert(fd >= 0);
    g_assert(len <= 0xfff0);
    memset(rom, 0xf4, 64 * KiB);
    memcpy(rom, code, len);
    memcpy(rom + 0xfff0, reset, sizeof(reset));
    g_assert_cmpint(write(fd, rom, 64 * KiB), ==, 64 * KiB);
    close(fd);
    return path;
}

static char *debug_bios_fault(void)
{
    static const uint8_t code[] = {
        0x31, 0xc0, 0x8e, 0xd8,
        0xc7, 0x06, 0x00, 0x00, 0x1e, 0x00,
        0xc7, 0x06, 0x02, 0x00, 0x00, 0xf0,
        0x8e, 0xd0, 0xbc, 0x00, 0x7c,
        0x31, 0xdb,
        0xb8, 0x01, 0x00,
        0xf6, 0xf3,
        0xeb, 0xf7,
        0x43, 0xcf,
    };

    return debug_bios_code(code, sizeof(code));
}

/*
 * An arrival that comes right after the instruction took an
 * interruption may be the CPU running it again: debug-run says so,
 * and debug-step marks the instruction.  The profile has the
 * physical addresses, which in real mode are the same.
 */
static void test_debug_retry(void)
{
    g_autofree char *bios = debug_bios_fault();
    g_autofree char *at = g_strdup_printf("{'address': '0x%x'}", FAULT_DIV);
    QTestState *qts;
    QDict *ret, *top;
    QList *list;

    /* from the reset, so that the first arrival is with bl = 0 */
    qts = qtest_initf("-accel tcg -machine pc -nodefaults -vga none "
                      "-bios %s -S", bios);

    /* the first arrival: the div has not run yet */
    ret = debug_ok(qts, "debug-run", at);
    g_assert(qdict_get_bool(ret, "reached"));
    g_assert(!qdict_get_try_bool(ret, "retry", false));
    g_assert_cmpint(debug_reg(ret, "BX") & 0xff, ==, 0);
    qobject_unref(ret);

    /* going on, the div faults, and the handler comes back to it */
    ret = debug_ok(qts, "debug-run", at);
    g_assert(qdict_get_bool(ret, "reached"));
    g_assert(qdict_get_try_bool(ret, "retry", false));
    g_assert(strstr(qdict_get_str(ret, "summary"), "coming back"));
    g_assert_cmpint(debug_reg(ret, "BX") & 0xff, ==, 1);
    qobject_unref(ret);

    /* now it runs, and the loop gets there anew */
    ret = debug_ok(qts, "debug-run", at);
    g_assert(qdict_get_bool(ret, "reached"));
    g_assert(!qdict_get_try_bool(ret, "retry", false));
    g_assert_cmpint(debug_reg(ret, "BX") & 0xff, ==, 0);
    qobject_unref(ret);

    /* a step into the fault says where the CPU went */
    ret = debug_ok(qts, "debug-step", "{}");
    g_assert(strstr(qdict_get_str(ret, "summary"),
                    "-> it took an interruption: the CPU went to 0xf001e"));
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, "0xf001e");
    qobject_unref(ret);

    /* and the next run finds it back at the div: that is a retry too */
    ret = debug_ok(qts, "debug-run", at);
    g_assert(qdict_get_try_bool(ret, "retry", false));
    g_assert_cmpint(debug_reg(ret, "BX") & 0xff, ==, 1);
    qobject_unref(ret);

    qobject_unref(debug_ok(qts, "cont", "{}"));
    ret = debug_ok(qts, "debug-profile", "{'interval-ms': 300}");
    list = qdict_get_qlist(ret, "top");
    g_assert(!qlist_empty(list));
    top = qobject_to(QDict, qlist_peek(list));
    g_assert_cmpstr(qdict_get_str(top, "physical"), ==,
                    qdict_get_str(top, "pc"));
    g_assert(qdict_haskey(top, "instruction"));
    qobject_unref(ret);

    qtest_quit(qts);
    unlink(bios);
}

/*
 * A BIOS that puts the address of a string in ESI and 0x7c00 in ESP,
 * then loops at REGS_LOOP.  At f000:0000, after a far jump:
 *
 *  0000  mov esi,000f0040
 *  0006  mov esp,00007c00
 *  000c  jmp 000c
 *  0040  the string
 */
#define REGS_LOOP       0xf000c

static char *debug_bios_regs(void)
{
    static const uint8_t code[] = {
        0x66, 0xbe, 0x40, 0x00, 0x0f, 0x00,
        0x66, 0xbc, 0x00, 0x7c, 0x00, 0x00,
        0xeb, 0xfe,
    };
    static const char str[] = "Hi, \"MCP\"\n\x01";
    uint8_t buf[0x40 + sizeof(str)];

    memset(buf, 0xf4, sizeof(buf));
    memcpy(buf, code, sizeof(code));
    memcpy(buf + 0x40, str, sizeof(str));
    return debug_bios_code(buf, sizeof(buf));
}

static void debug_summary_has(QDict *ret, const char *want)
{
    const char *summary = qdict_get_str(ret, "summary");

    if (!strstr(summary, want)) {
        g_test_message("'%s' lacks '%s'", summary, want);
        g_assert_not_reached();
    }
}

/* Memory at a register plus or minus an offset, and C strings */
static void test_debug_registers(void)
{
    g_autofree char *bios = debug_bios_regs();
    g_autofree char *args = NULL;
    QTestState *qts;
    QDict *ret;

    qts = qtest_initf("-accel tcg -machine pc -nodefaults -vga none "
                      "-bios %s -S", bios);

    args = g_strdup_printf("{'address': '0x%x', 'memory': ["
                           " {'address': 'esi', 'string': true},"
                           " {'address': '$ESI+4', 'string': true},"
                           " {'address': 'pc+0x100', 'string': true},"
                           " {'address': 'esp-4', 'words': 1},"
                           " {'address': 'pc', 'words': 1}]}", REGS_LOOP);
    ret = debug_ok(qts, "debug-run", args);
    g_assert(qdict_get_bool(ret, "reached"));
    debug_summary_has(ret, "String at esi = 0xf0040: "
                      "\"Hi, \\\"MCP\\\"\\n\\x01\"\n");
    debug_summary_has(ret, "String at $ESI+4 = 0xf0044: "
                      "\"\\\"MCP\\\"\\n\\x01\"\n");
    debug_summary_has(ret, "String at pc+0x100 = 0xf010c: \"\\xf4\\xf4");
    debug_summary_has(ret, "(no zero byte in the first 256 bytes)");
    debug_summary_has(ret, "Memory at esp-4 = 0x7bfc:\n");
    debug_summary_has(ret, "Memory at pc = 0xf000c:\n000f000c: 0xf4f4feeb");
    qobject_unref(ret);

    /* a base and a register, as for a segment or a space */
    ret = debug_ok(qts, "debug-step", "{'memory': [{'address':"
                   " '0x10 + esi - 0x50', 'words': 1},"
                   " {'address': '0x1000', 'words': 1}]}");
    debug_summary_has(ret, "Memory at 0x10 + esi - 0x50 = 0xf0000:\n"
                      "000f0000: 0x0040be66");
    debug_summary_has(ret, "Memory at 0x1000:\n");
    qobject_unref(ret);

    debug_check_error(qts, "debug-step", "{'memory': [{'address': 'foo'}]}",
                      "'foo' is not a number or a register; the registers"
                      " are pc, eax, ecx");
    debug_check_error(qts, "debug-step",
                      "{'memory': [{'address': 'esp+0xzz'}]}",
                      "'0xzz' is not a number");
    debug_check_error(qts, "debug-step",
                      "{'memory': [{'address': 'esp+'}]}",
                      "'' is not a number or a register");
    debug_check_error(qts, "debug-step",
                      "{'memory': [{'address': '1+2+3+4+5'}]}",
                      "more than 4 parts");
    qtest_quit(qts);
    unlink(bios);
}

/*
 * A BIOS that writes a counter to 0x1000 and reads it back, in a loop.
 * At f000:0000, after a far jump:
 *
 *  0000  xor ax,ax / mov ds,ax
 *  0004  mov bx,1000
 *  0007  inc ax                          loop
 *  0008  mov [bx],ax                     WATCH_WRITER
 *  000a  mov cx,[bx]                     WATCH_READER
 *  000c  jmp 0007
 */
#define WATCH_WRITER    0xf0008
#define WATCH_READER    0xf000a

static char *debug_bios_watch(void)
{
    static const uint8_t code[] = {
        0x31, 0xc0, 0x8e, 0xd8,
        0xbb, 0x00, 0x10,
        0x40,
        0x89, 0x07,
        0x8b, 0x0f,
        0xeb, 0xf9,
    };

    return debug_bios_code(code, sizeof(code));
}

static void debug_check_watch(QDict *ret, const char *access, uint64_t pc,
                              uint64_t next)
{
    QDict *watch = qdict_get_qdict(ret, "watch");
    g_autofree char *want_pc = g_strdup_printf("0x%" PRIx64, pc);
    g_autofree char *want_next = g_strdup_printf("0x%" PRIx64, next);

    g_assert(qdict_get_bool(ret, "reached"));
    g_assert(watch);
    g_assert_cmpstr(qdict_get_str(watch, "address"), ==, "0x1000");
    g_assert_cmpstr(qdict_get_str(watch, "access"), ==, access);
    g_assert_cmpstr(qdict_get_str(watch, "pc"), ==, want_pc);
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, want_next);
}

/* debug-run stops after a CPU writes or reads watched memory */
static void test_debug_watch(void)
{
    g_autofree char *bios = debug_bios_watch();
    QTestState *qts;
    QDict *ret;
    int i;

    qts = qtest_initf("-accel tcg -machine pc -nodefaults -vga none "
                      "-bios %s -S", bios);

    ret = debug_ok(qts, "debug-run",
                   "{'watch': {'address': '0x1000', 'length': 2}}");
    debug_check_watch(ret, "write", WATCH_WRITER, WATCH_READER);
    debug_summary_has(ret, "CPU 0 wrote to 0x1000; the VM is stopped after"
                      " the instruction that did it:\n0x000f0008:");
    debug_summary_has(ret, "Memory at 0x1000:\n00001000: 0x00000001\n");
    qobject_unref(ret);

    /* the next write, then the read after it */
    ret = debug_ok(qts, "debug-run",
                   "{'watch': {'address': '0x1000', 'length': 2}}");
    debug_check_watch(ret, "write", WATCH_WRITER, WATCH_READER);
    debug_summary_has(ret, "00001000: 0x00000002\n");
    qobject_unref(ret);
    ret = debug_ok(qts, "debug-run",
                   "{'watch': {'address': '0x1001', 'access': 'read'}}");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(ret, "watch"), "access"),
                    ==, "read");
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, "0xf000c");
    debug_summary_has(ret, "CPU 0 read 0x1001;");
    qobject_unref(ret);

    /* the third write from here; a register needs the VM stopped */
    ret = debug_ok(qts, "debug-run",
                   "{'watch': {'address': 'ebx', 'length': 2}, 'hits': 3}");
    debug_check_watch(ret, "write", WATCH_WRITER, WATCH_READER);
    debug_summary_has(ret, "(access 3)");
    debug_summary_has(ret, "00001000: 0x00000005\n");
    qobject_unref(ret);

    /*
     * A breakpoint and a watch.  The CPU at the address steps past it,
     * and that step reads the watched memory; next time it steps past
     * the jmp, and the write comes first; then the breakpoint does.
     */
    ret = debug_ok(qts, "debug-run", "{'address': '0xf000a',"
                   " 'watch': {'address': '0x1000', 'access': 'access'}}");
    debug_check_watch(ret, "read", WATCH_READER, 0xf000c);
    qobject_unref(ret);
    ret = debug_ok(qts, "debug-run", "{'address': '0xf000c',"
                   " 'watch': {'address': '0x1000', 'access': 'access'}}");
    debug_check_watch(ret, "write", WATCH_WRITER, WATCH_READER);
    qobject_unref(ret);
    ret = debug_ok(qts, "debug-run", "{'address': '0xf0007',"
                   " 'watch': {'address': '0x1000'}}");
    g_assert(qdict_get_bool(ret, "reached"));
    g_assert(!qdict_haskey(ret, "watch"));
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, "0xf0007");
    qobject_unref(ret);

    /*
     * The time runs out while the guest keeps writing, maybe just as a
     * CPU writes: either way, the reply says whether the VM runs
     */
    for (i = 0; i < 10; i++) {
        bool running;

        ret = debug_ok(qts, "debug-run", "{'watch': {'address': '0x1000'},"
                       " 'hits': 1000000, 'timeout-ms': 20}");
        running = qdict_get_bool(ret, "running");
        qobject_unref(ret);
        g_usleep(50 * 1000);
        ret = debug_ok(qts, "query-status", "{}");
        g_assert_cmpint(qdict_get_bool(ret, "running"), ==, running);
        qobject_unref(ret);
    }

    /* nobody touches 0x2000: the guest goes on */
    ret = debug_ok(qts, "debug-run", "{'watch': {'address': '0x2000'},"
                   " 'timeout-ms': 200}");
    g_assert(!qdict_get_bool(ret, "reached"));
    g_assert(qdict_get_bool(ret, "running"));
    debug_summary_has(ret, "no CPU wrote to 0x2000 in 0.2 s;");
    qobject_unref(ret);

    debug_check_error(qts, "debug-run", "{'watch': {'address': 'ebx'}}",
                      "needs the VM stopped");
    debug_check_error(qts, "debug-run", "{}", "or more of them");
    debug_check_error(qts, "debug-run",
                      "{'watch': {'address': '0x1000', 'length': 0}}",
                      "watch length must be 1 to 4096");
    qtest_quit(qts);
    unlink(bios);
}

/*
 * A BIOS that counts down in a loop of one instruction, writes 0x1000,
 * then stays in a jump to itself.  At f000:0000, after a far jump:
 *
 *  0000  mov cx,1000
 *  0003  loop 0003
 *  0005  mov [1000],ax
 *  0008  jmp 0008
 */
static char *debug_bios_history(void)
{
    static const uint8_t code[] = {
        0xb9, 0xe8, 0x03,
        0xe2, 0xfe,
        0xa3, 0x00, 0x10,
        0xeb, 0xfe,
    };

    return debug_bios_code(code, sizeof(code));
}

/* The history entry @i of @ret */
static const char *debug_history(QDict *ret, int i)
{
    QList *hist = qdict_get_qlist(ret, "history");
    QListEntry *e;

    g_assert(hist);
    QLIST_FOREACH_ENTRY(hist, e) {
        if (!i--) {
            return qstring_get_str(qobject_to(QString, qlist_entry_obj(e)));
        }
    }
    g_assert_not_reached();
}

/* The loop at 0xf0003 ran @lo to @hi times in a row */
static void debug_check_loop(const char *entry, unsigned long lo,
                             unsigned long hi)
{
    unsigned long n;

    g_assert(g_str_has_prefix(entry, "0xf0003-0xf0004 x"));
    n = g_ascii_strtoull(entry + strlen("0xf0003-0xf0004 x"), NULL, 10);
    g_assert_cmpuint(n, >=, lo);
    g_assert_cmpuint(n, <=, hi);
}

/* debug-run with history shows the blocks of code that led there */
static void test_debug_history(void)
{
    g_autofree char *bios = debug_bios_history();
    QTestState *qts;
    QDict *ret;

    qts = qtest_initf("-accel tcg -machine pc -nodefaults -vga none "
                      "-bios %s -S", bios);

    /* the loop is one block, run 999 times in a row: one entry */
    ret = debug_ok(qts, "debug-run", "{'watch': {'address': '0x1000'},"
                   " 'history': 3}");
    g_assert(qdict_get_bool(ret, "reached"));
    g_assert_cmpint(qlist_size(qdict_get_qlist(ret, "history")), ==, 3);
    g_assert_cmpstr(debug_history(ret, 0), ==, "0xf0000-0xf0004");
    debug_check_loop(debug_history(ret, 1), 999, 1001);
    g_assert_cmpstr(debug_history(ret, 2), ==, "0xf0005-0xf0009");
    debug_summary_has(ret, "\n\nHow it got here, the last 3 blocks of code"
                      " CPU 0 ran, oldest first:\n  0xf0000-0xf0004\n"
                      "  0xf0003-0xf0004 x");
    qobject_unref(ret);

    /* fewer blocks than asked: only the step past the jump */
    ret = debug_ok(qts, "debug-run", "{'address': '0xf0008',"
                   " 'history': 4}");
    g_assert(qdict_get_bool(ret, "reached"));
    g_assert_cmpint(qlist_size(qdict_get_qlist(ret, "history")), ==, 1);
    g_assert_cmpstr(debug_history(ret, 0), ==, "0xf0008-0xf0009");
    debug_summary_has(ret, "the last 1 block of code CPU 0 ran");
    qobject_unref(ret);

    /* the time runs out with the VM running: no history, it goes on */
    ret = debug_ok(qts, "debug-run", "{'watch': {'address': '0x2000'},"
                   " 'history': 4, 'timeout-ms': 200}");
    g_assert(!qdict_get_bool(ret, "reached"));
    g_assert(qdict_get_bool(ret, "running"));
    g_assert(!qdict_haskey(ret, "history"));
    qobject_unref(ret);
    ret = debug_ok(qts, "query-status", "{}");
    g_assert(qdict_get_bool(ret, "running"));
    qobject_unref(ret);

    debug_check_error(qts, "debug-run", "{'address': '0xf0008',"
                      " 'history': 1025}", "history must be 0 to 1024");
    qtest_quit(qts);

    /*
     * A breakpoint on the page makes each instruction there a block:
     * the loop is the same block 1000 times
     */
    qts = qtest_initf("-accel tcg -machine pc -nodefaults -vga none "
                      "-bios %s -S", bios);
    ret = debug_ok(qts, "debug-run", "{'address': '0xf0008',"
                   " 'history': 3}");
    g_assert(qdict_get_bool(ret, "reached"));
    g_assert_cmpstr(debug_history(ret, 0), ==, "0xf0000-0xf0002");
    debug_check_loop(debug_history(ret, 1), 1000, 1002);
    g_assert_cmpstr(debug_history(ret, 2), ==, "0xf0005-0xf0007");
    qobject_unref(ret);
    qtest_quit(qts);
    unlink(bios);
}

/*
 * A BIOS that puts a far return at 1000:0008 and at 2000:0008, then
 * calls the first once and the second twice, over and over.  At
 * f000:0000, after a far jump:
 *
 *  0000  xor ax,ax / mov ss,ax / mov sp,7c00
 *  0007  mov ax,1000 / mov es,ax / mov byte [es:0008],cb
 *  0012  mov ax,2000 / mov es,ax / mov byte [es:0008],cb
 *  001d  call 1000:0008
 *  0022  call 2000:0008
 *  0027  call 2000:0008
 *  002c  jmp 001d
 */
static char *debug_bios_mask(void)
{
    static const uint8_t code[] = {
        0x31, 0xc0, 0x8e, 0xd0, 0xbc, 0x00, 0x7c,
        0xb8, 0x00, 0x10, 0x8e, 0xc0, 0x26, 0xc6, 0x06, 0x08, 0x00, 0xcb,
        0xb8, 0x00, 0x20, 0x8e, 0xc0, 0x26, 0xc6, 0x06, 0x08, 0x00, 0xcb,
        0x9a, 0x08, 0x00, 0x00, 0x10,
        0x9a, 0x08, 0x00, 0x00, 0x20,
        0x9a, 0x08, 0x00, 0x00, 0x20,
        0xeb, 0xef,
    };

    return debug_bios_code(code, sizeof(code));
}

/* How often the trace in @ret counted @pc, 0 if never */
static int64_t debug_trace_count(QDict *ret, const char *pc)
{
    QList *list = qdict_get_qlist(ret, "instructions");
    QListEntry *e;

    g_assert(list);
    QLIST_FOREACH_ENTRY(list, e) {
        QDict *in = qobject_to(QDict, qlist_entry_obj(e));

        if (!strcmp(qdict_get_str(in, "pc"), pc)) {
            return qdict_get_int(in, "count");
        }
    }
    return 0;
}

/* A breakpoint with a mask stops at every address with those bits */
static void test_debug_mask(void)
{
    g_autofree char *bios = debug_bios_mask();
    QTestState *qts;
    QDict *ret;

    qts = qtest_initf("-accel tcg -machine pc -nodefaults -vga none "
                      "-bios %s -S", bios);

    /* the low 16 bits: the first call, then the second */
    ret = debug_ok(qts, "debug-run", "{'address': '0x10008',"
                   " 'address-mask': '0xffff', 'hits': 2}");
    g_assert(qdict_get_bool(ret, "reached"));
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, "0x20008");
    debug_summary_has(ret, "CPU 0 reached 0x20008 (arrival 2); the VM is"
                      " stopped");
    qobject_unref(ret);

    /* all the bits: past the second call of the second, to the first */
    ret = debug_ok(qts, "debug-run", "{'address': '0x10008'}");
    g_assert(qdict_get_bool(ret, "reached"));
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, "0x10008");
    qobject_unref(ret);

    /* the CPU there steps past it, then goes 10 times round the loop */
    ret = debug_ok(qts, "debug-trace", "{'address': '0x10008',"
                   " 'address-mask': '0xffff', 'count': 30}");
    g_assert_cmpint(qdict_get_int(ret, "hits"), ==, 30);
    g_assert_cmpint(debug_trace_count(ret, "0x20008"), ==, 20);
    g_assert_cmpint(debug_trace_count(ret, "0x10008"), ==, 10);
    g_assert_cmpint(qlist_size(qdict_get_qlist(ret, "entries")), ==, 20);
    debug_summary_has(ret, "Traced arrivals at 0x10008 (mask 0xffff) for ");
    debug_summary_has(ret, "\n\nBy instruction:\n      20 x 0x00020008:");
    debug_summary_has(ret, "\n      10 x 0x00010008:");
    debug_summary_has(ret, "\n\nHits (the first 20, then the first arrival"
                      " at each new address):\n#1 CPU 0 at 0x20008\n"
                      "#2 CPU 0 at 0x20008\n#3 CPU 0 at 0x10008\n");
    qobject_unref(ret);

    /* the guest runs on; any 6 arrivals in a row are 2 and 4 */
    ret = debug_ok(qts, "debug-trace", "{'address': '0x10008',"
                   " 'address-mask': '0xffff', 'count': 6, 'list': 0}");
    g_assert_cmpint(qdict_get_int(ret, "hits"), ==, 6);
    g_assert_cmpint(debug_trace_count(ret, "0x20008"), ==, 4);
    g_assert_cmpint(debug_trace_count(ret, "0x10008"), ==, 2);
    g_assert_cmpint(qlist_size(qdict_get_qlist(ret, "entries")), ==, 2);
    debug_summary_has(ret, "\n\nHits (the first arrival at each"
                      " address):\n#");
    qobject_unref(ret);

    debug_check_error(qts, "debug-run", "{'watch': {'address': '0x2000'},"
                      " 'address-mask': '0xffff'}",
                      "address-mask needs an address");
    debug_check_error(qts, "debug-run", "{'address': '0x10008',"
                      " 'address-mask': '0'}",
                      "address-mask must keep at least one bit");
    debug_check_error(qts, "debug-trace", "{'address': '0x10008',"
                      " 'address-mask': 'ffff'}",
                      "address-mask 'ffff' is not a number");
    qtest_quit(qts);
    unlink(bios);
}

/* debug-run and debug-trace take several addresses and watched areas */
static void test_debug_several(void)
{
    g_autofree char *bios = debug_bios_mask();
    g_autofree char *bios_watch = debug_bios_watch();
    g_autoptr(GString) many = g_string_new("{'address': [");
    QTestState *qts;
    QDict *ret;
    int i;

    qts = qtest_initf("-accel tcg -machine pc -nodefaults -vga none "
                      "-bios %s -S", bios);

    /* each loop calls the first once and the second twice */
    ret = debug_ok(qts, "debug-run", "{'address': ['0x10008', '0x20008'],"
                   " 'hits': 3}");
    g_assert(qdict_get_bool(ret, "reached"));
    g_assert_cmpint(qdict_get_int(ret, "hits"), ==, 3);
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, "0x20008");
    debug_summary_has(ret, "CPU 0 reached 0x20008 (arrival 3); the VM is"
                      " stopped");
    qobject_unref(ret);
    ret = debug_ok(qts, "debug-run", "{'address': ['0x20008', '0x10008']}");
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, "0x10008");
    qobject_unref(ret);

    /* the trace counts them by address */
    ret = debug_ok(qts, "debug-trace", "{'address': ['0x10008', '0x20008'],"
                   " 'count': 9, 'list': 3}");
    g_assert_cmpint(qdict_get_int(ret, "hits"), ==, 9);
    g_assert_cmpint(debug_trace_count(ret, "0x20008"), ==, 6);
    g_assert_cmpint(debug_trace_count(ret, "0x10008"), ==, 3);
    debug_summary_has(ret, "Traced arrivals at 0x10008 and 0x20008 for ");
    debug_summary_has(ret, "\n\nHits (the first 3, then the first arrival"
                      " at each new address):\n#1 CPU 0 at 0x20008\n"
                      "#2 CPU 0 at 0x20008\n#3 CPU 0 at 0x10008");
    qobject_unref(ret);

    /* the same address twice is one */
    ret = debug_ok(qts, "debug-trace", "{'address': ['0x10008', '0x10008'],"
                   " 'count': 2}");
    g_assert_cmpint(qdict_get_int(ret, "hits"), ==, 2);
    g_assert(!qdict_haskey(ret, "instructions"));
    debug_summary_has(ret, "Traced arrivals at 0x10008 for ");
    qobject_unref(ret);

    /* nothing gets there: the summary names the first ones */
    ret = debug_ok(qts, "debug-run", "{'address': ['0x1', '0x2', '0x3',"
                   " '0x4', '0x5', '0x6', '0x7', '0x8', '0x9'],"
                   " 'timeout-ms': 100}");
    g_assert(!qdict_get_bool(ret, "reached"));
    debug_summary_has(ret, "no CPU reached 0x1, 0x2, 0x3, 0x4, 0x5, 0x6,"
                      " 0x7 or 2 more in 0.1 s;");
    qobject_unref(ret);

    for (i = 0; i < 65; i++) {
        g_string_append_printf(many, "%s'0x%x'", i ? ", " : "", 0x100 + i);
    }
    g_string_append(many, "]}");
    debug_check_error(qts, "debug-run", many->str,
                      "give at most 64 addresses");
    debug_check_error(qts, "debug-run", "{'address': []}",
                      "give at least one address");
    debug_check_error(qts, "debug-trace", "{'address': ['0x1000', 'zz']}",
                      "address 'zz' is not a number");
    qtest_quit(qts);

    /* the guest at 0xf0008 writes 0x1000 and at 0xf000a reads it */
    qts = qtest_initf("-accel tcg -machine pc -nodefaults -vga none "
                      "-bios %s -S", bios_watch);
    ret = debug_ok(qts, "debug-run", "{'watch': [{'address': '0x2000'},"
                   " {'address': '0x1000', 'access': 'read'}]}");
    g_assert(qdict_get_bool(ret, "reached"));
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(ret, "watch"), "access"),
                    ==, "read");
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, "0xf000c");
    debug_summary_has(ret, "CPU 0 read 0x1000;");
    debug_summary_has(ret, "Memory at 0x2000:\n00002000: 0x00000000\n");
    debug_summary_has(ret, "Memory at 0x1000:\n00001000: 0x00000001\n");
    qobject_unref(ret);

    ret = debug_ok(qts, "debug-trace", "{'watch': [{'address': '0x1000',"
                   " 'length': 2}, {'address': '0x1000', 'length': 2,"
                   " 'access': 'read'}], 'count': 6, 'list': 0}");
    g_assert_cmpint(qdict_get_int(ret, "hits"), ==, 6);
    g_assert_cmpint(debug_trace_count(ret, "0xf0008"), ==, 3);
    g_assert_cmpint(debug_trace_count(ret, "0xf000a"), ==, 3);
    debug_summary_has(ret, "Traced writes to 0x1000 (2 bytes) and reads of"
                      " 0x1000 (2 bytes) for ");
    qobject_unref(ret);

    ret = debug_ok(qts, "debug-run", "{'watch': [{'address': '0x2000'},"
                   " {'address': '0x3000', 'access': 'read'}],"
                   " 'timeout-ms': 100}");
    g_assert(!qdict_get_bool(ret, "reached"));
    debug_summary_has(ret, "no CPU wrote to 0x2000 or read 0x3000 in 0.1 s;");
    qobject_unref(ret);

    g_string_assign(many, "{'watch': [");
    for (i = 0; i < 17; i++) {
        g_string_append_printf(many, "%s{'address': '0x%x'}", i ? ", " : "",
                               0x2000 + 4 * i);
    }
    g_string_append(many, "]}");
    debug_check_error(qts, "debug-run", many->str, "watch at most 16 areas");
    debug_check_error(qts, "debug-run", "{'watch': []}",
                      "give at least one area to watch");
    qtest_quit(qts);
    unlink(bios);
    unlink(bios_watch);
}

/* The exception in @ret: @number, taken at @pc, handled at @handler */
static void debug_check_excp(QDict *ret, int number, uint64_t pc,
                             uint64_t handler)
{
    QDict *e = qdict_get_qdict(ret, "exception");
    g_autofree char *want_pc = g_strdup_printf("0x%" PRIx64, pc);
    g_autofree char *want_handler = g_strdup_printf("0x%" PRIx64, handler);

    g_assert(e);
    g_assert_cmpint(qdict_get_int(e, "number"), ==, number);
    g_assert_cmpstr(qdict_get_str(e, "pc"), ==, want_pc);
    g_assert_cmpstr(qdict_get_str(e, "handler"), ==, want_handler);
}

/*
 * debug-run stops at the handler of an exception, and debug-trace
 * counts them.  Each loop of the fault BIOS divides by zero once: x86
 * exception 0, the divide error.
 */
static void test_debug_exception(void)
{
    g_autofree char *bios = debug_bios_fault();
    g_autoptr(GString) many = g_string_new("{'exception': {'numbers': [");
    QTestState *qts;
    QDict *ret, *x;
    QList *list;
    int i;

    qts = qtest_initf("-accel tcg -machine pc -nodefaults -vga none "
                      "-bios %s -S", bios);

    ret = debug_ok(qts, "debug-run", "{'exception': {}}");
    g_assert(qdict_get_bool(ret, "reached"));
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, "0xf001e");
    debug_check_excp(ret, 0, FAULT_DIV, FAULT_HANDLER);
    debug_summary_has(ret, "CPU 0 took exception 0 at 0xf001a; the VM is"
                      " stopped at the first instruction of the guest's"
                      " handler for it, 0xf001e. The instruction that took"
                      " it:\n0x000f001a:");
    debug_summary_has(ret, "Next instruction:\n0x000f001e:");
    g_assert_cmpint(debug_reg(ret, "BX") & 0xff, ==, 0);
    qobject_unref(ret);

    /* the third from here; the handler has not run, so bl is still 0 */
    ret = debug_ok(qts, "debug-run", "{'exception': {'numbers': [0]},"
                   " 'hits': 3}");
    g_assert_cmpint(qdict_get_int(ret, "hits"), ==, 3);
    debug_check_excp(ret, 0, FAULT_DIV, FAULT_HANDLER);
    debug_summary_has(ret, "CPU 0 took exception 0 at 0xf001a (exception"
                      " 3); the VM is stopped");
    g_assert_cmpint(debug_reg(ret, "BX") & 0xff, ==, 0);
    qobject_unref(ret);

    /*
     * Back at the div from the handler, bl is 1; at the next loop, 0.
     * A CPU stopped there steps past it, into the handler.
     */
    ret = debug_ok(qts, "debug-run", "{'address': '0xf001a'}");
    g_assert_cmpint(debug_reg(ret, "BX") & 0xff, ==, 1);
    qobject_unref(ret);
    ret = debug_ok(qts, "debug-run", "{'address': '0xf001a'}");
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, "0xf001a");
    g_assert_cmpint(debug_reg(ret, "BX") & 0xff, ==, 0);
    qobject_unref(ret);
    ret = debug_ok(qts, "debug-run", "{'address': '0xf001a', 'exception':"
                   " {}}");
    g_assert(qdict_get_bool(ret, "reached"));
    g_assert_cmpint(qdict_get_int(ret, "hits"), ==, 1);
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, "0xf001e");
    debug_check_excp(ret, 0, FAULT_DIV, FAULT_HANDLER);
    qobject_unref(ret);

    /* the guest takes no other: the time runs out, the guest goes on */
    ret = debug_ok(qts, "debug-run", "{'exception': {'skip': [0]},"
                   " 'timeout-ms': 200}");
    g_assert(!qdict_get_bool(ret, "reached"));
    g_assert(qdict_get_bool(ret, "running"));
    g_assert(!qdict_haskey(ret, "exception"));
    debug_summary_has(ret, "no CPU took an exception other than 0 in 0.2 s;"
                      " the VM keeps running");
    qobject_unref(ret);
    ret = debug_ok(qts, "debug-run", "{'exception': {'numbers': [6, 13]},"
                   " 'timeout-ms': 100}");
    debug_summary_has(ret, "no CPU took exceptions 6 or 13 in 0.1 s;");
    qobject_unref(ret);

    /* counted by number and by instruction, each listed */
    ret = debug_ok(qts, "debug-trace", "{'exception': {}, 'count': 5,"
                   " 'registers': ['ebx']}");
    g_assert_cmpint(qdict_get_int(ret, "hits"), ==, 5);
    g_assert(qdict_get_bool(ret, "running"));
    list = qdict_get_qlist(ret, "exceptions");
    g_assert_cmpint(qlist_size(list), ==, 1);
    x = qobject_to(QDict, qlist_peek(list));
    g_assert_cmpint(qdict_get_int(x, "number"), ==, 0);
    g_assert_cmpint(qdict_get_int(x, "count"), ==, 5);
    g_assert_cmpstr(qdict_get_str(x, "handler"), ==, "0xf001e");
    g_assert_cmpint(debug_trace_count(ret, "0xf001a"), ==, 5);
    x = qobject_to(QDict, qlist_peek(qdict_get_qlist(ret, "entries")));
    debug_check_excp(x, 0, FAULT_DIV, FAULT_HANDLER);
    debug_summary_has(ret, "Traced exceptions for ");
    debug_summary_has(ret, "\n\nBy exception:\n       5 x exception 0,"
                      " handler 0xf001e\n\nBy instruction:\n"
                      "       5 x 0x000f001a:");
    debug_summary_has(ret, "\n\nHits:\n#1 CPU 0 at 0xf001a took exception"
                      " 0, went to 0xf001e: ebx=0x0\n#2 ");
    qobject_unref(ret);

    /* with an address: arrivals at the loop and the exceptions */
    ret = debug_ok(qts, "debug-trace", "{'address': '0xf0015',"
                   " 'exception': {'numbers': [0]}, 'count': 6, 'list': 0}");
    g_assert_cmpint(qdict_get_int(ret, "hits"), ==, 6);
    debug_summary_has(ret, "Traced arrivals at 0xf0015 and exception 0 for ");
    debug_summary_has(ret, "Hits (the first exception taken by each"
                      " instruction):\n#");
    qobject_unref(ret);

    for (i = 0; i < 33; i++) {
        g_string_append_printf(many, "%s%d", i ? ", " : "", i);
    }
    g_string_append(many, "]}}");
    debug_check_error(qts, "debug-run", many->str,
                      "give at most 32 exception numbers");
    debug_check_error(qts, "debug-run", "{'exception': {'numbers': [1],"
                      " 'skip': [2]}}", "not both");
    debug_check_error(qts, "debug-trace", "{'exception': {'skip':"
                      " [65536]}}", "exception numbers are 0 to 65535");
    qtest_quit(qts);
    unlink(bios);
}

/*
 * register-write sets a register by GDB's name, or the pc.  In real
 * mode the pc adds the code segment's base to EIP, so the pc moves EIP,
 * and CS moves the pc.
 */
static void test_debug_register(void)
{
    g_autofree char *bios = debug_bios_fault();
    QTestState *qts;
    QDict *ret;

    qts = qtest_initf("-accel tcg -machine pc -nodefaults -vga none "
                      "-bios %s -S", bios);

    /* at the div with bl = 0: make it 1, and the div does not fault */
    ret = debug_ok(qts, "debug-run", "{'address': '0xf001a'}");
    g_assert_cmpint(debug_reg(ret, "BX") & 0xff, ==, 0);
    qobject_unref(ret);
    ret = debug_ok(qts, "register-write", "{'register': 'ebx',"
                   " 'value': '1'}");
    g_assert_cmpstr(qdict_get_str(ret, "before"), ==, "0x0");
    g_assert_cmpstr(qdict_get_str(ret, "after"), ==, "0x1");
    g_assert_cmpstr(qdict_get_str(ret, "summary"), ==,
                    "set ebx of CPU 0 to 0x1 (it was 0x0)");
    qobject_unref(ret);
    ret = debug_ok(qts, "debug-step", "{}");
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, "0xf001c");
    qobject_unref(ret);

    /* back to the div, with bl = 0 again: now it faults */
    ret = debug_ok(qts, "register-write", "{'register': 'pc',"
                   " 'value': '0xf001a'}");
    g_assert_cmpstr(qdict_get_str(ret, "before"), ==, "0xf001c");
    g_assert_cmpstr(qdict_get_str(ret, "after"), ==, "0xf001a");
    g_assert_cmpstr(qdict_get_str(ret, "summary"), ==,
                    "set the pc of CPU 0 to 0xf001a (it was 0xf001c), in eip"
                    " (0x1c to 0x1a)");
    qobject_unref(ret);
    ret = debug_ok(qts, "register-write", "{'register': 'EBX',"
                   " 'value': '-256'}");
    g_assert_cmpstr(qdict_get_str(ret, "after"), ==, "0xffffff00");
    qobject_unref(ret);
    ret = debug_ok(qts, "debug-step", "{}");
    debug_summary_has(ret, "-> it took an interruption: the CPU went to"
                      " 0xf001e");
    qobject_unref(ret);

    /* a pc it can't get to changes nothing */
    debug_check_error(qts, "register-write", "{'register': 'pc',"
                      " 'value': '0x100000000'}", "CPU 0 stays at 0xf001e:"
                      " setting its pc to 0x100000000 would put it at 0x0"
                      " instead");
    ret = debug_ok(qts, "debug-step", "{}");
    g_assert_cmpstr(qdict_get_str(ret, "pc"), ==, "0xf001f");
    qobject_unref(ret);

    /* the code segment takes the pc with it */
    ret = debug_ok(qts, "register-write", "{'register': 'cs',"
                   " 'value': '0xe000'}");
    debug_summary_has(ret, "set cs of CPU 0 to 0xe000 (it was 0xf000); the"
                      " pc moved with it, to 0xe001f (it was 0xf001f)");
    qobject_unref(ret);
    qobject_unref(debug_ok(qts, "register-write", "{'register': 'cs',"
                           " 'value': '0xf000'}"));

    debug_check_error(qts, "register-write", "{'register': 'nope',"
                      " 'value': '0'}", "'nope' is not a register; the"
                      " registers are pc, eax");
    debug_check_error(qts, "register-write", "{'register': 'eax',"
                      " 'value': '0x100000000'}",
                      "value 0x100000000 does not fit in 4 bytes");
    debug_check_error(qts, "register-write", "{'register': 'eax',"
                      " 'value': '-2147483649'}", "does not fit in 4 bytes");
    debug_check_error(qts, "register-write", "{'register': 'eax',"
                      " 'value': 'zz'}", "is not a number");
    debug_check_error(qts, "register-write", "{'register': 'st0',"
                      " 'value': '0'}", "st0 has 10 bytes");
    debug_check_error(qts, "register-write", "{'register': 'eax',"
                      " 'value': '0', 'cpu': 3}", "there is no CPU 3");
    qobject_unref(debug_ok(qts, "cont", "{}"));
    debug_check_error(qts, "register-write", "{'register': 'eax',"
                      " 'value': '0'}", "the VM is running");
    qtest_quit(qts);
    unlink(bios);
}

/* debug-trace logs the hits and lets the guest go on */
static void test_debug_trace(void)
{
    g_autofree char *bios = debug_bios_watch();
    QTestState *qts;
    QDict *ret, *e;
    QList *list;

    qts = qtest_initf("-accel tcg -machine pc -nodefaults -vga none "
                      "-bios %s -S", bios);

    /* arrivals at the write: ax counts up, the word follows it */
    ret = debug_ok(qts, "debug-trace", "{'address': '0xf0008', 'count': 3,"
                   " 'registers': ['eax'], 'memory': [{'address': 'ebx'}]}");
    g_assert_cmpint(qdict_get_int(ret, "hits"), ==, 3);
    g_assert(qdict_get_bool(ret, "running"));
    g_assert(!qdict_haskey(ret, "instructions"));
    g_assert_cmpint(qlist_size(qdict_get_qlist(ret, "entries")), ==, 3);
    debug_summary_has(ret, "Traced arrivals at 0xf0008 for ");
    debug_summary_has(ret, ": 3 hits, as many as asked for; the guest runs"
                      " on.\n\nHits:\n"
                      "#1 CPU 0 at 0xf0008: eax=0x1 [ebx]=0x00000000\n"
                      "#2 CPU 0 at 0xf0008: eax=0x2 [ebx]=0x00000001\n"
                      "#3 CPU 0 at 0xf0008: eax=0x3 [ebx]=0x00000002");
    qobject_unref(ret);

    /* what touches 0x1000: each instruction three times, listed once */
    ret = debug_ok(qts, "debug-trace", "{'watch': {'address': '0x1000',"
                   " 'length': 2, 'access': 'access'}, 'count': 6,"
                   " 'list': 0}");
    g_assert_cmpint(qdict_get_int(ret, "hits"), ==, 6);
    g_assert(qdict_get_bool(ret, "running"));
    list = qdict_get_qlist(ret, "instructions");
    g_assert_cmpint(qlist_size(list), ==, 2);
    e = qobject_to(QDict, qlist_peek(list));
    g_assert_cmpstr(qdict_get_str(e, "pc"), ==, "0xf0008");
    g_assert_cmpint(qdict_get_int(e, "count"), ==, 3);
    g_assert_cmpint(qlist_size(qdict_get_qlist(ret, "entries")), ==, 2);
    debug_summary_has(ret, "Traced accesses to 0x1000 (2 bytes) for ");
    debug_summary_has(ret, "\n\nBy instruction:\n"
                      "       3 x 0x000f0008:");
    debug_summary_has(ret, "\n       3 x 0x000f000a:");
    debug_summary_has(ret, "\n\nHits (the first access by each"
                      " instruction):\n#");
    debug_summary_has(ret, " CPU 0 at 0xf0008 wrote to 0x1000 (now 0x");
    debug_summary_has(ret, " CPU 0 at 0xf000a read 0x1000 (now 0x");
    qobject_unref(ret);

    /* an address and a watch: arrivals are not accesses */
    ret = debug_ok(qts, "debug-trace", "{'address': '0xf000c',"
                   " 'watch': {'address': '0x1000'}, 'count': 4}");
    g_assert_cmpint(qdict_get_int(ret, "hits"), ==, 4);
    list = qdict_get_qlist(ret, "instructions");
    g_assert_cmpint(qlist_size(list), ==, 1);
    e = qobject_to(QDict, qlist_peek(list));
    g_assert_cmpint(qdict_get_int(e, "count"), ==, 2);
    g_assert_cmpint(qlist_size(qdict_get_qlist(ret, "entries")), ==, 4);
    qobject_unref(ret);

    /* nobody touches 0x2000 */
    ret = debug_ok(qts, "debug-trace", "{'watch': {'address': '0x2000'},"
                   " 'duration-ms': 200}");
    g_assert_cmpint(qdict_get_int(ret, "hits"), ==, 0);
    g_assert(qdict_get_bool(ret, "running"));
    debug_summary_has(ret, " for 0.2 s: no hits; the guest runs on.");
    qobject_unref(ret);

    debug_check_error(qts, "debug-trace", "{}", "or more of them");
    debug_check_error(qts, "debug-trace", "{'address': '0xf0008',"
                      " 'registers': ['nope']}",
                      "'nope' is not a register; the registers are pc,");
    debug_check_error(qts, "debug-trace", "{'address': '0xf0008',"
                      " 'count': 0}", "count must be 1 to 100000");
    debug_check_error(qts, "debug-trace", "{'address': '0xf0008',"
                      " 'list': 501}", "list must be 0 to 500");
    qtest_quit(qts);
    unlink(bios);
}

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

/*
 * memory-write changes RAM and ROM but no device, and code made from
 * the old bytes goes away; debug-profile finds the loop
 */
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
    if (qtest_has_device("bochs-display")) {
        qtest_add_func("/mcp/screen/text-none", test_screen_text_none);
    }
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
    if (qtest_has_device("ati-vga")) {
        qtest_add_func("/mcp/pointer/hw-cursor", test_pointer_hw_cursor);
    }
    qtest_add_func("/mcp/vm-stats", test_vm_stats);
    qtest_add_func("/mcp/serial/expect", test_serial_expect);
    qtest_add_func("/mcp/serial/run", test_serial_run);
    qtest_add_func("/mcp/serial/run-cancel", test_serial_run_cancel);
    qtest_add_func("/mcp/serial/telnet", test_serial_telnet);
    qtest_add_func("/mcp/steps", test_steps);
    qtest_add_func("/mcp/media/disc", test_media_disc);
    qtest_add_func("/mcp/media/floppy", test_media_floppy);
    if (qtest_has_accel("tcg")) {
        qtest_add_func("/mcp/screen/text", test_screen_text);
        qtest_add_func("/mcp/debug/run", test_debug_run);
        qtest_add_func("/mcp/debug/memory", test_debug_memory);
        qtest_add_func("/mcp/steps/debug", test_steps_debug);
        qtest_add_func("/mcp/debug/retry", test_debug_retry);
        qtest_add_func("/mcp/debug/registers", test_debug_registers);
        qtest_add_func("/mcp/debug/watch", test_debug_watch);
        qtest_add_func("/mcp/debug/trace", test_debug_trace);
        qtest_add_func("/mcp/debug/history", test_debug_history);
        qtest_add_func("/mcp/debug/mask", test_debug_mask);
        qtest_add_func("/mcp/debug/several", test_debug_several);
        qtest_add_func("/mcp/debug/exception", test_debug_exception);
        qtest_add_func("/mcp/debug/register", test_debug_register);
    }
    return g_test_run();
}
