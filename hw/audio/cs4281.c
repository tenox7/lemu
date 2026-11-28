/*
 * Cirrus Logic CS4281 CrystalClear PCI audio
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

/*
 * The PCI side follows the CS4281 data sheet (DS308PP4): the device ID, the
 * 4 KB register window in BAR 0, the 64 KB FIFO memory in BAR 1, the power
 * management capability and the Cirrus registers at E0h to FFh, which BAR 0
 * also shows at 3E0h to 3FFh.
 *
 * The registers in BAR 0 follow the Linux driver. Four bus master DMA
 * engines move samples between guest memory and the AC'97 link. Here a
 * playback engine feeds a QEMU output voice and a capture engine reads an
 * input voice, in the engine's own sample format, at the rate of the
 * sample rate converter or of the codec. The FIFOs, the FM synthesizer,
 * MIDI and the joystick keep their registers but do nothing.
 *
 * The AC'97 codec behind the link is a Realtek ALC655 by default.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/audio.h"
#include "qom/object.h"
#include "ac97.h"
#include "trace.h"

#define TYPE_CS4281 "cs4281"
OBJECT_DECLARE_SIMPLE_TYPE(CS4281State, CS4281)

#define CS4281_CHANNELS     4
#define CS4281_BA0_SIZE     0x1000
#define CS4281_BA1_SIZE     0x10000

/* BAR 0 registers */
#define BA0_HISR            0x000
#define BA0_HICR            0x008
#define BA0_HIMR            0x00c
#define BA0_IIER            0x010
#define BA0_HDSR0           0x0f0
#define BA0_DCA0            0x110
#define BA0_DMR0            0x150
#define BA0_FCR0            0x180
#define BA0_FPDR0           0x190
#define BA0_FCHS            0x20c
#define BA0_FSIC0           0x210
#define BA0_PMCS            0x344
#define BA0_CFG             0x3e0   /* config space E0h-FFh */
#define BA0_CLKCR1          0x400
#define BA0_FRR             0x410
#define BA0_SERC1           0x428
#define BA0_SERC2           0x42c
#define BA0_ACCTL           0x460
#define BA0_ACSTS           0x464
#define BA0_ACOSV           0x468
#define BA0_ACCAD           0x46c
#define BA0_ACCDA           0x470
#define BA0_ACISV           0x474
#define BA0_ACSAD           0x478
#define BA0_ACSDA           0x47c
#define BA0_MIDSR           0x494
#define BA0_MIDRP           0x49c
#define BA0_ACSTS2          0x4e4
#define BA0_ACISV2          0x4f4
#define BA0_ACSDA2          0x4fc
#define BA0_SSPM            0x740
#define BA0_DACSR           0x744
#define BA0_ADCSR           0x748

#define HISR_INTENA         BIT(31)
#define HISR_DMAI           BIT(18)
#define HISR_DMA(c)         BIT(8 + (c))

#define HICR_CHGM           BIT(1)
#define HICR_IEV            BIT(0)

#define HDSR_DHTC           BIT(17)
#define HDSR_DTC            BIT(16)
#define HDSR_DRUN           BIT(15)

#define DMR_DMA             BIT(29)
#define DMR_POLL            BIT(28)
#define DMR_SIZE20          BIT(20)
#define DMR_USIGN           BIT(19)
#define DMR_BEND            BIT(18)
#define DMR_MONO            BIT(17)
#define DMR_SIZE8           BIT(16)
#define DMR_AUTO            BIT(4)
#define DMR_TR_MASK         (3 << 2)
#define DMR_TR_WRITE        (1 << 2)    /* to memory: capture */
#define DMR_TR_READ         (2 << 2)    /* from memory: playback */

#define DCR_HTCIE           BIT(17)
#define DCR_TCIE            BIT(16)
#define DCR_MSK             BIT(0)

#define CLKCR1_CLKON        BIT(25)
#define CLKCR1_DLLRDY       BIT(24)
#define CLKCR1_SWCE         BIT(5)
#define CLKCR1_DLLP         BIT(4)

#define SERC_AC97_EN        0x3

#define ACCTL_TC            BIT(6)
#define ACCTL_CRW           BIT(4)
#define ACCTL_DCV           BIT(3)

