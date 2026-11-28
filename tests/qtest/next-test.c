/*
 * QTest for the NeXT 68k machines
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Each machine starts without its ROM. The test reads SCR1, raises the
 * software interrupts through SCR2, reads the clock chip and the NVRAM
 * one bit at a time as the ROM does, and copies memory with the
 * memory-to-memory DMA channels, chained.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define NEXT_IO         0x02000000
#define DMA_CSR         (NEXT_IO + 0x0000)
#define DMA_REGS        (NEXT_IO + 0x4000)
#define INT_STATUS      (NEXT_IO + 0x7000)
#define INT_MASK        (NEXT_IO + 0x7800)
#define SCR1            (NEXT_IO + 0xc000)
#define SCR2            (NEXT_IO + 0xd000)
#define RAM             0x04000000

/* SCR2 byte 0: software interrupts; byte 2: the clock chip's lines */
#define SCR2_SOFTINT1   0x01
#define SCR2_SOFTINT2   0x02
#define SCR2_RTCE       0x01
#define SCR2_RTCLK      0x02
#define SCR2_RTDATA     0x04

/* DMA CSR bits as the CPU writes and reads them */
#define DMA_ENABLE      0x01000000
#define DMA_COMPLETE    0x08000000
#define DMA_SETENABLE   0x00010000
#define DMA_SETSUPDATE  0x00020000
#define DMA_RESET       0x00100000
#define DMA_R2M         0x1c0
#define DMA_M2R         0x1d0

typedef struct NeXTModel {
    const char *machine;
    uint32_t scr1;
    bool new_rtc;
    bool turbo;
} NeXTModel;

static const NeXTModel models[] = {
    /* DMA revision 1, the model in bits 12-15, board revision, 25 MHz */
    { "next-computer",            0x00010102, false, false },
    { "next-station",             0x00011002, false, false },
    { "next-cube",                0x00012002, false, false },
    { "next-station-color",       0x00013002, false, false },
    /* Turbo: model 4; the stations say so in the top nibble */
    { "next-cube-turbo",          0x00004000, true,  true  },
    { "next-station-turbo",       0xf0004000, true,  true  },
    { "next-station-turbo-color", 0xf0004000, true,  true  },
};

static QTestState *next_start(const NeXTModel *m)
{
    return qtest_initf("-M %s", m->machine);
}

static void test_scr1(const void *data)
{
    const NeXTModel *m = data;
    QTestState *qts = next_start(m);

    g_assert_cmphex(qtest_readl(qts, SCR1), ==, m->scr1);
    qtest_quit(qts);
}

static void test_softint(const void *data)
{
    const NeXTModel *m = data;
    QTestState *qts = next_start(m);

    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & 3, ==, 0);

    /* the status shows the software interrupts, masked or not */
    qtest_writeb(qts, SCR2, SCR2_SOFTINT1);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & 3, ==, 1);
    qtest_writeb(qts, SCR2, SCR2_SOFTINT2);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & 3, ==, 2);
    qtest_writeb(qts, SCR2, 0);
    g_assert_cmphex(qtest_readl(qts, INT_STATUS) & 3, ==, 0);

    /* the mask keeps what the CPU writes */
    qtest_writel(qts, INT_MASK, 0x00000001);
    g_assert_cmphex(qtest_readl(qts, INT_MASK) & 3, ==, 1);
    qtest_writel(qts, INT_MASK, 0x00000002);
    g_assert_cmphex(qtest_readl(qts, INT_MASK) & 3, ==, 2);
    /* byte writes reach the low byte of the long register */
    qtest_writeb(qts, INT_MASK + 3, 0x03);
    g_assert_cmphex(qtest_readl(qts, INT_MASK) & 3, ==, 3);
    qtest_quit(qts);
}

/* One clock of the serial line: the chip takes the bit as it goes low */
static bool rtc_clock(QTestState *qts, uint8_t base, bool bit)
{
    uint8_t d = bit ? SCR2_RTDATA : 0;

    qtest_writeb(qts, SCR2 + 2, base | SCR2_RTCE | SCR2_RTCLK | d);
    qtest_writeb(qts, SCR2 + 2, base | SCR2_RTCE | d);
    return qtest_readb(qts, SCR2 + 2) & SCR2_RTDATA;
}

/* Read n bytes from addr on: the address goes up after each byte */
static void rtc_read(QTestState *qts, uint8_t addr, uint8_t *buf, int n)
{
    uint8_t base = qtest_readb(qts, SCR2 + 2) &
                   ~(SCR2_RTCE | SCR2_RTCLK | SCR2_RTDATA);

    qtest_writeb(qts, SCR2 + 2, base | SCR2_RTCE);
    for (int i = 7; i >= 0; i--) {
        rtc_clock(qts, base, (addr >> i) & 1);
    }
    for (int b = 0; b < n; b++) {
        uint8_t v = 0;

        for (int i = 0; i < 8; i++) {
            v = (v << 1) | rtc_clock(qts, base, false);
        }
        buf[b] = v;
    }
    qtest_writeb(qts, SCR2 + 2, base);
}

