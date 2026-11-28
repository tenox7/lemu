/*
 * NeXT magneto-optical disk controller and drives
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_BLOCK_NEXT_MO_H
#define HW_BLOCK_NEXT_MO_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_NEXT_MO "next-mo"
OBJECT_DECLARE_SIMPLE_TYPE(NeXTMOState, NEXT_MO)

typedef struct NeXTDMAState NeXTDMAState;
typedef struct BlockBackend BlockBackend;

#define NEXT_MO_DRIVES          2
#define NEXT_MO_SECTOR_DISK     1296    /* a sector on the disk, with ECC */
#define NEXT_MO_SECTOR_DATA     1024    /* a sector as software sees it */
#define NEXT_MO_DMA_BURST       16

typedef struct NeXTMOBuffer {
    uint8_t data[NEXT_MO_SECTOR_DISK];
    uint32_t size;
    uint32_t limit;
} NeXTMOBuffer;

typedef struct NeXTMODrive {
    BlockBackend *blk;

    uint16_t status;
    uint16_t dstat;
    uint16_t estat;
    uint16_t hstat;

    uint8_t head;
    uint32_t head_pos;
    uint32_t ho_head_pos;
    uint32_t sec_offset;

    bool spinning;
    bool spiraling;
    bool seeking;
    bool attn;
    bool complete;

    bool protect;
    bool inserted;
    bool enabled;
    bool connected;
} NeXTMODrive;

struct NeXTMOState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    qemu_irq irq;
    NeXTDMAState *dma;

    /* the optical storage processor's registers */
    uint8_t tracknumh;
    uint8_t tracknuml;
    uint8_t sector_num;
    uint8_t sector_incr;
    int32_t sector_count;
    uint8_t intstatus;
    uint8_t intmask;
    uint8_t csr2;
    uint8_t csr1;
    uint8_t csrh;
    uint8_t csrl;
    uint8_t err_stat;
    uint8_t ecc_cnt;
    uint8_t init;
    uint8_t format;
    uint8_t mark;
    uint8_t reg0f;
    uint8_t flag[7];

    /* formatter */
    int32_t fmt_mode;
    bool write_timing;
    int32_t sector_timer;

    /* ECC: two buffers, filled and drained in turn */
    NeXTMOBuffer ecc_buf[2];
    int32_t eccin;
    int32_t eccout;
    int32_t ecc_mode;
    int32_t ecc_state;
    bool ecc_repeat;

    /* the DMA channel's burst buffer */
    uint8_t dma_buf[NEXT_MO_DMA_BURST];
    int32_t dma_size;
    int32_t dma_limit;

    /* drives */
    NeXTMODrive drive[NEXT_MO_DRIVES];
    int32_t dnum;

    /* a drive's signals that come after a while */
    bool delayed_compl;
    bool delayed_attn;
    int32_t delayed_drive;

    /* a command started, and the CPU has not yet seen it in progress */
    bool busy_unseen;
    int64_t busy_deadline;

    QEMUTimer *signal_timer;
    QEMUTimer *io_timer;
    QEMUTimer *ecc_timer;
};

#endif /* HW_BLOCK_NEXT_MO_H */
