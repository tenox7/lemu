/*
 * QTest testcase for the PowerPC 4xx EMAC and MAL on the sam440ep (EMAC)
 * and the sam460ex (EMAC4)
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

/*
 * The MAL is set up through DCRs, which qtest cannot reach, so the test
 * makes a small firmware that does it: it sets the channel table pointers,
 * the receive buffer size and the active channels, maps RAM, writes a mark
 * to RAM and spins. The test then puts the descriptors and a frame in RAM
 * and sends the frame from EMAC0 to EMAC1 through a hub.
 */

#define EMAC_MR0        0x00
#define EMAC_TMR0       0x08
#define EMAC_RMR        0x10
#define EMAC_STACR      0x5c
#define EMAC_OCTX       0x68
#define EMAC_OCRX       0x6c

#define MR0_TXE         0x10000000
#define MR0_RXE         0x08000000
#define TMR0_GNP0       0x80000000
#define RMR_SFCS        0x40000000
#define RMR_BAE         0x00100000
#define STACR_OC        0x00008000
#define STACR_READ      0x00001000

#define MAL0_TXCASR     0x184
#define MAL0_RXCASR     0x190
#define MAL0_TXCTP0R    0x1a0
#define MAL0_RXCTP0R    0x1c0
#define MAL0_RCBS0      0x1e0

#define TX_DESC         0x00100000
#define RX_DESC         0x00100010
#define MARK            0x00100100
#define TX_BUF          0x00101000
#define RX_BUF          0x00102000
#define MARK_VALUE      0x600dcafe

#define MAL_READY       0x8000      /* TX: ready, RX: empty */
#define MAL_WRAP        0x4000
#define MAL_LAST        0x1000
#define MAL_FIRST       0x0800

#define FRAME_LEN       64

typedef struct {
    const char *machine;
    const char *fw_option;      /* how the firmware is given */
    uint32_t fw_size;           /* it ends at 0xffffffff */
    uint64_t emac[2];
    int phy[2];
    bool emac4;                 /* STACR: OC set while busy */
    int tx_chan;                /* EMAC0 */
    int rx_chan;                /* EMAC1 */
    uint32_t rx_bit;            /* its bit in RXCASR */
} Board;

static const Board sam440ep = {
    .machine = "sam440ep",
    .fw_option = "-bios %s",
    .fw_size = 512 * 1024,
    .emac = { 0xef600e00, 0xef600f00 },
    .phy = { 24, 25 },
    .tx_chan = 0,
    .rx_chan = 1,
    .rx_bit = 0x80000000u >> 1,
};

/* EMAC1 receives on channel 8, the first of its group of 8: RXCASR bit 1 */
static const Board sam460ex = {
    .machine = "sam460ex",
    .fw_option = "-drive if=pflash,format=raw,file=%s",
    .fw_size = 1024 * 1024,
    .emac = { 0x4ef600e00ULL, 0x4ef600f00ULL },
    .phy = { 0, 1 },
    .emac4 = true,
    .tx_chan = 0,
    .rx_chan = 8,
    .rx_bit = 0x80000000u >> 1,
};

/* PowerPC instructions */
static uint32_t lis(int rd, uint16_t imm)
{
    return 0x3c000000 | rd << 21 | imm;
}

static uint32_t ori(int ra, int rs, uint16_t imm)
{
    return 0x60000000 | rs << 21 | ra << 16 | imm;
}

static uint32_t mtdcr(int dcrn, int rs)
{
    return 0x7c000386 | rs << 21 | (dcrn & 0x1f) << 16 |
           ((dcrn >> 5) & 0x1f) << 11;
}

static uint32_t tlbwe(int rs, int ra, int ws)
{
    return 0x7c0007a4 | rs << 21 | ra << 16 | ws << 11;
}

static uint32_t stw(int rs, int ra, uint16_t d)
{
    return 0x90000000 | rs << 21 | ra << 16 | d;
}

#define ISYNC           0x4c00012c
#define B_SELF          0x48000000

