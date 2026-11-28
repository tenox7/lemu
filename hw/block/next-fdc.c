/*
 * NeXT floppy disk controller: Intel 82077AA with the NeXT control register
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Based on the Previous emulator's floppy.c (GPL-2.0-or-later).
 *
 * The 68040 machines have a 2.88 MB drive on an 82077 at 0x02014100, with
 * a NeXT control and status register at 0x02014108 that selects the
 * controller for the SCSI DMA channel, ejects the disk and tells the media
 * size. The controller only does what NeXT software asks of it: read,
 * write, format, read ID, seek and recalibrate.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/block/next-fdc.h"
#include "migration/vmstate.h"
#include "system/block-backend.h"

#define SRA_INT         0x80
#define SRA_STEP        0x20
#define SRA_TRK0_N      0x10
#define SRA_HDSEL       0x08
#define SRA_WP_N        0x02
#define SRA_DIR         0x01

#define SRB_ALL1        0xC0
#define SRB_DRVSEL0_N   0x20
#define SRB_W_TOGGLE    0x10
#define SRB_R_TOGGLE    0x08
#define SRB_MOTEN_MASK  0x03

#define DOR_MOT0EN      0x10
#define DOR_MOTEN_MASK  0xF0
#define DOR_RESET_N     0x04
#define DOR_SEL_MSK     0x03

#define MSR_RQM         0x80
#define MSR_DIO         0x40
#define MSR_BSY_MASK    0x0F

#define DSR_RESET       0x80

#define DIR_HIDENS_N    0x01

#define CCR_RATE_MASK   0x03
#define CCR_RATE250     0x02
#define CCR_RATE500     0x00
#define CCR_RATE1000    0x03

#define CTRL_EJECT      0x80
#define CTRL_82077      0x40
#define CTRL_RESET      0x20
#define CTRL_DRV_ID     0x04

#define ST0_SE          0x20
#define ST1_EN          0x80
#define ST1_OR          0x10
#define ST1_ND          0x04
#define ST1_NW          0x02
#define ST2_WC          0x10

#define IC_NORMAL       0x00
#define IC_ABNORMAL     0x40
#define IC_INV_CMD      0x80

#define CMD_OPCODE_MSK  0x1F
#define CMD_READ        0x06
#define CMD_WRITE       0x05
#define CMD_FORMAT      0x0D
#define CMD_RECAL       0x07
#define CMD_INTSTAT     0x08
#define CMD_SPECIFY     0x03
#define CMD_SEEK        0x0F
#define CMD_CONFIGURE   0x13
#define CMD_READ_ID     0x0A
#define CMD_PERPEND     0x12

#define SIZE_720K       737280
#define SIZE_1440K      1474560
#define SIZE_2880K      2949120

#define NUM_CYLINDERS   80
#define TRACKS_PER_CYL  2
#define FLP_SEEK_TIME   200000  /* us */
#define FLP_NEXT_SECTOR_TIME 20 /* us */

#define MEDIA_ID_NONE   0
#define MEDIA_ID_720K   3
#define MEDIA_ID_1440K  2
#define MEDIA_ID_2880K  1

enum {
    FLP_STATE_WRITE,
    FLP_STATE_READ,
    FLP_STATE_FORMAT,
    FLP_STATE_INTERRUPT,
    FLP_STATE_DONE,
};

static const uint8_t cmd_data_size[32] = {
    0, 0, 8, 2, 1, 8, 8, 1,
    0, 8, 1, 0, 8, 5, 0, 2,
    0, 8, 1, 3, 0, 0, 8, 0,
    0, 8, 0, 0, 0, 8, 0, 0
};

static bool next_fdc_inserted(NeXTFDCState *s)
{
    return s->blk && blk_is_inserted(s->blk) && s->disk_size;
}

static void next_fdc_set_irq(NeXTFDCState *s, bool level)
{
    qemu_set_irq(s->irq, level);
}

static void next_fdc_schedule(NeXTFDCState *s, int64_t us)
{
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              us * SCALE_US);
}

