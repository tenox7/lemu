/*
 * VIA south bridges sound support
 *
 * Copyright (c) 2022-2023 BALATON Zoltan
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

/*
 * Playback and capture SGD channels with the AC97 codec. The FM channel
 * and the mixer settings are kept but make no sound.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/isa/vt82c686.h"
#include "ac97.h"
#include "trace.h"

#define CLEN_IS_EOL(x)  ((x)->clen & BIT(31))
#define CLEN_IS_FLAG(x) ((x)->clen & BIT(30))
#define CLEN_IS_STOP(x) ((x)->clen & BIT(29))
#define CLEN_LEN(x)     ((x)->clen & 0xffffff)

#define STAT_ACTIVE BIT(7)
#define STAT_PAUSED BIT(6)
#define STAT_TRIG   BIT(3)
#define STAT_STOP   BIT(2)
#define STAT_EOL    BIT(1)
#define STAT_FLAG   BIT(0)

#define CNTL_START  BIT(7)
#define CNTL_TERM   BIT(6)
#define CNTL_PAUSE  BIT(3)

static void open_voice_out(ViaAC97State *s);
static void open_voice_in(ViaAC97State *s);

static uint16_t codec_rates[] = { 8000, 11025, 16000, 22050, 32000, 44100,
                                  48000 };

#define CODEC_REG(s, o)  ((s)->codec_regs[(o) / 2])
#define CODEC_VOL(vol, mask)  ((255 * ((vol) & mask)) / mask)

static void codec_volume_set_out(ViaAC97State *s)
{
    int lvol, rvol, mute;

    lvol = 255 - CODEC_VOL(CODEC_REG(s, AC97_Master_Volume_Mute) >> 8, 0x1f);
    lvol *= 255 - CODEC_VOL(CODEC_REG(s, AC97_PCM_Out_Volume_Mute) >> 8, 0x1f);
    lvol /= 255;
    rvol = 255 - CODEC_VOL(CODEC_REG(s, AC97_Master_Volume_Mute), 0x1f);
    rvol *= 255 - CODEC_VOL(CODEC_REG(s, AC97_PCM_Out_Volume_Mute), 0x1f);
    rvol /= 255;
    mute = CODEC_REG(s, AC97_Master_Volume_Mute) >> MUTE_SHIFT;
    mute |= CODEC_REG(s, AC97_PCM_Out_Volume_Mute) >> MUTE_SHIFT;
    audio_be_set_volume_out_lr(s->audio_be, s->vo, mute, lvol, rvol);
}

static void codec_reset(ViaAC97State *s)
{
    memset(s->codec_regs, 0, sizeof(s->codec_regs));
    CODEC_REG(s, AC97_Reset) = 0x6a90;
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
    CODEC_REG(s, AC97_Extended_Audio_ID) = 0x0a05;
    CODEC_REG(s, AC97_Extended_Audio_Ctrl_Stat) = 0x0400;
    CODEC_REG(s, AC97_PCM_Front_DAC_Rate) = 48000;
    CODEC_REG(s, AC97_PCM_LR_ADC_Rate) = 48000;
    /* Sigmatel 9766 (STAC9766) */
    CODEC_REG(s, AC97_Vendor_ID1) = 0x8384;
    CODEC_REG(s, AC97_Vendor_ID2) = 0x7666;
}

static uint16_t codec_read(ViaAC97State *s, uint8_t addr)
{
    return CODEC_REG(s, addr);
}

