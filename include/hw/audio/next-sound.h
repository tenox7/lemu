/*
 * NeXT sound: 16-bit stereo output and mu-law input through the monitor
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_AUDIO_NEXT_SOUND_H
#define HW_AUDIO_NEXT_SOUND_H

#include "hw/core/sysbus.h"
#include "qemu/audio.h"
#include "qom/object.h"

#define TYPE_NEXT_SOUND "next-sound"
OBJECT_DECLARE_SIMPLE_TYPE(NeXTSoundState, NEXT_SOUND)

typedef struct NeXTDMAState NeXTDMAState;
typedef struct NeXTKMSState NeXTKMSState;

#define NEXT_SOUND_DMA_MAX  2048    /* bytes taken from memory at a time */
#define NEXT_SOUND_RING     32768   /* bytes waiting for the host */

struct NeXTSoundState {
    SysBusDevice parent_obj;

    NeXTDMAState *dma;
    NeXTKMSState *kms;
    AudioBackend *audio_be;
    SWVoiceOut *voice_out;
    SWVoiceIn *voice_in;
    QEMUTimer *out_timer;
    QEMUTimer *in_timer;

    /* output */
    bool out_active;
    uint8_t mode;
    bool mute;
    bool deemph;
    uint8_t attenuation[2];
    double volume[2];
    double deemph_li[2];
    double deemph_lo[2];
    uint16_t vol_shift;
    int32_t vol_bits;
    uint8_t gpo;

    /* the last block taken from memory, until the next request */
    uint8_t dma_buf[NEXT_SOUND_DMA_MAX];
    uint32_t dma_len;
    bool fetched;

    /* samples for the host, big-endian stereo */
    uint8_t ring[NEXT_SOUND_RING];
    uint32_t ring_head;
    uint32_t ring_len;

    /* input */
    bool in_active;
    uint32_t in_word;
    uint32_t in_count;
};

#endif
