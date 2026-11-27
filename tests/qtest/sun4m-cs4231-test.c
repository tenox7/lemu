/*
 * QTest: CS4231 playback through the APC DMA engine on the SPARCstation 5
 *
 * The test programs the chip the way the SunOS and NetBSD drivers do:
 * IOMMU mappings for two u-law buffers, the playback format in the codec,
 * the first buffer in the APC's next registers, the CSR with DMA on and
 * its interrupts enabled, and then playback on in the codec (PEN). Driven
 * by the virtual clock, the APC must take the first buffer (PI, the next
 * registers free again: PD), take the second one when the first has been
 * played and then run empty (PMI, PM), in real time at 8000 Hz, with each
 * of those interrupts on system interrupt bit 9, the CPU's level 5, where
 * OpenBIOS's "intr" 5 puts the guest's handler. The samples must reach the
 * audiodev decoded, all of them, also when the driver turns the DMA off as
 * soon as the pipe is empty: a wav file with the right values for exactly
 * the length of the two buffers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"

/* SPARCstation 5 */
#define CS_BASE         0x6c000000ULL
#define IOMMU_BASE      0x10000000ULL
#define INTCTL_CPU0     0x71e00000ULL   /* per-CPU pending: 1 << PIL */
#define INTCTL_SYS      0x71e10000ULL   /* system pending: 1 << bit */

/* Codec and APC registers */
#define CS_IAR          (CS_BASE + 0x00)
#define CS_IDR          (CS_BASE + 0x04)
#define APC_CSR         (CS_BASE + 0x10)
#define APC_PVA         (CS_BASE + 0x30)
#define APC_PC          (CS_BASE + 0x34)
#define APC_PNVA        (CS_BASE + 0x38)
#define APC_PNC         (CS_BASE + 0x3c)

#define APC_IP          0x00800000
#define APC_PI          0x00400000
#define APC_IE          0x00080000
#define APC_PIE         0x00040000
#define APC_PMI         0x00008000
#define APC_PM          0x00004000
#define APC_PD          0x00002000
#define APC_PMIE        0x00001000
#define APC_PDMA_GO     0x00000008
#define APC_ENABLES     (APC_IE | APC_PIE | APC_PMIE | APC_PDMA_GO)

#define CS_PFMT         8
#define CS_ICONF        9
#define CS_PFMT_ULAW_8K 0x20            /* u-law, mono, 24.576 MHz / 3072 */
#define CS_PEN          0x01

/* The buffers in RAM and their DVMA addresses through the IOMMU */
#define IOPTE_TABLE     0x200000ULL
#define BUF_RAM         0x100000ULL
#define BUF_DVMA        0xff000000U
#define BUF_LEN         800             /* 100 ms at 8000 Hz */
#define BUF2_OFFSET     0x1000          /* the second one on the next page */

#define CS_LEVEL        5               /* processor level, the "intr" */
#define CS_INTBIT       9               /* system bit: SBus level 3 */
#define OTHER_LEVELS    ((1u << 9) | (1u << 11))    /* not these */

/* Two u-law values and what they decode to (G.711) */
#define ULAW_POS        0xa0
#define ULAW_NEG        0x20
#define PCM_VALUE       7932

static void codec_write(QTestState *qts, uint8_t reg, uint8_t val)
{
    qtest_writeb(qts, CS_IAR, reg);
    qtest_writeb(qts, CS_IDR, val);
}

static bool level_pending(QTestState *qts, int level)
{
    return qtest_readl(qts, INTCTL_CPU0) & (1u << level);
}

/* Step the virtual clock 1 ms at a time until the CSR has `bits` */
static int64_t wait_csr(QTestState *qts, uint32_t bits, int64_t limit_ms)
{
    int64_t ms;

    for (ms = 0; ms < limit_ms; ms++) {
        if ((qtest_readl(qts, APC_CSR) & bits) == bits) {
            return ms;
        }
        qtest_clock_step(qts, 1000 * 1000);
    }
    g_assert_not_reached();
}

