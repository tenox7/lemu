/*
 * QTest: a tick interrupt that fires during a write to %softint on sun4u
 *
 * A boot PROM arms %tick_cmpr 10 us ahead and, with interrupts off, sets
 * and clears soft interrupt 1 with %set_softint and %clear_softint until
 * %softint shows the tick's bit (TICK_INT), clears it and counts the
 * tick; if 16M rounds go by without one, it counts the tick as lost. Then
 * it arms the next one.
 *
 * The tick timer sets TICK_INT from the main loop, holding the BQL; the
 * writes to %softint read, modify and write it in the vCPU thread. They
 * didn't take the BQL, so a tick that fired between their read and their
 * write was overwritten and lost, and a guest that waits for its clock
 * interrupt before arming the next (illumos) lost its clock for good.
 * The race can't be made to happen on demand, but the loop is almost all
 * %softint writes: without the lock a tick was lost within the first
 * three in each of 30 runs. With it, no tick can be lost.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"

/* RAM: the ticks the PROM saw and lost */
#define RAM_SEEN        0x800ULL
#define RAM_LOST        0x808ULL
#define PROM_WORDS      0x100
#define RESET           (0x20 / 4)      /* power-on reset: prom + 0x20 */

#define TICKS           20000           /* enough ticks */
#define MIN_TICKS       1000            /* the least for a pass */
#define SECONDS         10

/*
 * Runs from power-on reset: RED state, MMU off, PSTATE.IE clear, so the
 * tick's level 14 interrupt is never taken and its bit stays in %softint.
 */
static const uint32_t code[] = {
    0xa0102000,     /*        mov   0, %l0          ticks seen */
    0xa2102000,     /*        mov   0, %l1          ticks lost */
    0x27004000,     /*        sethi %hi(0x1000000), %l3 */
    0x83410000,     /* again: rd    %tick, %g1 */
    0x820063e8,     /*        add   %g1, 1000, %g1  10 us on */
    0x83287001,     /*        sllx  %g1, 1, %g1 */
    0x83307001,     /*        srlx  %g1, 1, %g1     INT_DIS off */
    0xaf806000,     /*        wr    %g1, 0, %tick_cmpr */
    0xa4100013,     /*        mov   %l3, %l2 */
    0xa9802002,     /* spin:  wr    %g0, 2, %set_softint */
    0xab802002,     /*        wr    %g0, 2, %clear_softint */
    0x85458000,     /*        rd    %softint, %g2 */
    0x8088a001,     /*        andcc %g2, 1, %g0     TICK_INT */
    0x12800009,     /*        bne   got */
    0x01000000,     /*         nop */
    0xa4a4a001,     /*        subcc %l2, 1, %l2 */
    0x12bffff9,     /*        bne   spin */
    0x01000000,     /*         nop */
    0xa2046001,     /*        inc   %l1             lost */
    0xe2702808,     /*        stx   %l1, [%g0 + 0x808] */
    0x10bfffef,     /*        ba    again */
    0x01000000,     /*         nop */
    0xab802001,     /* got:   wr    %g0, 1, %clear_softint */
    0xa0042001,     /*        inc   %l0 */
    0xe0702800,     /*        stx   %l0, [%g0 + 0x800] */
    0x10bfffea,     /*        ba    again */
    0x01000000,     /*         nop */
};

static char *write_prom(void)
{
    uint32_t be[PROM_WORDS];
    char *path = NULL;
    int fd, i;

    /* Anything else: b . */
    for (i = 0; i < PROM_WORDS; i++) {
        be[i] = cpu_to_be32(0x10800000);
    }
    for (i = 0; i < ARRAY_SIZE(code); i++) {
        be[RESET + i] = cpu_to_be32(code[i]);
    }
    fd = g_file_open_tmp("sun4u-softint-prom-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, be, sizeof(be)), ==, sizeof(be));
    close(fd);
    return path;
}

static void test_tick(void)
{
    g_autofree char *prom = write_prom();
    gint64 end = g_get_monotonic_time() + SECONDS * G_TIME_SPAN_SECOND;
    QTestState *qts = qtest_initf("-machine sun4u -accel tcg -nodefaults "
                                  "-bios %s", prom);
    uint64_t seen = 0, lost = 0;

    while (g_get_monotonic_time() < end) {
        g_usleep(10000);
        seen = qtest_readq(qts, RAM_SEEN);
        lost = qtest_readq(qts, RAM_LOST);
        if (lost || seen >= TICKS) {
            break;
        }
    }
    qtest_quit(qts);
    unlink(prom);

    g_test_message("ticks seen %" PRIu64 ", lost %" PRIu64, seen, lost);
    g_assert_cmpuint(lost, ==, 0);
    g_assert_cmpuint(seen, >=, MIN_TICKS);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (qtest_has_accel("tcg")) {
        qtest_add_func("sun4u-softint/tick", test_tick);
    }

    return g_test_run();
}