#define ACSTS_VSTS          BIT(1)
#define ACSTS_CRDY          BIT(0)

#define MIDSR_TBE           BIT(14)
#define MIDSR_RBE           BIT(7)

#define SSPM_PSRCEN         BIT(4)
#define SSPM_CSRCEN         BIT(5)

/* config space */
#define CFG_PM              0x40
#define CFG_CWPR            0xe0
#define CFG_CFLR            0xf0
#define CFG_SSID            0xfc
#define CWPR_UNLOCK         0x4281
#define CFLR_AC97           0x00000001

typedef struct CS4281DMA {
    CS4281State *s;
    int index;
    uint32_t dba, dbc, dca, dcc;
    uint32_t dmr, dcr, hdsr;
    uint32_t done;          /* frames moved since the last reload */
    uint32_t rate;
    uint32_t mode;          /* DMR bits the voice was opened with */
    SWVoiceOut *out;
    SWVoiceIn *in;
} CS4281DMA;

struct CS4281State {
    PCIDevice dev;

    MemoryRegion ba0;
    MemoryRegion ba1;

    uint32_t regs[CS4281_BA0_SIZE / 4];
    uint8_t fifo[CS4281_BA1_SIZE];
    bool intena;
    CS4281DMA dma[CS4281_CHANNELS];
    uint16_t codec_regs[64];

    uint32_t codec_id;
    AudioBackend *audio_be;
};

#define REG(s, o)        ((s)->regs[(o) >> 2])
#define CODEC_REG(s, o)  ((s)->codec_regs[((o) & 0x7f) >> 1])
#define CODEC_VOL(vol, mask)  ((255 * ((vol) & (mask))) / (mask))

static void cs4281_open_voice(CS4281State *s, int c);

/* interrupts */

static uint32_t cs4281_hisr(CS4281State *s)
{
    uint32_t v = 0;
    int c;

    for (c = 0; c < CS4281_CHANNELS; c++) {
        if (s->dma[c].hdsr & (HDSR_DTC | HDSR_DHTC)) {
            v |= HISR_DMA(c) | HISR_DMAI;
        }
    }
    return v;
}

static void cs4281_update_irq(CS4281State *s)
{
    uint32_t pending = cs4281_hisr(s) & ~REG(s, BA0_HIMR) & ~HISR_INTENA;

    pci_set_irq(&s->dev, s->intena && pending);
}

/* the AC'97 codec */

static void codec_volume_set_out(CS4281State *s)
{
    uint16_t master = CODEC_REG(s, AC97_Master_Volume_Mute);
    uint16_t pcm = CODEC_REG(s, AC97_PCM_Out_Volume_Mute);
    int lvol, rvol, mute, c;

    lvol = 255 - CODEC_VOL(master >> 8, 0x1f);
    lvol *= 255 - CODEC_VOL(pcm >> 8, 0x1f);
    lvol /= 255;
    rvol = 255 - CODEC_VOL(master, 0x1f);
    rvol *= 255 - CODEC_VOL(pcm, 0x1f);
    rvol /= 255;
    mute = (master | pcm) >> MUTE_SHIFT;

    for (c = 0; c < CS4281_CHANNELS; c++) {
        if (s->dma[c].out) {
            audio_be_set_volume_out_lr(s->audio_be, s->dma[c].out, mute,
                                       lvol, rvol);
        }
    }
}

