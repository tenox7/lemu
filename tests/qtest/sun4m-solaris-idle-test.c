/*
 * QTest: halting the CPU in the Solaris idle loop on sun4m
 *
 * A boot PROM runs a loop laid out as the idle loop of Solaris 2.6: it
 * counts its rounds in RAM, then calls "swtch" when the word at
 * CPU + 0x58 (disp_nrunnable) is not 0 and calls the function that the
 * word at CPU + 0x5c (idle_cpu) points to when it is. With interrupts
 * enabled at level 0 and none pending, QEMU halts the CPU at that call,
 * and as no device interrupts, the count stops. It must keep going with
 * solaris-idle=off, with a runnable thread, at interrupt level 15 and with
 * two CPUs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"

/* RAM: the CPU structure, its round count at 0 and the trap marker */
#define RAM_CPU         0x1000ULL
#define RAM_COUNT       (RAM_CPU + 0)
#define RAM_TRAP        (RAM_CPU + 0x20)
#define PROM_WORDS      0x420           /* up to past PROM_DECOY */
#define PROM_DECOY      ((RAM_CPU + 0x58) / 4)
#define CODE            0x100           /* word index of the code */

/*
 * The PROM: a trap table whose entries store their trap type at RAM_TRAP
 * and stop, and at 0x400 the loop. It runs from reset: MMU off, so
 * instructions come from the PROM and data from RAM.
 */
static char *write_prom(uint32_t nrunnable, uint32_t psr)
{
    const uint32_t code[] = {
        0x31000004,             /*        sethi %hi(0x1000), %i0 */
        0x82102000 | nrunnable, /*        mov   nrunnable, %g1 */
        0xc2262058,             /*        st    %g1, [%i0 + 0x58] */
        0x82102464,             /*        mov   ret, %g1 */
        0xc226205c,             /*        st    %g1, [%i0 + 0x5c] */
        0x81882000 | psr,       /*        wr    %g0, psr, %psr */
        0x01000000,             /*        nop */
        0x01000000,             /*        nop */
        0x01000000,             /*        nop */
        0xca062000,             /* top:   ld    [%i0], %g5 */
        0x8a016001,             /*        inc   %g5 */
        0xca262000,             /*        st    %g5, [%i0] */
        0xd0062058,             /*        ld    [%i0 + 0x58], %o0 */
        0x80a22000,             /*        cmp   %o0, 0 */
        0x02800006,             /*        be    idle */
        0x21000004,             /*         sethi %hi(0x1000), %l0 */
        0x40000009,             /*        call  ret          swtch */
        0x01000000,             /*         nop */
        0x10bffff7,             /*        b     top */
        0x01000000,             /*         nop */
        0xe004205c,             /* idle:  ld    [%l0 + 0x5c], %l0 */
        0x9fc40000,             /*        call  %l0          idle_cpu */
        0x01000000,             /*         nop */
        0x10bffff2,             /*        b     top */
        0x01000000,             /*         nop */
        0x81c3e008,             /* ret:   retl */
        0x01000000,             /*         nop */
    };
    uint32_t be[PROM_WORDS] = { 0 };
    char *path = NULL;
    int fd, i;

    /* Reset (trap 0): b 0x400 */
    be[0] = cpu_to_be32(0x10800000 | CODE);
    be[1] = cpu_to_be32(0x01000000);
    for (i = 1; i < CODE / 4; i++) {
        be[4 * i] = cpu_to_be32(0x03000004);     /* sethi %hi(0x1000), %g1 */
        be[4 * i + 1] = cpu_to_be32(0x84102000 | i);    /* mov i, %g2 */
        be[4 * i + 2] = cpu_to_be32(0xc4206020); /* st %g2, [%g1 + 0x20] */
        be[4 * i + 3] = cpu_to_be32(0x10800000); /* b . */
    }
    for (i = 0; i < ARRAY_SIZE(code); i++) {
        be[CODE + i] = cpu_to_be32(code[i]);
    }
    /*
     * The PROM word at the RAM address of disp_nrunnable says 1. With the
     * MMU off in boot mode an instruction fetch from there reads it, a data
     * load reads RAM: the idle helper must read RAM, like the loop.
     */
    be[PROM_DECOY] = cpu_to_be32(1);
    fd = g_file_open_tmp("sun4m-solaris-idle-prom-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, be, sizeof(be)), ==, sizeof(be));
    close(fd);
    return path;
}

/* The loop's round count 300 ms apart, once it has started */
static void run(const char *machine, uint32_t nrunnable, uint32_t psr,
                uint32_t *before, uint32_t *after)
{
    g_autofree char *prom = write_prom(nrunnable, psr);
    gint64 end = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    QTestState *qts = qtest_initf("-machine %s -accel tcg -bios %s",
                                  machine, prom);

    while (qtest_readl(qts, RAM_COUNT) == 0) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(1000);
    }
    g_usleep(300 * 1000);
    *before = qtest_readl(qts, RAM_COUNT);
    g_usleep(300 * 1000);
    *after = qtest_readl(qts, RAM_COUNT);
    g_assert_cmphex(qtest_readl(qts, RAM_TRAP), ==, 0);
    qtest_quit(qts);
    unlink(prom);
}

#define PSR_S_ET        0xa0
#define PSR_S_ET_PIL15  0xfa0

typedef struct IdleCase {
    const char *name;
    const char *machine;
    uint32_t nrunnable;
    uint32_t psr;
} IdleCase;

/* Nothing runnable, interrupts enabled at level 0: the CPU stops */
static void test_idle(void)
{
    uint32_t before, after;

    run("SS-5", 0, PSR_S_ET, &before, &after);
    g_assert_cmpuint(after, ==, before);
}

static void test_running(const void *data)
{
    const IdleCase *t = data;
    uint32_t before, after;

    run(t->machine, t->nrunnable, t->psr, &before, &after);
    g_assert_cmpuint(after, >, before);
}

int main(int argc, char **argv)
{
    static const IdleCase running[] = {
        { "off", "SS-5,solaris-idle=off", 0, PSR_S_ET },
        { "runnable", "SS-5", 1, PSR_S_ET },
        { "pil15", "SS-5", 0, PSR_S_ET_PIL15 },
        { "smp2", "SS-20 -smp 2", 0, PSR_S_ET },
    };
    int i;

    g_test_init(&argc, &argv, NULL);

    if (qtest_has_accel("tcg")) {
        qtest_add_func("sun4m-solaris-idle/idle", test_idle);
        for (i = 0; i < ARRAY_SIZE(running); i++) {
            g_autofree char *name =
                g_strdup_printf("sun4m-solaris-idle/%s", running[i].name);
            qtest_add_data_func(name, &running[i], test_running);
        }
    }

    return g_test_run();
}