static void next_fdc_stop(NeXTFDCState *s)
{
    s->sra &= ~SRA_INT;
    next_fdc_set_irq(s, false);
    s->io_state = FLP_STATE_DONE;
    timer_del(s->timer);
}

static void next_fdc_start(NeXTFDCState *s)
{
    s->sra &= ~(SRA_INT | SRA_STEP | SRA_HDSEL | SRA_DIR);
    s->srb &= ~(SRB_R_TOGGLE | SRB_W_TOGGLE);
    s->msr &= ~MSR_DIO;
    s->msr |= MSR_RQM;
    next_fdc_set_irq(s, false);
    /* one poll interrupt after a reset */
    s->io_state = FLP_STATE_INTERRUPT;
    next_fdc_schedule(s, 1000 * 1000);
}

static void next_fdc_reset_ctrl(NeXTFDCState *s)
{
    s->dor = s->sel = 0;
    s->sra &= ~(SRA_INT | SRA_STEP | SRA_HDSEL | SRA_DIR);
    s->srb &= ~(SRB_R_TOGGLE | SRB_W_TOGGLE | SRB_MOTEN_MASK |
                SRB_DRVSEL0_N);
    s->din &= ~DIR_HIDENS_N;
    memset(s->st, 0, sizeof(s->st));
    s->pcn = 0;
    next_fdc_stop(s);
}

static void next_fdc_interrupt(NeXTFDCState *s)
{
    s->msr |= MSR_RQM;
    if (s->result_size > 0) {
        s->msr |= MSR_DIO;
    }
    s->msr &= ~MSR_BSY_MASK;
    s->sra |= SRA_INT;
    next_fdc_set_irq(s, true);
}

/* ---- helpers ---- */

static uint32_t next_fdc_logical_sec(NeXTFDCState *s)
{
    uint32_t blocksize = 0x80 << s->blocksize;
    uint32_t spt = s->disk_size / blocksize / TRACKS_PER_CYL / NUM_CYLINDERS;

    if (s->sector > spt) {
        s->st[0] |= IC_ABNORMAL;
        s->st[1] |= ST1_EN;
    }
    if (s->cyl >= NUM_CYLINDERS) {
        s->st[0] |= IC_ABNORMAL;
        s->st[1] |= ST1_ND;
    }
    return ((s->cyl * TRACKS_PER_CYL + s->head) * spt) + s->sector - 1;
}

static void next_fdc_check_blocksize(NeXTFDCState *s, uint8_t bs)
{
    if (bs != s->blocksize) {
        s->st[0] |= IC_ABNORMAL;
        s->st[1] |= ST1_ND;
    }
}

static void next_fdc_check_protection(NeXTFDCState *s)
{
    if (s->protect) {
        s->st[0] |= IC_ABNORMAL;
        s->st[1] |= ST1_NW;
    }
}

static void next_fdc_seek_track(NeXTFDCState *s, uint8_t c)
{
    s->st[0] |= ST0_SE;
    s->seekoffset = s->cyl < c ? c - s->cyl : s->cyl - c;
    s->cyl = c;
}

static int next_fdc_sector_time(NeXTFDCState *s)
{
    switch (s->disk_size) {
    case SIZE_720K:
        return 22000;
    case SIZE_1440K:
        return 11000;
    case SIZE_2880K:
        return 5500;
    default:
        return 1000;
    }
}

static int next_fdc_seek_time(NeXTFDCState *s)
{
    if (s->seekoffset > NUM_CYLINDERS) {
        return FLP_SEEK_TIME;
    }
    return s->seekoffset * FLP_SEEK_TIME / NUM_CYLINDERS;
}

static uint8_t next_fdc_media_id(NeXTFDCState *s)
{
    if (!next_fdc_inserted(s)) {
        return MEDIA_ID_NONE;
    }
    switch (s->disk_size) {
    case SIZE_720K:
        return MEDIA_ID_720K;
    case SIZE_1440K:
        return MEDIA_ID_1440K;
    case SIZE_2880K:
        return MEDIA_ID_2880K;
    default:
        return MEDIA_ID_NONE;
    }
}

