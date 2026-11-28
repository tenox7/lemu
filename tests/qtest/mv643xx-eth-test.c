/*
 * QTest testcase for the Marvell Discovery II Gigabit Ethernet unit on the
 * Pegasos II
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/sockets.h"
#include "qemu/iov.h"
#include "libqtest.h"

/* The MV64361's registers after reset, and port 1, the board's port */
#define MV_BASE         0xf1000000
#define PHY_ADDR        0x2000
#define SMI             0x2004
#define PORT1           0x2800
#define MACAL           (PORT1 + 0x14)
#define MACAH           (PORT1 + 0x18)
#define SDCR            (PORT1 + 0x1c)
#define PSCR            (PORT1 + 0x3c)
#define PSR             (PORT1 + 0x44)
#define TQCR            (PORT1 + 0x48)
#define ICR             (PORT1 + 0x60)
#define ICER            (PORT1 + 0x64)
#define PIMR            (PORT1 + 0x68)
#define CRDP0           (PORT1 + 0x20c)
#define RQCR            (PORT1 + 0x280)
#define TCQDP0          (PORT1 + 0x2c0)
#define DFUT1           0x3a00
#define MAIN_CAUSE_HIGH 0x00c

#define SMI_READ        (1u << 26)
#define SMI_VALID       (1u << 27)
#define PSR_LINK_UP     0x2
#define PSCR_PORT_EN    0x1
#define SDCR_SWAP_DESC  0x30        /* BLMR and BLMT clear: big endian */

#define OWN             0x80000000u
#define TX_EI           (1u << 23)
#define TX_F            (1u << 21)
#define TX_L            (1u << 20)
#define TX_P            (1u << 19)
#define RX_EI           (1u << 29)
#define RX_F            (1u << 27)
#define RX_L            (1u << 26)

#define DESC            0x00100000
#define BUF             0x00101000

static const uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

static QTestState *start(int *sock)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    /* the Rhine stays out: only a NIC with this model goes to the port */
    qts = qtest_initf("-M pegasos2 -nic socket,fd=%d,model=mv643xx_eth,"
                      "mac=52:54:00:12:34:56", sv[1]);
    close(sv[1]);
    *sock = sv[0];
    return qts;
}

/* The registers are little endian, the CPU big endian */
static uint32_t rd(QTestState *qts, uint32_t reg)
{
    return bswap32(qtest_readl(qts, MV_BASE + reg));
}

static void wr(QTestState *qts, uint32_t reg, uint32_t val)
{
    qtest_writel(qts, MV_BASE + reg, bswap32(val));
}

/* A descriptor in the little endian layout */
static void desc_write(QTestState *qts, uint64_t addr, uint32_t cmd,
                       uint16_t lo, uint16_t hi, uint32_t buf, uint32_t next)
{
    uint8_t d[16];

    stl_le_p(d, cmd);
    stw_le_p(d + 4, lo);
    stw_le_p(d + 6, hi);
    stl_le_p(d + 8, buf);
    stl_le_p(d + 12, next);
    qtest_memwrite(qts, addr, d, sizeof(d));
}

static void test_phy(void)
{
    QTestState *qts;
    int sock, phy;

    qts = start(&sock);
    /* the address of port 1's PHY, after reset 9 */
    phy = (rd(qts, PHY_ADDR) >> 5) & 0x1f;
    g_assert_cmpint(phy, ==, 9);
    wr(qts, SMI, SMI_READ | (2 << 21) | (phy << 16));
    g_assert_cmphex(rd(qts, SMI), ==,
                    SMI_READ | SMI_VALID | (2 << 21) | (phy << 16) | 0x0141);
    wr(qts, SMI, SMI_READ | (1 << 21) | (phy << 16));
    g_assert(rd(qts, SMI) & 0x0004);    /* link */
    /* no PHY elsewhere */
    wr(qts, SMI, SMI_READ | (2 << 21) | (3 << 16));
    g_assert_cmphex(rd(qts, SMI) & 0xffff, ==, 0xffff);
    /* the firmware's address is in the port's registers */
    g_assert_cmphex(rd(qts, MACAH), ==, 0x52540012);
    g_assert_cmphex(rd(qts, MACAL), ==, 0x3456);

    g_assert_cmphex(rd(qts, PSR) & PSR_LINK_UP, ==, 0);
    wr(qts, PSCR, PSCR_PORT_EN);
    g_assert_cmphex(rd(qts, PSR) & PSR_LINK_UP, ==, PSR_LINK_UP);
    qtest_quit(qts);
    close(sock);
}