static void codec_reset(CS4281State *s)
{
    memset(s->codec_regs, 0, sizeof(s->codec_regs));
    CODEC_REG(s, AC97_Reset) = 0x0690;
    CODEC_REG(s, AC97_Master_Volume_Mute) = 0x8000;
    CODEC_REG(s, AC97_Headphone_Volume_Mute) = 0x8000;
    CODEC_REG(s, AC97_Master_Volume_Mono_Mute) = 0x8000;
    CODEC_REG(s, AC97_Phone_Volume_Mute) = 0x8008;
    CODEC_REG(s, AC97_Mic_Volume_Mute) = 0x8008;
    CODEC_REG(s, AC97_Line_In_Volume_Mute) = 0x8808;
    CODEC_REG(s, AC97_CD_Volume_Mute) = 0x8808;
    CODEC_REG(s, AC97_Video_Volume_Mute) = 0x8808;
    CODEC_REG(s, AC97_Aux_Volume_Mute) = 0x8808;
    CODEC_REG(s, AC97_PCM_Out_Volume_Mute) = 0x8808;
    CODEC_REG(s, AC97_Record_Gain_Mute) = 0x8000;
    CODEC_REG(s, AC97_Powerdown_Ctrl_Stat) = 0x000f;
    CODEC_REG(s, AC97_Extended_Audio_ID) = 0x0001;     /* VRA */
    CODEC_REG(s, AC97_PCM_Front_DAC_Rate) = 48000;
    CODEC_REG(s, AC97_PCM_LR_ADC_Rate) = 48000;
    CODEC_REG(s, AC97_Vendor_ID1) = s->codec_id >> 16;
    CODEC_REG(s, AC97_Vendor_ID2) = s->codec_id & 0xffff;
}

static void cs4281_reopen_voices(CS4281State *s)
{
    int c;

    for (c = 0; c < CS4281_CHANNELS; c++) {
        if (s->dma[c].out || s->dma[c].in) {
            cs4281_open_voice(s, c);
        }
    }
}

static void codec_write(CS4281State *s, uint8_t addr, uint16_t val)
{
    trace_cs4281_codec_write(addr, val);
    switch (addr) {
    case AC97_Reset:
        codec_reset(s);
        codec_volume_set_out(s);
        return;
    case AC97_Master_Volume_Mute:
    case AC97_PCM_Out_Volume_Mute:
        if (addr == AC97_Master_Volume_Mute) {
            if (val & BIT(13)) {
                val |= 0x1f00;
            }
            if (val & BIT(5)) {
                val |= 0x1f;
            }
        }
        CODEC_REG(s, addr) = val & 0x9f1f;
        codec_volume_set_out(s);
        return;
    case AC97_Extended_Audio_Ctrl_Stat:
        CODEC_REG(s, addr) = val & EACS_VRA;
        if (!(val & EACS_VRA)) {
            CODEC_REG(s, AC97_PCM_Front_DAC_Rate) = 48000;
            CODEC_REG(s, AC97_PCM_LR_ADC_Rate) = 48000;
        }
        cs4281_reopen_voices(s);
        return;
    case AC97_PCM_Front_DAC_Rate:
    case AC97_PCM_LR_ADC_Rate:
        if (CODEC_REG(s, AC97_Extended_Audio_Ctrl_Stat) & EACS_VRA) {
            CODEC_REG(s, addr) = MAX(MIN(val, 48000), 8000);
            cs4281_reopen_voices(s);
        }
        return;
    case AC97_Powerdown_Ctrl_Stat:
        CODEC_REG(s, addr) = (val & 0xff00) | 0x000f;
        return;
    case AC97_Extended_Audio_ID:
    case AC97_Vendor_ID1:
    case AC97_Vendor_ID2:
        return;
    default:
        /* the mixer: kept for the driver, not applied to the sound */
        CODEC_REG(s, addr) = val;
    }
}

static void cs4281_codec_command(CS4281State *s, uint32_t acctl)
{
    uint8_t addr = REG(s, BA0_ACCAD) & 0x7e;

    if (acctl & ACCTL_TC) {
        /* there is no secondary codec */
        if (acctl & ACCTL_CRW) {
            REG(s, BA0_ACSDA2) = 0xffff;
        }
        return;
    }
    if (acctl & ACCTL_CRW) {
        REG(s, BA0_ACSAD) = addr;
        REG(s, BA0_ACSDA) = CODEC_REG(s, addr);
        REG(s, BA0_ACSTS) |= ACSTS_VSTS;
        trace_cs4281_codec_read(addr, CODEC_REG(s, addr));
    } else {
        codec_write(s, addr, REG(s, BA0_ACCDA) & 0xffff);
    }
}

/* DMA engines */

static bool cs4281_dma_running(CS4281DMA *d)
{
    return (d->dmr & DMR_DMA) && !(d->dcr & DCR_MSK) &&
           ((d->dmr & DMR_TR_MASK) == DMR_TR_READ ||
            (d->dmr & DMR_TR_MASK) == DMR_TR_WRITE);
}

