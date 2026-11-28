/*
 * NeXT keyboard, mouse and sound interface (KMS)
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_INPUT_NEXT_KMS_H
#define HW_INPUT_NEXT_KMS_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "ui/input.h"

#define TYPE_NEXT_KMS "next-kms"
#define NEXT_KMS_QUEUE 64
OBJECT_DECLARE_SIMPLE_TYPE(NeXTKMSState, NEXT_KMS)

enum {
    NEXT_KMS_IRQ_KEYMOUSE,
    NEXT_KMS_IRQ_MONITOR,
    NEXT_KMS_IRQ_SOUND_OVRUN,
    NEXT_KMS_IRQ_NMI,
    NEXT_KMS_NUM_IRQS
};

/* Sound commands from the CPU to the monitor */
#define KMSCMD_ASNDOUT      0xC7    /* one sample, in the data */
#define KMSCMD_CTRLOUT      0xC4    /* volume control logic */
#define KMSCMD_VOLCTRL      0xC2    /* volume, on newer monitors */
#define KMSCMD_SND_IN       0x03
#define KMSCMD_SND_OUT      0x07
#define KMSCMD_SIO_MASK     0xC7
#define KMSCMD_SIO_ENABLE   0x08
#define KMSCMD_SIO_DBL_SMPL 0x10
#define KMSCMD_SIO_ZERO     0x20

/* A sound command for the monitor: command 0 stops all sound */
typedef void (*NeXTKMSSoundCommand)(void *opaque, uint8_t command,
                                    uint32_t data);
/* The monitor asks for more sound */
typedef void (*NeXTKMSSoundRequest)(void *opaque);
typedef void (*NeXTKMSPowerKey)(void *opaque, bool down);

struct NeXTKMSState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    qemu_irq irq[NEXT_KMS_NUM_IRQS];
    QemuInputHandlerState *hs;

    bool turbo;
    bool input;     /* the keyboard and mouse are on this link */

    uint8_t status_snd;
    uint8_t status_km;
    uint8_t status_tx;
    uint8_t status_cmd;
    uint8_t command;
    uint32_t data;
    uint32_t kmdata;

    uint8_t rev;
    uint32_t km_addr;
    uint32_t km_data[16];
    uint32_t km_mask;

    /* keyboard and mouse events the CPU has not read yet */
    uint32_t queue[NEXT_KMS_QUEUE];
    uint32_t q_head;
    uint32_t q_len;

    uint8_t modifiers;
    bool button_left;
    bool button_right;
    bool buttons_changed;
    int dx, dy;

    NeXTKMSSoundCommand sound_command;
    NeXTKMSSoundRequest sound_request;
    void *sound_opaque;
    NeXTKMSPowerKey power_key;
    void *power_opaque;
};

void next_kms_set_sound(NeXTKMSState *s, NeXTKMSSoundCommand command,
                        NeXTKMSSoundRequest request, void *opaque);
void next_kms_set_power_key(NeXTKMSState *s, NeXTKMSPowerKey fn,
                            void *opaque);

/*
 * From the sound hardware. The request and the codec data go only over an
 * enabled link: they return false when it is off.
 */
bool next_kms_sound_request(NeXTKMSState *s);
void next_kms_sound_underrun(NeXTKMSState *s);
void next_kms_sound_in_overrun(NeXTKMSState *s);
bool next_kms_codec_in(NeXTKMSState *s);

#endif /* HW_INPUT_NEXT_KMS_H */
