/*
 * NeXT sound: 16-bit stereo output and mu-law input through the monitor
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Based on the Previous emulator's snd.c, kms.c and dma.c
 * (GPL-2.0-or-later).
 *
 * The sound hardware is in the monitor (or the sound box) at the other
 * end of the KMS link. To play, the CPU tells the monitor to start; the
 * monitor then asks for samples, and each request takes the next block
 * from memory through the sound out DMA channel and ends the block before
 * it. Samples are 16-bit big-endian stereo at 44.1 kHz, or at 22.05 kHz
 * with every sample doubled by repetition or by a zero. When a request
 * finds no data the monitor reports an underrun.
 *
 * Recording is by the CODEC in the monitor: 8012 Hz mu-law, four samples
 * at a time, into memory through the sound in channel.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/audio/next-sound.h"
#include "hw/dma/next-dma.h"
#include "hw/input/next-kms.h"
#include "migration/vmstate.h"

#define OUT_FREQ        44100
#define IN_FREQ         8012
#define OUT_FRAME_NS    (NANOSECONDS_PER_SECOND / OUT_FREQ)
#define IN_SAMPLE_NS    (NANOSECONDS_PER_SECOND / IN_FREQ)
#define IN_BLOCK        256         /* samples at a time */

/* Keep this much for the host; take blocks faster below the low mark */
#define RING_HIGH       (16 * 1024)
#define RING_LOW        (4 * 1024)

#define MODE_NORMAL     0x00
#define MODE_DBL_RP     0x10
#define MODE_DBL_ZF     0x30

/* Volume control logic, from KMSCMD_CTRLOUT */
#define SPEAKER_ENABLE  0x10        /* really: mute the speaker */
#define LOWPASS_ENABLE  0x08
#define INTFC_CLOCK     0x04
#define INTFC_DATA      0x02
#define INTFC_STROBE    0x01

#define MAX_ATTEN       43          /* steps of 2 dB */

/* ---- output ---- */

static double next_sound_volume_factor(uint8_t atten)
{
    switch (atten) {
    case 0:
        return 1.0;
    case MAX_ATTEN:
        return 0.0;
    default:
        return pow(10.0, atten * -2.0 * 0.05);
    }
}

/* De-emphasis for 44.1 kHz pre-emphasised CD audio, as in Previous */
static double next_sound_deemph(NeXTSoundState *s, double in, int c)
{
    static const double a1 = -0.62786881719628784282;
    static const double b0 =  0.45995451989513153057;
    static const double b1 = -0.08782333709141937339;
    double out = in * b0 + s->deemph_li[c] * b1 - s->deemph_lo[c] * a1;

    s->deemph_li[c] = in;
    s->deemph_lo[c] = out;
    return out;
}

static int16_t clip16(double v)
{
    v += v < 0.0 ? -0.5 : 0.5;
    return v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : (int16_t)v;
}

static void next_sound_ring_put(NeXTSoundState *s, const uint8_t *buf,
                                size_t len)
{
    if (!s->voice_out) {
        return;
    }
    len = MIN(len, NEXT_SOUND_RING - s->ring_len);
    for (size_t i = 0; i < len; i++) {
        s->ring[(s->ring_head + s->ring_len + i) % NEXT_SOUND_RING] = buf[i];
    }
    s->ring_len += len;
}

/* Turn len bytes of samples into what the host plays; returns frames */
static int next_sound_play(NeXTSoundState *s, const uint8_t *in, size_t len)
{
    uint8_t out[NEXT_SOUND_DMA_MAX * 2];
    size_t n = 0;

    len &= ~3;
    for (size_t i = 0; i < len; i += 4) {
        memcpy(out + n, in + i, 4);
        n += 4;
        if (s->mode == MODE_DBL_RP) {
            memcpy(out + n, in + i, 4);
            n += 4;
        } else if (s->mode == MODE_DBL_ZF) {
            memset(out + n, 0, 4);
            n += 4;
        }
    }

    if (s->mute) {
        memset(out, 0, n);
    } else if (s->attenuation[0] || s->attenuation[1] || s->deemph) {
        for (size_t i = 0; i < n; i += 4) {
            double l = (int16_t)lduw_be_p(out + i);
            double r = (int16_t)lduw_be_p(out + i + 2);

            if (s->deemph) {
                l = next_sound_deemph(s, l, 0);
                r = next_sound_deemph(s, r, 1);
            }
            stw_be_p(out + i, clip16(l * s->volume[0]));
            stw_be_p(out + i + 2, clip16(r * s->volume[1]));
        }
    }
    next_sound_ring_put(s, out, n);
    return n / 4;
}

