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
 * The serial tests play the guest on COM1: a terminal that echoes what
 * "serial-run" types, and a shell that answers it.
 *
 * The debug tests run a BIOS of two instructions under TCG, step it,
 * stop it at an address and patch it while it runs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <sys/socket.h>
#include <sys/un.h>
#include "libqtest.h"
#include "libqos/pci-pc.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
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

static void screen_init(Screen *s)
{
    s->qts = qtest_init("-machine pc -display none -vga std");
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

static void screen_free(Screen *s)
{
    g_free(s->dev);
    qpci_free_pc(s->bus);
    qtest_quit(s->qts);
}

static void screen_fill(Screen *s, int x, int y, int w, int h, uint32_t rgb)
{
    g_autofree uint32_t *row = g_new(uint32_t, w);
    int i;

    for (i = 0; i < w; i++) {
        row[i] = cpu_to_le32(rgb);
    }
    for (i = 0; i < h; i++) {
        qpci_memwrite(s->dev, s->lfb, ((y + i) * SCR_W + x) * 4, row, w * 4);
    }
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
    qtest_add_func("/mcp/vm-stats", test_vm_stats);
    qtest_add_func("/mcp/serial/expect", test_serial_expect);
    qtest_add_func("/mcp/serial/run", test_serial_run);
    qtest_add_func("/mcp/serial/run-cancel", test_serial_run_cancel);
    if (qtest_has_accel("tcg")) {
        qtest_add_func("/mcp/debug/run", test_debug_run);
        qtest_add_func("/mcp/debug/memory", test_debug_memory);
    }
    return g_test_run();
}