static int cs4281_frame_bytes(uint32_t dmr)
{
    int size = (dmr & DMR_SIZE8) ? 1 : (dmr & DMR_SIZE20) ? 4 : 2;

    return (dmr & DMR_MONO) ? size : size * 2;
}

static uint32_t cs4281_src_rate(uint32_t code)
{
    static const uint32_t fixed[] = { 48000, 44100, 22050, 16000, 11025,
                                      8000 };

    code &= 0xff;
    if (code < ARRAY_SIZE(fixed)) {
        return fixed[code];
    }
    return 1536000 / code;
}

static uint32_t cs4281_rate(CS4281State *s, bool capture)
{
    if (capture) {
        if (REG(s, BA0_SSPM) & SSPM_CSRCEN) {
            return cs4281_src_rate(REG(s, BA0_ADCSR));
        }
        return CODEC_REG(s, AC97_PCM_LR_ADC_Rate);
    }
    if (REG(s, BA0_SSPM) & SSPM_PSRCEN) {
        return cs4281_src_rate(REG(s, BA0_DACSR));
    }
    return CODEC_REG(s, AC97_PCM_Front_DAC_Rate);
}

/* frames moved: one count step is one frame */
static void cs4281_dma_advance(CS4281State *s, int c, uint32_t frames)
{
    CS4281DMA *d = &s->dma[c];
    uint32_t total = d->dbc + 1;
    uint32_t half = total / 2;
    uint32_t before = d->done;

    d->done += frames;
    d->dca += frames * cs4281_frame_bytes(d->dmr);
    d->dcc = d->dbc - d->done;

    if (before < half && d->done >= half && d->done < total &&
        (d->dcr & DCR_HTCIE)) {
        d->hdsr |= HDSR_DHTC;
    }
    if (d->done >= total) {
        if (d->dcr & DCR_TCIE) {
            d->hdsr |= HDSR_DTC;
        }
        d->done = 0;
        d->dca = d->dba;
        d->dcc = d->dbc;
        if (!(d->dmr & DMR_AUTO)) {
            d->dmr &= ~DMR_DMA;
            cs4281_open_voice(s, c);
        }
    }
    trace_cs4281_dma_advance(c, frames, d->dca, d->dcc, d->hdsr);
    cs4281_update_irq(s);
}

static void cs4281_out_cb(void *opaque, int free)
{
    CS4281DMA *d = opaque;
    CS4281State *s = d->s;
    QEMU_UNINITIALIZED uint8_t buf[4096];
    int c = d->index, fb;

    fb = cs4281_frame_bytes(d->dmr);

    while (cs4281_dma_running(d) && free >= fb) {
        uint32_t frames = MIN(free, (int)sizeof(buf)) / fb;
        uint32_t left = d->dbc + 1 - d->done;
        size_t done;

        frames = MIN(frames, left);
        pci_dma_read(&s->dev, d->dca, buf, frames * fb);
        done = audio_be_write(s->audio_be, d->out, buf, frames * fb);
        frames = done / fb;
        if (!frames) {
            break;
        }
        free -= frames * fb;
        cs4281_dma_advance(s, c, frames);
    }
}

static void cs4281_in_cb(void *opaque, int avail)
{
    CS4281DMA *d = opaque;
    CS4281State *s = d->s;
    QEMU_UNINITIALIZED uint8_t buf[4096];
    int c = d->index, fb;

    fb = cs4281_frame_bytes(d->dmr);

    while (cs4281_dma_running(d) && avail >= fb) {
        uint32_t frames = MIN(avail, (int)sizeof(buf)) / fb;
        uint32_t left = d->dbc + 1 - d->done;
        size_t got;

        frames = MIN(frames, left);
        got = audio_be_read(s->audio_be, d->in, buf, frames * fb);
        frames = got / fb;
        if (!frames) {
            break;
        }
        pci_dma_write(&s->dev, d->dca, buf, frames * fb);
        avail -= frames * fb;
        cs4281_dma_advance(s, c, frames);
    }
}

