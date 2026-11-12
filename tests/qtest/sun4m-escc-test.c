/*
 * QTest: the Sun keyboard and mouse on the two channels of one ESCC
 *
 * On a sun4m machine the keyboard is channel A and the mouse channel B of
 * one chip, with one interrupt for both. SunOS 4 reads RR2 of channel B to
 * learn which one to service. When a keyboard byte and a mouse byte wait
 * together, RR2 must name the keyboard first and the mouse after it, RR3
 * must show both, and the interrupt must stay up until both are read and
 * go down then. Each key must give one code when it goes down and one when
 * it comes up, also Caps Lock and a key that the host repeats, and the
 * mouse must send whole packets of five bytes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* SPARCstation 5: the chip, with a register every 2 bytes */
#define ESCC            0x71000000ULL
#define MS_CTRL         (ESCC + 0)
#define MS_DATA         (ESCC + 2)
#define KBD_CTRL        (ESCC + 4)
#define KBD_DATA        (ESCC + 6)

/* The pending interrupts of the machine, and the one of this chip */
#define INTCTL_PENDING  0x71e10000ULL
#define KBD_MS_IRQ      (1 << 14)

#define WR1_RXINTALL    0x10

#define RR3_RXINTA      0x20
#define RR3_RXINTB      0x04

/* RR2 of channel B with the status in bits 1 to 3 */
#define RR2_RXINTA      0x0c
#define RR2_RXINTB      0x04
#define RR2_NOINT       0x06

#define SUN_KEY_SHIFT   0x63
#define SUN_KEY_CAPS    0x77
#define SUN_KEY_X       0x65
#define SUN_KEY_CTRL    0x4c
#define SUN_KEY_UP      0x80

#define MS_START        0x87    /* start of a packet, no button down */

static uint8_t rreg(QTestState *qts, uint64_t ctrl, uint8_t reg)
{
    qtest_writeb(qts, ctrl, reg);
    return qtest_readb(qts, ctrl);
}

static void wreg(QTestState *qts, uint64_t ctrl, uint8_t reg, uint8_t val)
{
    qtest_writeb(qts, ctrl, reg);
    qtest_writeb(qts, ctrl, val);
}

static bool irq(QTestState *qts)
{
    return qtest_readl(qts, INTCTL_PENDING) & KBD_MS_IRQ;
}

static QTestState *start(void)
{
    QTestState *qts = qtest_init("-machine SS-5 -display none");

    wreg(qts, KBD_CTRL, 1, WR1_RXINTALL);
    wreg(qts, MS_CTRL, 1, WR1_RXINTALL);
    return qts;
}

static void key(QTestState *qts, const char *qcode, bool down)
{
    qtest_qmp_assert_success(qts,
        "{ 'execute': 'input-send-event', 'arguments': { 'events': ["
        "  { 'type': 'key', 'data': { 'down': %i,"
        "    'key': { 'type': 'qcode', 'data': %s } } } ] } }", down, qcode);
}

static void move(QTestState *qts, int dx)
{
    qtest_qmp_assert_success(qts,
        "{ 'execute': 'input-send-event', 'arguments': { 'events': ["
        "  { 'type': 'rel', 'data': { 'axis': 'x', 'value': %d } } ] } }",
        dx);
}

/* Read all that the keyboard has sent, up to len codes */
static int kbd_read(QTestState *qts, uint8_t *buf, int len)
{
    int n = 0;

    while (n < len && (rreg(qts, KBD_CTRL, 0) & 1)) {
        buf[n++] = qtest_readb(qts, KBD_DATA);
    }
    return n;
}