/*
 * The wav file is stereo: the mixer doubles a mono voice played into a
 * mono output (it adds left and right), so the test does not use one.
 */
static void check_wav(const char *path)
{
    g_autofree uint8_t *wav = NULL;
    gsize len, frames, i, start, sound;
    int16_t l, r;

    g_assert_true(g_file_get_contents(path, (gchar **)&wav, &len, NULL));
    g_assert_cmpuint(len, >, 44);
    g_assert_cmpmem(wav, 4, "RIFF", 4);
    g_assert_cmpuint(lduw_le_p(wav + 22), ==, 2);           /* channels */
    g_assert_cmpuint(ldl_le_p(wav + 24), ==, 8000);         /* Hz */
    g_assert_cmpuint(lduw_le_p(wav + 34), ==, 16);          /* bits */
    frames = (len - 44) / 4;

    /* Skip any silence before the sound starts */
    for (start = 0; start < frames; start++) {
        if (lduw_le_p(wav + 44 + 4 * start) != 0) {
            break;
        }
    }
    /* Both buffers, every sample, in order, and nothing after them */
    for (sound = 0; start + sound < frames; sound++) {
        if (lduw_le_p(wav + 44 + 4 * (start + sound)) == 0) {
            break;
        }
    }
    g_assert_cmpuint(sound, ==, 2 * BUF_LEN);
    for (i = 0; i < 2 * BUF_LEN; i++) {
        l = lduw_le_p(wav + 44 + 4 * (start + i));
        r = lduw_le_p(wav + 44 + 4 * (start + i) + 2);
        g_assert_cmpint(l, ==, r);
        g_assert_cmpint(abs(l), >=, PCM_VALUE - 2);
        g_assert_cmpint(abs(l), <=, PCM_VALUE + 2);
        g_assert_cmpint(l > 0, ==, i < BUF_LEN);
    }
}