/* open, close, start or stop the voice of an engine to match its registers */
static void cs4281_open_voice(CS4281State *s, int c)
{
    CS4281DMA *d = &s->dma[c];
    bool run = cs4281_dma_running(d);
    bool capture = (d->dmr & DMR_TR_MASK) == DMR_TR_WRITE;
    uint32_t fmt = d->dmr & (DMR_SIZE20 | DMR_USIGN | DMR_BEND | DMR_MONO |
                             DMR_SIZE8 | DMR_TR_MASK);
    uint32_t rate = cs4281_rate(s, capture);
    struct audsettings as;
    static const char *const names[CS4281_CHANNELS][2] = {
        { "cs4281.out0", "cs4281.in0" }, { "cs4281.out1", "cs4281.in1" },
        { "cs4281.out2", "cs4281.in2" }, { "cs4281.out3", "cs4281.in3" },
    };

    if (!run) {
        if (d->out) {
            audio_be_set_active_out(s->audio_be, d->out, false);
        }
        if (d->in) {
            audio_be_set_active_in(s->audio_be, d->in, false);
        }
        return;
    }

    if (fmt != d->mode || rate != d->rate || (!capture && !d->out) ||
        (capture && !d->in)) {
        bool sign = !(d->dmr & DMR_USIGN);

        as.freq = rate;
        as.nchannels = (d->dmr & DMR_MONO) ? 1 : 2;
        if (d->dmr & DMR_SIZE8) {
            as.fmt = sign ? AUDIO_FORMAT_S8 : AUDIO_FORMAT_U8;
        } else if (d->dmr & DMR_SIZE20) {
            as.fmt = sign ? AUDIO_FORMAT_S32 : AUDIO_FORMAT_U32;
        } else {
            as.fmt = sign ? AUDIO_FORMAT_S16 : AUDIO_FORMAT_U16;
        }
        as.big_endian = !!(d->dmr & DMR_BEND);
        trace_cs4281_voice(c, capture ? "in" : "out", rate, as.nchannels,
                           as.fmt, as.big_endian);

        if (capture) {
            if (d->out) {
                audio_be_close_out(s->audio_be, d->out);
                d->out = NULL;
            }
            d->in = audio_be_open_in(s->audio_be, d->in, names[c][1], d,
                                     cs4281_in_cb, &as);
        } else {
            if (d->in) {
                audio_be_close_in(s->audio_be, d->in);
                d->in = NULL;
            }
            d->out = audio_be_open_out(s->audio_be, d->out, names[c][0], d,
                                       cs4281_out_cb, &as);
            codec_volume_set_out(s);
        }
        d->mode = fmt;
        d->rate = rate;
    }

    if (capture && d->in) {
        audio_be_set_active_in(s->audio_be, d->in, true);
    } else if (!capture && d->out) {
        audio_be_set_active_out(s->audio_be, d->out, true);
    }
}

/* BAR 0 */

static uint32_t cs4281_cfg_read(CS4281State *s, uint32_t off)
{
    return pci_get_long(s->dev.config + CFG_CWPR + (off & 0x1c));
}

static void cs4281_cfg_write(CS4281State *s, uint32_t off, uint32_t val)
{
    uint8_t *c = s->dev.config;

    off = CFG_CWPR + (off & 0x1c);
    if (off == CFG_CWPR) {
        val &= 0xffff;
    }
    pci_set_long(c + off, val);
    if (off == CFG_SSID) {
        pci_set_long(c + PCI_SUBSYSTEM_VENDOR_ID, val);
    }
}

