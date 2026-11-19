/*
 * QTest: an interrupt let in by a write to %psr in a delay slot on sun4m
 *
 * A boot PROM loops: it raises PIL to 15, writes the set register of its
 * CPU's interrupt controller, counts the round in RAM and lowers PIL to 0
 * again. In the first round it writes 0 there, from the second on the bit
 * of soft interrupt 13, which must then be taken as soon as PIL is 0. The
 * write to %psr that lowers PIL is a plain instruction, or in the delay
 * slot of a branch (always, conditional, conditional with annul, jmpl):
 * Solaris lowers PIL in the delay slot of "bl,a" in splx().
 *
 * QEMU looks at pending interrupts between translation blocks that don't
 * chain, and a write to %psr ends its block for that. One in a delay slot
 * used to chain on to the branch target instead, so once the first round
 * had linked the blocks, the interrupt waited for something else to stop
 * the CPU, and here nothing does. That is how two CPUs running Solaris
 * stopped: the one that was sent a cross-call never took it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"

/* RAM: the round the PROM is in and the trap marker */
#define RAM_ROUND       0x1004ULL
#define RAM_TRAP        0x1020ULL
#define PROM_WORDS      0x200
#define CODE            0x100           /* word index of the code */
#define VARIANT         15              /* word index of the variant */
#define LOOP            19              /* and of what follows it */
#define TT_INTERRUPT_13 0x1d

/* Four words that lower PIL to 0 and get to LOOP */
static const uint32_t variants[][4] = {
    {   /* plain */
        0x818820a0,             /* wr    %g0, 0xa0, %psr     S, ET, PIL 0 */
        0x01000000,             /* nop */
        0x01000000,             /* nop */
        0x01000000,             /* nop */
    }, { /* ba */
        0x10800004,             /* ba    next */
        0x818820a0,             /*  wr   %g0, 0xa0, %psr */
        0x01000000,             /* nop (not reached) */
        0x01000000,             /* nop (not reached) */
    }, { /* bne, taken */
        0x80a02001,             /* cmp   %g0, 1 */
        0x12800003,             /* bne   next */
        0x818820a0,             /*  wr   %g0, 0xa0, %psr */
        0x01000000,             /* nop (not reached) */
    }, { /* bne,a, taken: splx() in Solaris */
        0x80a02001,             /* cmp   %g0, 1 */
        0x32800003,             /* bne,a next */
        0x818820a0,             /*  wr   %g0, 0xa0, %psr */
        0x01000000,             /* nop (not reached) */
    }, { /* jmpl */
        0x84102000 | ((CODE + LOOP) * 4),   /* mov   next, %g2 */
        0x81c0a000,             /* jmp   %g2 */
        0x818820a0,             /*  wr   %g0, 0xa0, %psr */
        0x01000000,             /* nop (not reached) */
    },
};

static const char *const names[] = {
    "plain", "ba", "bne", "bne-annul", "jmpl"
};

/*
 * The PROM: a trap table whose entries store their trap type at RAM_TRAP
 * and stop, and at 0x400 the loop. It runs from reset: MMU off, so
 * instructions come from the PROM and data from RAM and the devices.
 */
static char *write_prom(int variant)
{
    uint32_t code[] = {
        0x111c7800,             /*        sethi %hi(0x71e00000), %o0  intctl */
        0x31000004,             /*        sethi %hi(0x1000), %i0 */
        0x92102000,             /*        mov   0, %o1          nothing yet */
        0xa2102001,             /*        mov   1, %l1          round */
        0x81882fa0,             /* again: wr    %g0, 0xfa0, %psr  PIL 15 */
        0x01000000,             /*        nop */
        0x01000000,             /*        nop */
        0x01000000,             /*        nop */
        0xd2222008,             /*        st    %o1, [%o0 + 8]  set soft ints */
        0xc0022000,             /*        ld    [%o0], %g0 */
        0x81882fa0,             /*        wr    %g0, 0xfa0, %psr  the CPU */
        0x01000000,             /*        nop    looks at the interrupt */
        0x01000000,             /*        nop    here and can't take it */
        0x01000000,             /*        nop */
        0xe2262004,             /*        st    %l1, [%i0 + 4]  the round */
        0, 0, 0, 0,             /*        the variant: PIL 0 */
        0x13080000,             /* next:  sethi %hi(0x20000000), %o1  soft 13 */
        0xa2046001,             /*        inc   %l1 */
        0x10bfffef,             /*        ba    again */
        0x01000000,             /*         nop */
    };
    uint32_t be[PROM_WORDS] = { 0 };
    char *path = NULL;
    int fd, i;

    memcpy(&code[VARIANT], variants[variant], sizeof(variants[variant]));

    /* Reset (trap 0): b 0x400 */
    be[0] = cpu_to_be32(0x10800000 | CODE);
    be[1] = cpu_to_be32(0x01000000);
    /* Trap i: store i at RAM_TRAP and stop */
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
    fd = g_file_open_tmp("sun4m-pil-delay-slot-prom-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, be, sizeof(be)), ==, sizeof(be));
    close(fd);
    return path;
}

static void test_interrupt(const void *data)
{
    int variant = GPOINTER_TO_INT(data);
    g_autofree char *prom = write_prom(variant);
    gint64 end = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    QTestState *qts = qtest_initf("-machine SS-5 -accel tcg -bios %s", prom);

    /* Without the interrupt the PROM goes round and round */
    while (qtest_readl(qts, RAM_TRAP) == 0 &&
           qtest_readl(qts, RAM_ROUND) < 1000) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readl(qts, RAM_TRAP), ==, TT_INTERRUPT_13);
    g_assert_cmpuint(qtest_readl(qts, RAM_ROUND), ==, 2);
    qtest_quit(qts);
    unlink(prom);
}

int main(int argc, char **argv)
{
    int i;

    g_test_init(&argc, &argv, NULL);

    if (qtest_has_accel("tcg")) {
        for (i = 0; i < ARRAY_SIZE(variants); i++) {
            g_autofree char *name =
                g_strdup_printf("sun4m-pil-delay-slot/%s", names[i]);
            qtest_add_data_func(name, GINT_TO_POINTER(i), test_interrupt);
        }
    }

    return g_test_run();
}
