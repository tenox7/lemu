/*
 * QTest: the MAC address in the sun4u IDPROM
 *
 * OpenBIOS gives the guest the IDPROM's MAC address as the system's, and
 * NetBSD and Solaris use it for hme0 (the sunhme has no address of its
 * own in the device tree). It must be the one given to the NIC, also when
 * the NIC is added with -device rather than as the onboard -nic.
 *
 * The IDPROM is the last 32 bytes of the m48t59 NVRAM, at 0x2000 in the
 * PCI I/O space of the ebus's bus (bus 1, behind the simba bridge at
 * 00:01.1). Nothing runs on the CPU, so the test opens the bridge's I/O
 * window over the NVRAM itself, as OpenBIOS would.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "hw/pci/pci_regs.h"

#define PCI_CONFIG      0x1fe01000000ULL    /* PCI configuration space */
#define PCI_IO          0x1fe02000000ULL    /* PCI I/O space */
#define DEVFN_SIMBA_A   0x09                /* 00:01.1 */
#define NVRAM_IO        0x2000              /* m48t59, 8 KB */
#define IDPROM          (PCI_IO + NVRAM_IO + 0x1fd8)
#define IDPROM_LEN      16

#define DEFAULT_MAC     "52:54:00:12:34:56"

static void cfg_writeb(QTestState *qts, int bus, int devfn, int reg,
                       uint8_t val)
{
    qtest_writeb(qts, PCI_CONFIG + (bus << 16) + (devfn << 8) + reg, val);
}

/* Start the machine with `args` and read its IDPROM */
static void read_idprom(const char *args, uint8_t *idprom)
{
    QTestState *qts;
    int i;

    qts = qtest_initf("-machine sun4u -nodefaults %s", args);

    /* Bus 1 behind 00:01.1, its I/O window over 0x2000-0x3fff */
    cfg_writeb(qts, 0, DEVFN_SIMBA_A, PCI_SECONDARY_BUS, 1);
    cfg_writeb(qts, 0, DEVFN_SIMBA_A, PCI_SUBORDINATE_BUS, 1);
    cfg_writeb(qts, 0, DEVFN_SIMBA_A, PCI_IO_BASE, NVRAM_IO >> 8);
    cfg_writeb(qts, 0, DEVFN_SIMBA_A, PCI_IO_LIMIT, (NVRAM_IO + 0x1fff) >> 8);

    for (i = 0; i < IDPROM_LEN; i++) {
        idprom[i] = qtest_readb(qts, IDPROM + i);
    }
    qtest_quit(qts);
}

/* The IDPROM's MAC address is `mac`, and the header around it is valid */
static void check_idprom(const char *args, const char *mac)
{
    uint8_t idprom[IDPROM_LEN], sum = 0;
    char got[18];
    int i;

    read_idprom(args, idprom);
    snprintf(got, sizeof(got), "%02x:%02x:%02x:%02x:%02x:%02x",
             idprom[2], idprom[3], idprom[4], idprom[5], idprom[6], idprom[7]);
    g_assert_cmpstr(got, ==, mac);

    g_assert_cmphex(idprom[0], ==, 0x01);               /* format */
    g_assert_cmphex(idprom[1], ==, 0x80);               /* machine type */
    g_assert_cmpmem(&idprom[12], 3, &idprom[5], 3);     /* host id serial */
    for (i = 0; i < IDPROM_LEN - 1; i++) {
        sum ^= idprom[i];
    }
    g_assert_cmphex(idprom[15], ==, sum);
}

/* No NIC: QEMU's default address, as before */
static void test_no_nic(void)
{
    check_idprom("", DEFAULT_MAC);
}

/* A sunhme added with -device, as qemu-server and libvirt do */
static void test_device(void)
{
    check_idprom("-device sunhme,mac=bc:24:11:00:00:01,bus=pciB",
                 "bc:24:11:00:00:01");
}

/* The first sunhme, though another NIC model comes first */
static void test_first_sunhme(void)
{
    check_idprom("-device virtio-net-pci,mac=bc:24:11:00:00:02,bus=pciB "
                 "-device sunhme,mac=bc:24:11:00:00:03,bus=pciB "
                 "-device sunhme,mac=bc:24:11:00:00:04,bus=pciB",
                 "bc:24:11:00:00:03");
}

/* No sunhme: the first NIC of any model */
static void test_other_model(void)
{
    check_idprom("-device virtio-net-pci,mac=bc:24:11:00:00:05,bus=pciB "
                 "-device virtio-net-pci,mac=bc:24:11:00:00:06,bus=pciB",
                 "bc:24:11:00:00:05");
}

/* The onboard NIC keeps its place in the IDPROM, as before */
static void test_onboard(void)
{
    check_idprom("-net nic,model=sunhme,macaddr=bc:24:11:00:00:07 "
                 "-device sunhme,mac=bc:24:11:00:00:08,bus=pciB",
                 "bc:24:11:00:00:07");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("sun4u-idprom/no-nic", test_no_nic);
    qtest_add_func("sun4u-idprom/device", test_device);
    if (qtest_has_device("virtio-net-pci")) {
        qtest_add_func("sun4u-idprom/first-sunhme", test_first_sunhme);
        qtest_add_func("sun4u-idprom/other-model", test_other_model);
    }
    qtest_add_func("sun4u-idprom/onboard", test_onboard);

    return g_test_run();
}
