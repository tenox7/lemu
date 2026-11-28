/*
 * NeXT floppy disk controller: Intel 82077AA with the NeXT control register
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_BLOCK_NEXT_FDC_H
#define HW_BLOCK_NEXT_FDC_H

#include "hw/core/sysbus.h"
#include "hw/m68k/next-scsi.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_NEXT_FDC "next-fdc"
OBJECT_DECLARE_SIMPLE_TYPE(NeXTFDCState, NEXT_FDC)

#define NEXT_FDC_BUF_SIZE 1024

struct NeXTFDCState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    qemu_irq irq;
    BlockBackend *blk;
    NeXTSCSIState *scsi;
    QEMUTimer *timer;

    bool turbo;
    bool station;
    bool color;
    uint8_t reserved3;
    uint8_t reserved6;

    /* 82077 */
    uint8_t sra;
    uint8_t srb;
    uint8_t dor;
    uint8_t msr;
    uint8_t dsr;
    uint8_t fifo[16];
    uint8_t din;
    uint8_t ccr;
    uint8_t csr;
    uint8_t st[4];
    uint8_t pcn;
    uint8_t sel;
    uint8_t eis;

    int32_t io_state;
    int32_t sector_counter;

    bool cmd_phase;
    int32_t cmd_size;
    int32_t cmd_limit;
    uint8_t command;
    uint8_t cmd_data[8];
    int32_t result_size;

    /* the drive */
    uint32_t disk_size;
    bool protect;
    uint8_t cyl;
    uint8_t head;
    uint8_t sector;
    uint8_t blocksize;
    uint32_t seekoffset;
    bool spinning;

    uint32_t buf_size;
    uint32_t buf_limit;
    uint8_t buf[NEXT_FDC_BUF_SIZE];
};

#endif /* HW_BLOCK_NEXT_FDC_H */
