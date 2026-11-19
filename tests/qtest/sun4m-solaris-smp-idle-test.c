/*
 * QTest: halting idle CPUs in the multiprocessor idle loop of Solaris on
 * sun4m
 *
 * A boot PROM runs a loop laid out as the multiprocessor idle loop of
 * Solaris 2.6 on the first CPU of an SS-20 with two: it calls "swtch" when
 * the word at CPU + 0x58 (disp_nrunnable) is not 0, else "disp_getwork",
 * which finds nothing, and counts its rounds in RAM. With interrupts
 * enabled at level 0 and none pending, QEMU halts the CPU at the call of
 * disp_getwork, and as no device interrupts, only the bounded halt's timer
 * (10 ms) wakes it: the loop goes on at about 100 rounds a second instead
 * of millions. When the test makes a thread runnable, the timer ends the
 * halt and the loop runs at full speed. It must run at full speed with
 * solaris-smp-idle=off, at interrupt level 15 and with one CPU.
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
#define RAM_NRUNNABLE   (RAM_CPU + 0x58)
#define PROM_WORDS      0x420           /* up to past PROM_DECOY */
#define PROM_DECOY      (RAM_NRUNNABLE / 4)
#define CODE            0x100           /* word index of the code */

/* A loop that spins does far more rounds than this in 300 ms */
#define HALTED_MAX      300

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
        0x81882000 | psr,       /*        wr    %g0, psr, %psr */
        0x01000000,             /*        nop */
        0x01000000,             /*        nop */
        0x01000000,             /*        nop */
        0x90102000,             /*        clr   %o0            cpu_flags */
        0x808a2004,             /* quies: btst  4, %o0 */
        0x32bfffff,             /*        bne,a quies */
        0xd0162008,             /*         lduh [%i0 + 8], %o0 */
        0xd0062058,             /*        ld    [%i0 + 0x58], %o0 */
        0x80a22000,             /*        cmp   %o0, 0 */
        0x22800008,             /*        be,a  idle */
        0xd0162008,             /*         lduh [%i0 + 8], %o0 */
        0x40000013,             /*        call  ret            unset_idle_cpu */
        0x01000000,             /*         nop */
        0x40000011,             /*        call  ret            swtch */
        0x01000000,             /*         nop */
        0x1080000a,             /*        b     count */
        0x01000000,             /*         nop */
        0x808a2020,             /* idle:  btst  0x20, %o0 */
        0x32bffff2,             /*        bne,a quies */
        0xd0162008,             /*         lduh [%i0 + 8], %o0 */
        0x4000000c,             /*        call  ret0           disp_getwork */
        0x90100018,             /*         mov  %i0, %o0 */
        0x80900008,             /*        tst   %o0 */
        0x10800002,             /*        b     count */
        0x01000000,             /*         nop */
        0xca062000,             /* count: ld    [%i0], %g5 */
        0x8a016001,             /*        inc   %g5 */
        0xca262000,             /*        st    %g5, [%i0] */
        0x10bfffe8,             /*        b     quies */
        0xd0162008,             /*         lduh [%i0 + 8], %o0 */
        0x81c3e008,             /* ret:   retl */
        0x01000000,             /*         nop */
        0x81c3e008,             /* ret0:  retl */
        0x90102000,             /*         clr  %o0 */
    };
    uint32_t be[PROM_WORDS] = { 0 };
    char *path = NULL;
    int fd, i;

    /* Reset (trap 0): b 0x400 */
    be[0] = cpu_to_be32(0x10800000 | CODE);
    be[1] = cpu_to_be32(0x01000000);
    for (i = 1; i < CODE / 4; i++) {
        /* sethi %hi(0x1000), %g1; mov i, %g2; st %g2, [%g1 + 0x20]; b . */
        be[4 * i] = cpu_to_be32(0x03000004);
        be[4 * i + 1] = cpu_to_be32(0x84102000 | i);
        be[4 * i + 2] = cpu_to_be32(0xc4206020);
        be[4 * i + 3] = cpu_to_be32(0x10800000);
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
    fd = g_file_open_tmp("sun4m-solaris-smp-idle-prom-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, be, sizeof(be)), ==, sizeof(be));
    close(fd);
    return path;
}

typedef struct Run {
    QTestState *qts;
    char *prom;
} Run;

static void start(Run *r, const char *machine, uint32_t nrunnable,
                  uint32_t psr)
{
    gint64 end = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    r->prom = write_prom(nrunnable, psr);
    r->qts = qtest_initf("-machine %s -accel tcg -bios %s", machine, r->prom);
    while (qtest_readl(r->qts, RAM_COUNT) == 0) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(1000);
    }
    g_usleep(300 * 1000);
}

/* The loop's rounds over the next 300 ms */
static uint32_t rounds(Run *r)
{
    uint32_t before = qtest_readl(r->qts, RAM_COUNT);

    g_assert_cmphex(qtest_readl(r->qts, RAM_TRAP), ==, 0);

    g_usleep(300 * 1000);
    return qtest_readl(r->qts, RAM_COUNT) - before;
}

static void finish(Run *r)
{
    g_assert_cmphex(qtest_readl(r->qts, RAM_TRAP), ==, 0);
    qtest_quit(r->qts);
    unlink(r->prom);
    g_free(r->prom);
}

#define PSR_S_ET        0xa0
#define PSR_S_ET_PIL15  0xfa0

/*
 * Nothing runnable, interrupts enabled at level 0: the CPU halts and the
 * timer wakes it now and then. Then a thread becomes runnable: the timer
 * ends the halt and the loop runs.
 */
static void test_idle(void)
{
    Run r;
    uint32_t n;

    start(&r, "SS-20 -smp 2", 0, PSR_S_ET);
    n = rounds(&r);
    g_assert_cmpuint(n, >, 0);
    g_assert_cmpuint(n, <, HALTED_MAX);
    qtest_writel(r.qts, RAM_NRUNNABLE, 1);
    g_assert_cmpuint(rounds(&r), >, 10 * HALTED_MAX);
    finish(&r);
}

typedef struct RunningCase {
    const char *name;
    const char *machine;
    uint32_t psr;
} RunningCase;

static void test_running(const void *data)
{
    const RunningCase *t = data;
    Run r;

    start(&r, t->machine, 0, t->psr);
    g_assert_cmpuint(rounds(&r), >, 10 * HALTED_MAX);
    finish(&r);
}

int main(int argc, char **argv)
{
    static const RunningCase running[] = {
        { "off", "SS-20,solaris-smp-idle=off -smp 2", PSR_S_ET },
        { "pil15", "SS-20 -smp 2", PSR_S_ET_PIL15 },
        { "smp1", "SS-20", PSR_S_ET },
    };
    int i;

    g_test_init(&argc, &argv, NULL);

    if (qtest_has_accel("tcg")) {
        qtest_add_func("sun4m-solaris-smp-idle/idle", test_idle);
        for (i = 0; i < ARRAY_SIZE(running); i++) {
            g_autofree char *name =
                g_strdup_printf("sun4m-solaris-smp-idle/%s", running[i].name);
            qtest_add_data_func(name, &running[i], test_running);
        }
    }

    return g_test_run();
}
