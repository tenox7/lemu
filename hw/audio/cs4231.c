/*
 * QEMU Crystal CS4231 audio chip emulation
 *
 * Copyright (c) 2006 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * The SPARCstation 5 has the CS4231 codec behind Sun's APC DMA engine.
 * The first 16 bytes are the codec's own registers, the rest the APC's:
 *
 *   0x00 codec index      0x04 codec data       0x08 codec status
 *   0x0c codec PIO data   0x10 APC CSR
 *   0x20 capture VA       0x24 capture count    0x28 next VA   0x2c next count
 *   0x30 playback VA      0x34 playback count   0x38 next VA   0x3c next count
 *
 * Playback: the driver writes the next address and then the next count.
 * When the codec first asks for samples, and again each time the current
 * buffer is done, the APC moves the next buffer into the current
 * registers, sets PD (the next registers are free again) and raises PI.
 * When a buffer ends and no next one is loaded the pipe is
 * empty: PM and PMI. The interrupt bits are cleared by writing 1s. The
 * addresses go through the IOMMU, and the samples are played into the
 * audiodev at the codec's rate, so a driver that waits for the end of its
 * sound (SunOS AUDIO_DRAIN) gets it in real time. The buffers are taken
 * at that rate too, at most 50 ms ahead: the real APC has only a small
 * FIFO, and SunOS loads the next buffer only after PI, so taking a whole
 * buffer at once would leave the pipe empty.
 *
 * Because the samples are taken that far ahead, the voice stays on for
 * CS_DRAIN_NS after playback stops, at the end of the last buffer or when
 * the driver turns the DMA off, so that the audiodev still plays what was
 * taken: turning it off at once drops the last 50 ms of every sound.
 *
 * The interrupts are not raised when the samples are taken but when the
 * codec clock gets to that point, as on the real chip: PI when the
 * codec starts on the next buffer, PMI when it has played the last one.
 * PMI also waits until the driver has cleared the PI before it. SunOS
 * 4.1.4's audio_4231_cintr looks at PMI before PI and acts on PMI only
 * after a PI has found no next buffer to load; when both come in one
 * interrupt it drops the PMI, the last buffers are never marked played,
 * and a process in AUDIO_DRAIN (SimCity's audiomixer) sleeps for ever.
 * Taking a short buffer ahead would raise both at once.
 *
 * Capture is not done: its registers keep what is written, and the
 * capture pipe always reads as empty.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/sparc/sun4m_iommu.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/audio.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "trace.h"
#include "qom/object.h"

#define CS_SIZE 0x40
#define CS_DRAIN_NS (200 * SCALE_MS)   /* well past the 50 ms taken ahead */
#define CS_REGS 16
#define CS_DREGS 32
#define CS_MAXDREG (CS_DREGS - 1)

/* Registers, as offset / 4 */
#define CS_IAR      0       /* codec index */
#define CS_IDR      1       /* codec data */
#define CS_STATUS   2       /* codec status */
#define CS_CSR      4       /* APC control and status */
#define CS_PVA      12      /* playback address */
#define CS_PC       13      /* playback count */
#define CS_PNVA     14      /* next playback address */
#define CS_PNC      15      /* next playback count */

/* APC CSR */
#define APC_IP      0x00800000  /* an interrupt is pending (read only) */
#define APC_PI      0x00400000  /* playback took the next buffer */
#define APC_CI      0x00200000  /* capture took the next buffer */
#define APC_EI      0x00100000  /* error */
#define APC_IE      0x00080000  /* interrupt enable */
#define APC_PIE     0x00040000  /* PI enable */
#define APC_CIE     0x00020000  /* CI enable */
#define APC_EIE     0x00010000  /* EI enable */
#define APC_PMI     0x00008000  /* playback pipe went empty */
#define APC_PM      0x00004000  /* playback pipe is empty (read only) */
#define APC_PD      0x00002000  /* next playback registers free (read only) */
#define APC_PMIE    0x00001000  /* PMI enable */
#define APC_CM      0x00000800  /* capture pipe is empty (read only) */
#define APC_CD      0x00000400  /* next capture registers free (read only) */
#define APC_CMI     0x00000200  /* capture pipe went empty */
#define APC_CMIE    0x00000100  /* CMI enable */
#define APC_PPAUSE  0x00000080  /* pause playback */
#define APC_CPAUSE  0x00000040  /* pause capture */
#define APC_PDMA_GO 0x00000008  /* playback DMA on */
#define APC_CDMA_GO 0x00000004  /* capture DMA on */
#define APC_RESET   0x00000001