static void codec_write(ViaAC97State *s, uint8_t addr, uint16_t val)
{
    trace_via_ac97_codec_write(addr, val);
    switch (addr) {
    case AC97_Reset:
        codec_reset(s);
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
        CODEC_REG(s, addr) &= ~EACS_VRA;
        CODEC_REG(s, addr) |= val & EACS_VRA;
        if (!(val & EACS_VRA)) {
            CODEC_REG(s, AC97_PCM_Front_DAC_Rate) = 48000;
            CODEC_REG(s, AC97_PCM_LR_ADC_Rate) = 48000;
            open_voice_out(s);
            open_voice_in(s);
        }
        return;
    case AC97_PCM_Front_DAC_Rate:
    case AC97_PCM_LR_ADC_Rate:
        if (CODEC_REG(s, AC97_Extended_Audio_Ctrl_Stat) & EACS_VRA) {
            int i;
            uint16_t rate = val;

            for (i = 0; i < ARRAY_SIZE(codec_rates) - 1; i++) {
                if (rate < codec_rates[i] +
                    (codec_rates[i + 1] - codec_rates[i]) / 2) {
                    rate = codec_rates[i];
                    break;
                }
            }
            if (rate > 48000) {
                rate = 48000;
            }
            CODEC_REG(s, addr) = rate;
            if (addr == AC97_PCM_Front_DAC_Rate) {
                open_voice_out(s);
            } else {
                open_voice_in(s);
            }
        }
        return;
    case AC97_Powerdown_Ctrl_Stat:
        CODEC_REG(s, addr) = (val & 0xff00) | (CODEC_REG(s, addr) & 0xff);
        return;
    case AC97_Headphone_Volume_Mute:
    case AC97_Master_Volume_Mono_Mute:
    case AC97_Master_Tone_RL:
    case AC97_PC_BEEP_Volume_Mute:
    case AC97_Phone_Volume_Mute:
    case AC97_Mic_Volume_Mute:
    case AC97_Line_In_Volume_Mute:
    case AC97_CD_Volume_Mute:
    case AC97_Video_Volume_Mute:
    case AC97_Aux_Volume_Mute:
    case AC97_Record_Select:
    case AC97_Record_Gain_Mute:
    case AC97_Record_Gain_Mic_Mute:
    case AC97_General_Purpose:
    case AC97_3D_Control:
        /* mixer settings: kept for the driver, not applied to the sound */
        CODEC_REG(s, addr) = val;
        return;
    case AC97_Extended_Audio_ID:
    case AC97_Vendor_ID1:
    case AC97_Vendor_ID2:
        /* Read only registers */
        return;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "via-ac97: Unimplemented codec register 0x%x\n", addr);
        CODEC_REG(s, addr) = val;
    }
}

static void fetch_sgd(ViaAC97SGDChannel *c, PCIDevice *d)
{
    uint32_t b[2];

    if (c->curr < c->base) {
        c->curr = c->base;
    }
    if (unlikely(pci_dma_read(d, c->curr, b, sizeof(b)) != MEMTX_OK)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "via-ac97: DMA error reading SGD table\n");
        return;
    }
    c->addr = le32_to_cpu(b[0]);
    c->clen = le32_to_cpu(b[1]);
    trace_via_ac97_sgd_fetch(c->curr, c->addr, CLEN_IS_STOP(c) ? 'S' : '-',
                             CLEN_IS_EOL(c) ? 'E' : '-',
                             CLEN_IS_FLAG(c) ? 'F' : '-', CLEN_LEN(c));
}

/* One interrupt line for all channels: up while any enabled status is set */
static void update_irq(ViaAC97State *s)
{
    int level = 0;

    for (int i = 0; i < VIA_AC97_SGD_CHANNELS; i++) {
        ViaAC97SGDChannel *c = &s->sgd_ch[i];

        if (c->stat & c->type & (STAT_EOL | STAT_FLAG)) {
            level = 1;
        }
    }
    via_isa_set_irq(&s->dev, 0, level);
}

static void set_active(ViaAC97State *s, int ch, bool on)
{
    if (ch == VIA_AC97_SGD_PLAY) {
        audio_be_set_active_out(s->audio_be, s->vo, on);
    } else if (ch == VIA_AC97_SGD_REC) {
        audio_be_set_active_in(s->audio_be, s->vi, on);
    }
}

/* A block of the SGD table is done: set its status and maybe interrupt */
static void sgd_block_end(ViaAC97State *s, int ch)
{
    ViaAC97SGDChannel *c = &s->sgd_ch[ch];

    c->curr += 8;
    if (CLEN_IS_EOL(c)) {
        c->stat |= STAT_EOL;
        if (c->type & CNTL_START) {
            c->curr = c->base;
            c->stat |= STAT_PAUSED;
        } else {
            c->stat &= ~STAT_ACTIVE;
            set_active(s, ch, false);
        }
    }
    if (CLEN_IS_FLAG(c)) {
        c->stat |= STAT_FLAG | STAT_PAUSED;
    }
    if (CLEN_IS_STOP(c)) {
        c->stat |= STAT_STOP | STAT_PAUSED;
    }
    c->clen = 0;
    update_irq(s);
}

