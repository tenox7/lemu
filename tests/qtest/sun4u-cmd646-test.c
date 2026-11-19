/*
 * QTest: the cmd646 interrupt status bits on sun4u
 *
 * The cmd646's interrupt status bits (MRDMODE, mirrored in CFR and
 * ARTTIM23) latch a drive's INTRQ and stay set until the guest writes 1 to
 * clear them. Solaris's ata driver reads the drive's status register,
 * which drops INTRQ, and then looks at these bits; it warned "Interrupt
 * not seen after set_features" when they had followed INTRQ down.
 *
 * The PCI interrupt line must still follow INTRQ: guests that never clear
 * the bits (Linux, OpenBSD) would otherwise see it stuck high. It is
 * watched through the sabre, which turns the line into a request for the
 * CPU and re-raises that request when the guest clears it while the line
 * is still up.
 *
 * Nothing runs on the CPU, so the test sets up what OpenBIOS would: bus 1
 * behind the simba bridge at 00:01.1 and the cmd646's I/O BARs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "hw/pci/pci_regs.h"

#define SABRE_CONFIG    0x1fe00000000ULL    /* sabre registers (big-endian) */
#define PCI_CONFIG      0x1fe01000000ULL    /* PCI configuration space */
#define PCI_IO          0x1fe02000000ULL    /* PCI I/O space */
/* OBIO interrupt map and clear registers of ino 0x20, the IDE controller */
#define OBIO_IMR_HDD    (SABRE_CONFIG + 0x1004)
#define OBIO_CLR_HDD    (SABRE_CONFIG + 0x1804)
#define IMR_ENABLED     0x80000000
#define IVEC_HDD        0x20

#define DEVFN_SIMBA_A   0x09                /* 00:01.1 */
#define DEVFN_CMD646    0x18                /* 01:03.0 */
#define IO_CMD0         0x400               /* BAR0: channel 0 command block */
#define IO_CTL0         0x408               /* BAR1: channel 0 control block */

#define CFR             0x50
#define CFR_INTR_CH0    0x04
#define MRDMODE         0x71
#define MRDMODE_INTR_CH0 0x04

#define ATA_FEATURE     1
#define ATA_NSECTOR     2
#define ATA_DEVICE      6
#define ATA_STATUS      7                   /* read: status, write: command */
#define ATA_DEVCTL      2                   /* in the control block */
#define CMD_SET_FEATURES 0xef
#define STATUS_BSY      0x80
#define STATUS_ERR      0x01

static uint64_t cfg_addr(int bus, int devfn, int reg)
{
    return PCI_CONFIG + (bus << 16) + (devfn << 8) + reg;
}

/* Byte accesses only: both spaces are little-endian on a big-endian CPU */
static uint8_t cfg_read(QTestState *qts, int bus, int devfn, int reg)
{
    return qtest_readb(qts, cfg_addr(bus, devfn, reg));
}

static void cfg_write(QTestState *qts, int bus, int devfn, int reg,
                      uint32_t val, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        qtest_writeb(qts, cfg_addr(bus, devfn, reg + i), val >> (8 * i));
    }
}

static const char *find_sabre(QTestState *qts)
{
    static char path[64];
    int i;

    for (i = 0; i < 64; i++) {
        QDict *rsp;
        bool found;

        snprintf(path, sizeof(path), "/machine/unattached/device[%d]", i);
        rsp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                        "{ 'path': %s, 'property': 'type' } }", path);
        found = qdict_haskey(rsp, "return") &&
                !strcmp(qdict_get_str(rsp, "return"), "sabre");
        qobject_unref(rsp);
        if (found) {
            return path;
        }
    }
    g_assert_not_reached();
}

/* What OpenBIOS would set up, after power-on or a reset */
static void configure(QTestState *qts)
{
    qtest_writel(qts, OBIO_IMR_HDD, IMR_ENABLED);

    /* Bus 1 behind 00:01.1, which already forwards I/O 0-0xfff */
    cfg_write(qts, 0, DEVFN_SIMBA_A, PCI_SECONDARY_BUS, 1, 1);
    cfg_write(qts, 0, DEVFN_SIMBA_A, PCI_SUBORDINATE_BUS, 1, 1);
    g_assert_cmphex(cfg_read(qts, 1, DEVFN_CMD646, PCI_VENDOR_ID), ==, 0x95);
    g_assert_cmphex(cfg_read(qts, 1, DEVFN_CMD646, PCI_DEVICE_ID), ==, 0x46);

    cfg_write(qts, 1, DEVFN_CMD646, PCI_BASE_ADDRESS_0, IO_CMD0, 4);
    cfg_write(qts, 1, DEVFN_CMD646, PCI_BASE_ADDRESS_1, IO_CTL0, 4);
    cfg_write(qts, 1, DEVFN_CMD646, PCI_COMMAND, PCI_COMMAND_IO, 2);

    /* Drive interrupts on (nIEN clear), no interrupt so far */
    qtest_writeb(qts, PCI_IO + IO_CTL0 + ATA_DEVCTL, 0);
    g_assert_false(qtest_get_irq(qts, IVEC_HDD));
}

