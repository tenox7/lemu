/*
 * QTest: ld and st %fsr with any rd on sun4m
 *
 * V7 and V8 chips ignore the rd field of LDFSR and STFSR. Old static
 * SunOS 4 binaries have rd 4 there: the _sigtramp of an old libc saves
 * the FSR with c92ba0e0 (st %fsr, [%sp + 0xe0]) and loads it back with
 * c90ba0e0 (ld [%sp + 0xe0], %fsr). QEMU 8.2 to 11.1 took these words
 * only with rd 0 and raised an illegal instruction trap for any other rd.
 *
 * A boot PROM enables the FPU, stores a value at [%sp + 0xe0], loads it
 * into the FSR, clears the word and stores the FSR back there, with the
 * same rd in both words. It must get to the end without a trap, and the
 * fields that LDFSR sets must read back.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"

/* RAM: the trap marker, the end marker and the FSR word at %sp + 0xe0 */
#define RAM_TRAP        0x1020ULL
#define RAM_DONE        0x1024ULL
#define RAM_FSR         0x20e0ULL
#define PROM_WORDS      0x200
#define CODE            0x100           /* word index of the code */

/* The FSR fields LDFSR sets: RD, TEM, fcc, aexc and cexc */
#define FSR_LD_MASK     0xcf800fffu

typedef struct FsrCase {
    const char *name;
    unsigned rd;
    uint32_t fsr;
} FsrCase;

static const FsrCase cases[] = {
    { "rd0", 0, 0x80000460 },   /* RD +inf, fcc 1, aexc dz nx */
    { "rd4", 4, 0x48000ca0 },   /* RD zero, NVM, fcc 3, aexc uf nx */
    { "rd31", 31, 0xc0800815 }, /* RD -inf, NXM, fcc 2, cexc nv uf nx */
};

/*
 * The PROM: a trap table whose entries store their trap type at RAM_TRAP
 * and stop, and at 0x400 the test. It runs from reset: MMU off, so
 * instructions come from the PROM and data from RAM.
 */
static char *write_prom(const FsrCase *c)
{
    uint32_t code[] = {
        0x03000004,                 /* sethi %hi(0x1000), %g1 */
        0x82106fa0,                 /* or    %g1, 0xfa0, %g1 */
        0x81886000,                 /* wr    %g1, %psr     EF, PIL 15, S, ET */
        0x01000000,                 /* nop */
        0x01000000,                 /* nop */
        0x01000000,                 /* nop */
        0x1d000008,                 /* sethi %hi(0x2000), %sp */
        0x05000000 | (c->fsr >> 10),    /* sethi %hi(fsr), %g2 */
        0x8410a000 | (c->fsr & 0x3ff),  /* or    %g2, %lo(fsr), %g2 */
        0xc423a0e0,                 /* st    %g2, [%sp + 0xe0] */
        0xc10ba0e0 | (c->rd << 25), /* ld    [%sp + 0xe0], %fsr (c90ba0e0) */
        0xc023a0e0,                 /* st    %g0, [%sp + 0xe0] */
        0xc12ba0e0 | (c->rd << 25), /* st    %fsr, [%sp + 0xe0] (c92ba0e0) */
        0x03000004,                 /* sethi %hi(0x1000), %g1 */
        0x86102001,                 /* mov   1, %g3 */
        0xc6206024,                 /* st    %g3, [%g1 + 0x24]  done */
        0x10800000,                 /* b     . */
        0x01000000,                 /*  nop */
    };
    uint32_t be[PROM_WORDS] = { 0 };
    char *path = NULL;
    int fd, i;

    /* Reset (trap 0): b 0x400 */
    be[0] = cpu_to_be32(0x10800000 | CODE);
    be[1] = cpu_to_be32(0x01000000);
    /* Trap i: store i at RAM_TRAP and stop */
    for (i = 1; i < CODE / 4; i++) {
        be[4 * i] = cpu_to_be32(0x03000004);         /* sethi %hi(0x1000), %g1 */
        be[4 * i + 1] = cpu_to_be32(0x84102000 | i); /* mov   i, %g2 */
        be[4 * i + 2] = cpu_to_be32(0xc4206020);     /* st    %g2, [%g1 + 0x20] */
        be[4 * i + 3] = cpu_to_be32(0x10800000);     /* b     . */
    }
    for (i = 0; i < ARRAY_SIZE(code); i++) {
        be[CODE + i] = cpu_to_be32(code[i]);
    }
    fd = g_file_open_tmp("sun4m-fsr-rd-prom-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, be, sizeof(be)), ==, sizeof(be));
    close(fd);
    return path;
}

static void test_fsr_rd(const void *data)
{
    const FsrCase *c = data;
    g_autofree char *prom = write_prom(c);
    gint64 end = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    QTestState *qts = qtest_initf("-machine SS-5 -accel tcg -bios %s", prom);

    while (qtest_readl(qts, RAM_TRAP) == 0 &&
           qtest_readl(qts, RAM_DONE) == 0) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(1000);
    }
    /* An illegal instruction would be trap type 2 */
    g_assert_cmphex(qtest_readl(qts, RAM_TRAP), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, RAM_DONE), ==, 1);
    g_assert_cmphex(qtest_readl(qts, RAM_FSR) & FSR_LD_MASK, ==, c->fsr);
    qtest_quit(qts);
    unlink(prom);
}

int main(int argc, char **argv)
{
    int i;

    g_test_init(&argc, &argv, NULL);

    if (qtest_has_accel("tcg")) {
        for (i = 0; i < ARRAY_SIZE(cases); i++) {
            g_autofree char *name =
                g_strdup_printf("sun4m-fsr-rd/%s", cases[i].name);
            qtest_add_data_func(name, &cases[i], test_fsr_rd);
        }
    }

    return g_test_run();
}
