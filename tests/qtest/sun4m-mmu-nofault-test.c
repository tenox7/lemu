/*
 * QTest: no-fault mode of the sun4m SRMMU and the TLB
 *
 * A boot PROM turns the MMU on with three 16 MB mappings: the PROM itself
 * at virtual 0, RAM read-write at 0x01000000 and RAM that user mode may
 * only read at 0x02000000. With the no-fault bit (NF) set in the MMU
 * control register it writes to the read-only RAM in user mode (ASI 0x0a),
 * which must not trap; QEMU puts an entry with overridden permissions in
 * its TLB for that. After NF is cleared the same write must trap (data
 * access exception), so that entry must not outlive no-fault mode. Before
 * that the PROM sets and clears NF once with no fault in between, which
 * QEMU need not flush the TLB for.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"

/* RAM: the stage the PROM reached and the trap marker */
#define RAM_STAGE       0x1004ULL
#define RAM_TRAP        0x1020ULL
#define PROM_WORDS      0x200
#define CODE            0x100           /* word index of the code */
#define TT_DATA_ACCESS  9

static char *write_prom(uint64_t prom_base)
{
    /* L1 page table entry: the PROM at virtual 0, ACC 2 (RX), 16 MB */
    uint32_t pte = (uint32_t)(prom_base >> 4) | 0x0a;
    const uint32_t code[] = {
        0x11000008,     /* sethi %hi(0x2000), %o0    context table */
        0x92102301,     /* mov   0x301, %o1          PTD: L1 at 0x3000 */
        0xd2220000,     /* st    %o1, [%o0] */
        0x1100000c,     /* sethi %hi(0x3000), %o0    L1 table */
        0x13000000 | (pte >> 10), /* sethi %hi(pte), %o1 */
        0x92126000 | (pte & 0x3ff), /* or    %o1, %lo(pte), %o1 */
        0xd2220000,     /* st    %o1, [%o0]          0x00000000: PROM */
        0x9210200e,     /* mov   0xe, %o1            RAM, ACC 3 (RWX) */
        0xd2222004,     /* st    %o1, [%o0 + 4]      0x01000000 */
        0x92102016,     /* mov   0x16, %o1           RAM, ACC 5 (user R) */
        0xd2222008,     /* st    %o1, [%o0 + 8]      0x02000000 */
        0x90102100,     /* mov   0x100, %o0 */
        0x92102200,     /* mov   0x200, %o1 */
        0xd2a20080,     /* sta   %o1, [%o0] 4        context table pointer */
        0x90102200,     /* mov   0x200, %o0 */
        0xc0a20080,     /* sta   %g0, [%o0] 4        context 0 */
        0x81902000,     /* wr    %g0, 0, %wim */
        0x81882fa0,     /* wr    %g0, 0xfa0, %psr    S, ET, PIL 15 */
        0x01000000,     /* nop */
        0x01000000,     /* nop */
        0x01000000,     /* nop */
        0xd4800080,     /* lda   [%g0] 4, %o2        MMU control register */
        0x9412a001,     /* or    %o2, 1, %o2         MMU on */
        0xd4a00080,     /* sta   %o2, [%g0] 4 */
        0x9612a002,     /* or    %o2, 2, %o3         the same with NF */
        0x07004004,     /* sethi %hi(0x01001000), %g3 */
        0x88102001,     /* mov   1, %g4 */
        0xc820e004,     /* st    %g4, [%g3 + 4]      stage 1: MMU on */
        0x0b008000,     /* sethi %hi(0x02000000), %g5 */
        0x8a116100,     /* or    %g5, 0x100, %g5     user read-only word */
        0x84016100,     /* add   %g5, 0x100, %g2     another one */
        0x8c102055,     /* mov   0x55, %g6 */
        0xd6a00080,     /* sta   %o3, [%g0] 4        NF on */
        0xd4a00080,     /* sta   %o2, [%g0] 4        NF off, no fault */
        0xce808140,     /* lda   [%g2] 0x0a, %g7     user read: allowed */
        0xd6a00080,     /* sta   %o3, [%g0] 4        NF on */
        0xcca14140,     /* sta   %g6, [%g5] 0x0a     user write: no trap */
        0xd4a00080,     /* sta   %o2, [%g0] 4        NF off */
        0x88102002,     /* mov   2, %g4 */
        0xc820e004,     /* st    %g4, [%g3 + 4]      stage 2 */
        0xcca14140,     /* sta   %g6, [%g5] 0x0a     user write: traps */
        0x88102003,     /* mov   3, %g4 */
        0xc820e004,     /* st    %g4, [%g3 + 4]      stage 3: it didn't */
        0x10800000,     /* b     . */
        0x01000000,     /* nop */
    };
    uint32_t be[PROM_WORDS] = { 0 };
    char *path = NULL;
    int fd, i;

    /* Reset (trap 0): b 0x400 */
    be[0] = cpu_to_be32(0x10800000 | CODE);
    be[1] = cpu_to_be32(0x01000000);
    /* Trap i: store i at RAM_TRAP through its virtual address, and stop */
    for (i = 1; i < CODE / 4; i++) {
        /* sethi %hi(0x01001000), %g1; mov i, %g2; st %g2, [%g1 + 0x20]; b . */
        be[4 * i] = cpu_to_be32(0x03004004);
        be[4 * i + 1] = cpu_to_be32(0x84102000 | i);
        be[4 * i + 2] = cpu_to_be32(0xc4206020);
        be[4 * i + 3] = cpu_to_be32(0x10800000);
    }
    for (i = 0; i < ARRAY_SIZE(code); i++) {
        be[CODE + i] = cpu_to_be32(code[i]);
    }
    fd = g_file_open_tmp("sun4m-mmu-nofault-prom-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, be, sizeof(be)), ==, sizeof(be));
    close(fd);
    return path;
}

typedef struct {
    const char *machine;
    uint64_t prom_base;
} TestCase;

static void test_nofault(const void *data)
{
    const TestCase *t = data;
    g_autofree char *prom = write_prom(t->prom_base);
    gint64 end = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    QTestState *qts = qtest_initf("-machine %s -accel tcg -m 64 -bios %s",
                                  t->machine, prom);

    while (qtest_readl(qts, RAM_TRAP) == 0 && qtest_readl(qts, RAM_STAGE) < 3) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(1000);
    }
    g_assert_cmpuint(qtest_readl(qts, RAM_STAGE), ==, 2);
    g_assert_cmpuint(qtest_readl(qts, RAM_TRAP), ==, TT_DATA_ACCESS);
    qtest_quit(qts);
    unlink(prom);
}

int main(int argc, char **argv)
{
    static const TestCase ss5 = { "SS-5", 0x70000000ULL };
    static const TestCase ss20 = { "SS-20", 0xff0000000ULL };

    g_test_init(&argc, &argv, NULL);

    if (qtest_has_accel("tcg")) {
        qtest_add_data_func("sun4m-mmu-nofault/ss5", &ss5, test_nofault);
        qtest_add_data_func("sun4m-mmu-nofault/ss20", &ss20, test_nofault);
    }

    return g_test_run();
}