static uint64_t cs4281_ba0_read(void *opaque, hwaddr addr, unsigned size)
{
    CS4281State *s = opaque;
    uint32_t val;
    int c;

    addr &= CS4281_BA0_SIZE - 4;

    switch (addr) {
    case BA0_HISR:
        val = cs4281_hisr(s) | (s->intena ? HISR_INTENA : 0);
        /* reading a pending interrupt holds it off until the next EOI */
        if (val & ~REG(s, BA0_HIMR) & ~HISR_INTENA) {
            s->intena = false;
            cs4281_update_irq(s);
        }
        break;
    case BA0_HDSR0 ... BA0_HDSR0 + 0xc:
        c = (addr - BA0_HDSR0) >> 2;
        val = s->dma[c].hdsr;
        if (cs4281_dma_running(&s->dma[c])) {
            val |= HDSR_DRUN;
        }
        s->dma[c].hdsr &= ~(HDSR_DTC | HDSR_DHTC);
        cs4281_update_irq(s);
        break;
    case BA0_DCA0 ... BA0_DCA0 + 0x3f:
        c = (addr - BA0_DCA0) >> 4;
        switch (addr & 0xc) {
        case 0x0:
            val = s->dma[c].dca;
            break;
        case 0x4:
            val = s->dma[c].dcc;
            break;
        case 0x8:
            val = s->dma[c].dba;
            break;
        default:
            val = s->dma[c].dbc;
            break;
        }
        break;
    case BA0_DMR0 ... BA0_DMR0 + 0x1f:
        c = (addr - BA0_DMR0) >> 3;
        val = (addr & 4) ? s->dma[c].dcr : s->dma[c].dmr;
        break;
    case BA0_FCHS:
        /* every FIFO empty */
        val = 0x10101010;
        break;
    case BA0_CFG ... BA0_CFG + 0x1f:
        val = cs4281_cfg_read(s, addr);
        break;
    case BA0_CLKCR1:
        val = REG(s, addr);
        if (val & CLKCR1_DLLP) {
            val |= CLKCR1_DLLRDY;
        }
        if (val & CLKCR1_SWCE) {
            val |= CLKCR1_CLKON;
        }
        break;
    case BA0_ACSTS:
        val = REG(s, addr) | ACSTS_CRDY;
        break;
    case BA0_ACISV:
        /* the codec sends PCM left and right */
        val = 0x3;
        break;
    case BA0_ACSDA:
        val = REG(s, addr);
        REG(s, BA0_ACSTS) &= ~ACSTS_VSTS;
        break;
    case BA0_ACSTS2:
    case BA0_ACISV2:
        val = 0;
        break;
    case BA0_MIDSR:
        val = MIDSR_TBE | MIDSR_RBE;
        break;
    default:
        val = REG(s, addr);
        break;
    }

    trace_cs4281_read(addr, val);
    return val;
}

static void cs4281_ba0_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    CS4281State *s = opaque;
    int c;

    addr &= CS4281_BA0_SIZE - 4;
    trace_cs4281_write(addr, val);

    switch (addr) {
    case BA0_HISR:
    case BA0_HDSR0 ... BA0_HDSR0 + 0xc:
    case BA0_FCHS:
    case BA0_ACSTS:
    case BA0_ACISV:
    case BA0_ACSAD:
    case BA0_ACSDA:
    case BA0_ACSTS2:
    case BA0_ACISV2:
    case BA0_ACSDA2:
    case BA0_SERC1:
    case BA0_SERC2:
    case BA0_MIDRP:
        /* read only */
        return;
    case BA0_HICR:
        if (val & HICR_CHGM) {
            s->intena = val & HICR_IEV;
        }
        cs4281_update_irq(s);
        return;
    case BA0_HIMR:
        REG(s, addr) = val;
        cs4281_update_irq(s);
        return;
    case BA0_DCA0 ... BA0_DCA0 + 0x3f:
        c = (addr - BA0_DCA0) >> 4;
        switch (addr & 0xc) {
        case 0x8:
            s->dma[c].dba = val;
            s->dma[c].dca = val;
            s->dma[c].done = 0;
            break;
        case 0xc:
            s->dma[c].dbc = val;
            s->dma[c].dcc = val;
            s->dma[c].done = 0;
            break;
        default:
            /* the current registers are read only */
            break;
        }
        return;
    case BA0_DMR0 ... BA0_DMR0 + 0x1f: {
        CS4281DMA *d;

        c = (addr - BA0_DMR0) >> 3;
        d = &s->dma[c];
        if (addr & 4) {
            /* the mask bit pauses and resumes where the engine was */
            d->dcr = val & (DCR_HTCIE | DCR_TCIE | DCR_MSK);
        } else {
            if (!(d->dmr & DMR_DMA) && (val & DMR_DMA)) {
                /* enabling the engine starts it at the base */
                d->dca = d->dba;
                d->dcc = d->dbc;
                d->done = 0;
            }
            d->dmr = val;
        }
        cs4281_open_voice(s, c);
        return;
    }
    case BA0_CFG ... BA0_CFG + 0x1f:
        cs4281_cfg_write(s, addr, val);
        return;
    case BA0_ACCTL:
        REG(s, addr) = val & ~ACCTL_DCV;
        if (val & ACCTL_DCV) {
            cs4281_codec_command(s, val);
        }
        return;
    case BA0_SSPM:
    case BA0_DACSR:
    case BA0_ADCSR:
        REG(s, addr) = val;
        cs4281_reopen_voices(s);
        return;
    default:
        REG(s, addr) = val;
        return;
    }
}

