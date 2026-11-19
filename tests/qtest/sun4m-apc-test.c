/*
 * QTest: the sun4m power management registers (APC)
 *
 * The firmware describes the power management node with 16 bytes of
 * registers, and Solaris' pmc driver maps all of them and writes several
 * at attach time. Writing register 0 idles the CPU until the next
 * interrupt; the other registers must take a write without a bus error,
 * read back what was written, not idle the CPU, and be 0 after a reset.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"

#define APC_SIZE        16

/* SPARCstation 5: RAM at 0, the APC at 0x6a000000 */
#define SS5_APC         0x6a000000ULL
#define RAM_RESULT      0x1000ULL
#define RAM_COUNT       0x1004ULL

typedef struct {
    const char *machine;
    uint64_t apc;
} APCMachine;

static const APCMachine machines[] = {
    { "SS-5", SS5_APC },
    { "SS-20", 0xefa000000ULL },
    { "Voyager", 0x71300000ULL },
};

static void test_regs(const void *data)
{
    const APCMachine *m = data;
    g_autofree char *mtree = NULL;
    g_autofree char *region = NULL;
    QTestState *qts;
    int i;

    qts = qtest_initf("-machine %s", m->machine);

    mtree = qtest_hmp(qts, "info mtree");
    region = g_strdup_printf("%016" PRIx64 "-%016" PRIx64 " (prio 0, i/o): apc",
                             m->apc, m->apc + APC_SIZE - 1);
    g_assert_nonnull(strstr(mtree, region));

    for (i = 1; i < APC_SIZE; i++) {
        qtest_writeb(qts, m->apc + i, 0xa0 + i);
    }
    for (i = 1; i < APC_SIZE; i++) {
        g_assert_cmphex(qtest_readb(qts, m->apc + i), ==, 0xa0 + i);
    }
    /* The idle register reads as 0 */
    qtest_writeb(qts, m->apc, 0x01);
    g_assert_cmphex(qtest_readb(qts, m->apc), ==, 0);

    qtest_system_reset(qts);
    for (i = 0; i < APC_SIZE; i++) {
        g_assert_cmphex(qtest_readb(qts, m->apc + i), ==, 0);
    }
    qtest_quit(qts);
}

/*
 * A boot PROM that writes and reads back APC register 2, stores the value
 * at RAM_RESULT, writes 0 to APC register `poke`, then counts at RAM_COUNT
 * forever. It runs from reset: MMU off, traps disabled.
 */
static char *write_prom(uint8_t poke)
{
    const uint32_t code[] = {
        0x031a8000,         /*     sethi %hi(0x6a000000), %g1 */
        0x09000004,         /*     sethi %hi(0x1000), %g4 */
        0x841020a5,         /*     mov   0xa5, %g2 */
        0xc4286002,         /*     stb   %g2, [%g1 + 2] */
        0xc6086002,         /*     ldub  [%g1 + 2], %g3 */
        0xc6212000,         /*     st    %g3, [%g4] */
        0xc0286000 | poke,  /*     stb   %g0, [%g1 + poke] */
        0xca012004,         /* 1:  ld    [%g4 + 4], %g5 */
        0x8a016001,         /*     add   %g5, 1, %g5 */
        0xca212004,         /*     st    %g5, [%g4 + 4] */
        0x10bffffd,         /*     ba    1b */
        0x01000000,         /*      nop */
    };
    uint32_t be[ARRAY_SIZE(code)];
    char *path = NULL;
    int fd, i;

    for (i = 0; i < ARRAY_SIZE(code); i++) {
        be[i] = cpu_to_be32(code[i]);
    }
    fd = g_file_open_tmp("sun4m-apc-prom-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, be, sizeof(be)), ==, sizeof(be));
    close(fd);
    return path;
}

static QTestState *run_prom(const char *prom)
{
    QTestState *qts = qtest_initf("-machine SS-5 -accel tcg -bios %s", prom);
    gint64 end = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    /* The CPU read back from register 2 what it wrote there */
    while (qtest_readl(qts, RAM_RESULT) != 0xa5) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(1000);
    }
    return qts;
}

/* Any register but 0: the CPU keeps running */
static void test_no_idle(void)
{
    g_autofree char *prom = write_prom(0xc);
    gint64 end = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    QTestState *qts = run_prom(prom);

    while (qtest_readl(qts, RAM_COUNT) < 1000) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(1000);
    }
    qtest_quit(qts);
    unlink(prom);
}

/* Register 0: the CPU stops, and with traps disabled nothing wakes it */
static void test_idle(void)
{
    g_autofree char *prom = write_prom(0);
    QTestState *qts = run_prom(prom);

    g_usleep(200 * 1000);
    /* The first round of the loop may run before the CPU stops */
    g_assert_cmpuint(qtest_readl(qts, RAM_COUNT), <=, 1);
    qtest_quit(qts);
    unlink(prom);
}

int main(int argc, char **argv)
{
    int i;

    g_test_init(&argc, &argv, NULL);

    for (i = 0; i < ARRAY_SIZE(machines); i++) {
        g_autofree char *name = g_strdup_printf("sun4m-apc/%s/regs",
                                                machines[i].machine);
        qtest_add_data_func(name, &machines[i], test_regs);
    }
    if (qtest_has_accel("tcg")) {
        qtest_add_func("sun4m-apc/SS-5/no-idle", test_no_idle);
        qtest_add_func("sun4m-apc/SS-5/idle", test_idle);
    }

    return g_test_run();
}