static void test_tx(void)
{
    QTestState *qts;
    uint8_t frame[64], got[64];
    uint32_t len;
    int sock;

    qts = start(&sock);
    for (int i = 0; i < sizeof(frame); i++) {
        frame[i] = i;
    }
    /* a frame in two buffers */
    qtest_memwrite(qts, BUF, frame, 40);
    qtest_memwrite(qts, BUF + 0x100, frame + 40, 24);
    desc_write(qts, DESC, OWN | TX_F, 0, 40, BUF, DESC + 16);
    desc_write(qts, DESC + 16, OWN | TX_L | TX_EI, 0, 24, BUF + 0x100, 0);

    wr(qts, PSCR, PSCR_PORT_EN);
    wr(qts, PIMR, 0x7fffffff);
    wr(qts, TCQDP0, DESC);
    wr(qts, TQCR, 1);

    g_assert_cmpint(recv(sock, &len, sizeof(len), MSG_WAITALL), ==,
                    sizeof(len));
    g_assert_cmpint(ntohl(len), ==, sizeof(frame));
    g_assert_cmpint(recv(sock, got, sizeof(got), MSG_WAITALL), ==,
                    sizeof(got));
    g_assert(!memcmp(frame, got, sizeof(frame)));

    /* both closed, the queue at its end and stopped */
    g_assert_cmphex(qtest_readb(qts, DESC + 3) & 0x80, ==, 0);
    g_assert_cmphex(qtest_readb(qts, DESC + 19) & 0x80, ==, 0);
    g_assert_cmphex(rd(qts, TQCR) & 1, ==, 0);
    g_assert_cmphex(rd(qts, TCQDP0), ==, 0);
    g_assert(rd(qts, ICR) & (1u << 19));        /* TxEnd queue 0 */
    g_assert(rd(qts, ICER) & 1);                /* TxBuffer queue 0 */
    g_assert(rd(qts, ICR) & (1u << 31));
    /* the port's summary in the main cause register, GbE1, a bit later */
    g_assert_cmphex(rd(qts, MAIN_CAUSE_HIGH) & (1u << 1), ==, 0);
    qtest_clock_step(qts, 100000);
    g_assert(rd(qts, MAIN_CAUSE_HIGH) & (1u << 1));
    wr(qts, ICR, 0);
    wr(qts, ICER, 0);
    g_assert_cmphex(rd(qts, ICR), ==, 0);
    g_assert_cmphex(rd(qts, MAIN_CAUSE_HIGH) & (1u << 1), ==, 0);
    qtest_quit(qts);
    close(sock);
}