static void next_fdc_check_rate(NeXTFDCState *s)
{
    uint8_t rate = s->ccr & CCR_RATE_MASK;

    if ((s->disk_size == SIZE_720K && rate == CCR_RATE250) ||
        (s->disk_size == SIZE_1440K && rate == CCR_RATE500) ||
        (s->disk_size == SIZE_2880K && rate == CCR_RATE1000)) {
        return;
    }
    s->st[0] |= IC_ABNORMAL;
    s->st[1] |= ST1_ND;
}

static void next_fdc_rw_status(NeXTFDCState *s, int drive)
{
    s->st[0] |= drive | (s->head << 2);
    s->fifo[0] = s->st[0];
    s->fifo[1] = s->st[1];
    s->fifo[2] = s->st[2];
    s->fifo[3] = s->cyl;
    s->fifo[4] = s->head;
    s->fifo[5] = s->sector;
    s->fifo[6] = s->blocksize;
    s->result_size = 7;
}

/* ---- commands ---- */

static void next_fdc_rw(NeXTFDCState *s, bool write)
{
    int drive = s->cmd_data[0] & 0x03;
    uint8_t c = s->cmd_data[1];
    uint8_t h = s->cmd_data[2];
    uint8_t bs = s->cmd_data[4];
    uint32_t sector_size;

    memset(s->st, 0, 3);
    s->head = (s->cmd_data[0] & 0x04) >> 2;
    if (s->eis) {
        next_fdc_seek_track(s, c);
    }
    s->sector = s->cmd_data[3];
    if (s->cyl != c || s->head != h) {
        s->st[0] |= IC_ABNORMAL;
        s->st[2] |= ST2_WC;
    }
    next_fdc_check_blocksize(s, bs);
    sector_size = 0x80 << bs;
    next_fdc_check_rate(s);
    if (write) {
        next_fdc_check_protection(s);
    }
    if (drive != 0 || !next_fdc_inserted(s)) {
        s->st[0] |= IC_ABNORMAL;
        s->st[1] |= ST1_ND;
    }
    if (s->st[0] & IC_ABNORMAL) {
        next_fdc_rw_status(s, drive);
        s->io_state = FLP_STATE_INTERRUPT;
    } else {
        s->buf_size = 0;
        s->buf_limit = MIN(sector_size, sizeof(s->buf));
        s->sector_counter = s->cmd_data[5] - s->cmd_data[3] + 1;
        s->io_state = write ? FLP_STATE_WRITE : FLP_STATE_READ;
    }
    next_fdc_schedule(s, next_fdc_seek_time(s) + next_fdc_sector_time(s));
}

static void next_fdc_format(NeXTFDCState *s)
{
    int drive = s->cmd_data[0] & 0x03;

    memset(s->st, 0, 3);
    s->head = (s->cmd_data[0] & 0x04) >> 2;
    s->sector = 1;
    next_fdc_check_blocksize(s, s->cmd_data[1]);
    next_fdc_check_rate(s);
    next_fdc_check_protection(s);
    if (drive != 0 || !next_fdc_inserted(s)) {
        s->st[0] |= IC_ABNORMAL;
        s->st[1] |= ST1_ND;
    }
    if (s->st[0] & IC_ABNORMAL) {
        next_fdc_rw_status(s, drive);
        s->io_state = FLP_STATE_INTERRUPT;
        next_fdc_schedule(s, 1000);
    } else {
        s->buf_size = 0;
        s->buf_limit = 4;
        s->sector_counter = s->cmd_data[2];
        s->io_state = FLP_STATE_FORMAT;
        next_fdc_schedule(s, next_fdc_sector_time(s));
    }
}

static void next_fdc_read_id(NeXTFDCState *s)
{
    int drive = s->cmd_data[0] & 0x03;

    s->head = (s->cmd_data[0] & 0x04) >> 2;
    memset(s->st, 0, 3);
    next_fdc_check_rate(s);
    next_fdc_rw_status(s, drive);
    s->io_state = FLP_STATE_INTERRUPT;
    next_fdc_schedule(s, next_fdc_sector_time(s));
}

