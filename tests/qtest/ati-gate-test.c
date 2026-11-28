/*
 * QTest testcase for the ATI models: what only the R100 and R200 do
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/pci.h"
#include "libqos/pci-pc.h"

#define CLOCK_CNTL_INDEX    0x0008
#define CLOCK_CNTL_DATA     0x000c
#define GEN_INT_CNTL        0x0040
#define GEN_INT_STATUS      0x0044
#define CSQ_APER_PRIMARY    0x1000
#define PP_CNTL             0x1c38

#define PLL_WR_EN           0x80
#define PPLL_REF_DIV        0x03
#define PPLL_ATOMIC_UPDATE  0x00008000

#define CRTC_VBLANK_INT     0x00000001
#define SW_INT_TEST         0x02000000
#define SW_INT_FIRE         0x04000000

typedef struct {
    QTestState *qts;
    QPCIBus *bus;
    QPCIDevice *dev;
    QPCIBar mmio;
} Ctx;

static void start(Ctx *t, const char *model)
{
    t->qts = qtest_initf("-vga none -device ati-vga,model=%s,addr=04.0",
                         model);
    t->bus = qpci_new_pc(t->qts, NULL);
    t->dev = qpci_device_find(t->bus, QPCI_DEVFN(4, 0));
    g_assert(t->dev);
    qpci_device_enable(t->dev);
    t->mmio = qpci_iomap(t->dev, 2, NULL);
}

static void stop(Ctx *t)
{
    g_free(t->dev);
    qpci_free_pc(t->bus);
    qtest_quit(t->qts);
}

static void reg(Ctx *t, uint32_t r, uint32_t v)
{
    qpci_io_writel(t->dev, t->mmio, r, v);
}

static uint32_t rd(Ctx *t, uint32_t r)
{
    return qpci_io_readl(t->dev, t->mmio, r);
}

/* the models that keep the behaviour they had before */
static const char *const others[] = { "es1000", "rv200" };

/* registers that are not modeled read back only on the R100 and R200 */
static void test_readback(void)
{
    Ctx t;

    start(&t, "rv100");
    reg(&t, PP_CNTL, 0x12345678);
    g_assert_cmphex(rd(&t, PP_CNTL), ==, 0x12345678);
    stop(&t);

    for (int i = 0; i < ARRAY_SIZE(others); i++) {
        start(&t, others[i]);
        reg(&t, PP_CNTL, 0x12345678);
        g_assert_cmphex(rd(&t, PP_CNTL), ==, 0);
        stop(&t);
    }
}

/*
 * Every model keeps the PLL registers, and an update of the dividers is
 * done at once, so a driver that waits for it goes on.
 */
static void test_pll(void)
{
    static const char *const models[] = { "rv100", "es1000", "rv200" };
    Ctx t;

    for (int i = 0; i < ARRAY_SIZE(models); i++) {
        start(&t, models[i]);
        reg(&t, CLOCK_CNTL_INDEX, PPLL_REF_DIV | PLL_WR_EN);
        reg(&t, CLOCK_CNTL_DATA, PPLL_ATOMIC_UPDATE | 0x1c);
        reg(&t, CLOCK_CNTL_INDEX, PPLL_REF_DIV);
        g_assert_cmphex(rd(&t, CLOCK_CNTL_DATA), ==, 0x1c);
        stop(&t);
    }
}

/* the software interrupt that the CP fires can be acked */
static void test_sw_int(void)
{
    Ctx t;

    start(&t, "rv100");
    reg(&t, GEN_INT_STATUS, SW_INT_FIRE);
    g_assert_cmphex(rd(&t, GEN_INT_STATUS) & SW_INT_TEST, ==, SW_INT_TEST);
    reg(&t, GEN_INT_STATUS, SW_INT_TEST);
    g_assert_cmphex(rd(&t, GEN_INT_STATUS) & SW_INT_TEST, ==, 0);
    stop(&t);

    for (int i = 0; i < ARRAY_SIZE(others); i++) {
        start(&t, others[i]);
        reg(&t, GEN_INT_STATUS, SW_INT_FIRE);
        g_assert_cmphex(rd(&t, GEN_INT_STATUS) & SW_INT_TEST, ==, 0);
        stop(&t);
    }
}

/*
 * A type 0 packet that writes the CSQ aperture would feed the CP from
 * inside itself, one level for each dword of the packet: it is dropped, and
 * QEMU does not run out of stack. 0x4000 levels fit in Linux's 8 MB stack
 * but not in macOS's 512 KB thread stacks; run with ulimit -s 512 to see
 * QEMU crash without the check.
 */
static void test_csq_recursion(void)
{
    /* type 0, CSQ_APER_PRIMARY, one register, 0x4000 dwords */
    uint32_t hdr = 0x3fff << 16 | 1 << 15 | CSQ_APER_PRIMARY >> 2;
    Ctx t;

    start(&t, "rv100");
    for (int i = 0; i < 64; i++) {
        reg(&t, CSQ_APER_PRIMARY, hdr);
    }
    reg(&t, PP_CNTL, 0xcafe);
    g_assert_cmphex(rd(&t, PP_CNTL), ==, 0xcafe);
    stop(&t);
}

/*
 * The frame timer: always on the R100 and R200, which set the status bits
 * every frame; elsewhere only while the VBLANK interrupt is on.
 */
static void test_vblank(void)
{
    Ctx t;

    start(&t, "rv100");
    qtest_clock_step(t.qts, 100 * 1000 * 1000);
    g_assert_cmphex(rd(&t, GEN_INT_STATUS) & CRTC_VBLANK_INT, ==,
                    CRTC_VBLANK_INT);
    stop(&t);

    for (int i = 0; i < ARRAY_SIZE(others); i++) {
        start(&t, others[i]);
        qtest_clock_step(t.qts, 100 * 1000 * 1000);
        g_assert_cmphex(rd(&t, GEN_INT_STATUS) & CRTC_VBLANK_INT, ==, 0);
        reg(&t, GEN_INT_CNTL, CRTC_VBLANK_INT);
        qtest_clock_step(t.qts, 100 * 1000 * 1000);
        g_assert_cmphex(rd(&t, GEN_INT_STATUS) & CRTC_VBLANK_INT, ==,
                        CRTC_VBLANK_INT);
        stop(&t);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ati-gate/readback", test_readback);
    qtest_add_func("/ati-gate/pll", test_pll);
    qtest_add_func("/ati-gate/sw-int", test_sw_int);
    qtest_add_func("/ati-gate/csq-recursion", test_csq_recursion);
    qtest_add_func("/ati-gate/vblank", test_vblank);
    return g_test_run();
}
