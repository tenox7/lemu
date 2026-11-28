/*
 * QTest testcase for the VIA VT6102 Rhine-II Ethernet controller
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
#include "qemu/sockets.h"
#include "qemu/iov.h"
#include "qemu/bswap.h"

#define R_PAR       0x00
#define R_RCR       0x06
#define R_CR0       0x08
#define R_ISR       0x0c
#define R_IMR       0x0e
#define R_CRDA      0x18
#define R_CTDA      0x1c
#define R_MIICR     0x70
#define R_MIIADR    0x71
#define R_MIIDATA   0x72

#define CR0_STRT    0x02
#define CR0_RXON    0x08
#define CR0_TXON    0x10
#define CR0_TDMD    0x20
#define ISR_PRX     0x0001
#define ISR_PTX     0x0002
#define RCR_AB      0x08
#define OWN         0x80000000u
#define TDES1_STP   (1u << 21)
#define TDES1_EDP   (1u << 22)
#define RDES0_RXOK  (1u << 15)

static const uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

typedef struct {
    QOSState *qs;
    QPCIDevice *dev;
    QPCIBar bar;
    int sock;
} Rhine;

static void save_fn(QPCIDevice *dev, int devfn, void *data)
{
    *(QPCIDevice **)data = dev;
}

static void rhine_start(Rhine *r)
{
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    r->sock = sv[0];
    r->qs = qtest_pc_boot("-netdev socket,fd=%d,id=hs0 "
                          "-device via-rhine,netdev=hs0,"
                          "mac=52:54:00:12:34:56", sv[1]);
    close(sv[1]);
    r->dev = NULL;
    qpci_device_foreach(r->qs->pcibus, 0x1106, 0x3065, save_fn, &r->dev);
    g_assert(r->dev);
    qpci_device_enable(r->dev);
    r->bar = qpci_iomap(r->dev, 0, NULL);
}

static void rhine_stop(Rhine *r)
{
    g_free(r->dev);
    qtest_shutdown(r->qs);
    close(r->sock);
}

static void test_mac_and_mii(void)
{
    Rhine r;
    int i;

    rhine_start(&r);
    for (i = 0; i < 6; i++) {
        g_assert_cmphex(qpci_io_readb(r.dev, r.bar, R_PAR + i), ==, mac[i]);
    }
    /* with auto polling off the management port is idle */
    qpci_io_writeb(r.dev, r.bar, R_MIICR, 0);
    g_assert(qpci_io_readb(r.dev, r.bar, R_MIIADR) & 0x80);
    /* the PHY at address 1 has a link */
    qpci_io_writeb(r.dev, r.bar, R_MIIADR, 1);
    qpci_io_writeb(r.dev, r.bar, R_MIICR, 0x40);
    g_assert_cmphex(qpci_io_readb(r.dev, r.bar, R_MIICR) & 0x40, ==, 0);
    g_assert(qpci_io_readw(r.dev, r.bar, R_MIIDATA) & 0x0004);
    rhine_stop(&r);
}

static void test_tx(void)
{
    Rhine r;
    uint8_t frame[64], got[64];
    uint64_t desc, buf;
    uint32_t len;

    rhine_start(&r);
    for (int i = 0; i < sizeof(frame); i++) {
        frame[i] = i;
    }
    buf = qmalloc(r.qs, sizeof(frame));
    desc = qmalloc(r.qs, 16);
    qtest_memwrite(r.qs->qts, buf, frame, sizeof(frame));
    qtest_writel(r.qs->qts, desc, OWN);
    qtest_writel(r.qs->qts, desc + 4,
                 TDES1_STP | TDES1_EDP | sizeof(frame));
    qtest_writel(r.qs->qts, desc + 8, buf);
    qtest_writel(r.qs->qts, desc + 12, desc);

    qpci_io_writel(r.dev, r.bar, R_CTDA, desc);
    qpci_io_writeb(r.dev, r.bar, R_CR0, CR0_STRT | CR0_TXON | CR0_TDMD);

    g_assert_cmpint(recv(r.sock, &len, sizeof(len), MSG_WAITALL), ==,
                    sizeof(len));
    g_assert_cmpint(ntohl(len), ==, sizeof(frame));
    g_assert_cmpint(recv(r.sock, got, sizeof(got), MSG_WAITALL), ==,
                    sizeof(got));
    g_assert(!memcmp(frame, got, sizeof(frame)));

    g_assert_cmphex(qtest_readl(r.qs->qts, desc) & OWN, ==, 0);
    g_assert(qpci_io_readw(r.dev, r.bar, R_ISR) & ISR_PTX);
    rhine_stop(&r);
}

static void test_rx(void)
{
    Rhine r;
    uint8_t frame[60], got[60];
    uint64_t desc, buf;
    uint32_t len = htonl(sizeof(frame)), status = OWN;
    struct iovec iov[] = {
        { .iov_base = &len, .iov_len = sizeof(len) },
        { .iov_base = frame, .iov_len = sizeof(frame) },
    };
    gint64 end;

    rhine_start(&r);
    memcpy(frame, mac, 6);
    for (int i = 6; i < sizeof(frame); i++) {
        frame[i] = i;
    }
    buf = qmalloc(r.qs, 1536);
    desc = qmalloc(r.qs, 16);
    qtest_writel(r.qs->qts, desc, OWN);
    qtest_writel(r.qs->qts, desc + 4, 1536);
    qtest_writel(r.qs->qts, desc + 8, buf);
    qtest_writel(r.qs->qts, desc + 12, desc);

    qpci_io_writel(r.dev, r.bar, R_CRDA, desc);
    qpci_io_writeb(r.dev, r.bar, R_RCR, RCR_AB);
    qpci_io_writeb(r.dev, r.bar, R_CR0, CR0_STRT | CR0_RXON);

    g_assert_cmpint(iov_send(r.sock, iov, 2, 0, sizeof(len) + sizeof(frame)),
                    ==, sizeof(len) + sizeof(frame));
    end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < end) {
        status = qtest_readl(r.qs->qts, desc);
        if (!(status & OWN)) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_cmphex(status & OWN, ==, 0);
    g_assert(status & RDES0_RXOK);
    /* the length counts the CRC */
    g_assert_cmpint((status >> 16) & 0x7ff, ==, sizeof(frame) + 4);
    qtest_memread(r.qs->qts, buf, got, sizeof(got));
    g_assert(!memcmp(frame, got, sizeof(frame)));
    g_assert(qpci_io_readw(r.dev, r.bar, R_ISR) & ISR_PRX);
    rhine_stop(&r);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/via-rhine/mac-mii", test_mac_and_mii);
    qtest_add_func("/via-rhine/tx", test_tx);
    qtest_add_func("/via-rhine/rx", test_rx);
    return g_test_run();
}