static void next_fdc_recalibrate(NeXTFDCState *s)
{
    int drive = s->cmd_data[0] & 0x03;

    if (drive == 0 && s->blk) {
        s->msr |= 1 << drive;
        s->cyl = s->pcn = 0;
        memset(s->st, 0, 3);
        next_fdc_seek_track(s, 0);
        s->st[0] = IC_NORMAL | ST0_SE;
        s->sra &= ~SRA_TRK0_N;
        s->io_state = FLP_STATE_INTERRUPT;
        next_fdc_schedule(s, next_fdc_seek_time(s));
    }
}

static void next_fdc_seek(NeXTFDCState *s)
{
    memset(s->st, 0, 3);
    s->head = (s->cmd_data[0] & 0x04) >> 2;
    s->st[0] = IC_NORMAL;
    next_fdc_seek_track(s, s->cmd_data[1]);
    s->pcn = s->cyl;
    s->io_state = FLP_STATE_INTERRUPT;
    next_fdc_schedule(s, next_fdc_seek_time(s));
}

static void next_fdc_intstat(NeXTFDCState *s)
{
    s->sra &= ~SRA_INT;
    next_fdc_set_irq(s, false);
    s->msr |= MSR_RQM | MSR_DIO;
    s->fifo[0] = s->st[0];
    s->fifo[1] = s->pcn;
    s->result_size = 2;
}

static void next_fdc_invalid(NeXTFDCState *s)
{
    s->st[0] = IC_INV_CMD;
    s->fifo[0] = s->st[0];
    s->result_size = 1;
    s->io_state = FLP_STATE_INTERRUPT;
    next_fdc_schedule(s, 1000);
}

static void next_fdc_execute(NeXTFDCState *s)
{
    switch (s->command & CMD_OPCODE_MSK) {
    case CMD_READ:
        next_fdc_rw(s, false);
        break;
    case CMD_WRITE:
        next_fdc_rw(s, true);
        break;
    case CMD_FORMAT:
        next_fdc_format(s);
        break;
    case CMD_RECAL:
        next_fdc_recalibrate(s);
        break;
    case CMD_INTSTAT:
        next_fdc_intstat(s);
        break;
    case CMD_SPECIFY:
    case CMD_PERPEND:
        s->msr |= MSR_RQM;
        break;
    case CMD_SEEK:
        if (s->command & 0x80) {
            qemu_log_mask(LOG_UNIMP, "next-fdc: relative seek\n");
            next_fdc_invalid(s);
            break;
        }
        next_fdc_seek(s);
        break;
    case CMD_CONFIGURE:
        s->eis = s->cmd_data[1] & 0x40;
        if (s->cmd_data[1] & 0x10) {
            /* polling off: no poll interrupt after the reset */
            timer_del(s->timer);
            s->io_state = FLP_STATE_DONE;
        }
        s->msr |= MSR_RQM;
        break;
    case CMD_READ_ID:
        next_fdc_read_id(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "next-fdc: command 0x%02x\n", s->command);
        next_fdc_invalid(s);
        break;
    }
}

static void next_fdc_fifo_write(NeXTFDCState *s, uint8_t val)
{
    s->msr &= ~MSR_RQM;
    if (!s->cmd_phase) {
        s->cmd_phase = true;
        s->command = val;
        s->cmd_limit = s->cmd_size = cmd_data_size[val & CMD_OPCODE_MSK];
        s->msr |= MSR_RQM;
    } else if (s->cmd_size > 0) {
        s->cmd_data[s->cmd_limit - s->cmd_size] = val;
        s->cmd_size--;
        s->msr |= MSR_RQM;
    }
    if (s->cmd_size == 0) {
        s->cmd_phase = false;
        next_fdc_execute(s);
    }
}