static void out_cb(void *opaque, int avail)
{
    ViaAC97State *s = opaque;
    ViaAC97SGDChannel *c = &s->sgd_ch[VIA_AC97_SGD_PLAY];
    int temp, to_copy, copied;
    bool stop = false;
    QEMU_UNINITIALIZED uint8_t tmpbuf[4096];

    if (c->stat & STAT_PAUSED) {
        return;
    }
    c->stat |= STAT_ACTIVE;
    while (avail && !stop) {
        if (!c->clen) {
            fetch_sgd(c, &s->dev);
        }
        temp = MIN(CLEN_LEN(c), avail);
        while (temp) {
            to_copy = MIN(temp, sizeof(tmpbuf));
            pci_dma_read(&s->dev, c->addr, tmpbuf, to_copy);
            copied = audio_be_write(s->audio_be, s->vo, tmpbuf, to_copy);
            if (!copied) {
                stop = true;
                break;
            }
            temp -= copied;
            avail -= copied;
            c->addr += copied;
            c->clen -= copied;
        }
        if (CLEN_LEN(c) == 0) {
            sgd_block_end(s, VIA_AC97_SGD_PLAY);
            stop = true;
        }
    }
}

static void in_cb(void *opaque, int avail)
{
    ViaAC97State *s = opaque;
    ViaAC97SGDChannel *c = &s->sgd_ch[VIA_AC97_SGD_REC];
    QEMU_UNINITIALIZED uint8_t tmpbuf[4096];

    if (c->stat & STAT_PAUSED) {
        return;
    }
    c->stat |= STAT_ACTIVE;
    while (avail) {
        size_t to_copy, got;

        if (!c->clen) {
            fetch_sgd(c, &s->dev);
            if (!CLEN_LEN(c)) {
                sgd_block_end(s, VIA_AC97_SGD_REC);
                break;
            }
        }
        to_copy = MIN(MIN(CLEN_LEN(c), avail), sizeof(tmpbuf));
        got = audio_be_read(s->audio_be, s->vi, tmpbuf, to_copy);
        if (!got) {
            break;
        }
        pci_dma_write(&s->dev, c->addr, tmpbuf, got);
        avail -= got;
        c->addr += got;
        c->clen -= got;
        if (CLEN_LEN(c) == 0) {
            sgd_block_end(s, VIA_AC97_SGD_REC);
            break;
        }
    }
}

static void open_voice_out(ViaAC97State *s)
{
    ViaAC97SGDChannel *c = &s->sgd_ch[VIA_AC97_SGD_PLAY];
    struct audsettings as = {
        .freq = CODEC_REG(s, AC97_PCM_Front_DAC_Rate),
        .nchannels = c->type & BIT(4) ? 2 : 1,
        .fmt = c->type & BIT(5) ? AUDIO_FORMAT_S16 : AUDIO_FORMAT_S8,
        .big_endian = false,
    };
    s->vo = audio_be_open_out(s->audio_be, s->vo, "via-ac97.out", s, out_cb,
                              &as);
}

static void open_voice_in(ViaAC97State *s)
{
    ViaAC97SGDChannel *c = &s->sgd_ch[VIA_AC97_SGD_REC];
    struct audsettings as = {
        .freq = CODEC_REG(s, AC97_PCM_LR_ADC_Rate),
        .nchannels = c->type & BIT(4) ? 2 : 1,
        .fmt = c->type & BIT(5) ? AUDIO_FORMAT_S16 : AUDIO_FORMAT_S8,
        .big_endian = false,
    };
    s->vi = audio_be_open_in(s->audio_be, s->vi, "via-ac97.in", s, in_cb,
                             &as);
}

static void open_voice(ViaAC97State *s, int ch)
{
    if (ch == VIA_AC97_SGD_PLAY) {
        open_voice_out(s);
    } else if (ch == VIA_AC97_SGD_REC) {
        open_voice_in(s);
    }
}

static uint64_t sgd_read(void *opaque, hwaddr addr, unsigned size)
{
    ViaAC97State *s = opaque;
    uint64_t val = 0;

    if (addr < 0x10 * VIA_AC97_SGD_CHANNELS) {
        /* the registers of one channel: 0x00 playback, 0x10 capture, 0x20 FM */
        ViaAC97SGDChannel *c = &s->sgd_ch[addr >> 4];

        switch (addr & 0xf) {
        case 0:
            val = c->stat;
            if (c->type & CNTL_START) {
                val |= STAT_TRIG;
            }
            break;
        case 1:
            val = c->stat & STAT_PAUSED ? BIT(3) : 0;
            break;
        case 2:
            val = c->type;
            break;
        case 4:
            val = c->curr;
            break;
        case 0xc:
            val = CLEN_LEN(c);
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "via-ac97: Unimplemented register read "
                          "0x%" HWADDR_PRIx "\n", addr);
        }
        trace_via_ac97_sgd_read(addr, size, val);
        return val;
    }

    switch (addr) {
    case 0x80:
        val = s->ac97_cmd;
        break;
    case 0x84:
        /* status shadow: bit n of each nibble is channel n */
        for (int i = 0; i < VIA_AC97_SGD_CHANNELS; i++) {
            ViaAC97SGDChannel *c = &s->sgd_ch[i];

            if (c->stat & STAT_FLAG) {
                val |= BIT(i);
            }
            if (c->stat & STAT_EOL) {
                val |= BIT(4 + i);
            }
            if (c->stat & STAT_STOP) {
                val |= BIT(8 + i);
            }
            if (c->stat & STAT_ACTIVE) {
                val |= BIT(12 + i);
            }
        }
        break;
    case 0x88:
        /* GPI status */
        break;
    case 0x8c:
        val = s->gpi_intr;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "via-ac97: Unimplemented register read 0x%"
                      HWADDR_PRIx"\n", addr);
    }
    trace_via_ac97_sgd_read(addr, size, val);
    return val;
}