/* The monitor asks for samples: end the last block, take the next */
static void next_sound_request(void *opaque)
{
    NeXTSoundState *s = opaque;
    NeXTDMAChan *c = next_dma_chan(s->dma, NEXT_DMA_SOUNDOUT);

    s->fetched = true;
    if (s->dma_len) {
        s->dma_len = 0;
        if (next_dma_enabled(s->dma, NEXT_DMA_SOUNDOUT)) {
            next_dma_update(s->dma, NEXT_DMA_SOUNDOUT);
        }
    }
    if (!next_dma_enabled(s->dma, NEXT_DMA_SOUNDOUT)) {
        return;
    }
    c->next &= ~3;
    c->limit &= ~3;
    if (c->next < c->limit) {
        uint32_t len = MIN(c->limit - c->next, NEXT_SOUND_DMA_MAX);

        if (next_dma_read_mem(s->dma, NEXT_DMA_SOUNDOUT, s->dma_buf, len)) {
            s->dma_len = len;
        }
    }
}

static void next_sound_out_tick(void *opaque)
{
    NeXTSoundState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t delay;
    int frames;

    if (!s->out_active) {
        return;
    }
    /* wait while the host has enough */
    if (s->voice_out && s->ring_len >= RING_HIGH) {
        timer_mod(s->out_timer,
                  now + (s->ring_len - RING_HIGH + 4) / 4 * OUT_FRAME_NS);
        return;
    }

    s->fetched = false;
    if (next_kms_sound_request(s->kms) && s->fetched && s->dma_len) {
        frames = next_sound_play(s, s->dma_buf, s->dma_len);
        delay = frames * OUT_FRAME_NS;
        if (s->voice_out && s->ring_len < RING_LOW) {
            delay /= 3;     /* catch up with the host */
        }
    } else {
        next_kms_sound_underrun(s->kms);
        delay = 100 * SCALE_US;
    }
    timer_mod(s->out_timer, now + delay);
}

static void next_sound_out_callback(void *opaque, int avail)
{
    NeXTSoundState *s = opaque;

    while (avail > 0 && s->ring_len) {
        size_t n = MIN(MIN((size_t)avail, s->ring_len),
                       NEXT_SOUND_RING - s->ring_head);

        n = audio_be_write(s->audio_be, s->voice_out, s->ring + s->ring_head, n);
        if (!n) {
            break;
        }
        s->ring_head = (s->ring_head + n) % NEXT_SOUND_RING;
        s->ring_len -= n;
        avail -= n;
    }
}

/* The volume register: 3 start bits, the channels, 6 bits of volume */
static void next_sound_set_volume(NeXTSoundState *s, uint16_t reg)
{
    uint8_t chan = (reg & 0xC0) >> 6;
    uint8_t atten = MIN(reg & 0x3F, MAX_ATTEN);

    if (chan & 1) {
        s->attenuation[0] = atten;
        s->volume[0] = next_sound_volume_factor(atten);
    }
    if (chan & 2) {
        s->attenuation[1] = atten;
        s->volume[1] = next_sound_volume_factor(atten);
    }
}

static void next_sound_gpo(NeXTSoundState *s, uint8_t data)
{
    s->mute = data & SPEAKER_ENABLE;
    s->deemph = data & LOWPASS_ENABLE;

    if (data & INTFC_STROBE) {
        if (s->vol_bits == 11 && ((s->vol_shift >> 8) & 7) == 7) {
            next_sound_set_volume(s, s->vol_shift);
        }
    } else if (s->gpo & INTFC_STROBE) {
        s->vol_bits = 0;
        s->vol_shift = 0;
    } else if ((data & INTFC_CLOCK) && !(s->gpo & INTFC_CLOCK)) {
        s->vol_shift = (s->vol_shift << 1) | !!(data & INTFC_DATA);
        s->vol_bits++;
    }
    s->gpo = data;
}

static void next_sound_start_output(NeXTSoundState *s, uint8_t mode)
{
    s->mode = mode;
    if (!s->out_active) {
        s->out_active = true;
        memset(s->deemph_li, 0, sizeof(s->deemph_li));
        memset(s->deemph_lo, 0, sizeof(s->deemph_lo));
        s->ring_len = 0;
    }
    if (s->voice_out) {
        audio_be_set_active_out(s->audio_be, s->voice_out, true);
    }
    timer_mod(s->out_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_US);
}

static void next_sound_stop_output(NeXTSoundState *s)
{
    s->out_active = false;
    timer_del(s->out_timer);
}

/* ---- input ---- */