static uint8_t next_fdc_fifo_read(NeXTFDCState *s)
{
    uint8_t val = 0;

    if (s->result_size > 0) {
        val = s->fifo[0];
        memmove(s->fifo, s->fifo + 1, sizeof(s->fifo) - 1);
        s->fifo[sizeof(s->fifo) - 1] = 0;
        s->result_size--;
    }
    if (s->result_size == 0) {
        s->msr |= MSR_RQM;
        s->msr &= ~MSR_DIO;
        s->sra &= ~SRA_INT;
        next_fdc_set_irq(s, false);
    }
    return val;
}

static void next_fdc_dor_write(NeXTFDCState *s, uint8_t val)
{
    uint8_t changed = s->dor ^ val;

    s->dor = val;
    s->sel = val & DOR_SEL_MSK;
    if (changed & DOR_RESET_N) {
        if (val & DOR_RESET_N) {
            next_fdc_start(s);
        } else {
            next_fdc_stop(s);
        }
    }
    if ((changed & DOR_MOTEN_MASK) && s->sel == 0 && s->blk) {
        s->spinning = val & DOR_MOT0EN;
    }
}

/* ---- data path through the SCSI DMA channel ---- */

static uint32_t next_fdc_dma_avail(void *opaque)
{
    NeXTFDCState *s = opaque;

    return s->io_state == FLP_STATE_READ ? s->buf_size : 0;
}

static uint8_t next_fdc_dma_get(void *opaque)
{
    NeXTFDCState *s = opaque;

    return s->buf[s->buf_limit - s->buf_size--];
}

static uint32_t next_fdc_dma_room(void *opaque)
{
    NeXTFDCState *s = opaque;

    if (s->io_state != FLP_STATE_WRITE && s->io_state != FLP_STATE_FORMAT) {
        return 0;
    }
    return s->buf_limit - s->buf_size;
}

static void next_fdc_dma_put(void *opaque, uint8_t val)
{
    NeXTFDCState *s = opaque;

    s->buf[s->buf_size++] = val;
}

static const NeXTSCSIFloppyOps next_fdc_dma_ops = {
    .avail = next_fdc_dma_avail,
    .get = next_fdc_dma_get,
    .room = next_fdc_dma_room,
    .put = next_fdc_dma_put,
};

/* ---- I/O ---- */

static void next_fdc_rw_nodata(NeXTFDCState *s)
{
    s->sector_counter = 0;
    s->st[0] |= IC_ABNORMAL;
    s->st[1] |= ST1_OR;
    next_fdc_rw_status(s, 0);
}

static void next_fdc_read_sector(NeXTFDCState *s)
{
    uint32_t sec_size = 0x80 << s->blocksize;
    uint32_t sec = next_fdc_logical_sec(s);

    if (s->st[0] & IC_ABNORMAL) {
        s->sector_counter = 0;
        s->io_state = FLP_STATE_INTERRUPT;
        next_fdc_rw_status(s, 0);
        return;
    }
    s->buf_size = s->buf_limit = sec_size;
    if (blk_pread(s->blk, (int64_t)sec * sec_size, sec_size, s->buf, 0) < 0) {
        memset(s->buf, 0, sec_size);
    }
    s->sector++;
    s->sector_counter--;
    if (s->sector_counter == 0) {
        /* the NeXT hardware ends every transfer like this */
        s->st[0] = IC_ABNORMAL;
        s->st[1] |= ST1_EN;
        next_fdc_rw_status(s, 0);
    }
}

static void next_fdc_write_sector(NeXTFDCState *s)
{
    uint32_t sec_size = 0x80 << s->blocksize;
    uint32_t sec = next_fdc_logical_sec(s);

    if (s->st[0] & IC_ABNORMAL) {
        s->sector_counter = 0;
        s->io_state = FLP_STATE_INTERRUPT;
        next_fdc_rw_status(s, 0);
        return;
    }
    blk_pwrite(s->blk, (int64_t)sec * sec_size, s->buf_size, s->buf, 0);
    s->buf_size = 0;
    s->buf_limit = sec_size;
    s->sector++;
    s->sector_counter--;
    if (s->sector_counter == 0) {
        s->st[0] = IC_ABNORMAL;
        s->st[1] |= ST1_EN;
        next_fdc_rw_status(s, 0);
    }
}