#define APC_INTS    (APC_PI | APC_CI | APC_EI | APC_PMI | APC_CMI)
#define APC_RW      (APC_IE | APC_PIE | APC_CIE | APC_EIE | APC_PMIE | \
                     APC_CMIE | 0xff)

/* Codec indirect registers */
#define CS_LDAC     6       /* left DAC output: mute, attenuation */
#define CS_RDAC     7       /* right DAC output */
#define CS_PFMT     8       /* playback format and rate */
#define CS_ICONF    9       /* interface configuration */
#define CS_MODE     12      /* mode and ID */
#define CS_DAC_MUTE 0x80
#define CS_PEN      0x01    /* playback on (in CS_ICONF) */
#define CS_MODE2    0x40

/* Playback formats, bits 7-5 of CS_PFMT */
#define CS_FMT_U8       0
#define CS_FMT_ULAW     1
#define CS_FMT_S16LE    2
#define CS_FMT_ALAW     3
#define CS_FMT_ADPCM    5
#define CS_FMT_S16BE    6

#define TYPE_CS4231 "sun-CS4231"
typedef struct CSState CSState;
DECLARE_INSTANCE_CHECKER(CSState, CS4231,
                         TYPE_CS4231)

struct CSState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    IOMMUState *iommu;
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    uint32_t regs[CS_REGS];
    uint8_t dregs[CS_DREGS];
    bool play_next;         /* the next playback registers are loaded */
    int play_fmt;           /* from CS_PFMT */
    int play_channels;
    int play_freq;
    bool play_running;      /* since play_start, without a gap */
    int64_t play_start;     /* QEMU_CLOCK_VIRTUAL ns */
    uint64_t play_units;    /* taken since play_start */
    int64_t play_drain;     /* the voice stays on until then */
    int64_t play_end;       /* what was taken has been played by then */
    int64_t pi_at;          /* PI is due then, or -1 */
    int64_t pmi_at;         /* PMI is due then, or -1 */
    QEMUTimer *irq_timer;   /* raises them when they are due */
};

#define CS_RAP(s) ((s)->regs[CS_IAR] & CS_MAXDREG)
#define CS_VER 0xa0
#define CS_CDC_VER 0x8a

static AddressSpace *cs_dma_as(CSState *s)
{
    return s->iommu ? &s->iommu->iommu_as : &address_space_memory;
}

static bool cs_play_go(CSState *s)
{
    return (s->regs[CS_CSR] & (APC_PDMA_GO | APC_PPAUSE)) == APC_PDMA_GO;
}

/*
 * The APC fetches only when the codec asks for samples, so it takes no
 * buffer before playback is on in the codec too. SunOS and the BSDs load
 * the first buffer, then write the CSR with PI set to clear old
 * interrupts, and only then set PEN: a buffer taken before that write
 * would lose its PI, and the driver would never load the next one.
 *
 * The codec asks a moment after PEN, here at the next audio callback,
 * not in the register write. SunOS queues the rest of a write in that
 * moment. If the first PI comes before that, its driver thinks the
 * sound has ended, and it plays the first buffer again with the rest.
 */
static bool cs_play_fetch(CSState *s)
{
    return cs_play_go(s) && (s->dregs[CS_ICONF] & CS_PEN);
}

