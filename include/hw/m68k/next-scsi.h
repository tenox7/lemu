/*
 * NeXT SCSI: NCR53C90(A) with the NeXT DMA control and status registers
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_M68K_NEXT_SCSI_H
#define HW_M68K_NEXT_SCSI_H

#include "hw/core/sysbus.h"
#include "hw/scsi/esp.h"
#include "hw/dma/next-dma.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_NEXT_SCSI "next-scsi"
#define NEXT_SCSI_QUEUE (128 * 1024)
OBJECT_DECLARE_SIMPLE_TYPE(NeXTSCSIState, NEXT_SCSI)

/*
 * The floppy controller shares the SCSI DMA channel. When it is selected,
 * the channel takes its data from the floppy buffer instead of the ESP.
 */
typedef struct NeXTSCSIFloppyOps {
    /* bytes the floppy has for memory */
    uint32_t (*avail)(void *opaque);
    uint8_t (*get)(void *opaque);
    /* room the floppy has for bytes from memory */
    uint32_t (*room)(void *opaque);
    void (*put)(void *opaque, uint8_t val);
} NeXTSCSIFloppyOps;

struct NeXTSCSIState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    SysBusESPState esp;
    NeXTDMAState *dma;
    qemu_irq irq;

    uint8_t control;
    uint8_t status;
    bool esp_irq;
    bool esp_irq_out;
    QEMUTimer *irq_timer;
    bool esp_dma;

    /* bytes from the ESP waiting for the channel */
    uint8_t q[NEXT_SCSI_QUEUE];
    uint32_t q_head;
    uint32_t q_len;

    /* the SCSI channel's burst buffer */
    uint8_t buf[NEXT_DMA_BURST];
    int32_t buf_size;
    int32_t buf_limit;

    bool in_pump;
    bool pump_again;

    const NeXTSCSIFloppyOps *floppy_ops;
    void *floppy_opaque;
    bool floppy_select;
    bool is_53c90a;
};

void next_scsi_set_floppy(NeXTSCSIState *s, const NeXTSCSIFloppyOps *ops,
                          void *opaque);
void next_scsi_select_floppy(NeXTSCSIState *s, bool floppy);
/* Move data through the channel now */
void next_scsi_dma_run(NeXTSCSIState *s);

#endif /* HW_M68K_NEXT_SCSI_H */