static const MemoryRegionOps cs4281_ba0_ops = {
    .read = cs4281_ba0_read,
    .write = cs4281_ba0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* BAR 1: the FIFO memory, plain storage here */

static uint64_t cs4281_ba1_read(void *opaque, hwaddr addr, unsigned size)
{
    CS4281State *s = opaque;

    return ldl_le_p(s->fifo + (addr & (CS4281_BA1_SIZE - 4)));
}

static void cs4281_ba1_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    CS4281State *s = opaque;

    stl_le_p(s->fifo + (addr & (CS4281_BA1_SIZE - 4)), val);
}

static const MemoryRegionOps cs4281_ba1_ops = {
    .read = cs4281_ba1_read,
    .write = cs4281_ba1_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* config space: E4h to FFh are writable only after CWPR is 4281h */

static void cs4281_config_write(PCIDevice *dev, uint32_t addr, uint32_t val,
                                int len)
{
    uint8_t *c = dev->config;
    int i;

    if (addr + len <= CFG_CWPR) {
        pci_default_write_config(dev, addr, val, len);
        return;
    }
    for (i = 0; i < len; i++) {
        uint32_t a = addr + i;
        uint8_t b = val >> (8 * i);

        if (a < CFG_CWPR) {
            pci_default_write_config(dev, a, b, 1);
        } else if (a < CFG_CWPR + 2) {
            c[a] = b;
        } else if (a >= CFG_CWPR + 4 &&
                   pci_get_word(c + CFG_CWPR) == CWPR_UNLOCK) {
            c[a] = b;
            if (a >= CFG_SSID) {
                c[PCI_SUBSYSTEM_VENDOR_ID + (a - CFG_SSID)] = b;
            }
        }
    }
}

static void cs4281_reset(DeviceState *dev)
{
    CS4281State *s = CS4281(dev);
    int c;

    for (c = 0; c < CS4281_CHANNELS; c++) {
        CS4281DMA *d = &s->dma[c];

        if (d->out) {
            audio_be_set_active_out(s->audio_be, d->out, false);
        }
        if (d->in) {
            audio_be_set_active_in(s->audio_be, d->in, false);
        }
        d->dba = d->dbc = d->dca = d->dcc = 0;
        d->dmr = d->hdsr = d->done = 0;
        d->dcr = DCR_MSK;
    }
    memset(s->regs, 0, sizeof(s->regs));
    REG(s, BA0_SERC1) = SERC_AC97_EN;
    REG(s, BA0_SERC2) = SERC_AC97_EN;
    REG(s, BA0_HIMR) = 0x7fffffff;
    pci_set_long(s->dev.config + CFG_CFLR, CFLR_AC97);
    pci_set_word(s->dev.config + CFG_CWPR, 0);
    s->intena = false;
    codec_reset(s);
    pci_set_irq(&s->dev, 0);
}

static void cs4281_realize(PCIDevice *dev, Error **errp)
{
    CS4281State *s = CS4281(dev);
    uint8_t *c = dev->config;
    int i;

    for (i = 0; i < CS4281_CHANNELS; i++) {
        s->dma[i].s = s;
        s->dma[i].index = i;
    }

    if (!audio_be_check(&s->audio_be, errp)) {
        return;
    }

    pci_set_word(c + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM |
                 PCI_STATUS_CAP_LIST);
    c[PCI_INTERRUPT_PIN] = 1;
    c[PCI_MIN_GNT] = 0x04;
    c[PCI_MAX_LAT] = 0x18;

    /* power management: D1, D2, PME# from D0 to D3hot, version 1 */
    c[PCI_CAPABILITY_LIST] = CFG_PM;
    c[CFG_PM] = PCI_CAP_ID_PM;
    c[CFG_PM + 1] = 0;
    pci_set_word(c + CFG_PM + PCI_PM_PMC, 0x7e21);
    pci_set_word(dev->wmask + CFG_PM + PCI_PM_CTRL,
                 PCI_PM_CTRL_STATE_MASK | PCI_PM_CTRL_PME_ENABLE);
    pci_set_word(dev->w1cmask + CFG_PM + PCI_PM_CTRL, PCI_PM_CTRL_PME_STATUS);

    memory_region_init_io(&s->ba0, OBJECT(s), &cs4281_ba0_ops, s,
                          "cs4281.ba0", CS4281_BA0_SIZE);
    memory_region_init_io(&s->ba1, OBJECT(s), &cs4281_ba1_ops, s,
                          "cs4281.ba1", CS4281_BA1_SIZE);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->ba0);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->ba1);
}