/* There is a buffer to play, or one to take. */
static bool cs_play_active(CSState *s)
{
    return cs_play_fetch(s) && (s->regs[CS_PC] != 0 || s->play_next);
}

static uint32_t cs_read_csr(CSState *s)
{
    uint32_t csr = s->regs[CS_CSR];

    if (csr & APC_INTS) {
        csr |= APC_IP;
    }
    /*
     * The next registers are free when the codec gets to the buffer
     * taken from them, with its PI: a driver that loads on PD would
     * otherwise load one more before that PI, and lose a PI.
     */
    if (!s->play_next && s->pi_at < 0) {
        csr |= APC_PD;
    }
    if (!s->play_next) {
        if (s->regs[CS_PC] == 0) {
            csr |= APC_PM;
        }
    }
    if (!cs_play_go(s)) {
        csr |= APC_PM;
    }
    return csr | APC_CM | APC_CD;
}

static void cs_update_irq(CSState *s)
{
    uint32_t csr = s->regs[CS_CSR];
    bool level = (csr & APC_IE) &&
                 (((csr & APC_PI) && (csr & APC_PIE)) ||
                  ((csr & APC_PMI) && (csr & APC_PMIE)) ||
                  ((csr & APC_CI) && (csr & APC_CIE)) ||
                  ((csr & APC_CMI) && (csr & APC_CMIE)) ||
                  ((csr & APC_EI) && (csr & APC_EIE)));

    qemu_set_irq(s->irq, level);
}

/* units a second: one sample of each channel, two for ADPCM */
static uint64_t cs_play_rate(CSState *s)
{
    return s->play_freq / (s->play_fmt == CS_FMT_ADPCM ? 2 : 1);
}

/* Raise the PI and PMI that are due, and set the timer for the others */
static void cs_play_irqs(CSState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t next = INT64_MAX;
    uint32_t csr = s->regs[CS_CSR];
    bool pi_pending;

    /* paused: nothing moves on until the driver lets it go again */
    if (cs_play_go(s)) {
        if (s->pi_at >= 0) {
            if (now >= s->pi_at) {
                s->regs[CS_CSR] |= APC_PI;
                s->pi_at = -1;
            } else {
                next = s->pi_at;
            }
        }
        /* PMI after the driver has taken the PI before it */
        pi_pending = s->pi_at >= 0 ||
                     ((s->regs[CS_CSR] & APC_PI) &&
                      (csr & APC_IE) && (csr & APC_PIE));
        /* a buffer loaded in time keeps the pipe from running empty */
        if (s->pmi_at >= 0 && !pi_pending && !s->play_next) {
            if (now >= s->pmi_at) {
                s->regs[CS_CSR] |= APC_PMI;
                s->pmi_at = -1;
                trace_cs4231_play_empty();
            } else {
                next = MIN(next, s->pmi_at);
            }
        }
    }
    if (next != INT64_MAX) {
        timer_mod(s->irq_timer, next);
    } else {
        timer_del(s->irq_timer);
    }
    cs_update_irq(s);
}

static void cs_irq_timer(void *opaque)
{
    cs_play_irqs(opaque);
}

/*
 * The current playback buffer is done: take the next one, if any. The
 * codec gets to this point when it has played what was taken before.
 */
static void cs_play_next(CSState *s)
{
    int64_t at = MAX(s->play_end, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));

    if (s->play_next) {
        s->regs[CS_PVA] = s->regs[CS_PNVA];
        s->regs[CS_PC] = s->regs[CS_PNC];
        s->play_next = false;
        /* loaded before the pipe ran empty: no PMI, the PI then */
        s->pmi_at = -1;
        if (s->pi_at < 0) {
            s->pi_at = at;
        }
        trace_cs4231_play_next(s->regs[CS_PVA], s->regs[CS_PC]);
    } else {
        s->pmi_at = at;
    }
    cs_play_irqs(s);
}

