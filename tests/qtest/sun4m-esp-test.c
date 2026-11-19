/*
 * QTest: a CDB sent byte by byte after a message out phase on sun4m's ESP
 *
 * Linux 2.6's esp driver negotiates synchronous transfers: it selects
 * the target with Select with ATN and Stop (IDENTIFY only), sends an
 * extended SDTR message in the message out phase, and then puts the
 * CDB on the bus one byte at a time, a Transfer Information command
 * for each byte. The command must reach the target only when the whole
 * CDB is in, however many message bytes came before it.
 *
 * The ESP counted the IDENTIFY and message bytes as part of the CDB, so
 * here the first CDB byte alone made a "complete" 6-byte command: the
 * target got one byte, answered CHECK CONDITION (ILLEGAL REQUEST), and
 * Linux reset the bus and took the disk and CD-ROM offline.
 *
 * Nothing runs on the CPU: the test drives the ESP's registers itself,
 * without DMA, as the guest's driver does.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* SPARCstation 5: the ESP's registers are 4 bytes apart */
#define ESP_BASE        0x78800000ULL
#define ESP_REG(r)      (ESP_BASE + (r) * 4)

#define ESP_FIFO        0x2
#define ESP_CMD         0x3
#define ESP_RSTAT       0x4
#define ESP_WBUSID      0x4
#define ESP_RINTR       0x5
#define ESP_RFLAGS      0x7

#define CMD_TI          0x10
#define CMD_ICCS        0x11
#define CMD_MSGACC      0x12
#define CMD_SELATNS     0x43

#define STAT_PHASE      0x07
#define STAT_CD         0x02
#define STAT_ST         0x03
#define STAT_MO         0x06
#define STAT_GE         0x40

#define INTR_FC         0x08
#define INTR_BS         0x10
#define INTR_DC         0x20

static uint8_t esp_readb(QTestState *qts, int reg)
{
    return qtest_readb(qts, ESP_REG(reg));
}

static void esp_writeb(QTestState *qts, int reg, uint8_t val)
{
    qtest_writeb(qts, ESP_REG(reg), val);
}

/* The bus phase after a command, with no gross error */
static uint8_t esp_phase(QTestState *qts)
{
    uint8_t stat = esp_readb(qts, ESP_RSTAT);

    g_assert_cmphex(stat & STAT_GE, ==, 0);
    return stat & STAT_PHASE;
}

/*
 * Select target 0 with ATN and Stop, send an SDTR message, then TEST UNIT
 * READY one byte at a time; return the status byte
 */
static uint8_t tur_after_sdtr(QTestState *qts)
{
    /* SDTR: 100 ns, offset 15 */
    static const uint8_t sdtr[] = { 0x01, 0x03, 0x01, 0x19, 0x0f };
    static const uint8_t cdb[6] = { 0x00 };     /* TEST UNIT READY */
    uint8_t intr, status;
    int i;

    /* IDENTIFY (disconnects allowed, LUN 0), and stop in message out */
    esp_writeb(qts, ESP_FIFO, 0xc0);
    esp_writeb(qts, ESP_WBUSID, 0);
    esp_writeb(qts, ESP_CMD, CMD_SELATNS);
    g_assert_cmphex(esp_phase(qts), ==, STAT_MO);
    intr = esp_readb(qts, ESP_RINTR);
    g_assert_cmphex(intr & (INTR_BS | INTR_FC), ==, INTR_BS | INTR_FC);

    /* The SDTR message: the target goes on to the command phase */
    for (i = 0; i < ARRAY_SIZE(sdtr); i++) {
        esp_writeb(qts, ESP_FIFO, sdtr[i]);
    }
    esp_writeb(qts, ESP_CMD, CMD_TI);
    g_assert_cmphex(esp_phase(qts), ==, STAT_CD);
    intr = esp_readb(qts, ESP_RINTR);
    g_assert_cmphex(intr & INTR_BS, ==, INTR_BS);

    /* The CDB, one byte per Transfer Information */
    for (i = 0; i < ARRAY_SIZE(cdb); i++) {
        esp_writeb(qts, ESP_FIFO, cdb[i]);
        esp_writeb(qts, ESP_CMD, CMD_TI);
        if (i < ARRAY_SIZE(cdb) - 1) {
            /* Not the whole CDB yet: the target waits for more */
            g_assert_cmphex(esp_phase(qts), ==, STAT_CD);
        }
        esp_readb(qts, ESP_RINTR);
    }

    /* The whole CDB: the target ran it and has its status */
    g_assert_cmphex(esp_phase(qts), ==, STAT_ST);
    esp_writeb(qts, ESP_CMD, CMD_ICCS);
    intr = esp_readb(qts, ESP_RINTR);
    g_assert_cmphex(intr & INTR_FC, ==, INTR_FC);
    g_assert_cmpuint(esp_readb(qts, ESP_RFLAGS) & 0x1f, ==, 2);
    status = esp_readb(qts, ESP_FIFO);
    g_assert_cmphex(esp_readb(qts, ESP_FIFO), ==, 0x00);   /* COMMAND COMPLETE */

    /* Accept the message: the target goes to bus free */
    esp_writeb(qts, ESP_CMD, CMD_MSGACC);
    intr = esp_readb(qts, ESP_RINTR);
    g_assert_cmphex(intr & INTR_DC, ==, INTR_DC);

    return status;
}

static void test_cdb_after_sdtr(void)
{
    QTestState *qts = qtest_init("-machine SS-5 -nodefaults "
                                 "-drive if=scsi,index=0,format=raw,"
                                 "file=null-co://");

    /*
     * The first command after the reset gets CHECK CONDITION with the
     * unit attention; the second one GOOD. A CDB cut short would get
     * CHECK CONDITION (ILLEGAL REQUEST) every time.
     */
    g_assert_cmphex(tur_after_sdtr(qts), ==, 0x02);
    g_assert_cmphex(tur_after_sdtr(qts), ==, 0x00);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("sun4m-esp/cdb-after-sdtr", test_cdb_after_sdtr);

    return g_test_run();
}