/* 16-bit linear to mu-law, as in Previous */
static uint8_t next_sound_ulaw(int16_t sample)
{
    static const uint8_t exp_lut[256] = {
        0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
        5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
        6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
        6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
        6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
        6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    };
    int v = sample;
    uint8_t sign = v < 0 ? 0x80 : 0;
    uint8_t exponent, mantissa;

    if (sign) {
        v = -v;
    }
    if (v > 32635) {
        v = 32635;
    }
    v += 0x84;
    exponent = exp_lut[(v >> 7) & 0xFF];
    mantissa = (v >> (exponent + 3)) & 0x0F;
    return ~(sign | (exponent << 4) | mantissa);
}

/* Four samples into memory; returns false when recording must stop */
static bool next_sound_codec_word(NeXTSoundState *s, uint32_t word)
{
    NeXTDMAChan *c = next_dma_chan(s->dma, NEXT_DMA_SOUNDIN);
    uint8_t buf[4];
    bool end = true;

    if (!next_kms_codec_in(s->kms)) {
        return true;
    }
    if (next_dma_enabled(s->dma, NEXT_DMA_SOUNDIN)) {
        if (c->next < c->limit) {
            stl_be_p(buf, word);
            next_dma_write_mem(s->dma, NEXT_DMA_SOUNDIN, buf, 4);
        }
        c->saved_limit = c->next;
        end = c->next == c->limit;
    }
    if (end) {
        if (next_dma_enabled(s->dma, NEXT_DMA_SOUNDIN)) {
            next_dma_update(s->dma, NEXT_DMA_SOUNDIN);
        }
        if (!next_dma_enabled(s->dma, NEXT_DMA_SOUNDIN)) {
            next_kms_sound_in_overrun(s->kms);
            return false;
        }
    }
    return true;
}

static void next_sound_in_tick(void *opaque)
{
    NeXTSoundState *s = opaque;
    int16_t samples[IN_BLOCK];
    size_t got = 0;

    if (!s->in_active) {
        return;
    }
    if (!next_dma_enabled(s->dma, NEXT_DMA_SOUNDIN)) {
        next_kms_sound_in_overrun(s->kms);
        return;
    }
    if (s->voice_in) {
        got = audio_be_read(s->audio_be, s->voice_in, samples,
                            sizeof(samples)) / 2;
    }
    /* without enough from the host, the rest is silence */
    memset(samples + got, 0, (IN_BLOCK - got) * 2);

    for (int i = 0; i < IN_BLOCK; i++) {
        s->in_word = (s->in_word << 8) | next_sound_ulaw(samples[i]);
        if (++s->in_count == 4) {
            s->in_count = 0;
            if (!next_sound_codec_word(s, s->in_word)) {
                return;
            }
        }
    }
    timer_mod(s->in_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              IN_BLOCK * IN_SAMPLE_NS);
}

static void next_sound_in_callback(void *opaque, int avail)
{
    /* the input timer takes the samples */
}

static void next_sound_start_input(NeXTSoundState *s)
{
    if (!s->in_active) {
        s->in_active = true;
        s->in_count = 0;
    }
    if (s->voice_in) {
        audio_be_set_active_in(s->audio_be, s->voice_in, true);
    }
    timer_mod(s->in_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_US);
}

static void next_sound_stop_input(NeXTSoundState *s)
{
    s->in_active = false;
    timer_del(s->in_timer);
    if (s->voice_in) {
        audio_be_set_active_in(s->audio_be, s->voice_in, false);
    }
}

/* ---- commands from the CPU ---- */

static void next_sound_command(void *opaque, uint8_t command, uint32_t data)
{
    NeXTSoundState *s = opaque;
    uint8_t sample[4];

    switch (command) {
    case 0:     /* the monitor was reset */
        next_sound_stop_output(s);
        next_sound_stop_input(s);
        return;
    case KMSCMD_ASNDOUT:
        if (s->out_active) {
            stl_be_p(sample, data);
            next_sound_play(s, sample, 4);
        }
        return;
    case KMSCMD_CTRLOUT:
        next_sound_gpo(s, data >> 24);
        return;
    case KMSCMD_VOLCTRL:
        next_sound_set_volume(s, data >> 24);
        return;
    }

    if ((command & KMSCMD_SIO_MASK) == KMSCMD_SND_OUT) {
        if (command & KMSCMD_SIO_ENABLE) {
            next_sound_start_output(s, command & (KMSCMD_SIO_DBL_SMPL |
                                                  KMSCMD_SIO_ZERO));
        } else {
            next_sound_stop_output(s);
        }
    } else if ((command & KMSCMD_SIO_MASK) == KMSCMD_SND_IN) {
        if (command & KMSCMD_SIO_ENABLE) {
            next_sound_start_input(s);
        } else {
            next_sound_stop_input(s);
        }
    }
}

