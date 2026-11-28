/*
 * QTest testcase for the Cirrus Logic CS4281
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/libqos-pc.h"
#include "libqos/pci.h"

#define BA0_HISR    0x000
#define BA0_HICR    0x008
#define BA0_HIMR    0x00c
#define BA0_HDSR0   0x0f0
#define BA0_DBA0    0x118
#define BA0_DBC0    0x11c
#define BA0_DMR0    0x150
#define BA0_DCR0    0x154
#define BA0_ACCTL   0x460
#define BA0_ACSTS   0x464
#define BA0_ACCAD   0x46c
#define BA0_ACCDA   0x470
#define BA0_ACSDA   0x47c

#define HISR_DMA0   (1u << 8)
#define HICR_EOI    0x3
#define HDSR_DTC    (1u << 16)
#define HDSR_DHTC   (1u << 17)
#define DMR_DMA     (1u << 29)
#define DMR_AUTO    (1u << 4)
#define DMR_TR_READ (2u << 2)
#define DCR_HTCIE   (1u << 17)
#define DCR_TCIE    (1u << 16)
#define ACCTL_CRW   (1u << 4)
#define ACCTL_DCV   (1u << 3)
#define ACSTS_VSTS  (1u << 1)

#define CFG_CWPR    0xe0
#define CFG_SSID    0xfc

typedef struct {
    QOSState *qs;
    QPCIDevice *dev;
    QPCIBar bar;
} CS;

static void save_fn(QPCIDevice *dev, int devfn, void *data)
{
    *(QPCIDevice **)data = dev;
}

static void cs_start(CS *c)
{
    c->qs = qtest_pc_boot("-audiodev none,id=snd "
                          "-device cs4281,audiodev=snd");
    c->dev = NULL;
    qpci_device_foreach(c->qs->pcibus, 0x1013, 0x6005, save_fn, &c->dev);
    g_assert(c->dev);
    qpci_device_enable(c->dev);
    c->bar = qpci_iomap(c->dev, 0, NULL);
}

static void cs_stop(CS *c)
{
    g_free(c->dev);
    qtest_shutdown(c->qs);
}

static uint32_t rd(CS *c, uint32_t reg)
{
    return qpci_io_readl(c->dev, c->bar, reg);
}

static void wr(CS *c, uint32_t reg, uint32_t val)
{
    qpci_io_writel(c->dev, c->bar, reg, val);
}

/* E4h-FFh of the configuration space only take writes after CWPR unlocks */
static void test_config_protect(void)
{
    CS c;

    cs_start(&c);
    qpci_config_writel(c.dev, CFG_SSID, 0x12345678);
    g_assert_cmphex(qpci_config_readl(c.dev, CFG_SSID), !=, 0x12345678);
    qpci_config_writel(c.dev, CFG_CWPR, 0x4281);
    qpci_config_writel(c.dev, CFG_SSID, 0x12345678);
    g_assert_cmphex(qpci_config_readl(c.dev, CFG_SSID), ==, 0x12345678);
    /* the subsystem ID shows at 2Ch too */
    g_assert_cmphex(qpci_config_readl(c.dev, 0x2c), ==,
                    0x12345678);
    cs_stop(&c);
}

static uint16_t codec_read(CS *c, uint8_t reg)
{
    wr(c, BA0_ACCAD, reg);
    wr(c, BA0_ACCDA, 0);
    wr(c, BA0_ACCTL, ACCTL_DCV | ACCTL_CRW);
    g_assert_cmphex(rd(c, BA0_ACCTL) & ACCTL_DCV, ==, 0);
    g_assert(rd(c, BA0_ACSTS) & ACSTS_VSTS);
    return rd(c, BA0_ACSDA);
}

static void codec_write(CS *c, uint8_t reg, uint16_t val)
{
    wr(c, BA0_ACCAD, reg);
    wr(c, BA0_ACCDA, val);
    wr(c, BA0_ACCTL, ACCTL_DCV);
}

static void test_codec(void)
{
    CS c;

    cs_start(&c);
    /* the ALC655 */
    g_assert_cmphex(codec_read(&c, 0x7c), ==, 0x414c);
    g_assert_cmphex(codec_read(&c, 0x7e), ==, 0x4760);
    codec_write(&c, 0x18, 0x0404);
    g_assert_cmphex(codec_read(&c, 0x18), ==, 0x0404);
    cs_stop(&c);
}

/* play a buffer in a loop and see the half and terminal count status */
static void test_playback(void)
{
    CS c;
    uint64_t buf;
    uint32_t hisr = 0, hdsr = 0;
    int i;

    cs_start(&c);
    buf = qmalloc(c.qs, 4096);
    wr(&c, BA0_DBA0, buf);
    wr(&c, BA0_DBC0, 4096 / 4 - 1);
    wr(&c, BA0_DCR0, DCR_HTCIE | DCR_TCIE);
    wr(&c, BA0_HIMR, 0);
    wr(&c, BA0_HICR, HICR_EOI);
    wr(&c, BA0_DMR0, DMR_DMA | DMR_AUTO | DMR_TR_READ);

    for (i = 0; i < 50; i++) {
        qtest_clock_step(c.qs->qts, 10 * 1000 * 1000);
        hisr |= rd(&c, BA0_HISR);
        hdsr |= rd(&c, BA0_HDSR0);
        if ((hdsr & (HDSR_DTC | HDSR_DHTC)) == (HDSR_DTC | HDSR_DHTC)) {
            break;
        }
        wr(&c, BA0_HICR, HICR_EOI);
    }
    g_assert(hisr & HISR_DMA0);
    g_assert(hdsr & HDSR_DHTC);
    g_assert(hdsr & HDSR_DTC);
    cs_stop(&c);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/cs4281/config-protect", test_config_protect);
    qtest_add_func("/cs4281/codec", test_codec);
    qtest_add_func("/cs4281/playback", test_playback);
    return g_test_run();
}
