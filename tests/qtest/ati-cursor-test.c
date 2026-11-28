/*
 * QTest testcase for the ATI Radeon hardware cursor
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/pci.h"
#include "libqos/pci-pc.h"

#define CRTC_GEN_CNTL       0x0050
#define CRTC_EXT_CNTL       0x0054
#define MC_FB_LOCATION      0x0148
#define CRTC_H_TOTAL_DISP   0x0200
#define CRTC_V_TOTAL_DISP   0x0208
#define CRTC_OFFSET         0x0224
#define CRTC_PITCH          0x022c
#define CUR_OFFSET          0x0260
#define CUR_HORZ_VERT_POSN  0x0264
#define CUR_HORZ_VERT_OFF   0x0268
#define SURFACE_CNTL        0x0b00

#define PIX_WIDTH_32BPP     0x00000600
#define CUR_EN              0x00010000
#define CUR_MODE(m)         ((m) << 20)
#define EXT_DISP_EN         0x01000000
#define CRTC_EN             0x02000000

#define W                   640
#define H                   480
#define CUR                 0x200000    /* the cursor image */
#define CX                  100         /* where the cursor is */
#define CY                  50
#define BG                  0x00204060  /* the screen under it */

typedef struct {
    QTestState *qts;
    QPCIBus *bus;
    QPCIDevice *dev;
    QPCIBar fb, mmio;
} Ctx;

static void reg(Ctx *t, uint32_t r, uint32_t v)
{
    qpci_io_writel(t->dev, t->mmio, r, v);
}

static void start(Ctx *t)
{
    t->qts = qtest_init("-vga none -device ati-vga,model=rv100,addr=04.0,"
                        "guest_hwcursor=true");
    t->bus = qpci_new_pc(t->qts, NULL);
    t->dev = qpci_device_find(t->bus, QPCI_DEVFN(4, 0));
    g_assert(t->dev);
    qpci_device_enable(t->dev);
    t->fb = qpci_iomap(t->dev, 0, NULL);
    t->mmio = qpci_iomap(t->dev, 2, NULL);

    reg(t, MC_FB_LOCATION, 0x00ff0000);
    reg(t, SURFACE_CNTL, 0);
    for (int y = CY; y < CY + 4; y++) {
        for (int x = CX; x < CX + 8; x++) {
            qpci_io_writel(t->dev, t->fb, (y * W + x) * 4, BG);
        }
    }
    /* 640x480, 32 bits, at offset 0 */
    reg(t, CRTC_H_TOTAL_DISP, (W / 8 - 1) << 16);
    reg(t, CRTC_V_TOTAL_DISP, (H - 1) << 16);
    reg(t, CRTC_PITCH, W / 8);
    reg(t, CRTC_OFFSET, 0);
    reg(t, CRTC_GEN_CNTL, EXT_DISP_EN | CRTC_EN | PIX_WIDTH_32BPP);
    reg(t, CRTC_EXT_CNTL, 0);   /* the display on */
}

static void stop(Ctx *t)
{
    g_free(t->dev);
    qpci_free_pc(t->bus);
    qtest_quit(t->qts);
}

/* the cursor's first line; the rest is clear */
static void cursor(Ctx *t, int mode, const uint32_t *px, int n)
{
    for (int i = 0; i < 64 * 64; i++) {
        qpci_io_writel(t->dev, t->fb, CUR + i * 4, i < n ? px[i] : 0);
    }
    reg(t, CUR_OFFSET, CUR);
    reg(t, CUR_HORZ_VERT_OFF, 0);
    reg(t, CUR_HORZ_VERT_POSN, CX << 16 | CY);
    reg(t, CRTC_GEN_CNTL, EXT_DISP_EN | CRTC_EN | PIX_WIDTH_32BPP |
                          CUR_EN | CUR_MODE(mode));
}

/* the screen as the display shows it, RGB */
static uint8_t *screen(Ctx *t)
{
    g_autofree char *path = NULL;
    g_autofree char *data = NULL;
    uint8_t *rgb;
    size_t len;
    int fd, w, h, max, off;

    fd = g_file_open_tmp("ati-cursor-XXXXXX.ppm", &path, NULL);
    g_assert(fd >= 0);
    close(fd);
    qtest_qmp_assert_success(t->qts, "{'execute': 'screendump', "
                             "'arguments': {'filename': %s}}", path);
    g_assert(g_file_get_contents(path, &data, &len, NULL));
    unlink(path);
    g_assert_cmpint(sscanf(data, "P6 %d %d %d%n", &w, &h, &max, &off), ==, 3);
    g_assert_cmpint(w, ==, W);
    g_assert_cmpint(h, ==, H);
    off++;
    g_assert_cmpint(len, ==, off + W * H * 3);
    rgb = g_malloc(W * H * 3);
    memcpy(rgb, data + off, W * H * 3);
    return rgb;
}

static uint32_t at(const uint8_t *rgb, int x, int y)
{
    const uint8_t *p = rgb + (y * W + x) * 3;

    return p[0] << 16 | p[1] << 8 | p[2];
}

/* blends each channel: (c * a + d * (255 - a)) / 255 */
static uint32_t blend(uint32_t c, int a, uint32_t d)
{
    uint32_t v = 0;

    for (int sh = 0; sh < 24; sh += 8) {
        v |= (((c >> sh) & 0xff) * a + ((d >> sh) & 0xff) * (255 - a)) / 255
             << sh;
    }
    return v;
}

/* mode 3, as AmigaOS 4.1 and MorphOS use it: alpha not multiplied in */
static void test_argb(void)
{
    static const uint32_t px[] = {
        0xffcc2211,     /* opaque */
        0x00ffffff,     /* clear, though white */
        0x800000ff,     /* half blue */
    };
    g_autofree uint8_t *rgb = NULL;
    Ctx t;

    start(&t);
    cursor(&t, 3, px, ARRAY_SIZE(px));
    rgb = screen(&t);
    g_assert_cmphex(at(rgb, CX, CY), ==, 0xcc2211);
    g_assert_cmphex(at(rgb, CX + 1, CY), ==, BG);
    g_assert_cmphex(at(rgb, CX + 2, CY), ==, blend(0x0000ff, 0x80, BG));
    g_assert_cmphex(at(rgb, CX + 3, CY), ==, BG);
    g_assert_cmphex(at(rgb, CX, CY + 1), ==, BG);
    stop(&t);
}

/* mode 2, as X.org uses it: the colour is multiplied by the alpha */
static void test_argb_premultiplied(void)
{
    static const uint32_t px[] = {
        0xffcc2211,     /* opaque */
        0x00000000,     /* clear */
        0x80000080,     /* half blue */
    };
    g_autofree uint8_t *rgb = NULL;
    Ctx t;

    start(&t);
    cursor(&t, 2, px, ARRAY_SIZE(px));
    rgb = screen(&t);
    g_assert_cmphex(at(rgb, CX, CY), ==, 0xcc2211);
    g_assert_cmphex(at(rgb, CX + 1, CY), ==, BG);
    g_assert_cmphex(at(rgb, CX + 2, CY), ==, blend(0, 0x80, BG) + 0x80);
    stop(&t);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ati-cursor/argb", test_argb);
    qtest_add_func("/ati-cursor/argb-premultiplied", test_argb_premultiplied);
    return g_test_run();
}
