/*
 * NeXT laser printer interface
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_CHAR_NEXT_PRINTER_H
#define HW_CHAR_NEXT_PRINTER_H

#include "hw/core/sysbus.h"
#include "hw/dma/next-dma.h"
#include "qom/object.h"

#define TYPE_NEXT_PRINTER "next-printer"
OBJECT_DECLARE_SIMPLE_TYPE(NeXTPrinterState, NEXT_PRINTER)

struct NeXTPrinterState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    qemu_irq irq;
    NeXTDMAState *dma;
    QEMUTimer *timer;

    bool connected;
    char *pages_dir;

    uint8_t csr[4];
    uint8_t command;
    uint32_t data;
    uint8_t stat;
    uint8_t statmask;
    uint32_t margins;
    uint8_t serial_status[16];
    uint8_t serial_phase;
    int32_t copyright;
    uint8_t gpo_cmd;
    uint8_t gpo_stat;
    uint8_t gpo_old;

    bool transfer;
    uint32_t buf_size;
    uint8_t buf[4096];

    /* the page being printed */
    uint8_t *page;
    uint32_t page_width;
    uint32_t page_size;
    uint32_t page_count;
    int dpi;
};

#endif /* HW_CHAR_NEXT_PRINTER_H */