static void test_rx(bool swap)
{
    QTestState *qts;
    uint8_t frame[60], got[60], d[16];
    uint32_t len = htonl(sizeof(frame)), cmd, bc;
    struct iovec iov[] = {
        { .iov_base = &len, .iov_len = sizeof(len) },
        { .iov_base = frame, .iov_len = sizeof(frame) },
    };
    int sock;

    qts = start(&sock);
    memcpy(frame, mac, 6);
    memcpy(frame + 6, "\x52\x54\x00\xaa\xbb\xcc", 6);
    frame[12] = 0x88;
    frame[13] = 0xb5;
    for (int i = 14; i < sizeof(frame); i++) {
        frame[i] = i;
    }

    /* one 1536 byte buffer; then a descriptor the CPU owns */
    if (swap) {
        /* the same descriptor, each 64-bit word byte swapped */
        uint8_t le[16];

        stl_le_p(le, OWN | RX_EI);
        stw_le_p(le + 4, 1536);
        stw_le_p(le + 6, 0);
        stl_le_p(le + 8, BUF);
        stl_le_p(le + 12, DESC + 16);
        for (int i = 0; i < 16; i++) {
            d[i] = le[(i & ~7) + 7 - (i & 7)];
        }
        qtest_memwrite(qts, DESC, d, sizeof(d));
        wr(qts, SDCR, rd(qts, SDCR) & ~SDCR_SWAP_DESC);
    } else {
        desc_write(qts, DESC, OWN | RX_EI, 1536, 0, BUF, DESC + 16);
    }
    desc_write(qts, DESC + 16, 0, 1536, 0, BUF + 0x800, 0);

    /* the address is the port's; its last nibble passes to queue 0 */
    wr(qts, DFUT1 + 4, 0x01 << 16);     /* nibble 6 */
    wr(qts, CRDP0, DESC);
    wr(qts, RQCR, 1);
    wr(qts, PIMR, 0x7fffffff);
    wr(qts, PSCR, PSCR_PORT_EN);

    g_assert_cmpint(iov_send(sock, iov, 2, 0, sizeof(len) + sizeof(frame)),
                    ==, sizeof(len) + sizeof(frame));
    for (int i = 0; i < 1000; i++) {
        qtest_memread(qts, DESC, d, sizeof(d));
        if (!(d[swap ? 4 : 3] & 0x80)) {
            break;
        }
        g_usleep(1000);
    }
    if (swap) {
        cmd = ldl_be_p(d + 4);
        bc = lduw_be_p(d);
    } else {
        cmd = ldl_le_p(d);
        bc = lduw_le_p(d + 6);
    }
    g_assert_cmphex(cmd & (OWN | RX_F | RX_L | 1), ==, RX_F | RX_L);
    /* two bytes in front, the frame, its FCS */
    g_assert_cmpint(bc, ==, 2 + sizeof(frame) + 4);
    qtest_memread(qts, BUF + 2, got, sizeof(got));
    g_assert(!memcmp(frame, got, sizeof(frame)));
    g_assert_cmphex(rd(qts, CRDP0), ==, DESC + 16);
    g_assert(rd(qts, ICR) & (1u << 2));         /* RxBuffer queue 0 */

    /* no descriptor for the next one: a resource error, nothing written */
    wr(qts, ICR, 0);
    g_assert_cmpint(iov_send(sock, iov, 2, 0, sizeof(len) + sizeof(frame)),
                    ==, sizeof(len) + sizeof(frame));
    for (int i = 0; i < 1000 && !(rd(qts, ICR) & (1u << 11)); i++) {
        g_usleep(1000);
    }
    g_assert(rd(qts, ICR) & (1u << 11));        /* RxError queue 0 */
    g_assert_cmphex(rd(qts, CRDP0), ==, DESC + 16);

    /* another address is dropped */
    wr(qts, ICR, 0);
    frame[5] = 0x57;
    g_assert_cmpint(iov_send(sock, iov, 2, 0, sizeof(len) + sizeof(frame)),
                    ==, sizeof(len) + sizeof(frame));
    g_usleep(20000);
    g_assert_cmphex(rd(qts, ICR), ==, 0);
    qtest_quit(qts);
    close(sock);
}

static void test_rx_le(void)
{
    test_rx(false);
}

static void test_rx_be(void)
{
    test_rx(true);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("mv643xx-eth/phy", test_phy);
    qtest_add_func("mv643xx-eth/tx", test_tx);
    qtest_add_func("mv643xx-eth/rx", test_rx_le);
    qtest_add_func("mv643xx-eth/rx-swapped", test_rx_be);
    return g_test_run();
}