static void cs4281_exit(PCIDevice *dev)
{
    CS4281State *s = CS4281(dev);
    int c;

    for (c = 0; c < CS4281_CHANNELS; c++) {
        audio_be_close_out(s->audio_be, s->dma[c].out);
        audio_be_close_in(s->audio_be, s->dma[c].in);
    }
}

static int cs4281_post_load(void *opaque, int version_id)
{
    CS4281State *s = opaque;
    int c;

    for (c = 0; c < CS4281_CHANNELS; c++) {
        s->dma[c].mode = ~0u;
        cs4281_open_voice(s, c);
    }
    codec_volume_set_out(s);
    cs4281_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_cs4281_dma = {
    .name = "cs4281-dma",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(dba, CS4281DMA),
        VMSTATE_UINT32(dbc, CS4281DMA),
        VMSTATE_UINT32(dca, CS4281DMA),
        VMSTATE_UINT32(dcc, CS4281DMA),
        VMSTATE_UINT32(dmr, CS4281DMA),
        VMSTATE_UINT32(dcr, CS4281DMA),
        VMSTATE_UINT32(hdsr, CS4281DMA),
        VMSTATE_UINT32(done, CS4281DMA),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_cs4281 = {
    .name = "cs4281",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = cs4281_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, CS4281State),
        VMSTATE_UINT32_ARRAY(regs, CS4281State, CS4281_BA0_SIZE / 4),
        VMSTATE_UINT8_ARRAY(fifo, CS4281State, CS4281_BA1_SIZE),
        VMSTATE_BOOL(intena, CS4281State),
        VMSTATE_STRUCT_ARRAY(dma, CS4281State, CS4281_CHANNELS, 1,
                             vmstate_cs4281_dma, CS4281DMA),
        VMSTATE_UINT16_ARRAY(codec_regs, CS4281State, 64),
        VMSTATE_END_OF_LIST()
    }
};

static const Property cs4281_properties[] = {
    DEFINE_AUDIO_PROPERTIES(CS4281State, audio_be),
    /* Realtek ALC655 */
    DEFINE_PROP_UINT32("codec-id", CS4281State, codec_id, 0x414c4760),
};

static void cs4281_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = cs4281_realize;
    k->exit = cs4281_exit;
    k->config_write = cs4281_config_write;
    k->vendor_id = PCI_VENDOR_ID_CIRRUS;
    k->device_id = 0x6005;
    k->revision = 0x01;
    k->class_id = PCI_CLASS_MULTIMEDIA_AUDIO;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Cirrus Logic CS4281 CrystalClear PCI audio";
    dc->vmsd = &vmstate_cs4281;
    device_class_set_legacy_reset(dc, cs4281_reset);
    device_class_set_props(dc, cs4281_properties);
}

static const TypeInfo cs4281_info = {
    .name          = TYPE_CS4281,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(CS4281State),
    .class_init    = cs4281_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void cs4281_register_types(void)
{
    type_register_static(&cs4281_info);
}

type_init(cs4281_register_types)