static void test_kbd_and_mouse(void)
{
    QTestState *qts = start();
    int i;

    g_assert_false(irq(qts));
    g_assert_cmphex(rreg(qts, MS_CTRL, 2), ==, RR2_NOINT);
    g_assert_cmphex(rreg(qts, KBD_CTRL, 3), ==, 0);

    /* A key goes down, then the mouse moves, before the guest reads */
    key(qts, "shift", true);
    move(qts, 2);

    g_assert_true(irq(qts));
    g_assert_cmphex(rreg(qts, KBD_CTRL, 3), ==, RR3_RXINTA | RR3_RXINTB);
    g_assert_cmphex(rreg(qts, MS_CTRL, 2), ==, RR2_RXINTA);

    /* The keyboard is read: now the mouse is the one to service */
    g_assert_cmphex(qtest_readb(qts, KBD_DATA), ==, SUN_KEY_SHIFT);
    g_assert_true(irq(qts));
    g_assert_cmphex(rreg(qts, KBD_CTRL, 3), ==, RR3_RXINTB);
    g_assert_cmphex(rreg(qts, MS_CTRL, 2), ==, RR2_RXINTB);

    g_assert_cmphex(qtest_readb(qts, MS_DATA), ==, MS_START);
    g_assert_cmphex(rreg(qts, MS_CTRL, 2), ==, RR2_RXINTB);
    g_assert_cmphex(qtest_readb(qts, MS_DATA), ==, 2);

    /* The key comes up in the middle of the mouse packet: it goes first */
    key(qts, "shift", false);
    g_assert_cmphex(rreg(qts, MS_CTRL, 2), ==, RR2_RXINTA);
    g_assert_cmphex(qtest_readb(qts, KBD_DATA), ==,
                    SUN_KEY_SHIFT | SUN_KEY_UP);
    g_assert_cmphex(rreg(qts, MS_CTRL, 2), ==, RR2_RXINTB);

    for (i = 0; i < 3; i++) {
        g_assert_true(irq(qts));
        g_assert_cmphex(qtest_readb(qts, MS_DATA), ==, 0);
    }

    /* All read: nothing pending, and the interrupt is down */
    g_assert_false(irq(qts));
    g_assert_cmphex(rreg(qts, KBD_CTRL, 3), ==, 0);
    g_assert_cmphex(rreg(qts, MS_CTRL, 2), ==, RR2_NOINT);

    /* The mouse first, the keyboard after it */
    move(qts, -3);
    key(qts, "ctrl", true);
    g_assert_cmphex(rreg(qts, MS_CTRL, 2), ==, RR2_RXINTA);
    for (i = 0; i < 5; i++) {
        qtest_readb(qts, MS_DATA);
    }
    /* Only the keyboard is left: its interrupt must not be lost */
    g_assert_true(irq(qts));
    g_assert_cmphex(rreg(qts, KBD_CTRL, 3), ==, RR3_RXINTA);
    g_assert_cmphex(rreg(qts, MS_CTRL, 2), ==, RR2_RXINTA);
    g_assert_cmphex(qtest_readb(qts, KBD_DATA), ==, SUN_KEY_CTRL);
    g_assert_false(irq(qts));

    qtest_quit(qts);
}

static void test_keys(void)
{
    QTestState *qts = start();
    uint8_t buf[16];
    int n;

    /* Caps Lock twice: each press and each release is sent */
    key(qts, "caps_lock", true);
    key(qts, "caps_lock", false);
    key(qts, "caps_lock", true);
    key(qts, "caps_lock", false);
    n = kbd_read(qts, buf, sizeof(buf));
    g_assert_cmpint(n, ==, 4);
    g_assert_cmphex(buf[0], ==, SUN_KEY_CAPS);
    g_assert_cmphex(buf[1], ==, SUN_KEY_CAPS | SUN_KEY_UP);
    g_assert_cmphex(buf[2], ==, SUN_KEY_CAPS);
    g_assert_cmphex(buf[3], ==, SUN_KEY_CAPS | SUN_KEY_UP);

    /* The host repeats a held key: the Sun keyboard does not */
    key(qts, "x", true);
    key(qts, "x", true);
    key(qts, "x", true);
    key(qts, "x", false);
    n = kbd_read(qts, buf, sizeof(buf));
    g_assert_cmpint(n, ==, 2);
    g_assert_cmphex(buf[0], ==, SUN_KEY_X);
    g_assert_cmphex(buf[1], ==, SUN_KEY_X | SUN_KEY_UP);

    /* Left and right Control are one key: up with the last of them */
    key(qts, "ctrl", true);
    key(qts, "ctrl_r", true);
    key(qts, "ctrl", false);
    key(qts, "ctrl_r", false);
    n = kbd_read(qts, buf, sizeof(buf));
    g_assert_cmpint(n, ==, 2);
    g_assert_cmphex(buf[0], ==, SUN_KEY_CTRL);
    g_assert_cmphex(buf[1], ==, SUN_KEY_CTRL | SUN_KEY_UP);

    /* A key that the Sun keyboard does not have sends nothing */
    key(qts, "volumeup", true);
    key(qts, "volumeup", false);
    g_assert_cmpint(kbd_read(qts, buf, sizeof(buf)), ==, 0);

    qtest_quit(qts);
}

static void test_mouse_packets(void)
{
    QTestState *qts = start();
    int i, n, sum = 0;

    /*
     * The guest reads nothing while the mouse moves 100 times, and once
     * by more than fits in one packet. All of the motion must arrive, in
     * whole packets.
     */
    for (i = 0; i < 100; i++) {
        move(qts, 1);
    }
    move(qts, 300);

    for (n = 0; n < 100 && (rreg(qts, MS_CTRL, 0) & 1); n++) {
        g_assert_cmphex(qtest_readb(qts, MS_DATA), ==, MS_START);
        sum += (int8_t)qtest_readb(qts, MS_DATA);
        g_assert_cmphex(qtest_readb(qts, MS_DATA), ==, 0);
        g_assert_cmphex(qtest_readb(qts, MS_DATA), ==, 0);
        g_assert_cmphex(qtest_readb(qts, MS_DATA), ==, 0);
    }
    g_assert_cmpint(sum, ==, 400);
    /* 2 packets waited, then the rest came 127 at a time */
    g_assert_cmpint(n, ==, 6);
    g_assert_false(irq(qts));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sun4m/escc/kbd-and-mouse", test_kbd_and_mouse);
    qtest_add_func("/sun4m/escc/keys", test_keys);
    qtest_add_func("/sun4m/escc/mouse-packets", test_mouse_packets);

    return g_test_run();
}