typedef struct {
    uint8_t *fw;
    int pc;
} Fw;

static void emit(Fw *f, uint32_t insn)
{
    stl_be_p(f->fw + f->pc, insn);
    f->pc += 4;
}

static void emit_li32(Fw *f, int rd, uint32_t val)
{
    emit(f, lis(rd, val >> 16));
    emit(f, ori(rd, rd, val & 0xffff));
}

static void emit_dcr(Fw *f, int dcrn, uint32_t val)
{
    emit_li32(f, 3, val);
    emit(f, mtdcr(dcrn, 3));
}

static char *make_firmware(const Board *b)
{
    Fw f = { .fw = g_malloc0(b->fw_size) };
    char *path;
    int fd;

    g_assert((fd = g_file_open_tmp("emac-fw-XXXXXX", &path, NULL)) >= 0);

    emit_dcr(&f, MAL0_TXCTP0R + b->tx_chan, TX_DESC);
    emit_dcr(&f, MAL0_RXCTP0R + b->rx_chan, RX_DESC);
    emit_dcr(&f, MAL0_RCBS0 + b->rx_chan, 256 / 16);
    emit_dcr(&f, MAL0_TXCASR, 0x80000000u >> b->tx_chan);
    emit_dcr(&f, MAL0_RXCASR, b->rx_bit);

    /* TLB entry 1: the first 256 MB of RAM, supervisor read and write */
    emit_li32(&f, 4, 1);
    emit_li32(&f, 3, 0x00000000 | 0x200 | 0x90);  /* EPN 0, valid, 256 MB */
    emit(&f, tlbwe(3, 4, 0));
    emit_li32(&f, 3, 0);                            /* RPN 0 */
    emit(&f, tlbwe(3, 4, 1));
    emit_li32(&f, 3, 0x3f);                         /* all permissions */
    emit(&f, tlbwe(3, 4, 2));
    emit(&f, ISYNC);

    emit_li32(&f, 3, MARK_VALUE);
    emit_li32(&f, 4, MARK);
    emit(&f, stw(3, 4, 0));
    emit(&f, B_SELF);

    /* the reset vector at 0xfffffffc branches to the start of the image */
    stl_be_p(f.fw + b->fw_size - 4,
             0x48000000 | ((-(b->fw_size - 4)) & 0x03fffffc));

    g_assert(write(fd, f.fw, b->fw_size) == b->fw_size);
    close(fd);
    g_free(f.fw);
    return path;
}

static QTestState *emac_start(const Board *b, char **fw)
{
    g_autofree char *fwarg = NULL;

    *fw = make_firmware(b);
    fwarg = g_strdup_printf(b->fw_option, *fw);
    return qtest_initf("-M %s -nodefaults -accel tcg %s "
                       "-netdev hubport,id=n0,hubid=0 "
                       "-netdev hubport,id=n1,hubid=0 "
                       "-net nic,model=ppc4xx-emac,netdev=n0 "
                       "-net nic,model=ppc4xx-emac,netdev=n1",
                       b->machine, fwarg);
}

static void emac_stop(QTestState *qts, char *fw)
{
    qtest_quit(qts);
    unlink(fw);
    g_free(fw);
}

static uint16_t mdio_read(QTestState *qts, const Board *b, uint64_t emac,
                          int phy, int reg)
{
    uint32_t v;

    qtest_writel(qts, emac + EMAC_STACR,
                 (b->emac4 ? STACR_OC : 0) | STACR_READ | phy << 5 | reg);
    v = qtest_readl(qts, emac + EMAC_STACR);
    /* done: OC set on the EMAC, clear on the EMAC4 */
    g_assert(!(v & STACR_OC) == b->emac4);
    return v >> 16;
}

/*
 * Each EMAC reaches its own PHY; on the 460EX, EMAC0's MDIO bus also
 * reaches EMAC1's PHY.
 */