static void next_fdc_format_sector(NeXTFDCState *s)
{
    uint8_t c = s->buf[0], h = s->buf[1], sec = s->buf[2], bs = s->buf[3];
    uint32_t sec_size, lsec;

    if (c != s->cyl || h != s->head || sec != s->sector ||
        bs != s->blocksize) {
        s->sector_counter = 0;
        s->io_state = FLP_STATE_INTERRUPT;
        next_fdc_rw_status(s, 0);
        return;
    }
    sec_size = 0x80 << s->blocksize;
    lsec = next_fdc_logical_sec(s);
    memset(s->buf, 0, sec_size);
    if (s->st[0] & IC_ABNORMAL) {
        s->buf_size = s->buf_limit = 0;
        s->sector_counter = 0;
        next_fdc_rw_status(s, 0);
    } else {
        blk_pwrite(s->blk, (int64_t)lsec * sec_size, sec_size, s->buf, 0);
        s->buf_size = 0;
        s->buf_limit = 4;
        s->sector++;
        s->sector_counter--;
    }
    if (s->sector_counter == 0) {
        next_fdc_rw_status(s, 0);
    }
}

static bool next_fdc_fill(NeXTFDCState *s)
{
    uint32_t old = s->buf_size;

    next_scsi_dma_run(s->scsi);
    if (s->buf_size == old) {
        next_fdc_rw_nodata(s);
        next_fdc_interrupt(s);
        s->io_state = FLP_STATE_DONE;
        return false;
    }
    return true;
}

static void next_fdc_io(void *opaque)
{
    NeXTFDCState *s = opaque;
    uint32_t old;

    switch (s->io_state) {
    case FLP_STATE_WRITE:
    case FLP_STATE_FORMAT:
        if (s->buf_size == s->buf_limit) {
            if (s->io_state == FLP_STATE_WRITE) {
                next_fdc_write_sector(s);
            } else {
                next_fdc_format_sector(s);
            }
            if (s->sector_counter == 0) {
                next_fdc_interrupt(s);
                s->io_state = FLP_STATE_DONE;
                return;
            }
        } else if (!next_fdc_fill(s)) {
            return;
        }
        break;
    case FLP_STATE_READ:
        if (s->buf_size == 0 && s->sector_counter > 0) {
            next_fdc_read_sector(s);
        }
        if (s->buf_size > 0) {
            old = s->buf_size;
            next_scsi_dma_run(s->scsi);
            if (s->buf_size == old) {
                next_fdc_rw_nodata(s);
                next_fdc_interrupt(s);
                s->io_state = FLP_STATE_DONE;
                return;
            }
        }
        if (s->buf_size == 0 && s->sector_counter == 0) {
            next_fdc_interrupt(s);
            s->io_state = FLP_STATE_DONE;
            return;
        }
        break;
    case FLP_STATE_INTERRUPT:
        next_fdc_interrupt(s);
        s->io_state = FLP_STATE_DONE;
        return;
    default:
        return;
    }
    /*
     * The next sector comes at once, not a sector time later: the ROM
     * times a transfer with an instruction loop made for a 25 MHz 68040,
     * which TCG runs through in a few milliseconds.
     */
    next_fdc_schedule(s, FLP_NEXT_SECTOR_TIME);
}

/* ---- registers ---- */

static uint8_t next_fdc_ctrl_read(NeXTFDCState *s)
{
    s->csr &= ~(CTRL_DRV_ID | 0x03);
    if (s->blk) {
        s->csr |= next_fdc_media_id(s);
    } else {
        s->csr |= CTRL_DRV_ID;
    }
    return s->csr;
}

static void next_fdc_ctrl_write(NeXTFDCState *s, uint8_t val)
{
    next_scsi_select_floppy(s->scsi, val & CTRL_82077);
    if (val & CTRL_RESET) {
        next_fdc_reset_ctrl(s);
    }
    if ((val & CTRL_EJECT) && s->blk && blk_is_inserted(s->blk)) {
        blk_eject(s->blk, true);
    }
}