static QTestState *setup(void)
{
    QTestState *qts;

    qts = qtest_init("-machine sun4u -nodefaults "
                     "-blockdev driver=null-co,read-zeroes=on,node-name=d0 "
                     "-device ide-hd,drive=d0,bus=ide.0,unit=0");

    /* The sabre's IDE interrupt request goes to the test, not the CPU */
    qtest_irq_intercept_out_named(qts, find_sabre(qts), "ivec-irq");
    configure(qts);
    return qts;
}

/* SET FEATURES (transfer mode PIO 4): completes at once, raising INTRQ */
static void set_features(QTestState *qts)
{
    qtest_writeb(qts, PCI_IO + IO_CMD0 + ATA_DEVICE, 0xa0);
    qtest_writeb(qts, PCI_IO + IO_CMD0 + ATA_FEATURE, 0x03);
    qtest_writeb(qts, PCI_IO + IO_CMD0 + ATA_NSECTOR, 0x0c);
    qtest_writeb(qts, PCI_IO + IO_CMD0 + ATA_STATUS, CMD_SET_FEATURES);
}

static uint8_t mrdmode(QTestState *qts)
{
    return cfg_read(qts, 1, DEVFN_CMD646, MRDMODE) & MRDMODE_INTR_CH0;
}

static uint8_t cfr(QTestState *qts)
{
    return cfg_read(qts, 1, DEVFN_CMD646, CFR) & CFR_INTR_CH0;
}

/*
 * The guest acknowledges the sabre's request; it comes back while the line
 * is up
 */
static bool line_after_clear(QTestState *qts)
{
    qtest_writel(qts, OBIO_CLR_HDD, 0);
    return qtest_get_irq(qts, IVEC_HDD);
}

static void test_status_latched(void)
{
    QTestState *qts = setup();
    uint8_t status;

    set_features(qts);
    g_assert_true(qtest_get_irq(qts, IVEC_HDD));
    g_assert_cmphex(mrdmode(qts), ==, MRDMODE_INTR_CH0);
    g_assert_cmphex(cfr(qts), ==, CFR_INTR_CH0);
    g_assert_true(line_after_clear(qts));

    /* Reading the status drops INTRQ: the line goes down, the bits stay */
    status = qtest_readb(qts, PCI_IO + IO_CMD0 + ATA_STATUS);
    g_assert_cmphex(status & (STATUS_BSY | STATUS_ERR), ==, 0);
    g_assert_false(line_after_clear(qts));
    g_assert_cmphex(mrdmode(qts), ==, MRDMODE_INTR_CH0);
    g_assert_cmphex(cfr(qts), ==, CFR_INTR_CH0);

    /* Writing 1 clears them, in MRDMODE and in its CFR mirror */
    cfg_write(qts, 1, DEVFN_CMD646, MRDMODE, MRDMODE_INTR_CH0, 1);
    g_assert_cmphex(mrdmode(qts), ==, 0);
    g_assert_cmphex(cfr(qts), ==, 0);
    g_assert_false(line_after_clear(qts));

    qtest_quit(qts);
}

static void test_clear_keeps_line(void)
{
    QTestState *qts = setup();

    /* Clearing the bits while INTRQ is up leaves the line up */
    set_features(qts);
    cfg_write(qts, 1, DEVFN_CMD646, MRDMODE, MRDMODE_INTR_CH0, 1);
    g_assert_cmphex(mrdmode(qts), ==, 0);
    g_assert_true(line_after_clear(qts));

    /* until the guest reads the status */
    qtest_readb(qts, PCI_IO + IO_CMD0 + ATA_STATUS);
    g_assert_false(line_after_clear(qts));

    /* The next command latches the bits again */
    set_features(qts);
    g_assert_cmphex(mrdmode(qts), ==, MRDMODE_INTR_CH0);
    g_assert_true(line_after_clear(qts));

    qtest_quit(qts);
}

/*
 * A reset with INTRQ up: the drives come out of it with INTRQ low, so the
 * line must stay down when the firmware sets the controller up again.
 */
static void test_reset_drops_intrq(void)
{
    QTestState *qts = setup();

    set_features(qts);
    g_assert_true(qtest_get_irq(qts, IVEC_HDD));
    /* Masking the interrupt in the sabre withdraws its request */
    qtest_writel(qts, OBIO_IMR_HDD, 0);
    g_assert_false(qtest_get_irq(qts, IVEC_HDD));

    qtest_system_reset(qts);
    configure(qts);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("sun4u-cmd646/status-latched", test_status_latched);
    qtest_add_func("sun4u-cmd646/clear-keeps-line", test_clear_keeps_line);
    qtest_add_func("sun4u-cmd646/reset-drops-intrq", test_reset_drops_intrq);

    return g_test_run();
}