/* Playback stopped: let the audiodev play what was taken ahead */
static void cs_play_stopped(CSState *s, int64_t now)
{
    s->play_running = false;
    s->play_drain = now + CS_DRAIN_NS;
}

static void cs_play_update(CSState *s)
{
    bool run = cs_play_active(s);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (run && !s->play_running) {
        /* the codec goes on after what it still has to play */
        s->play_start = MAX(now, s->play_end);
        s->play_end = s->play_start;
        s->play_units = 0;
    } else if (!run && s->play_running) {
        cs_play_stopped(s, now);
    }
    s->play_running = run;
    if (s->voice) {
        audio_be_set_active_out(s->audio_be, s->voice,
                                run || now < s->play_drain);
    }
    cs_update_irq(s);
}

/* G.711 decoders, after Sun's public domain g711.c */
static int16_t cs_ulaw(uint8_t u)
{
    int t;

    u = ~u;
    t = (((u & 0x0f) << 3) + 0x84) << ((u & 0x70) >> 4);
    return (u & 0x80) ? 0x84 - t : t - 0x84;
}

static int16_t cs_alaw(uint8_t a)
{
    int t, seg;

    a ^= 0x55;
    t = (a & 0x0f) << 4;
    seg = (a & 0x70) >> 4;
    if (seg == 0) {
        t += 8;
    } else {
        t = (t + 0x108) << (seg - 1);
    }
    return (a & 0x80) ? t : -t;
}

/*
 * Guest bytes per unit and host bytes per unit: a unit is one sample of
 * each channel, or two of them for 4-bit ADPCM. The host side is always
 * 16-bit signed.
 */
static void cs_play_units(CSState *s, uint32_t *in, uint32_t *out)
{
    uint32_t ch = s->play_channels;

    switch (s->play_fmt) {
    case CS_FMT_S16LE:
    case CS_FMT_S16BE:
        *in = 2 * ch;
        *out = 2 * ch;
        break;
    case CS_FMT_ADPCM:
        *in = ch;
        *out = 4 * ch;
        break;
    default:
        *in = ch;
        *out = 2 * ch;
        break;
    }
}

static void cs_convert(CSState *s, const uint8_t *in, int16_t *out,
                       uint32_t units)
{
    uint32_t i, n = units * s->play_channels;

    switch (s->play_fmt) {
    case CS_FMT_U8:
        for (i = 0; i < n; i++) {
            out[i] = (in[i] - 128) * 256;
        }
        break;
    case CS_FMT_ULAW:
        for (i = 0; i < n; i++) {
            out[i] = cs_ulaw(in[i]);
        }
        break;
    case CS_FMT_ALAW:
        for (i = 0; i < n; i++) {
            out[i] = cs_alaw(in[i]);
        }
        break;
    case CS_FMT_S16LE:
        for (i = 0; i < n; i++) {
            out[i] = lduw_le_p(in + 2 * i);
        }
        break;
    case CS_FMT_S16BE:
        for (i = 0; i < n; i++) {
            out[i] = lduw_be_p(in + 2 * i);
        }
        break;
    default:
        /* ADPCM and the reserved formats: silence at the right rate */
        memset(out, 0, units * 4 * s->play_channels);
        break;
    }
}

