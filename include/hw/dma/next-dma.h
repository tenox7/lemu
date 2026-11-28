/*
 * NeXT integrated channel processor (Fujitsu MB610313): DMA
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_DMA_NEXT_DMA_H
#define HW_DMA_NEXT_DMA_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_NEXT_DMA "next-dma"
OBJECT_DECLARE_SIMPLE_TYPE(NeXTDMAState, NEXT_DMA)

typedef enum NeXTDMAChannel {
    NEXT_DMA_SCSI,      /* 0x010 */
    NEXT_DMA_SOUNDOUT,  /* 0x040 */
    NEXT_DMA_DISK,      /* 0x050, MO drive */
    NEXT_DMA_SOUNDIN,   /* 0x080 */
    NEXT_DMA_PRINTER,   /* 0x090 */
    NEXT_DMA_SCC,       /* 0x0c0 */
    NEXT_DMA_DSP,       /* 0x0d0 */
    NEXT_DMA_EN_TX,     /* 0x110 */
    NEXT_DMA_EN_RX,     /* 0x150 */
    NEXT_DMA_VIDEO,     /* 0x180 */
    NEXT_DMA_M2R,       /* 0x1d0 */
    NEXT_DMA_R2M,       /* 0x1c0 */
    NEXT_DMA_NUM_CHANNELS
} NeXTDMAChannel;

/* CSR bits as the CPU reads them */
#define NEXT_DMA_ENABLE      0x01000000
#define NEXT_DMA_SUPDATE     0x02000000
#define NEXT_DMA_COMPLETE    0x08000000
#define NEXT_DMA_BUSEXC      0x10000000

/* CSR bits as the CPU writes them */
#define NEXT_DMA_SETENABLE   0x00010000
#define NEXT_DMA_SETSUPDATE  0x00020000
#define NEXT_DMA_M2DEV       0x00000000
#define NEXT_DMA_DEV2M       0x00040000
#define NEXT_DMA_CLRCOMPLETE 0x00080000
#define NEXT_DMA_RESET       0x00100000
#define NEXT_DMA_INITBUF     0x00200000

/* Turbo CSR write bits */
#define NEXT_TDMA_SETCOMPLETE 0x00200000
#define NEXT_TDMA_FLUSH       0x00400000
#define NEXT_TDMA_BUFRESET    0x00800000

/* Ethernet buffer flags in Next and Limit */
#define NEXT_DMA_EN_EOP      0x80000000
#define NEXT_DMA_EN_BOP      0x40000000
#define NEXT_DMA_EN_ADDR(x)  ((x) & ~(NEXT_DMA_EN_EOP | NEXT_DMA_EN_BOP))

/* Devices buffer data in bursts of this size */
#define NEXT_DMA_BURST       16

typedef struct NeXTDMAChan {
    uint32_t csr;
    uint32_t saved_next;
    uint32_t saved_limit;
    uint32_t saved_start;
    uint32_t saved_stop;
    uint32_t next;
    uint32_t limit;
    uint32_t start;
    uint32_t stop;
    uint32_t direction;
} NeXTDMAChan;

/* Called when the CPU starts a channel or changes its buffers */
typedef void (*NeXTDMAKickFn)(void *opaque, NeXTDMAChannel ch);
/* Called when the CPU asks the channel to reset its internal buffer */
typedef void (*NeXTDMAInitBufFn)(void *opaque, NeXTDMAChannel ch,
                                 unsigned offset);

struct NeXTDMAState {
    SysBusDevice parent_obj;

    MemoryRegion csr_mem;
    MemoryRegion reg_mem;

    NeXTDMAChan ch[NEXT_DMA_NUM_CHANNELS];
    qemu_irq irq[NEXT_DMA_NUM_CHANNELS];

    NeXTDMAKickFn kick[NEXT_DMA_NUM_CHANNELS];
    NeXTDMAInitBufFn initbuf[NEXT_DMA_NUM_CHANNELS];
    void *opaque[NEXT_DMA_NUM_CHANNELS];

    QEMUTimer *m2m_timer;
    uint8_t m2m_buf[NEXT_DMA_BURST];

    bool turbo;
};

void next_dma_set_client(NeXTDMAState *s, NeXTDMAChannel ch,
                         NeXTDMAKickFn kick, NeXTDMAInitBufFn initbuf,
                         void *opaque);

static inline NeXTDMAChan *next_dma_chan(NeXTDMAState *s, NeXTDMAChannel ch)
{
    return &s->ch[ch];
}

static inline bool next_dma_enabled(NeXTDMAState *s, NeXTDMAChannel ch)
{
    return s->ch[ch].csr & NEXT_DMA_ENABLE;
}

static inline bool next_dma_dev2mem(NeXTDMAState *s, NeXTDMAChannel ch)
{
    return s->ch[ch].direction & NEXT_DMA_DEV2M;
}

/*
 * Move data between the channel's Next pointer and a device buffer.
 * They advance Next and return false after a bus error, which also stops
 * the channel. They do not stop at Limit: callers check it.
 */
bool next_dma_write_mem(NeXTDMAState *s, NeXTDMAChannel ch,
                        const void *buf, uint32_t len);
bool next_dma_read_mem(NeXTDMAState *s, NeXTDMAChannel ch,
                       void *buf, uint32_t len);

/*
 * The channel finished a part of its work: when Next reached Limit, mark it
 * complete and chain to Start/Stop, or stop it. Raise the interrupt if it
 * is complete.
 */
void next_dma_update(NeXTDMAState *s, NeXTDMAChannel ch);

/* The same for the Ethernet channels, which complete per buffer */
void next_dma_enet_done(NeXTDMAState *s, NeXTDMAChannel ch);

/* Vertical retrace: the video channel interrupts through its Limit */
void next_dma_video_retrace(NeXTDMAState *s);

#endif /* HW_DMA_NEXT_DMA_H */
