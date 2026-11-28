/*
 * NeXT DSP56001 and its host interface
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_AUDIO_NEXT_DSP_H
#define HW_AUDIO_NEXT_DSP_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_NEXT_DSP "next-dsp"
OBJECT_DECLARE_SIMPLE_TYPE(NeXTDSPState, NEXT_DSP)

typedef struct NeXTDMAState NeXTDMAState;
typedef struct dsp_core_s dsp_core_t;

/* Its outputs, as sysbus IRQs */
enum {
    NEXT_DSP_HREQ,      /* host request */
    NEXT_DSP_TXD,       /* SCI transmit data line */
    NEXT_DSP_NUM_IRQS
};

/* The external RAM: 8K words, or 32K with the expansion */
#define NEXT_DSP_RAM_WORDS      (8 * 1024)
#define NEXT_DSP_RAM_WORDS_MAX  (32 * 1024)

struct NeXTDSPState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    qemu_irq irq[NEXT_DSP_NUM_IRQS];
    NeXTDMAState *dma;
    bool turbo;
    bool ram_expansion;

    dsp_core_t *core;       /* the DSP56001, from hw/audio/dsp56k */
    uint32_t ram[NEXT_DSP_RAM_WORDS_MAX];
    bool mem_enabled;

    /* SCR2 options for the DSP's DMA */
    bool block_end_irq;
    bool unpacked;

    /* the DSP runs behind the CPU and catches up */
    QEMUTimer *timer;
    int64_t last_ns;
    int64_t clocks;
    bool parked;            /* waiting in place for the host or the DMA */
    uint16_t wait_pc;       /* where the DSP was, to find its wait loop */
    uint32_t wait_regs[64];
    int wait_len;
};

/* The board's controls in SCR2 */
void next_dsp_set_running(NeXTDSPState *s, bool run, int mode);
void next_dsp_set_memory(NeXTDSPState *s, bool on);
void next_dsp_set_dma_options(NeXTDSPState *s, bool block_end_irq,
                              bool unpacked);

#endif /* HW_AUDIO_NEXT_DSP_H */