static void test_mdio(const void *data)
{
    const Board *b = data;
    char *fw;
    QTestState *qts = emac_start(b, &fw);
    uint16_t id = b->emac4 ? 0x0141 : 0x0181;

    g_assert_cmphex(mdio_read(qts, b, b->emac[0], b->phy[0], 2), ==, id);
    g_assert(mdio_read(qts, b, b->emac[0], b->phy[0], 1) & 0x0004);
    g_assert_cmphex(mdio_read(qts, b, b->emac[1], b->phy[1], 2), ==, id);
    g_assert_cmphex(mdio_read(qts, b, b->emac[0], b->phy[1], 2), ==,
                    b->emac4 ? id : 0xffff);
    g_assert_cmphex(mdio_read(qts, b, b->emac[0], 31, 2), ==, 0xffff);
    emac_stop(qts, fw);
}

static void wd(QTestState *qts, uint64_t addr, uint16_t ctrl, uint16_t len,
               uint32_t buf)
{
    qtest_writew(qts, addr, ctrl);
    qtest_writew(qts, addr + 2, len);
    qtest_writel(qts, addr + 4, buf);
}

/* a broadcast frame goes out of EMAC0 and lands in EMAC1's buffer */
static void test_loopback(const void *data)
{
    const Board *b = data;
    char *fw;
    QTestState *qts = emac_start(b, &fw);
    uint8_t frame[FRAME_LEN], got[FRAME_LEN];
    int i;

    for (i = 0; i < 200 && qtest_readl(qts, MARK) != MARK_VALUE; i++) {
        g_usleep(10 * 1000);
    }
    g_assert_cmphex(qtest_readl(qts, MARK), ==, MARK_VALUE);

    memset(frame, 0xff, 6);
    memcpy(frame + 6, "\x52\x54\x00\x12\x34\x56", 6);
    frame[12] = 0x08;
    frame[13] = 0x00;
    for (i = 14; i < FRAME_LEN; i++) {
        frame[i] = i;
    }
    qtest_memwrite(qts, TX_BUF, frame, sizeof(frame));
    wd(qts, TX_DESC, MAL_READY | MAL_WRAP | MAL_LAST, FRAME_LEN, TX_BUF);
    wd(qts, RX_DESC, MAL_READY | MAL_WRAP, 0, RX_BUF);

    qtest_writel(qts, b->emac[1] + EMAC_RMR, RMR_BAE | RMR_SFCS);
    qtest_writel(qts, b->emac[1] + EMAC_MR0, MR0_RXE);
    qtest_writel(qts, b->emac[0] + EMAC_MR0, MR0_TXE);
    qtest_writel(qts, b->emac[0] + EMAC_TMR0, TMR0_GNP0);

    /* sent: ready is clear */
    g_assert_cmphex(qtest_readw(qts, TX_DESC), ==, MAL_WRAP | MAL_LAST);
    /* received in one buffer: not empty, first and last */
    g_assert_cmphex(qtest_readw(qts, RX_DESC), ==,
                    MAL_WRAP | MAL_FIRST | MAL_LAST);
    g_assert_cmpuint(qtest_readw(qts, RX_DESC + 2), ==, FRAME_LEN);
    qtest_memread(qts, RX_BUF, got, sizeof(got));
    g_assert(!memcmp(frame, got, sizeof(got)));
    g_assert_cmpuint(qtest_readl(qts, b->emac[0] + EMAC_OCTX), ==, FRAME_LEN);
    g_assert_cmpuint(qtest_readl(qts, b->emac[1] + EMAC_OCRX), ==, FRAME_LEN);
    emac_stop(qts, fw);
}

static void add_board(const Board *b)
{
    g_autofree char *mdio = NULL;
    g_autofree char *loop = NULL;

    if (!qtest_has_machine(b->machine)) {
        return;
    }
    mdio = g_strdup_printf("/ppc4xx-emac/%s/mdio", b->machine);
    loop = g_strdup_printf("/ppc4xx-emac/%s/loopback", b->machine);
    qtest_add_data_func(mdio, b, test_mdio);
    qtest_add_data_func(loop, b, test_loopback);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    add_board(&sam440ep);
    add_board(&sam460ex);
    return g_test_run();
}