static void sgd_ch_write(ViaAC97State *s, int ch, hwaddr reg, uint64_t val)
{
    ViaAC97SGDChannel *c = &s->sgd_ch[ch];

    switch (reg) {
    case 0:
        if (val & STAT_STOP) {
            c->stat &= ~(STAT_STOP | STAT_PAUSED);
        }
        if (val & STAT_EOL) {
            c->stat &= ~(STAT_EOL | STAT_PAUSED);
        }
        if (val & STAT_FLAG) {
            c->stat &= ~(STAT_FLAG | STAT_PAUSED);
        }
        update_irq(s);
        break;
    case 1:
        if (val & CNTL_START) {
            c->stat = STAT_ACTIVE;
            c->clen = 0;
            c->curr = c->base;
            set_active(s, ch, true);
        }
        if (val & CNTL_TERM) {
            set_active(s, ch, false);
            c->stat &= ~(STAT_ACTIVE | STAT_PAUSED);
            c->clen = 0;
        }
        if (val & CNTL_PAUSE) {
            set_active(s, ch, false);
            c->stat &= ~STAT_ACTIVE;
            c->stat |= STAT_PAUSED;
        } else if (c->stat & STAT_PAUSED) {
            set_active(s, ch, true);
            c->stat |= STAT_ACTIVE;
            c->stat &= ~STAT_PAUSED;
        }
        update_irq(s);
        break;
    case 2:
    {
        uint32_t oldval = c->type;

        c->type = val;
        if ((oldval & 0x30) != (val & 0x30)) {
            open_voice(s, ch);
        }
        update_irq(s);
        break;
    }
    case 4:
        c->base = val & ~1ULL;
        c->curr = c->base;
        break;
    case 0xc:
        /* Read only */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "via-ac97: Unimplemented register write "
                      "0x%" HWADDR_PRIx "\n", (hwaddr)ch * 0x10 + reg);
    }
}

static void sgd_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    ViaAC97State *s = opaque;

    trace_via_ac97_sgd_write(addr, size, val);
    if (addr < 0x10 * VIA_AC97_SGD_CHANNELS) {
        sgd_ch_write(s, addr >> 4, addr & 0xf, val);
        return;
    }
    switch (addr) {
    case 0x80:
        if (val >> 30) {
            /* we only have primary codec */
            break;
        }
        if (val & BIT(23)) { /* read reg */
            s->ac97_cmd = val & 0xc0ff0000ULL;
            s->ac97_cmd |= codec_read(s, (val >> 16) & 0x7f);
            s->ac97_cmd |= BIT(25); /* data valid */
        } else {
            s->ac97_cmd = val & 0xc0ffffffULL;
            codec_write(s, (val >> 16) & 0x7f, val);
        }
        break;
    case 0x84:
    case 0x88:
        /* Read only */
        break;
    case 0x8c:
        /* GPI interrupt enables; there are no GPI inputs */
        s->gpi_intr = val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "via-ac97: Unimplemented register write 0x%"
                      HWADDR_PRIx"\n", addr);
    }
}

static const MemoryRegionOps sgd_ops = {
    .read = sgd_read,
    .write = sgd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t fm_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: 0x%"HWADDR_PRIx" %d\n", __func__, addr, size);
    return 0;
}

static void fm_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: 0x%"HWADDR_PRIx" %d <= 0x%"PRIX64"\n",
                  __func__, addr, size, val);
}

static const MemoryRegionOps fm_ops = {
    .read = fm_read,
    .write = fm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t midi_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: 0x%"HWADDR_PRIx" %d\n", __func__, addr, size);
    return 0;
}