static void test_rtc(const void *data)
{
    const NeXTModel *m = data;
    QTestState *qts = next_start(m);
    uint8_t status, ram[32];
    unsigned sum = 0;

    /* status bit 7 tells the MCCS1850 from the MC68HC68T1 */
    rtc_read(qts, 0x30, &status, 1);
    g_assert_cmpint(!!(status & 0x80), ==, m->new_rtc);

    /*
     * The NVRAM: the ROM's checksum over the first 30 bytes, an end-around
     * carry sum of words, is in the last two
     */
    rtc_read(qts, 0x00, ram, sizeof(ram));
    for (int i = 0; i < 30; i += 2) {
        sum += (ram[i] << 8) | ram[i + 1];
        if (sum >= 0x10000) {
            sum -= 0x10000;
            sum += 1;
        }
    }
    g_assert_cmphex(0xffff - sum, ==, (ram[30] << 8) | ram[31]);
    /* power-on test on, which the ROM needs; boot from SCSI */
    g_assert_cmphex(ram[14], ==, 0x4b);
    g_assert_cmpint(ram[18], ==, 's');
    g_assert_cmpint(ram[19], ==, 'd');
    qtest_quit(qts);
}

static void dma_set(QTestState *qts, unsigned ch, uint32_t next,
                    uint32_t limit, uint32_t start, uint32_t stop)
{
    qtest_writel(qts, DMA_REGS + ch + 0x0, next);
    qtest_writel(qts, DMA_REGS + ch + 0x4, limit);
    qtest_writel(qts, DMA_REGS + ch + 0x8, start);
    qtest_writel(qts, DMA_REGS + ch + 0xc, stop);
}

static void test_dma_chain(const void *data)
{
    const NeXTModel *m = data;
    QTestState *qts = next_start(m);
    uint32_t src = RAM + 0x10000, dst1 = RAM + 0x20000, dst2 = RAM + 0x30000;
    uint8_t in[64], out[32];

    for (int i = 0; i < sizeof(in); i++) {
        in[i] = i * 7 + 1;
    }
    qtest_memwrite(qts, src, in, sizeof(in));

    /* memory to the buffer: 64 bytes; the buffer to memory: 32, then 32 */
    qtest_writel(qts, DMA_CSR + DMA_R2M, DMA_RESET);
    qtest_writel(qts, DMA_CSR + DMA_M2R, DMA_RESET);
    dma_set(qts, DMA_R2M, dst1, dst1 + 32, dst2, dst2 + 32);
    dma_set(qts, DMA_M2R, src, src + 64, 0, 0);
    qtest_writel(qts, DMA_CSR + DMA_R2M, DMA_SETSUPDATE | DMA_SETENABLE);
    qtest_writel(qts, DMA_CSR + DMA_M2R, DMA_SETENABLE);
    qtest_clock_step(qts, 1000000);

    qtest_memread(qts, dst1, out, sizeof(out));
    g_assert(!memcmp(out, in, 32));
    qtest_memread(qts, dst2, out, sizeof(out));
    g_assert(!memcmp(out, in + 32, 32));

    /* both buffers done: complete, and no longer enabled */
    g_assert_cmphex(qtest_readl(qts, DMA_CSR + DMA_R2M) &
                    (DMA_COMPLETE | DMA_ENABLE), ==, DMA_COMPLETE);
    g_assert_cmphex(qtest_readl(qts, DMA_REGS + DMA_R2M), ==, dst2 + 32);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    for (int i = 0; i < ARRAY_SIZE(models); i++) {
        const NeXTModel *m = &models[i];
        g_autofree char *scr1 = g_strdup_printf("next/%s/scr1", m->machine);
        g_autofree char *intr = g_strdup_printf("next/%s/softint",
                                                m->machine);
        g_autofree char *rtc = g_strdup_printf("next/%s/rtc", m->machine);

        if (!qtest_has_machine(m->machine)) {
            continue;
        }
        qtest_add_data_func(scr1, m, test_scr1);
        qtest_add_data_func(intr, m, test_softint);
        qtest_add_data_func(rtc, m, test_rtc);
        /* the Turbo DMA does not do memory to memory here */
        if (!m->turbo) {
            g_autofree char *dma = g_strdup_printf("next/%s/dma-chain",
                                                   m->machine);

            qtest_add_data_func(dma, m, test_dma_chain);
        }
    }
    return g_test_run();
}