static uint64_t next_fdc_read(void *opaque, hwaddr addr, unsigned size)
{
    NeXTFDCState *s = opaque;
    uint8_t val = 0;

    if (addr & 8) {
        return next_fdc_ctrl_read(s);
    }
    switch (addr & 7) {
    case 0:
        val = s->sra | (s->protect ? 0 : SRA_WP_N);
        break;
    case 1:
        val = s->srb | SRB_ALL1;
        break;
    case 2:
        val = s->dor & ~DOR_MOTEN_MASK;
        if (s->spinning) {
            val |= DOR_MOT0EN;
        }
        break;
    case 3:
        val = s->turbo ? s->reserved3 : 0x03;
        break;
    case 4:
        val = s->msr;
        break;
    case 5:
        val = next_fdc_fifo_read(s);
        break;
    case 6:
        val = s->turbo ? s->reserved6 : 0x41;
        break;
    case 7:
        val = s->din;
        break;
    }
    return val;
}

static void next_fdc_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    NeXTFDCState *s = opaque;

    if (addr & 8) {
        next_fdc_ctrl_write(s, val);
        return;
    }
    switch (addr & 7) {
    case 2:
        next_fdc_dor_write(s, val);
        break;
    case 4:
        s->dsr = val;
        if (s->dsr & DSR_RESET) {
            next_fdc_stop(s);
            next_fdc_start(s);
            s->dsr &= ~DSR_RESET;
        }
        break;
    case 5:
        next_fdc_fifo_write(s, val);
        break;
    case 7:
        s->ccr = val;
        break;
    }
}

static const MemoryRegionOps next_fdc_ops = {
    .read = next_fdc_read,
    .write = next_fdc_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ---- media ---- */

static void next_fdc_update_media(NeXTFDCState *s)
{
    int64_t len;

    s->disk_size = 0;
    s->protect = false;
    if (!s->blk || !blk_is_inserted(s->blk)) {
        return;
    }
    len = blk_getlength(s->blk);
    if (len == SIZE_720K || len == SIZE_1440K || len == SIZE_2880K) {
        s->disk_size = len;
    }
    s->protect = !blk_is_writable(s->blk);
    s->blocksize = 2;   /* 512 bytes */
    s->cyl = s->head = s->sector = 0;
    s->seekoffset = 0;
    s->spinning = false;
}

static void next_fdc_change_media(void *opaque, bool load, Error **errp)
{
    next_fdc_update_media(opaque);
}

static const BlockDevOps next_fdc_block_ops = {
    .change_media_cb = next_fdc_change_media,
};

static void next_fdc_reset_hold(Object *obj, ResetType type)
{
    NeXTFDCState *s = NEXT_FDC(obj);

    s->msr = s->dsr = s->din = s->ccr = s->csr = 0;
    s->eis = 0;
    s->cmd_phase = false;
    s->cmd_size = s->result_size = 0;
    s->buf_size = s->buf_limit = 0;
    s->spinning = false;
    next_fdc_reset_ctrl(s);
}

static void next_fdc_realize(DeviceState *dev, Error **errp)
{
    NeXTFDCState *s = NEXT_FDC(dev);

    if (!s->scsi) {
        error_setg(errp, "next-fdc needs the scsi link");
        return;
    }
    if (s->blk) {
        blk_set_dev_ops(s->blk, &next_fdc_block_ops, s);
        if (!blk_supports_write_perm(s->blk)) {
            /* a read-only image is a write-protected disk */
        } else if (blk_set_perm(s->blk,
                                BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                                BLK_PERM_ALL, errp) < 0) {
            return;
        }
        next_fdc_update_media(s);
    }
    /* the Turbo Station and Turbo Cube answer differently here */
    s->reserved3 = s->station ? (s->color ? 0x03 : 0x02) : 0x00;
    s->reserved6 = s->station ? 0xc0 : 0xf0;
    next_scsi_set_floppy(s->scsi, &next_fdc_dma_ops, s);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, next_fdc_io, s);
}