/* Feed the audiodev, which has room for room bytes, from the DMA buffers. */
static void cs_play_feed(CSState *s, uint32_t room, int64_t now)
{
    uint8_t in[1024];
    int16_t out[2048];
    uint32_t unit_in, unit_out;
    uint64_t rate, due, budget;
    int64_t ahead;

    /* the codec starts when it asks for its first samples: now */
    if (s->play_units == 0 && now > s->play_start) {
        s->play_start = now;
        s->play_end = now;
    }
    /* up to 50 ms ahead of the codec, which may start in the future */
    rate = cs_play_rate(s);
    ahead = now - s->play_start + NANOSECONDS_PER_SECOND / 20;
    due = ahead > 0 ? muldiv64(ahead, rate, NANOSECONDS_PER_SECOND) : 0;
    budget = due > s->play_units ? due - s->play_units : 0;

    cs_play_units(s, &unit_in, &unit_out);
    while (cs_play_fetch(s)) {
        uint32_t pc = s->regs[CS_PC];
        uint32_t units;

        if (pc == 0) {
            /* the first buffer, or one loaded after the pipe ran empty */
            if (!s->play_next) {
                break;
            }
            cs_play_next(s);
            continue;
        }
        if (pc < unit_in) {
            /* a partial sample at the end of a buffer: skip it */
            s->regs[CS_PVA] += pc;
            s->regs[CS_PC] = 0;
        } else {
            units = MIN(pc / unit_in, room / unit_out);
            units = MIN(units, sizeof(in) / unit_in);
            units = MIN(units, sizeof(out) / unit_out);
            units = MIN(units, budget);
            if (units == 0) {
                break;
            }
            dma_memory_read(cs_dma_as(s), s->regs[CS_PVA], in,
                            units * unit_in, MEMTXATTRS_UNSPECIFIED);
            cs_convert(s, in, out, units);
            units = audio_be_write(s->audio_be, s->voice, out,
                                   units * unit_out) / unit_out;
            if (units == 0) {
                break;
            }
            s->regs[CS_PVA] += units * unit_in;
            s->regs[CS_PC] -= units * unit_in;
            room -= units * unit_out;
            budget -= units;
            s->play_units += units;
            s->play_end = s->play_start +
                muldiv64(s->play_units, NANOSECONDS_PER_SECOND, rate);
        }
        if (s->regs[CS_PC] == 0) {
            cs_play_next(s);
        }
    }
    /* after a gap the clock starts again with the next buffer */
    if (!cs_play_active(s)) {
        cs_play_stopped(s, now);
    }
}

/* The audiodev has room for avail bytes */
static void cs_audio_callback(void *opaque, int avail)
{
    CSState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!s->play_running) {
        /* stopped: the voice is on only to play out the last samples */
        if (now >= s->play_drain) {
            audio_be_set_active_out(s->audio_be, s->voice, false);
        }
        return;
    }
    cs_play_feed(s, MAX(avail, 0), now);
}

/* DAC attenuation is 1.5 dB a step; the mixer wants 0-255 */
static uint8_t cs_dac_level(uint8_t reg)
{
    uint32_t i, v = 255 << 8;

    if (reg & CS_DAC_MUTE) {
        return 0;
    }
    for (i = 0; i < (reg & 0x3f); i++) {
        v = v * 215 / 256;
    }
    return v >> 8;
}

static void cs_set_volume(CSState *s)
{
    uint8_t l = s->dregs[CS_LDAC], r = s->dregs[CS_RDAC];

    if (s->voice) {
        audio_be_set_volume_out_lr(s->audio_be, s->voice,
                                   (l & r & CS_DAC_MUTE) != 0,
                                   cs_dac_level(l), cs_dac_level(r));
    }
}

/* (Re)open the voice for the codec's playback format and rate */
static void cs_play_setup(CSState *s)
{
    static const int divide[8] = { 3072, 1536, 896, 768, 448, 384, 512, 2560 };
    uint8_t val = s->dregs[CS_PFMT];
    struct audsettings as;

    as.freq = ((val & 1) ? 16934400 : 24576000) / divide[(val >> 1) & 7];
    as.nchannels = (val & 0x10) ? 2 : 1;
    as.fmt = AUDIO_FORMAT_S16;
    as.big_endian = HOST_BIG_ENDIAN;
    s->play_channels = as.nchannels;
    s->play_freq = as.freq;
    s->play_fmt = (val >> 5) & ((s->dregs[CS_MODE] & CS_MODE2) ? 7 : 3);
    trace_cs4231_play_format(as.freq, as.nchannels, s->play_fmt);
    if (!s->audio_be) {
        return;
    }
    s->voice = audio_be_open_out(s->audio_be, s->voice, "cs4231", s,
                                 cs_audio_callback, &as);
    cs_set_volume(s);
}

