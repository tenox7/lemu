/*
 * QTest testcase for the 3Com 3C905C Fast EtherLink XL
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

#define R_COMMAND       0x0e
#define R_DN_LIST_PTR   0x24
#define R_UP_LIST_PTR   0x38

#define CMD(c, arg)     ((c) << 11 | (arg))
#define CMD_SELECT_WINDOW   1
#define CMD_RX_ENABLE       4
#define CMD_TX_ENABLE       9
#define CMD_SET_STATUS_ENB  15
#define CMD_SET_RX_FILTER   16

#define FILTER_STATION      0x01
#define FILTER_BROADCAST    0x04
#define INT_SOURCES         0x07fe
#define INT_DN_COMPLETE     0x0200
#define INT_UP_COMPLETE     0x0400
#define DPD_DN_COMPLETE     (1u << 16)
#define DPD_DN_INDICATE     (1u << 31)
#define FRAG_LAST           (1u << 31)
#define UPD_UP_COMPLETE     (1u << 15)

static const uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x57 };

typedef struct {
    QOSState *qs;
    QPCIDevice *dev;
    QPCIBar bar;
    int sock;
} TC;

static void save_fn(QPCIDevice *dev, int devfn, void *data)
{
    *(QPCIDevice **)data = dev;
}

static void tc_cmd(TC *t, uint16_t cmd)
{
    qpci_io_writew(t->dev, t->bar, R_COMMAND, cmd);
}

static void tc_start(TC *t)
{
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    t->sock = sv[0];
    t->qs = qtest_pc_boot("-netdev socket,fd=%d,id=hs0 "
                          "-device 3c905c,netdev=hs0,"
                          "mac=52:54:00:12:34:57", sv[1]);
    close(sv[1]);
    t->dev = NULL;
    qpci_device_foreach(t->qs->pcibus, 0x10b7, 0x9200, save_fn, &t->dev);
    g_assert(t->dev);
    qpci_device_enable(t->dev);
    t->bar = qpci_iomap(t->dev, 0, NULL);
    /* status bits only latch for the sources the driver indicates */
    tc_cmd(t, CMD(CMD_SET_STATUS_ENB, INT_SOURCES));
}

static void tc_stop(TC *t)
{
    g_free(t->dev);
    qtest_shutdown(t->qs);
    close(t->sock);
}

static void test_station_address(void)
{
    TC t;

    tc_start(&t);
    tc_cmd(&t, CMD(CMD_SELECT_WINDOW, 2));
    for (int i = 0; i < 6; i++) {
        g_assert_cmphex(qpci_io_readb(t.dev, t.bar, i), ==, mac[i]);
    }
    tc_stop(&t);
}

static void test_tx(void)
{
    TC t;
    uint8_t frame[64], got[64];
    uint64_t dpd, buf;
    uint32_t len;

    tc_start(&t);
    for (int i = 0; i < sizeof(frame); i++) {
        frame[i] = 0xa0 + i;
    }
    buf = qmalloc(t.qs, sizeof(frame));
    dpd = qmalloc(t.qs, 16);
    qtest_memwrite(t.qs->qts, buf, frame, sizeof(frame));
    qtest_writel(t.qs->qts, dpd, 0);
    qtest_writel(t.qs->qts, dpd + 4, DPD_DN_INDICATE);
    qtest_writel(t.qs->qts, dpd + 8, buf);
    qtest_writel(t.qs->qts, dpd + 12, FRAG_LAST | sizeof(frame));

    tc_cmd(&t, CMD(CMD_TX_ENABLE, 0));
    qpci_io_writel(t.dev, t.bar, R_DN_LIST_PTR, dpd);

    g_assert_cmpint(recv(t.sock, &len, sizeof(len), MSG_WAITALL), ==,
                    sizeof(len));
    g_assert_cmpint(ntohl(len), ==, sizeof(frame));
    g_assert_cmpint(recv(t.sock, got, sizeof(got), MSG_WAITALL), ==,
                    sizeof(got));
    g_assert(!memcmp(frame, got, sizeof(frame)));

    g_assert(qtest_readl(t.qs->qts, dpd + 4) & DPD_DN_COMPLETE);
    g_assert(qpci_io_readw(t.dev, t.bar, R_COMMAND) & INT_DN_COMPLETE);
    tc_stop(&t);
}

static void test_rx(void)
{
    TC t;
    uint8_t frame[60], got[60];
    uint64_t upd, buf;
    uint32_t len = htonl(sizeof(frame)), status = 0;
    struct iovec iov[] = {
        { .iov_base = &len, .iov_len = sizeof(len) },
        { .iov_base = frame, .iov_len = sizeof(frame) },
    };
    gint64 end;

    tc_start(&t);
    memcpy(frame, mac, 6);
    for (int i = 6; i < sizeof(frame); i++) {
        frame[i] = i;
    }
    buf = qmalloc(t.qs, 1536);
    upd = qmalloc(t.qs, 16);
    qtest_writel(t.qs->qts, upd, 0);
    qtest_writel(t.qs->qts, upd + 4, 0);
    qtest_writel(t.qs->qts, upd + 8, buf);
    qtest_writel(t.qs->qts, upd + 12, FRAG_LAST | 1536);

    tc_cmd(&t, CMD(CMD_SET_RX_FILTER, FILTER_STATION | FILTER_BROADCAST));
    qpci_io_writel(t.dev, t.bar, R_UP_LIST_PTR, upd);
    tc_cmd(&t, CMD(CMD_RX_ENABLE, 0));

    g_assert_cmpint(iov_send(t.sock, iov, 2, 0, sizeof(len) + sizeof(frame)),
                    ==, sizeof(len) + sizeof(frame));
    end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < end) {
        status = qtest_readl(t.qs->qts, upd + 4);
        if (status & UPD_UP_COMPLETE) {
            break;
        }
        g_usleep(1000);
    }
    g_assert(status & UPD_UP_COMPLETE);
    g_assert_cmpint(status & 0x1fff, ==, sizeof(frame));
    qtest_memread(t.qs->qts, buf, got, sizeof(got));
    g_assert(!memcmp(frame, got, sizeof(frame)));
    g_assert(qpci_io_readw(t.dev, t.bar, R_COMMAND) & INT_UP_COMPLETE);
    tc_stop(&t);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/3c905c/station-address", test_station_address);
    qtest_add_func("/3c905c/tx", test_tx);
    qtest_add_func("/3c905c/rx", test_rx);
    return g_test_run();
}