static void test_playback(void)
{
    g_autofree char *wav = NULL;
    uint8_t buf[BUF_LEN];
    QTestState *qts;
    uint32_t csr;
    int64_t t1, t2;
    int fd;

    fd = g_file_open_tmp("sun4m-cs4231-XXXXXX.wav", &wav, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    qts = qtest_initf("-machine SS-5,audiodev=snd0 "
                      "-audiodev wav,id=snd0,path=%s,out.frequency=8000,"
                      "out.channels=2,out.format=s16", wav);

    /* Before anything is loaded: the pipe is empty, no interrupt */
    csr = qtest_readl(qts, APC_CSR);
    g_assert_cmphex(csr & (APC_PM | APC_PD | APC_IP), ==, APC_PM | APC_PD);

    /* Two pages mapped at BUF_DVMA, the second buffer on the second */
    qtest_writel(qts, IOPTE_TABLE, (BUF_RAM >> 4) | 0x2);
    qtest_writel(qts, IOPTE_TABLE + 4, ((BUF_RAM + BUF2_OFFSET) >> 4) | 0x2);
    qtest_writel(qts, IOMMU_BASE + 4, IOPTE_TABLE >> 4);
    qtest_writel(qts, IOMMU_BASE, 0x1);         /* 16 MB range, enabled */
    memset(buf, ULAW_POS, sizeof(buf));
    qtest_memwrite(qts, BUF_RAM, buf, sizeof(buf));
    memset(buf, ULAW_NEG, sizeof(buf));
    qtest_memwrite(qts, BUF_RAM + BUF2_OFFSET, buf, sizeof(buf));

    codec_write(qts, CS_PFMT, CS_PFMT_ULAW_8K);
    qtest_writel(qts, APC_PNVA, BUF_DVMA);
    qtest_writel(qts, APC_PNC, BUF_LEN);
    qtest_writel(qts, APC_CSR, APC_PI | APC_PMI | APC_ENABLES);

    /* Loaded, but the codec has not asked for samples: nothing taken */
    qtest_clock_step(qts, 50 * 1000 * 1000);
    csr = qtest_readl(qts, APC_CSR);
    g_assert_cmphex(csr & (APC_PI | APC_PD | APC_IP), ==, 0);
    g_assert_false(level_pending(qts, CS_LEVEL));

    /* PEN: the first buffer is taken at the next audio callback */
    codec_write(qts, CS_ICONF, CS_PEN);
    t1 = wait_csr(qts, APC_PI, 100);
    g_assert_cmpint(t1, <=, 20);
    csr = qtest_readl(qts, APC_CSR);
    g_assert_cmphex(csr & (APC_IP | APC_PD | APC_PM), ==, APC_IP | APC_PD);
    g_assert_cmphex(qtest_readl(qts, APC_PVA) & 0xfffff000, ==, BUF_DVMA);
    g_assert_cmpuint(qtest_readl(qts, APC_PC), <=, BUF_LEN);
    g_assert_true(level_pending(qts, CS_LEVEL));
    g_assert_true(qtest_readl(qts, INTCTL_SYS) & (1u << CS_INTBIT));
    g_assert_false(qtest_readl(qts, INTCTL_CPU0) & OTHER_LEVELS);

    /* The driver clears PI and loads the second buffer */
    qtest_writel(qts, APC_CSR, APC_PI | APC_ENABLES);
    g_assert_false(qtest_readl(qts, APC_CSR) & (APC_PI | APC_IP));
    g_assert_false(level_pending(qts, CS_LEVEL));
    qtest_writel(qts, APC_PNVA, BUF_DVMA + BUF2_OFFSET);
    qtest_writel(qts, APC_PNC, BUF_LEN);
    g_assert_false(qtest_readl(qts, APC_CSR) & APC_PD);

    /* It takes the second one when the first has played: about 100 ms */
    t2 = wait_csr(qts, APC_PI, 300);
    g_assert_cmpint(t2, >=, 80);
    g_assert_cmpint(t2, <=, 130);
    g_assert_cmphex(qtest_readl(qts, APC_PVA) & 0xfffff000, ==,
                    BUF_DVMA + BUF2_OFFSET);
    g_assert_true(qtest_readl(qts, APC_CSR) & APC_PD);
    g_assert_true(level_pending(qts, CS_LEVEL));
    qtest_writel(qts, APC_CSR, APC_PI | APC_ENABLES);

    /* No third buffer: the pipe runs empty when the second has played */
    t2 = wait_csr(qts, APC_PMI, 300);
    g_assert_cmpint(t2, >=, 80);
    g_assert_cmpint(t2, <=, 130);
    csr = qtest_readl(qts, APC_CSR);
    g_assert_cmphex(csr & (APC_IP | APC_PM | APC_PD), ==,
                    APC_IP | APC_PM | APC_PD);
    g_assert_cmpuint(qtest_readl(qts, APC_PC), ==, 0);
    g_assert_cmphex(qtest_readl(qts, APC_PVA), ==,
                    BUF_DVMA + BUF2_OFFSET + BUF_LEN);
    g_assert_true(level_pending(qts, CS_LEVEL));

    /* At once PMI cleared and the DMA off, as a driver may: quiet */
    qtest_writel(qts, APC_CSR, APC_PMI);
    g_assert_false(qtest_readl(qts, APC_CSR) & APC_IP);
    g_assert_false(level_pending(qts, CS_LEVEL));
    /* The backend still plays what was taken ahead; then the file */
    qtest_clock_step(qts, 300 * 1000 * 1000);
    qtest_quit(qts);
    check_wav(wav);
    unlink(wav);
}

/* DMA off drops the buffers, and the reset bit clears the APC */
static void test_stop_reset(void)
{
    QTestState *qts = qtest_init("-machine SS-5,audiodev=snd0 "
                                 "-audiodev none,id=snd0");

    qtest_writel(qts, IOPTE_TABLE, (BUF_RAM >> 4) | 0x2);
    qtest_writel(qts, IOMMU_BASE + 4, IOPTE_TABLE >> 4);
    qtest_writel(qts, IOMMU_BASE, 0x1);
    codec_write(qts, CS_PFMT, CS_PFMT_ULAW_8K);
    qtest_writel(qts, APC_PNVA, BUF_DVMA);
    qtest_writel(qts, APC_PNC, BUF_LEN);
    qtest_writel(qts, APC_CSR, APC_ENABLES);
    codec_write(qts, CS_ICONF, CS_PEN);
    wait_csr(qts, APC_PI, 100);
    g_assert_true(level_pending(qts, CS_LEVEL));

    /* DMA off: the buffer is dropped, the pipe empty */
    qtest_writel(qts, APC_CSR, APC_PI | (APC_ENABLES & ~APC_PDMA_GO));
    g_assert_cmpuint(qtest_readl(qts, APC_PC), ==, 0);
    g_assert_true(qtest_readl(qts, APC_CSR) & APC_PM);
    g_assert_false(level_pending(qts, CS_LEVEL));

    /* Reset: CSR back to the empty state, nothing pending */
    qtest_writel(qts, APC_PNC, BUF_LEN);
    qtest_writel(qts, APC_CSR, 0x1);
    g_assert_cmphex(qtest_readl(qts, APC_CSR) & ~0x1, ==,
                    APC_PM | APC_PD | 0x800 | 0x400);
    g_assert_cmpuint(qtest_readl(qts, APC_PC), ==, 0);
    qtest_quit(qts);
}

/*
 * A short sound, as SimCity's audiomixer writes them: one buffer of 40
 * bytes (5 ms). SunOS's interrupt handler looks at PMI before PI and acts
 * on PMI only after a PI has found nothing more to load, so the two must
 * come in two interrupts: PI first, PMI only after PI has been cleared
 * and the codec has played the buffer. Before the fix both were set in
 * the same audio callback.
 */
#define SHORT_LEN       40

static void test_short_buffer(void)
{
    QTestState *qts;
    uint8_t buf[SHORT_LEN];
    uint32_t csr;
    int64_t ms;

    qts = qtest_init("-machine SS-5,audiodev=snd0 "
                     "-audiodev none,id=snd0");
    qtest_writel(qts, IOPTE_TABLE, (BUF_RAM >> 4) | 0x2);
    qtest_writel(qts, IOMMU_BASE + 4, IOPTE_TABLE >> 4);
    qtest_writel(qts, IOMMU_BASE, 0x1);
    memset(buf, ULAW_POS, sizeof(buf));
    qtest_memwrite(qts, BUF_RAM, buf, sizeof(buf));

    codec_write(qts, CS_PFMT, CS_PFMT_ULAW_8K);
    qtest_writel(qts, APC_PNVA, BUF_DVMA);
    qtest_writel(qts, APC_PNC, SHORT_LEN);
    qtest_writel(qts, APC_CSR, APC_PI | APC_PMI | APC_ENABLES);
    codec_write(qts, CS_ICONF, CS_PEN);

    /* The first interrupt: PI alone */
    for (ms = 0; ms < 100; ms++) {
        csr = qtest_readl(qts, APC_CSR);
        if (csr & (APC_PI | APC_PMI)) {
            break;
        }
        qtest_clock_step(qts, 1000 * 1000);
    }
    g_assert_cmpint(ms, <, 100);
    g_assert_cmphex(csr & (APC_PI | APC_PMI), ==, APC_PI);

    /* While the driver has not cleared PI, no PMI, however long */
    qtest_clock_step(qts, 50 * 1000 * 1000);
    g_assert_false(qtest_readl(qts, APC_CSR) & APC_PMI);

    /*
     * PI cleared, nothing more loaded: the buffer has long been played,
     * so PMI comes at once, as an interrupt of its own
     */
    qtest_writel(qts, APC_CSR, APC_PI | APC_ENABLES);
    g_assert_cmpint(wait_csr(qts, APC_PMI, 20), ==, 0);
    csr = qtest_readl(qts, APC_CSR);
    g_assert_cmphex(csr & (APC_IP | APC_PI | APC_PM), ==, APC_IP | APC_PM);
    g_assert_true(level_pending(qts, CS_LEVEL));
    qtest_quit(qts);
}

/*
 * A long buffer and then a short one: the short one's PI comes when the
 * codec gets to it, and its PMI not before the codec has played it.
 */
static void test_short_tail(void)
{
    QTestState *qts;
    uint8_t buf[BUF_LEN];
    int64_t t_pi, t_pmi;

    qts = qtest_init("-machine SS-5,audiodev=snd0 "
                     "-audiodev none,id=snd0");
    qtest_writel(qts, IOPTE_TABLE, (BUF_RAM >> 4) | 0x2);
    qtest_writel(qts, IOPTE_TABLE + 4, ((BUF_RAM + BUF2_OFFSET) >> 4) | 0x2);
    qtest_writel(qts, IOMMU_BASE + 4, IOPTE_TABLE >> 4);
    qtest_writel(qts, IOMMU_BASE, 0x1);
    memset(buf, ULAW_POS, sizeof(buf));
    qtest_memwrite(qts, BUF_RAM, buf, sizeof(buf));
    qtest_memwrite(qts, BUF_RAM + BUF2_OFFSET, buf, SHORT_LEN);

    codec_write(qts, CS_PFMT, CS_PFMT_ULAW_8K);
    qtest_writel(qts, APC_PNVA, BUF_DVMA);
    qtest_writel(qts, APC_PNC, BUF_LEN);
    qtest_writel(qts, APC_CSR, APC_PI | APC_PMI | APC_ENABLES);
    codec_write(qts, CS_ICONF, CS_PEN);
    wait_csr(qts, APC_PI, 100);
    qtest_writel(qts, APC_CSR, APC_PI | APC_ENABLES);
    qtest_writel(qts, APC_PNVA, BUF_DVMA + BUF2_OFFSET);
    qtest_writel(qts, APC_PNC, SHORT_LEN);

    /* PI for the 5 ms buffer when the 100 ms one has played, PMI apart */
    t_pi = wait_csr(qts, APC_PI, 300);
    g_assert_cmpint(t_pi, >=, 80);
    g_assert_false(qtest_readl(qts, APC_CSR) & APC_PMI);
    qtest_writel(qts, APC_CSR, APC_PI | APC_ENABLES);
    t_pmi = wait_csr(qts, APC_PMI, 100);
    g_assert_cmpint(t_pmi, >=, 3);
    g_assert_cmpint(t_pmi, <=, 20);
    qtest_quit(qts);
}

/*
 * A driver that loads the next buffer as soon as the next registers are
 * free (PD), without waiting for the PI: 800 bytes, then 40, then 40 more
 * when PD shows. Each buffer taken must give a PI of its own, so PD must
 * not show before the PI of the buffer that was taken.
 */
static void test_load_on_pd(void)
{
    QTestState *qts;
    uint8_t buf[BUF_LEN];
    uint32_t csr;
    int64_t ms;
    int npi = 0, npmi = 0;
    bool loaded = false;

    qts = qtest_init("-machine SS-5,audiodev=snd0 "
                     "-audiodev none,id=snd0");
    qtest_writel(qts, IOPTE_TABLE, (BUF_RAM >> 4) | 0x2);
    qtest_writel(qts, IOPTE_TABLE + 4, ((BUF_RAM + BUF2_OFFSET) >> 4) | 0x2);
    qtest_writel(qts, IOMMU_BASE + 4, IOPTE_TABLE >> 4);
    qtest_writel(qts, IOMMU_BASE, 0x1);
    memset(buf, ULAW_POS, sizeof(buf));
    qtest_memwrite(qts, BUF_RAM, buf, sizeof(buf));
    qtest_memwrite(qts, BUF_RAM + BUF2_OFFSET, buf, SHORT_LEN);

    codec_write(qts, CS_PFMT, CS_PFMT_ULAW_8K);
    qtest_writel(qts, APC_PNVA, BUF_DVMA);
    qtest_writel(qts, APC_PNC, BUF_LEN);
    qtest_writel(qts, APC_CSR, APC_PI | APC_PMI | APC_ENABLES);
    codec_write(qts, CS_ICONF, CS_PEN);
    wait_csr(qts, APC_PI, 100);
    npi++;
    qtest_writel(qts, APC_CSR, APC_PI | APC_ENABLES);
    qtest_writel(qts, APC_PNVA, BUF_DVMA + BUF2_OFFSET);
    qtest_writel(qts, APC_PNC, SHORT_LEN);

    for (ms = 0; ms < 400; ms++) {
        csr = qtest_readl(qts, APC_CSR);
        if (!loaded && (csr & APC_PD) && !(csr & APC_PI)) {
            /* the same 40 bytes once more */
            qtest_writel(qts, APC_PNVA, BUF_DVMA + BUF2_OFFSET);
            qtest_writel(qts, APC_PNC, SHORT_LEN);
            loaded = true;
        }
        if (csr & APC_PI) {
            npi++;
            qtest_writel(qts, APC_CSR, APC_PI | APC_ENABLES);
        }
        if (csr & APC_PMI) {
            npmi++;
            qtest_writel(qts, APC_CSR, APC_PMI | APC_ENABLES);
        }
        qtest_clock_step(qts, 1000 * 1000);
    }
    g_assert_true(loaded);
    g_assert_cmpint(npi, ==, 3);
    g_assert_cmpint(npmi, ==, 1);
    qtest_quit(qts);
}

/*
 * A stream of short buffers, each loaded when the PI for the one before
 * comes: 20 times 40 bytes are 100 ms of sound and must play in that
 * time, one after the other. Taken only at the audio callbacks, every
 * 10 ms, each 5 ms buffer was followed by 5 ms of nothing.
 */
#define CHAIN_BUFS      20

static void test_short_chain(void)
{
    QTestState *qts;
    uint8_t buf[SHORT_LEN];
    uint32_t csr;
    int64_t ms;
    int loaded = 1;

    qts = qtest_init("-machine SS-5,audiodev=snd0 "
                     "-audiodev none,id=snd0");
    qtest_writel(qts, IOPTE_TABLE, (BUF_RAM >> 4) | 0x2);
    qtest_writel(qts, IOMMU_BASE + 4, IOPTE_TABLE >> 4);
    qtest_writel(qts, IOMMU_BASE, 0x1);
    memset(buf, ULAW_POS, sizeof(buf));
    qtest_memwrite(qts, BUF_RAM, buf, sizeof(buf));

    codec_write(qts, CS_PFMT, CS_PFMT_ULAW_8K);
    qtest_writel(qts, APC_PNVA, BUF_DVMA);
    qtest_writel(qts, APC_PNC, SHORT_LEN);
    qtest_writel(qts, APC_CSR, APC_PI | APC_PMI | APC_ENABLES);
    codec_write(qts, CS_ICONF, CS_PEN);
    wait_csr(qts, APC_PI, 100);

    for (ms = 0; ms < 400; ms++) {
        csr = qtest_readl(qts, APC_CSR);
        if (csr & APC_PMI) {
            break;
        }
        if (csr & APC_PI) {
            /* never with the PMI: that is short-buffer's case */
            qtest_writel(qts, APC_CSR, APC_PI | APC_ENABLES);
            if (loaded < CHAIN_BUFS) {
                qtest_writel(qts, APC_PNVA, BUF_DVMA);
                qtest_writel(qts, APC_PNC, SHORT_LEN);
                loaded++;
            }
        }
        qtest_clock_step(qts, 1000 * 1000);
    }
    g_assert_cmpint(loaded, ==, CHAIN_BUFS);
    g_assert_cmpint(ms, >=, 95);
    g_assert_cmpint(ms, <=, 125);
    qtest_quit(qts);
}

static void migrate_to_file(QTestState *qts, const char *uri)
{
    QDict *rsp;
    const char *status;
    int i;

    qtest_qmp_assert_success(qts, "{'execute': 'migrate',"
                             " 'arguments': {'uri': %s}}", uri);
    for (i = 0; i < 3000; i++) {
        rsp = qtest_qmp_assert_success_ref(qts,
                                           "{'execute': 'query-migrate'}");
        status = qdict_haskey(rsp, "status") ? qdict_get_str(rsp, "status")
                                             : "";
        if (!strcmp(status, "completed")) {
            qobject_unref(rsp);
            return;
        }
        g_assert_cmpstr(status, !=, "failed");
        qobject_unref(rsp);
        g_usleep(10 * 1000);
    }
    g_assert_not_reached();
}

/*
 * The destination loads the file and resumes before our QMP client has
 * connected, so its RESUME event is lost: poll the status instead.
 */
static void wait_running(QTestState *qts)
{
    QDict *rsp;
    bool running;
    int i;

    for (i = 0; i < 1000; i++) {
        rsp = qtest_qmp_assert_success_ref(qts,
                                           "{'execute': 'query-status'}");
        running = !strcmp(qdict_get_str(rsp, "status"), "running");
        qobject_unref(rsp);
        if (running) {
            return;
        }
        g_usleep(10 * 1000);
    }
    g_error("destination not running 10 s after -incoming");
}

/*
 * A migration in the middle of a short sound: the 40 bytes have been
 * taken and PI is set, the driver has not cleared it yet, and PMI waits
 * behind it. On the other side the PI is cleared, and PMI must still
 * come: SunOS's AUDIO_DRAIN sleeps until it does.
 */
static void test_migrate(void)
{
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *src, *dst;
    uint8_t buf[SHORT_LEN];
    uint32_t csr;
    int fd;

    fd = g_file_open_tmp("sun4m-cs4231-XXXXXX.mig", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    uri = g_strdup_printf("file:%s", path);

    src = qtest_init("-machine SS-5,audiodev=snd0 "
                     "-audiodev none,id=snd0");
    qtest_writel(src, IOPTE_TABLE, (BUF_RAM >> 4) | 0x2);
    qtest_writel(src, IOMMU_BASE + 4, IOPTE_TABLE >> 4);
    qtest_writel(src, IOMMU_BASE, 0x1);
    memset(buf, ULAW_POS, sizeof(buf));
    qtest_memwrite(src, BUF_RAM, buf, sizeof(buf));
    codec_write(src, CS_PFMT, CS_PFMT_ULAW_8K);
    qtest_writel(src, APC_PNVA, BUF_DVMA);
    qtest_writel(src, APC_PNC, SHORT_LEN);
    qtest_writel(src, APC_CSR, APC_PI | APC_PMI | APC_ENABLES);
    codec_write(src, CS_ICONF, CS_PEN);
    wait_csr(src, APC_PI, 100);
    csr = qtest_readl(src, APC_CSR);
    g_assert_cmphex(csr & (APC_PI | APC_PMI), ==, APC_PI);
    migrate_to_file(src, uri);
    qtest_quit(src);

    dst = qtest_initf("-machine SS-5,audiodev=snd0 -audiodev none,id=snd0 "
                      "-incoming %s", uri);
    wait_running(dst);
    csr = qtest_readl(dst, APC_CSR);
    g_assert_cmphex(csr & (APC_PI | APC_PMI), ==, APC_PI);
    g_assert_true(level_pending(dst, CS_LEVEL));

    /* The driver clears the PI; the buffer plays out, then PMI */
    qtest_writel(dst, APC_CSR, APC_PI | APC_ENABLES);
    g_assert_cmpint(wait_csr(dst, APC_PMI, 100), <=, 20);
    g_assert_true(level_pending(dst, CS_LEVEL));
    qtest_quit(dst);
    unlink(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("sun4m-cs4231/playback", test_playback);
    qtest_add_func("sun4m-cs4231/stop-reset", test_stop_reset);
    qtest_add_func("sun4m-cs4231/short-buffer", test_short_buffer);
    qtest_add_func("sun4m-cs4231/short-tail", test_short_tail);
    qtest_add_func("sun4m-cs4231/load-on-pd", test_load_on_pd);
    qtest_add_func("sun4m-cs4231/short-chain", test_short_chain);
    qtest_add_func("sun4m-cs4231/migrate", test_migrate);

    return g_test_run();
}