static void cs_reset(DeviceState *d)
{
    CSState *s = CS4231(d);

    memset(s->regs, 0, CS_REGS * 4);
    memset(s->dregs, 0, CS_DREGS);
    s->dregs[12] = CS_CDC_VER;
    s->dregs[25] = CS_VER;
    s->play_next = false;
    s->play_running = false;
    s->play_drain = 0;
    s->play_end = 0;
    s->pi_at = -1;
    s->pmi_at = -1;
    timer_del(s->irq_timer);
    cs_play_setup(s);
    cs_play_update(s);
}

static uint64_t cs_mem_read(void *opaque, hwaddr addr,
                            unsigned size)
{
    CSState *s = opaque;
    uint32_t saddr, ret;

    saddr = addr >> 2;
    switch (saddr) {
    case CS_IDR:
        switch (CS_RAP(s)) {
        case 3: // Write only
            ret = 0;
            break;
        default:
            ret = s->dregs[CS_RAP(s)];
            break;
        }
        trace_cs4231_mem_readl_dreg(CS_RAP(s), ret);
        break;
    case CS_CSR:
        ret = cs_read_csr(s);
        trace_cs4231_mem_readl_reg(saddr, ret);
        break;
    default:
        ret = s->regs[saddr];
        trace_cs4231_mem_readl_reg(saddr, ret);
        break;
    }
    return ret;
}

static void cs_mem_write(void *opaque, hwaddr addr,
                         uint64_t val, unsigned size)
{
    CSState *s = opaque;
    uint32_t saddr, csr;
    bool was_go, feed;
    int64_t now;

    saddr = addr >> 2;
    trace_cs4231_mem_writel_reg(saddr, s->regs[saddr], val);
    switch (saddr) {
    case CS_IDR:
        trace_cs4231_mem_writel_dreg(CS_RAP(s), s->dregs[CS_RAP(s)], val);
        switch(CS_RAP(s)) {
        case 11:
        case 25: // Read only
            break;
        case 12:
            val &= 0x40;
            val |= CS_CDC_VER; // Codec version
            s->dregs[CS_RAP(s)] = val;
            cs_play_setup(s);
            break;
        case CS_LDAC:
        case CS_RDAC:
            s->dregs[CS_RAP(s)] = val;
            cs_set_volume(s);
            break;
        case CS_PFMT:
            s->dregs[CS_RAP(s)] = val;
            cs_play_setup(s);
            cs_play_update(s);
            break;
        case CS_ICONF:
            s->dregs[CS_RAP(s)] = val;
            cs_play_update(s);
            break;
        default:
            s->dregs[CS_RAP(s)] = val;
            break;
        }
        break;
    case CS_STATUS: // Read only
        break;
    case CS_CSR:
        csr = s->regs[CS_CSR];
        was_go = csr & APC_PDMA_GO;
        if (val & APC_RESET) {
            cs_reset(DEVICE(s));
            csr = 0;
            was_go = false;
        }
        /* 1s clear the interrupt bits; the read only bits are not kept */
        csr = (csr & APC_INTS & ~val) | (val & APC_RW);
        s->regs[CS_CSR] = csr;
        if (was_go && !(csr & APC_PDMA_GO)) {
            /* playback DMA off: drop what it still had */
            s->regs[CS_PC] = 0;
            s->play_next = false;
            s->pi_at = -1;
            s->pmi_at = -1;
        }
        cs_play_update(s);
        /* a PMI may have waited for this PI to be cleared */
        cs_play_irqs(s);
        break;
    case CS_PC:
        s->regs[saddr] = val;
        cs_play_update(s);
        break;
    case CS_PNC:
        /* the count loads the next buffer; a count of 0 unloads it */
        s->regs[saddr] = val;
        s->play_next = val != 0;
        now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        feed = s->play_next && s->regs[CS_PC] == 0 && now < s->play_end;
        cs_play_update(s);
        /*
         * Loaded while the codec still plays the end of the last buffer:
         * take it now, for the codec to go on without a gap. At the next
         * audio callback a buffer shorter than the callback's period
         * would be late every time. The PI comes when the codec gets to
         * it. After the pipe ran dry the codec asks again at the next
         * callback, as after PEN.
         */
        if (feed && s->voice && s->play_running) {
            cs_play_feed(s, UINT32_MAX, now);
        }
        /* a PMI still due waits while a next buffer is loaded */
        cs_play_irqs(s);
        break;
    default:
        s->regs[saddr] = val;
        break;
    }
}