/* ---- device ---- */

static void next_sound_realize(DeviceState *dev, Error **errp)
{
    NeXTSoundState *s = NEXT_SOUND(dev);
    struct audsettings out_as = { OUT_FREQ, 2, AUDIO_FORMAT_S16, 1 };
    struct audsettings in_as = { IN_FREQ, 1, AUDIO_FORMAT_S16,
                                 HOST_BIG_ENDIAN };

    if (!s->dma || !s->kms) {
        error_setg(errp, "next-sound: the dma and kms links are needed");
        return;
    }
    if (s->audio_be) {
        if (!audio_be_check(&s->audio_be, errp)) {
            return;
        }
    } else {
        Error *err = NULL;

        /* without a usable default the sound plays to nobody */
        if (!audio_be_check(&s->audio_be, &err)) {
            error_free(err);
            s->audio_be = NULL;
        }
    }
    if (s->audio_be) {
        s->voice_out = audio_be_open_out(s->audio_be, NULL, "next-sound.out",
                                         s, next_sound_out_callback, &out_as);
        s->voice_in = audio_be_open_in(s->audio_be, NULL, "next-sound.in",
                                       s, next_sound_in_callback, &in_as);
    }
    s->out_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, next_sound_out_tick, s);
    s->in_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, next_sound_in_tick, s);
    next_kms_set_sound(s->kms, next_sound_command, next_sound_request, s);
}

static void next_sound_reset_hold(Object *obj, ResetType type)
{
    NeXTSoundState *s = NEXT_SOUND(obj);

    next_sound_stop_output(s);
    next_sound_stop_input(s);
    s->dma_len = 0;
    s->ring_len = 0;
    s->mode = MODE_NORMAL;
    s->volume[0] = next_sound_volume_factor(s->attenuation[0]);
    s->volume[1] = next_sound_volume_factor(s->attenuation[1]);
}

static int next_sound_post_load(void *opaque, int version_id)
{
    NeXTSoundState *s = opaque;

    s->volume[0] = next_sound_volume_factor(s->attenuation[0]);
    s->volume[1] = next_sound_volume_factor(s->attenuation[1]);
    return 0;
}

static const VMStateDescription next_sound_vmstate = {
    .name = "next-sound",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = next_sound_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(out_active, NeXTSoundState),
        VMSTATE_UINT8(mode, NeXTSoundState),
        VMSTATE_BOOL(mute, NeXTSoundState),
        VMSTATE_BOOL(deemph, NeXTSoundState),
        VMSTATE_UINT8_ARRAY(attenuation, NeXTSoundState, 2),
        VMSTATE_UINT16(vol_shift, NeXTSoundState),
        VMSTATE_INT32(vol_bits, NeXTSoundState),
        VMSTATE_UINT8(gpo, NeXTSoundState),
        VMSTATE_UINT8_ARRAY(dma_buf, NeXTSoundState, NEXT_SOUND_DMA_MAX),
        VMSTATE_UINT32(dma_len, NeXTSoundState),
        VMSTATE_BOOL(in_active, NeXTSoundState),
        VMSTATE_UINT32(in_word, NeXTSoundState),
        VMSTATE_UINT32(in_count, NeXTSoundState),
        VMSTATE_TIMER_PTR(out_timer, NeXTSoundState),
        VMSTATE_TIMER_PTR(in_timer, NeXTSoundState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property next_sound_properties[] = {
    DEFINE_AUDIO_PROPERTIES(NeXTSoundState, audio_be),
    DEFINE_PROP_LINK("dma", NeXTSoundState, dma, TYPE_NEXT_DMA,
                     NeXTDMAState *),
    DEFINE_PROP_LINK("kms", NeXTSoundState, kms, TYPE_NEXT_KMS,
                     NeXTKMSState *),
};

static void next_sound_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "NeXT sound";
    dc->realize = next_sound_realize;
    dc->vmsd = &next_sound_vmstate;
    dc->user_creatable = false;
    rc->phases.hold = next_sound_reset_hold;
    device_class_set_props(dc, next_sound_properties);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
}

static const TypeInfo next_sound_info = {
    .name = TYPE_NEXT_SOUND,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NeXTSoundState),
    .class_init = next_sound_class_init,
};

static void next_sound_register_types(void)
{
    type_register_static(&next_sound_info);
}

type_init(next_sound_register_types)