static void next_fdc_init(Object *obj)
{
    NeXTFDCState *s = NEXT_FDC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mem, obj, &next_fdc_ops, s, "next.floppy",
                          0x10);
    sysbus_init_mmio(sbd, &s->mem);
    sysbus_init_irq(sbd, &s->irq);
}

static int next_fdc_post_load(void *opaque, int version_id)
{
    next_fdc_update_media(opaque);
    return 0;
}

static const VMStateDescription next_fdc_vmstate = {
    .name = "next-fdc",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = next_fdc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(sra, NeXTFDCState),
        VMSTATE_UINT8(srb, NeXTFDCState),
        VMSTATE_UINT8(dor, NeXTFDCState),
        VMSTATE_UINT8(msr, NeXTFDCState),
        VMSTATE_UINT8(dsr, NeXTFDCState),
        VMSTATE_UINT8_ARRAY(fifo, NeXTFDCState, 16),
        VMSTATE_UINT8(din, NeXTFDCState),
        VMSTATE_UINT8(ccr, NeXTFDCState),
        VMSTATE_UINT8(csr, NeXTFDCState),
        VMSTATE_UINT8_ARRAY(st, NeXTFDCState, 4),
        VMSTATE_UINT8(pcn, NeXTFDCState),
        VMSTATE_UINT8(sel, NeXTFDCState),
        VMSTATE_UINT8(eis, NeXTFDCState),
        VMSTATE_INT32(io_state, NeXTFDCState),
        VMSTATE_INT32(sector_counter, NeXTFDCState),
        VMSTATE_BOOL(cmd_phase, NeXTFDCState),
        VMSTATE_INT32(cmd_size, NeXTFDCState),
        VMSTATE_INT32(cmd_limit, NeXTFDCState),
        VMSTATE_UINT8(command, NeXTFDCState),
        VMSTATE_UINT8_ARRAY(cmd_data, NeXTFDCState, 8),
        VMSTATE_INT32(result_size, NeXTFDCState),
        VMSTATE_UINT8(cyl, NeXTFDCState),
        VMSTATE_UINT8(head, NeXTFDCState),
        VMSTATE_UINT8(sector, NeXTFDCState),
        VMSTATE_UINT8(blocksize, NeXTFDCState),
        VMSTATE_UINT32(seekoffset, NeXTFDCState),
        VMSTATE_BOOL(spinning, NeXTFDCState),
        VMSTATE_UINT32(buf_size, NeXTFDCState),
        VMSTATE_UINT32(buf_limit, NeXTFDCState),
        VMSTATE_UINT8_ARRAY(buf, NeXTFDCState, NEXT_FDC_BUF_SIZE),
        VMSTATE_TIMER_PTR(timer, NeXTFDCState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property next_fdc_properties[] = {
    DEFINE_PROP_DRIVE("drive", NeXTFDCState, blk),
    DEFINE_PROP_LINK("scsi", NeXTFDCState, scsi, TYPE_NEXT_SCSI,
                     NeXTSCSIState *),
    DEFINE_PROP_BOOL("turbo", NeXTFDCState, turbo, false),
    DEFINE_PROP_BOOL("station", NeXTFDCState, station, false),
    DEFINE_PROP_BOOL("color", NeXTFDCState, color, false),
};

static void next_fdc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "NeXT floppy controller (82077)";
    dc->realize = next_fdc_realize;
    dc->vmsd = &next_fdc_vmstate;
    device_class_set_props(dc, next_fdc_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    rc->phases.hold = next_fdc_reset_hold;
}

static const TypeInfo next_fdc_info = {
    .name = TYPE_NEXT_FDC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NeXTFDCState),
    .instance_init = next_fdc_init,
    .class_init = next_fdc_class_init,
};

static void next_fdc_register_types(void)
{
    type_register_static(&next_fdc_info);
}

type_init(next_fdc_register_types)