static const MemoryRegionOps cs_mem_ops = {
    .read = cs_mem_read,
    .write = cs_mem_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* without the subsection nothing is due */
static int cs_pre_load(void *opaque)
{
    CSState *s = opaque;

    s->play_end = 0;
    s->pi_at = -1;
    s->pmi_at = -1;
    return 0;
}

static int cs_post_load(void *opaque, int version_id)
{
    CSState *s = opaque;

    s->play_running = false;
    s->play_drain = 0;
    cs_play_setup(s);
    cs_play_update(s);
    /* a PI or PMI still due: set the timer again */
    cs_play_irqs(s);
    return 0;
}

/* A PI or PMI still due must come after a migration, or AUDIO_DRAIN hangs */
static bool cs_irqs_needed(void *opaque)
{
    CSState *s = opaque;

    return s->pi_at >= 0 || s->pmi_at >= 0;
}

static const VMStateDescription vmstate_cs4231_irqs = {
    .name = "cs4231/irqs",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = cs_irqs_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(play_end, CSState),
        VMSTATE_INT64(pi_at, CSState),
        VMSTATE_INT64(pmi_at, CSState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_cs4231 = {
    .name ="cs4231",
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_load = cs_pre_load,
    .post_load = cs_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, CSState, CS_REGS),
        VMSTATE_UINT8_ARRAY(dregs, CSState, CS_DREGS),
        VMSTATE_BOOL_V(play_next, CSState, 2),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_cs4231_irqs,
        NULL
    }
};

static void cs4231_init(Object *obj)
{
    CSState *s = CS4231(obj);
    SysBusDevice *dev = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &cs_mem_ops, s, "cs4321",
                          CS_SIZE);
    sysbus_init_mmio(dev, &s->iomem);
    sysbus_init_irq(dev, &s->irq);
    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cs_irq_timer, s);
    s->pi_at = -1;
    s->pmi_at = -1;
}

static void cs4231_realize(DeviceState *dev, Error **errp)
{
    CSState *s = CS4231(dev);
    Error *err = NULL;

    /*
     * Without an audiodev the codec takes the default backend. The codec
     * is on the board, so if there is none the machine still starts: the
     * codec keeps its registers and plays nothing.
     */
    if (!audio_be_check(&s->audio_be, &err)) {
        error_prepend(&err, "cs4231: no sound: ");
        warn_report_err(err);
        return;
    }
    cs_play_setup(s);
}

static const Property cs4231_properties[] = {
    DEFINE_AUDIO_PROPERTIES(CSState, audio_be),
    DEFINE_PROP_LINK("iommu", CSState, iommu, TYPE_SUN4M_IOMMU,
                     IOMMUState *),
};

static void cs4231_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = cs4231_realize;
    device_class_set_legacy_reset(dc, cs_reset);
    device_class_set_props(dc, cs4231_properties);
    dc->vmsd = &vmstate_cs4231;
}

static const TypeInfo cs4231_info = {
    .name          = TYPE_CS4231,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CSState),
    .instance_init = cs4231_init,
    .class_init    = cs4231_class_init,
};

static void cs4231_register_types(void)
{
    type_register_static(&cs4231_info);
}

type_init(cs4231_register_types)