static void midi_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: 0x%"HWADDR_PRIx" %d <= 0x%"PRIX64"\n",
                  __func__, addr, size, val);
}

static const MemoryRegionOps midi_ops = {
    .read = midi_read,
    .write = midi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void via_ac97_reset(DeviceState *dev)
{
    ViaAC97State *s = VIA_AC97(dev);

    codec_reset(s);
    memset(s->sgd_ch, 0, sizeof(s->sgd_ch));
    s->gpi_intr = 0;
    via_isa_set_irq(&s->dev, 0, 0);
}

static void via_ac97_realize(PCIDevice *pci_dev, Error **errp)
{
    ViaAC97State *s = VIA_AC97(pci_dev);
    Object *o = OBJECT(s);

    if (!audio_be_check(&s->audio_be, errp)) {
        return;
    }

    /*
     * Command register Bus Master bit is documented to be fixed at 0 but it's
     * needed for PCI DMA to work in QEMU. The pegasos2 firmware writes 0 here
     * and the AmigaOS driver writes 1 only enabling IO bit which works on
     * real hardware. So set it here and fix it to 1 to allow DMA.
     */
    pci_set_word(pci_dev->config + PCI_COMMAND, PCI_COMMAND_MASTER);
    pci_set_word(pci_dev->wmask + PCI_COMMAND, PCI_COMMAND_IO);
    pci_set_word(pci_dev->config + PCI_STATUS,
                 PCI_STATUS_CAP_LIST | PCI_STATUS_DEVSEL_MEDIUM);
    pci_set_long(pci_dev->config + PCI_INTERRUPT_PIN, 0x03);
    pci_set_byte(pci_dev->config + 0x40, 1); /* codec ready */

    memory_region_init_io(&s->sgd, o, &sgd_ops, s, "via-ac97.sgd", 256);
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &s->sgd);
    memory_region_init_io(&s->fm, o, &fm_ops, s, "via-ac97.fm", 4);
    pci_register_bar(pci_dev, 1, PCI_BASE_ADDRESS_SPACE_IO, &s->fm);
    memory_region_init_io(&s->midi, o, &midi_ops, s, "via-ac97.midi", 4);
    pci_register_bar(pci_dev, 2, PCI_BASE_ADDRESS_SPACE_IO, &s->midi);
}

static void via_ac97_exit(PCIDevice *dev)
{
    ViaAC97State *s = VIA_AC97(dev);

    audio_be_close_out(s->audio_be, s->vo);
    audio_be_close_in(s->audio_be, s->vi);
}

static const Property via_ac97_properties[] = {
    DEFINE_AUDIO_PROPERTIES(ViaAC97State, audio_be),
};

static void via_ac97_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = via_ac97_realize;
    k->exit = via_ac97_exit;
    k->vendor_id = PCI_VENDOR_ID_VIA;
    k->device_id = PCI_DEVICE_ID_VIA_AC97;
    k->revision = 0x50;
    k->class_id = PCI_CLASS_MULTIMEDIA_AUDIO;
    device_class_set_props(dc, via_ac97_properties);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "VIA AC97";
    device_class_set_legacy_reset(dc, via_ac97_reset);
    /* Reason: Part of a south bridge chip */
    dc->user_creatable = false;
}

static const TypeInfo via_ac97_info = {
    .name          = TYPE_VIA_AC97,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(ViaAC97State),
    .class_init    = via_ac97_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void via_mc97_realize(PCIDevice *pci_dev, Error **errp)
{
    pci_set_word(pci_dev->config + PCI_COMMAND,
                 PCI_COMMAND_INVALIDATE | PCI_COMMAND_VGA_PALETTE);
    pci_set_word(pci_dev->config + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM);
    pci_set_long(pci_dev->config + PCI_INTERRUPT_PIN, 0x03);
}

static void via_mc97_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = via_mc97_realize;
    k->vendor_id = PCI_VENDOR_ID_VIA;
    k->device_id = PCI_DEVICE_ID_VIA_MC97;
    k->class_id = PCI_CLASS_COMMUNICATION_OTHER;
    k->revision = 0x30;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->desc = "VIA MC97";
    /* Reason: Part of a south bridge chip */
    dc->user_creatable = false;
}

static const TypeInfo via_mc97_info = {
    .name          = TYPE_VIA_MC97,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIDevice),
    .class_init    = via_mc97_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void via_ac97_register_types(void)
{
    type_register_static(&via_ac97_info);
    type_register_static(&via_mc97_info);
}

type_init(via_ac97_register_types)
