/*
 * NeXT magneto-optical disk controller (Fujitsu MB600310 optical storage
 * processor) and Canon magneto-optical drives
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Ported from the Previous emulator's mo.c (GPL-2.0-or-later). The
 * optical storage processor (OSP) takes commands for the drive through
 * its drive CSR, and moves sectors between the disk and memory through
 * two 1296-byte buffers, adding or checking the Reed-Solomon code on the
 * way. The drive spirals over the disk, a sector at a time, and the
 * formatter acts on the sector under the head when its ID matches.
 *
 * A disk image holds the sectors as they are on the disk, 1296 bytes each
 * with their ECC, from track 4096 on: the format Previous uses. The
 * drives are given with -global next-mo.drive0=<id> and drive1.
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
#include "hw/block/next-mo.h"
#include "hw/dma/next-dma.h"
#include "migration/vmstate.h"
#include "system/block-backend.h"
#include "next-mo-rs.h"
#include "trace.h"

/* Registers */
enum {
    MO_TRACKH = 0x00,
    MO_TRACKL = 0x01,
    MO_SECTOR = 0x02,
    MO_COUNT = 0x03,
    MO_INTSTATUS = 0x04,
    MO_INTMASK = 0x05,
    MO_CSR2 = 0x06,
    MO_CSR1 = 0x07,
    MO_CSRH = 0x08,
    MO_CSRL = 0x09,
    MO_ERRSTAT = 0x0a,
    MO_ECCCNT = 0x0b,
    MO_INIT = 0x0c,
    MO_FORMAT = 0x0d,
    MO_MARK = 0x0e,
    MO_REG0F = 0x0f,
    MO_FLAG0 = 0x10,
    MO_FLAG6 = 0x16,
};

/* Sector increment and number */
#define MOSEC_NUM_MASK      0x0F
#define MOSEC_INCR_MASK     0xF0

/* Interrupt status */
#define MOINT_CMD_COMPL     0x01
#define MOINT_ATTN          0x02
#define MOINT_OPER_COMPL    0x04
#define MOINT_ECC_DONE      0x08
#define MOINT_TIMEOUT       0x10
#define MOINT_READ_FAULT    0x20
#define MOINT_PARITY_ERR    0x40
#define MOINT_DATA_ERR      0x80
#define MOINT_RESET         0x01    /* written */
#define MOINT_GPO           0x02    /* written */
#define MOINT_OSP_MASK      0xFC

/* Controller CSR 2 */
#define MOCSR2_DRIVE_SEL    0x01
#define MOCSR2_ECC_CMP      0x02
#define MOCSR2_BUF_TOGGLE   0x04
#define MOCSR2_CLR_BUFP     0x08
#define MOCSR2_ECC_BLOCKS   0x10
#define MOCSR2_ECC_MODE     0x20
#define MOCSR2_ECC_DIS      0x40
#define MOCSR2_SECT_TIMER   0x80

/* Data error status */
#define ERRSTAT_ECC         0x01
#define ERRSTAT_CMP         0x02
#define ERRSTAT_TIMING      0x04
#define ERRSTAT_STARVE      0x08

/* Init */
#define MOINIT_ID_MASK      0x03
#define MOINIT_ID_CMP_TRK   0x20
#define MOINIT_ID_0         2

/* Formatter commands (controller CSR 1) */
#define FMT_RESET           0x00
#define FMT_ECC_READ        0x80
#define FMT_ECC_WRITE       0x40
#define FMT_RD_STAT         0x20
#define FMT_ID_READ         0x10
#define FMT_VERIFY          0x08
#define FMT_ERASE           0x04
#define FMT_READ            0x02
#define FMT_WRITE           0x01

enum {
    FMT_MODE_READ,
    FMT_MODE_WRITE,
    FMT_MODE_ERASE,
    FMT_MODE_VERIFY,
    FMT_MODE_READ_ID,
    FMT_MODE_IDLE
};

enum {
    ECC_MODE_READ,
    ECC_MODE_WRITE,
    ECC_MODE_VERIFY
};

enum {
    ECC_STATE_FILLING,
    ECC_STATE_DRAINING,
    ECC_STATE_ECCING,
    ECC_STATE_WAITING,
    ECC_STATE_DONE
};

/* Heads */
enum {
    NO_HEAD,
    READ_HEAD,
    WRITE_HEAD,
    ERASE_HEAD,
    VERIFY_HEAD,
    RF_HEAD
};

/* Drive commands */
#define DRV_SEK     0x0000  /* seek (low 12 bits: track) */
#define DRV_HOS     0xA000  /* high order seek (low 4 bits: track << 12) */
#define DRV_REC     0x1000  /* recalibrate */
#define DRV_RDS     0x2000  /* return drive status */
#define DRV_RCA     0x2200  /* return current track address */
#define DRV_RES     0x2800  /* return extended status */
#define DRV_RHS     0x2A00  /* return hardware status */
#define DRV_RGC     0x3000  /* return general config */
#define DRV_RVI     0x3F00  /* return drive version information */
#define DRV_SRH     0x4100  /* select read head */
#define DRV_SVH     0x4200  /* select verify head */
#define DRV_SWH     0x4300  /* select write head */
#define DRV_SEH     0x4400  /* select erase head */
#define DRV_SFH     0x4500  /* select RF head */
#define DRV_RID     0x5000  /* reset attn and status */
#define DRV_RJ      0x5100  /* relative jump */
#define DRV_SPM     0x5200  /* stop motor */
#define DRV_STM     0x5300  /* start motor */
#define DRV_LC      0x5400  /* lock cartridge */
#define DRV_ULC     0x5500  /* unlock cartridge */
#define DRV_EC      0x5600  /* eject */
#define DRV_SOO     0x5900  /* spiral operation on */
#define DRV_SOF     0x5A00  /* spiral operation off */
#define DRV_RSD     0x8000  /* request self-diagnostic */
#define DRV_SD      0xB000  /* send data */

/* Head select for the relative jump */
#define RJ_READ     0x10
#define RJ_VERIFY   0x20
#define RJ_WRITE    0x30
#define RJ_ERASE    0x40

/* Drive status */
#define DS_INSERT   0x0004  /* load completed */
#define DS_RESET    0x0008  /* power on reset */
#define DS_CMD      0x0020  /* invalid or unimplemented command */
#define DS_STOPPED  0x0200  /* not spinning */
#define DS_WP       0x2000  /* write protected */
#define DS_EMPTY    0x4000  /* no disk */
#define DS_BUSY     0x8000  /* execute busy */

#define VI_VERSION  0x0880

/* Disk layout */
#define MO_SEC_PER_TRACK    16
#define MO_TRACK_OFFSET     4096
#define MO_TRACK_LIMIT      (19819 - MO_TRACK_OFFSET)

/* Timing, in microseconds */
#define SECTOR_IO_DELAY     1250
#define ECC_DELAY           (SECTOR_IO_DELAY / 5)
#define CMD_DELAY           40
#define SECTOR_TIMEOUT_COUNT 32
#define BUSY_SEEN_RETRY     10
#define BUSY_SEEN_MAX       100000

static void next_mo_ecc_read(NeXTMOState *s);
static void next_mo_ecc_write(NeXTMOState *s);
static void next_mo_ecc_verify(NeXTMOState *s);

static NeXTMODrive *cur(NeXTMOState *s)
{
    return &s->drive[s->dnum];
}

static void timer_in_us(QEMUTimer *t, int64_t us)
{
    timer_mod(t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + us * SCALE_US);
}

/* ---- the OSP's interrupts ---- */

static void osp_set_interrupts(NeXTMOState *s)
{
    qemu_set_irq(s->irq, (s->intstatus & s->intmask) != 0);
}

static void osp_interrupt(NeXTMOState *s, uint8_t bits)
{
    s->intstatus |= bits;
    osp_set_interrupts(s);
}

/* ---- the drive's signals ---- */

static void mo_connect_signals(NeXTMOState *s)
{
    NeXTMODrive *d = cur(s);

    if (d->complete) {
        s->intstatus |= MOINT_CMD_COMPL;
    } else {
        s->intstatus &= ~MOINT_CMD_COMPL;
    }
    if (d->attn) {
        s->intstatus |= MOINT_ATTN;
    } else {
        s->intstatus &= ~MOINT_ATTN;
    }
    osp_set_interrupts(s);
}

static void mo_set_signals(NeXTMOState *s, bool complete, bool attn,
                           int drive)
{
    NeXTMODrive *d = &s->drive[drive];

    trace_next_mo_signals(drive, complete, attn);
    d->complete = complete;
    d->attn = d->attn || attn;
    if (d->attn) {
        d->spiraling = false;
    }
    d->seeking = false;
    mo_connect_signals(s);
}

static void mo_set_signals_delayed(NeXTMOState *s, bool complete, bool attn,
                                   int64_t delay)
{
    if (delay <= 0) {
        mo_set_signals(s, complete, attn, s->dnum);
        return;
    }
    if (s->delayed_drive >= 0 && s->delayed_drive != s->dnum) {
        /* the other drive's signals come now */
        mo_set_signals(s, s->delayed_compl, s->delayed_attn,
                       s->delayed_drive);
    }
    s->delayed_drive = s->dnum;
    s->delayed_compl = complete;
    s->delayed_attn = attn;
    timer_in_us(s->signal_timer, delay);
}

static void next_mo_signal_timer(void *opaque)
{
    NeXTMOState *s = opaque;

    /*
     * NeXTSTEP's driver waits to see a command in progress before it
     * waits for the end, polling a few thousand times. A guest that runs
     * late on a busy host may miss the whole command: let the end wait
     * until the CPU has read the status once, for a while.
     */
    if (s->busy_unseen && s->delayed_drive == s->dnum && s->delayed_compl &&
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->busy_deadline) {
        timer_in_us(s->signal_timer, BUSY_SEEN_RETRY);
        return;
    }
    s->busy_unseen = false;
    if (s->delayed_drive >= 0) {
        mo_set_signals(s, s->delayed_compl, s->delayed_attn,
                       s->delayed_drive);
    }
    s->delayed_compl = false;
    s->delayed_attn = false;
    s->delayed_drive = -1;
}

/* ---- the disk ---- */

static int64_t mo_logical_sector(NeXTMOState *s, uint32_t sector_id)
{
    int32_t track = ((sector_id & 0xFFFF00) >> 8) - MO_TRACK_OFFSET;

    if (track < 0 || track >= MO_TRACK_LIMIT) {
        qemu_log_mask(LOG_GUEST_ERROR, "next-mo: sector %06x is off the "
                      "disk\n", sector_id);
        return -1;
    }
    return (int64_t)track * MO_SEC_PER_TRACK + (sector_id & 0x0F);
}

static void mo_read_sector(NeXTMOState *s, uint32_t sector_id)
{
    NeXTMOBuffer *b = &s->ecc_buf[s->eccin];
    int64_t sec = mo_logical_sector(s, sector_id);
    int64_t off = sec * NEXT_MO_SECTOR_DISK;

    /* past the end of the image, the disk is blank */
    memset(b->data, 0, NEXT_MO_SECTOR_DISK);
    if (sec >= 0 && off + NEXT_MO_SECTOR_DISK <= blk_getlength(cur(s)->blk)) {
        if (blk_pread(cur(s)->blk, off, NEXT_MO_SECTOR_DISK, b->data, 0) < 0) {
            qemu_log_mask(LOG_GUEST_ERROR, "next-mo: cannot read sector "
                          "%" PRId64 "\n", sec);
        }
    }
    b->limit = b->size = NEXT_MO_SECTOR_DISK;
}

static void mo_write_buf(NeXTMOState *s, uint32_t sector_id,
                         const uint8_t *data)
{
    int64_t sec = mo_logical_sector(s, sector_id);

    if (sec < 0) {
        return;
    }
    if (blk_pwrite(cur(s)->blk, sec * NEXT_MO_SECTOR_DISK,
                   NEXT_MO_SECTOR_DISK, data, 0) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "next-mo: cannot write sector "
                      "%" PRId64 "\n", sec);
    }
}

static void mo_write_sector(NeXTMOState *s, uint32_t sector_id)
{
    NeXTMOBuffer *b = &s->ecc_buf[s->eccout];

    if (b->limit != NEXT_MO_SECTOR_DISK) {
        qemu_log_mask(LOG_GUEST_ERROR, "next-mo: incomplete write\n");
        return;
    }
    mo_write_buf(s, sector_id, b->data);
    b->size = 0;
    b->limit = NEXT_MO_SECTOR_DATA;
}

static void mo_erase_sector(NeXTMOState *s, uint32_t sector_id)
{
    uint8_t erase[NEXT_MO_SECTOR_DISK];

    memset(erase, 0xFF, sizeof(erase));
    mo_write_buf(s, sector_id, erase);
}

/* ---- the DMA channel ---- */

/* buffer to memory */
static void next_mo_dma_write_memory(NeXTMOState *s)
{
    NeXTDMAChan *c = next_dma_chan(s->dma, NEXT_DMA_DISK);
    NeXTMOBuffer *b = &s->ecc_buf[s->eccout];

    if (!next_dma_enabled(s->dma, NEXT_DMA_DISK)) {
        return;
    }
    if ((c->limit % NEXT_MO_DMA_BURST) || (c->next % 4)) {
        qemu_log_mask(LOG_GUEST_ERROR, "next-mo: DMA buffer not aligned "
                      "(%08x-%08x)\n", c->next, c->limit);
        return;
    }
    while (c->next <= c->limit) {
        while (s->dma_limit < NEXT_MO_DMA_BURST && b->size > 0) {
            s->dma_buf[s->dma_limit++] = b->data[b->limit - b->size];
            b->size--;
            s->dma_size++;
        }
        if (s->dma_limit < NEXT_MO_DMA_BURST) {
            break;
        }
        while (c->next < c->limit && s->dma_size > 0) {
            if (!next_dma_write_mem(s->dma, NEXT_DMA_DISK,
                    &s->dma_buf[NEXT_MO_DMA_BURST - s->dma_size], 4)) {
                return;
            }
            s->dma_size -= 4;
        }
        if (s->dma_size > 0) {
            break;
        }
        s->dma_limit = s->dma_size;
    }
    next_dma_update(s->dma, NEXT_DMA_DISK);
}

/* memory to buffer */
static void next_mo_dma_read_memory(NeXTMOState *s)
{
    NeXTDMAChan *c = next_dma_chan(s->dma, NEXT_DMA_DISK);
    NeXTMOBuffer *b = &s->ecc_buf[s->eccin];

    if (!next_dma_enabled(s->dma, NEXT_DMA_DISK)) {
        return;
    }
    if ((c->limit % NEXT_MO_DMA_BURST) || (c->next % 4)) {
        qemu_log_mask(LOG_GUEST_ERROR, "next-mo: DMA buffer not aligned "
                      "(%08x-%08x)\n", c->next, c->limit);
        return;
    }
    while (c->next < c->limit) {
        while (c->next < c->limit && s->dma_limit < NEXT_MO_DMA_BURST) {
            if (!next_dma_read_mem(s->dma, NEXT_DMA_DISK,
                                   &s->dma_buf[s->dma_limit], 4)) {
                return;
            }
            s->dma_limit += 4;
            s->dma_size += 4;
        }
        if (s->dma_limit < NEXT_MO_DMA_BURST) {
            break;
        }
        while (s->dma_size > 0 && b->size < b->limit) {
            b->data[b->size++] = s->dma_buf[s->dma_limit - s->dma_size];
            s->dma_size--;
        }
        if (s->dma_size > 0) {
            break;
        }
        s->dma_limit = s->dma_size;
    }
    next_dma_update(s->dma, NEXT_DMA_DISK);
}

static void next_mo_dma_initbuf(void *opaque, NeXTDMAChannel ch,
                                unsigned offset)
{
    NeXTMOState *s = opaque;

    s->dma_size = 0;
    /* on a long word, or a 4-byte read would run past the buffer */
    s->dma_limit = offset & (NEXT_MO_DMA_BURST - 4);
}

/* ---- ECC ---- */

static void ecc_toggle_buffer(NeXTMOState *s)
{
    s->eccin ^= 1;
    s->eccout = !s->eccin;
}

static void ecc_clear_buffer(NeXTMOState *s)
{
    s->ecc_buf[0].size = s->ecc_buf[1].size = 0;
    s->ecc_buf[0].limit = s->ecc_buf[1].limit = NEXT_MO_SECTOR_DATA;
}

static void ecc_decode(NeXTMOState *s)
{
    NeXTMOBuffer *b = &s->ecc_buf[s->eccin];

    if ((s->csr2 & MOCSR2_ECC_DIS) && !(s->csr2 & MOCSR2_ECC_MODE)) {
        if ((s->csr2 & MOCSR2_ECC_BLOCKS) && s->ecc_repeat) {
            ecc_toggle_buffer(s);
        }
    } else if (b->size == NEXT_MO_SECTOR_DISK) {
        int errors;

        b->limit = b->size = NEXT_MO_SECTOR_DATA;
        errors = next_mo_rs_decode(b->data);
        if (errors < 0) {
            s->err_stat = ERRSTAT_ECC;
            osp_interrupt(s, MOINT_DATA_ERR);
        } else if (s->ecc_cnt == 0) {
            s->ecc_cnt = errors;
        }
        ecc_toggle_buffer(s);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "next-mo: ECC buffer not ready to "
                      "decode (%u bytes)\n", b->size);
    }
}

static void ecc_encode(NeXTMOState *s)
{
    NeXTMOBuffer *b = &s->ecc_buf[s->eccin];

    if ((s->csr2 & MOCSR2_ECC_DIS) && (s->csr2 & MOCSR2_ECC_MODE)) {
        if ((s->csr2 & MOCSR2_ECC_BLOCKS) && !s->ecc_repeat) {
            ecc_toggle_buffer(s);
        }
    } else if (b->limit == NEXT_MO_SECTOR_DATA) {
        b->limit = b->size = NEXT_MO_SECTOR_DISK;
        next_mo_rs_encode(b->data);
        ecc_toggle_buffer(s);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "next-mo: ECC buffer not ready to "
                      "encode (%u bytes)\n", b->size);
    }
}

static void next_mo_ecc_write(NeXTMOState *s)
{
    if (s->ecc_state != ECC_STATE_DONE) {
        return;
    }
    s->ecc_mode = ECC_MODE_WRITE;
    s->ecc_state = ECC_STATE_FILLING;
    if (s->csr2 & MOCSR2_ECC_BLOCKS) {
        s->ecc_repeat = true;
    }
    s->ecc_buf[s->eccin].size = 0;
    s->ecc_buf[s->eccin].limit = NEXT_MO_SECTOR_DATA;
    timer_in_us(s->ecc_timer, ECC_DELAY);
}

static void next_mo_ecc_read(NeXTMOState *s)
{
    if (s->ecc_state != ECC_STATE_DONE) {
        qemu_log_mask(LOG_GUEST_ERROR, "next-mo: ECC busy\n");
        return;
    }
    s->ecc_mode = ECC_MODE_READ;
    s->ecc_state = ECC_STATE_ECCING;
    if (s->csr2 & MOCSR2_ECC_BLOCKS) {
        s->ecc_repeat = true;
    }
    timer_in_us(s->ecc_timer, ECC_DELAY);
}

static void next_mo_ecc_verify(NeXTMOState *s)
{
    if (s->ecc_state != ECC_STATE_DONE) {
        qemu_log_mask(LOG_GUEST_ERROR, "next-mo: ECC busy\n");
        return;
    }
    s->ecc_mode = ECC_MODE_VERIFY;
    s->ecc_state = ECC_STATE_ECCING;
    timer_in_us(s->ecc_timer, ECC_DELAY);
}

static void ecc_sequence_done(NeXTMOState *s)
{
    if (s->ecc_repeat) {
        s->ecc_repeat = false;
        if (s->ecc_mode == ECC_MODE_WRITE) {
            s->ecc_buf[s->eccin].size = 0;
            s->ecc_state = ECC_STATE_FILLING;
        } else {
            s->ecc_state = ECC_STATE_ECCING;
        }
        timer_in_us(s->ecc_timer, ECC_DELAY);
        return;
    }
    s->ecc_state = ECC_STATE_DONE;
    if (s->csr2 & MOCSR2_ECC_DIS) {
        osp_interrupt(s, MOINT_ECC_DONE);
    }
}

static void next_mo_ecc_io(void *opaque)
{
    NeXTMOState *s = opaque;
    NeXTMOBuffer *in = &s->ecc_buf[s->eccin];
    uint32_t old_size;

    switch (s->ecc_state) {
    case ECC_STATE_FILLING:
        if (in->size < in->limit) {
            in->limit = (s->csr2 & MOCSR2_ECC_MODE) ? NEXT_MO_SECTOR_DISK
                                                    : NEXT_MO_SECTOR_DATA;
            /*
             * The mode can change while the buffer fills: what is past
             * the new limit is dropped, as limit - size must not wrap
             */
            in->size = MIN(in->size, in->limit);
            old_size = in->size;
            next_mo_dma_read_memory(s);
            if (in->size == old_size) {
                /* no more data: the ECC starves */
                s->err_stat = ERRSTAT_STARVE;
                osp_interrupt(s, MOINT_DATA_ERR);
            }
        }
        if (in->size == in->limit) {
            s->ecc_state = ECC_STATE_ECCING;
        }
        break;
    case ECC_STATE_ECCING:
        if (s->ecc_mode == ECC_MODE_WRITE) {
            if (s->csr2 & MOCSR2_ECC_DIS) {
                ecc_decode(s);
                ecc_sequence_done(s);
                return;
            }
            /* on to the disk */
            ecc_encode(s);
            s->ecc_state = ECC_STATE_WAITING;
            if (s->sector_count == 1) {
                osp_interrupt(s, MOINT_ECC_DONE);
            }
            break;
        }
        /* read or verify */
        if (s->csr2 & MOCSR2_ECC_DIS) {
            if (s->csr2 & MOCSR2_ECC_BLOCKS) {
                in->limit = in->size = NEXT_MO_SECTOR_DATA;
            }
            ecc_encode(s);
        } else {
            if (in->size != NEXT_MO_SECTOR_DISK) {
                break;      /* wait for the disk */
            }
            ecc_decode(s);
            if (s->sector_count == 0) {
                osp_interrupt(s, MOINT_ECC_DONE);
            }
            if (s->ecc_mode == ECC_MODE_VERIFY) {
                ecc_clear_buffer(s);
                ecc_sequence_done(s);
                return;
            }
        }
        s->ecc_state = ECC_STATE_DRAINING;
        break;
    case ECC_STATE_DRAINING:
        old_size = s->ecc_buf[s->eccout].size;
        next_mo_dma_write_memory(s);
        if (s->ecc_buf[s->eccout].size == old_size) {
            /* the DMA is not ready */
            ecc_sequence_done(s);
            return;
        }
        if (s->ecc_buf[s->eccout].size == 0) {
            next_mo_dma_write_memory(s);    /* flush */
            ecc_sequence_done(s);
            return;
        }
        break;
    case ECC_STATE_WAITING:
        if (s->ecc_buf[s->eccout].size == 0) {
            ecc_sequence_done(s);
            if (s->sector_count > 0) {
                next_mo_ecc_write(s);
            }
            return;
        }
        break;
    default:
        return;
    }
    timer_in_us(s->ecc_timer, ECC_DELAY);
}

/* ---- the formatter ---- */

static void osp_formatter_cmd(NeXTMOState *s)
{
    if (s->csr1 == FMT_RESET) {
        s->fmt_mode = FMT_MODE_IDLE;
        s->ecc_state = ECC_STATE_DONE;
        s->ecc_cnt = 0;
        s->err_stat = 0;
        return;
    }
    if (s->csr1 & FMT_ECC_READ) {
        next_mo_ecc_read(s);
    }
    if (s->csr1 & FMT_ECC_WRITE) {
        next_mo_ecc_write(s);
    }
    if (s->csr1 & FMT_RD_STAT) {
        s->csrh = cur(s)->status >> 8;
        s->csrl = cur(s)->status;
    }
    if (s->csr1 & FMT_ID_READ) {
        s->fmt_mode = FMT_MODE_READ_ID;
    }
    if (s->csr1 & FMT_VERIFY) {
        s->fmt_mode = FMT_MODE_VERIFY;
    }
    if (s->csr1 & FMT_ERASE) {
        s->fmt_mode = FMT_MODE_ERASE;
    }
    if (s->csr1 & FMT_READ) {
        s->fmt_mode = FMT_MODE_READ;
    }
    if (s->csr1 & FMT_WRITE) {
        s->write_timing = false;
        s->fmt_mode = FMT_MODE_WRITE;
    }
}

static void fmt_sector_done(NeXTMOState *s)
{
    uint16_t track = (s->tracknumh << 8) | s->tracknuml;

    s->sector_num += s->sector_incr;
    track += s->sector_num / MO_SEC_PER_TRACK;
    s->sector_num %= MO_SEC_PER_TRACK;
    s->tracknumh = track >> 8;
    s->tracknuml = track;
    s->sector_count--;
    if (s->sector_count == 0) {
        s->fmt_mode = FMT_MODE_IDLE;
        osp_interrupt(s, MOINT_OPER_COMPL);
    }
}

static bool fmt_match_id(NeXTMOState *s, uint32_t sector_id)
{
    uint32_t fmt_id = (s->tracknumh << 16) | (s->tracknuml << 8) |
                      s->sector_num;

    if ((s->init & MOINIT_ID_MASK) == MOINIT_ID_0) {
        qemu_log_mask(LOG_UNIMP, "next-mo: sector ID matching off\n");
        return true;
    }
    if (s->init & MOINIT_ID_CMP_TRK) {
        fmt_id &= 0xFFFF00;
        sector_id &= 0xFFFF00;
    }
    if (sector_id == fmt_id) {
        s->sector_timer = 0;
        return true;
    }
    if (s->csr2 & MOCSR2_SECT_TIMER) {
        s->sector_timer++;
        if (s->sector_timer > SECTOR_TIMEOUT_COUNT || sector_id > fmt_id) {
            s->sector_timer = 0;
            s->fmt_mode = FMT_MODE_IDLE;
            osp_interrupt(s, MOINT_TIMEOUT);
        }
    }
    return false;
}

static void fmt_io(NeXTMOState *s, uint32_t sector_id)
{
    NeXTMODrive *d = cur(s);

    trace_next_mo_fmt(s->fmt_mode, sector_id,
                      (s->tracknumh << 16) | (s->tracknuml << 8) |
                      s->sector_num);
    switch (s->fmt_mode) {
    case FMT_MODE_READ_ID:
        s->tracknumh = sector_id >> 16;
        s->tracknuml = sector_id >> 8;
        s->sector_num = sector_id & 0x0F;
        osp_interrupt(s, MOINT_OPER_COMPL);
        break;
    case FMT_MODE_READ:
        if (d->head == READ_HEAD && fmt_match_id(s, sector_id)) {
            mo_read_sector(s, sector_id);
            next_mo_ecc_read(s);
            fmt_sector_done(s);
        }
        break;
    case FMT_MODE_WRITE:
        if (d->head != WRITE_HEAD) {
            break;
        }
        /* the first sector only fills the ECC buffer */
        if (fmt_match_id(s, sector_id) && s->write_timing) {
            mo_write_sector(s, sector_id);
            fmt_sector_done(s);
        } else {
            s->write_timing = true;
        }
        next_mo_ecc_write(s);
        break;
    case FMT_MODE_ERASE:
        if (d->head == ERASE_HEAD && fmt_match_id(s, sector_id)) {
            mo_erase_sector(s, sector_id);
            fmt_sector_done(s);
        }
        break;
    case FMT_MODE_VERIFY:
        if (d->head == VERIFY_HEAD && fmt_match_id(s, sector_id)) {
            mo_read_sector(s, sector_id);
            next_mo_ecc_verify(s);
            fmt_sector_done(s);
        }
        break;
    default:
        break;
    }
}

static void osp_select(NeXTMOState *s, int drive)
{
    if (drive != s->dnum && s->drive[drive].attn) {
        /* as Previous does, for stability */
        s->drive[drive].attn = false;
    }
    s->dnum = drive;
    mo_connect_signals(s);
}

static void osp_formatter_cmd2(NeXTMOState *s)
{
    if (s->csr2 & MOCSR2_BUF_TOGGLE) {
        ecc_toggle_buffer(s);
    }
    if (s->csr2 & MOCSR2_CLR_BUFP) {
        ecc_clear_buffer(s);
    }
    osp_select(s, s->csr2 & MOCSR2_DRIVE_SEL);
}

/* ---- the drive ---- */

static void mo_unimplemented_cmd(NeXTMOState *s)
{
    cur(s)->dstat |= DS_CMD;
    mo_set_signals_delayed(s, true, true, CMD_DELAY);
}

static bool mo_empty(NeXTMOState *s)
{
    if (!cur(s)->inserted) {
        mo_set_signals_delayed(s, true, true, CMD_DELAY);
        return true;
    }
    return false;
}

static bool mo_protected(NeXTMOState *s)
{
    NeXTMODrive *d = cur(s);

    if (d->protect && (d->head == ERASE_HEAD || d->head == WRITE_HEAD)) {
        d->head = NO_HEAD;
        mo_set_signals_delayed(s, true, true, CMD_DELAY);
        return true;
    }
    return false;
}

static bool mo_stopped(NeXTMOState *s)
{
    if (!cur(s)->spinning) {
        mo_unimplemented_cmd(s);
        return true;
    }
    return false;
}

static void mo_seek(NeXTMOState *s, uint16_t command)
{
    NeXTMODrive *d = cur(s);
    uint32_t old = d->head_pos;
    int64_t seek_time;

    if (mo_stopped(s)) {
        return;
    }
    d->seeking = true;
    d->head_pos = (d->ho_head_pos & 0xF000) | (command & 0x0FFF);
    seek_time = MIN(old > d->head_pos ? old - d->head_pos
                                      : d->head_pos - old, 95000);
    mo_set_signals_delayed(s, true, false, seek_time + 5000);
}

static void mo_high_order_seek(NeXTMOState *s, uint16_t command)
{
    if (mo_stopped(s)) {
        return;
    }
    if ((command & 0xF) > 4) {
        mo_unimplemented_cmd(s);
        return;
    }
    cur(s)->ho_head_pos = (command & 0xF) << 12;
    mo_set_signals_delayed(s, true, false, CMD_DELAY);
}

static void mo_jump_head(NeXTMOState *s, uint16_t command)
{
    NeXTMODrive *d = cur(s);
    int offset = command & 0x7;

    if (mo_stopped(s)) {
        return;
    }
    d->seeking = true;
    if (command & 0x8) {
        d->head_pos -= 8 - offset;
    } else {
        d->head_pos += offset;
    }
    d->sec_offset = 0;
    switch (command & 0xF0) {
    case RJ_READ:
        d->head = READ_HEAD;
        break;
    case RJ_VERIFY:
        d->head = VERIFY_HEAD;
        break;
    case RJ_WRITE:
        d->head = WRITE_HEAD;
        break;
    case RJ_ERASE:
        d->head = ERASE_HEAD;
        break;
    default:
        d->head = NO_HEAD;
        break;
    }
    if (mo_protected(s)) {
        return;
    }
    mo_set_signals_delayed(s, true, false, 1600);
}

static void mo_select_head(NeXTMOState *s, int head)
{
    if (mo_stopped(s)) {
        return;
    }
    cur(s)->head = head;
    if (mo_protected(s)) {
        return;
    }
    mo_set_signals_delayed(s, true, false, CMD_DELAY);
}

static void mo_eject_disk(NeXTMOState *s, int drive)
{
    NeXTMODrive *d = &s->drive[drive];

    d->inserted = false;
    d->spinning = false;
    d->spiraling = false;
}

static void mo_start_spiraling(NeXTMOState *s)
{
    if (mo_stopped(s)) {
        return;
    }
    if (!timer_pending(s->io_timer)) {
        timer_in_us(s->io_timer, SECTOR_IO_DELAY);
    }
    cur(s)->spiraling = true;
    mo_set_signals_delayed(s, true, false, CMD_DELAY);
}

static void mo_drive_cmd(NeXTMOState *s, uint16_t command)
{
    NeXTMODrive *d = cur(s);

    trace_next_mo_drive_cmd(s->dnum, command, d->complete);
    if (!d->connected) {
        return;
    }
    if (!d->complete) {
        qemu_log_mask(LOG_GUEST_ERROR, "next-mo: drive %d busy, command "
                      "%04x\n", s->dnum, command);
        d->dstat |= DS_BUSY;
        mo_set_signals(s, true, true, s->dnum);
        return;
    }
    /* the command is in progress */
    mo_set_signals(s, false, false, s->dnum);
    s->busy_unseen = true;
    s->busy_deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                       BUSY_SEEN_MAX * SCALE_US;

    if ((command & 0xF000) == DRV_SEK) {
        mo_seek(s, command);
        return;
    }
    if ((command & 0xFF00) == DRV_RJ) {
        mo_jump_head(s, command);
        return;
    }
    if ((command & 0xFFF0) == DRV_HOS) {
        mo_high_order_seek(s, command);
        return;
    }
    switch (command) {
    case DRV_REC:
        if (mo_stopped(s)) {
            return;
        }
        d->head_pos = 0;
        d->sec_offset = 0;
        d->spiraling = false;
        mo_set_signals_delayed(s, true, false, CMD_DELAY);
        break;
    case DRV_RDS:
        d->dstat &= ~(DS_STOPPED | DS_EMPTY | DS_WP);
        if (!d->spinning) {
            d->dstat |= DS_STOPPED;
        }
        if (!d->inserted) {
            d->dstat |= DS_EMPTY;
        }
        if (d->protect) {
            d->dstat |= DS_WP;
        }
        d->status = d->dstat;
        mo_set_signals_delayed(s, true, false, CMD_DELAY);
        break;
    case DRV_RCA:
        d->status = d->head_pos;
        mo_set_signals_delayed(s, true, false, CMD_DELAY);
        break;
    case DRV_RES:
        d->status = d->estat;
        mo_set_signals_delayed(s, true, false, CMD_DELAY);
        break;
    case DRV_RHS:
        d->status = d->hstat;
        mo_set_signals_delayed(s, true, false, CMD_DELAY);
        break;
    case DRV_RVI:
        d->status = VI_VERSION;
        mo_set_signals_delayed(s, true, false, CMD_DELAY);
        break;
    case DRV_SRH:
        mo_select_head(s, READ_HEAD);
        break;
    case DRV_SVH:
        mo_select_head(s, VERIFY_HEAD);
        break;
    case DRV_SWH:
        mo_select_head(s, WRITE_HEAD);
        break;
    case DRV_SEH:
        mo_select_head(s, ERASE_HEAD);
        break;
    case DRV_SFH:
        mo_select_head(s, RF_HEAD);
        break;
    case DRV_RID:
        d->dstat = d->estat = d->hstat = 0;
        d->attn = false;
        mo_set_signals_delayed(s, true, false, CMD_DELAY);
        break;
    case DRV_SPM:
        if (mo_empty(s)) {
            return;
        }
        d->spinning = false;
        d->spiraling = false;
        mo_set_signals_delayed(s, true, false, CMD_DELAY);
        break;
    case DRV_STM:
        if (mo_empty(s)) {
            return;
        }
        d->spinning = true;
        mo_set_signals_delayed(s, true, false, 1600000);
        break;
    case DRV_EC:
        if (mo_empty(s)) {
            return;
        }
        mo_set_signals_delayed(s, true, false, CMD_DELAY);
        mo_eject_disk(s, s->dnum);
        if (d->blk) {
            blk_eject(d->blk, true);
        }
        break;
    case DRV_SOO:
        mo_start_spiraling(s);
        break;
    case DRV_SOF:
        if (mo_stopped(s)) {
            return;
        }
        d->spiraling = false;
        mo_set_signals_delayed(s, true, false, CMD_DELAY);
        break;
    case DRV_RSD:
        mo_set_signals_delayed(s, true, false, CMD_DELAY);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "next-mo: drive command %04x\n", command);
        mo_unimplemented_cmd(s);
        break;
    }
}

/*
 * Can the formatter take the sector under the head? On the real drive the
 * disk turns on whatever the CPU does, but here the guest may run late on a
 * busy host: then the disk waits for the ECC, which is still busy with the
 * last sector, instead of losing a sector.
 */
static bool fmt_ready(NeXTMOState *s)
{
    switch (s->fmt_mode) {
    case FMT_MODE_READ:
    case FMT_MODE_VERIFY:
        return s->ecc_state == ECC_STATE_DONE;
    case FMT_MODE_WRITE:
        return !s->write_timing || (s->ecc_state != ECC_STATE_FILLING &&
                                    s->ecc_state != ECC_STATE_ECCING);
    default:
        return true;
    }
}

static void next_mo_io(void *opaque)
{
    NeXTMOState *s = opaque;
    bool any = false;

    for (int i = 0; i < NEXT_MO_DRIVES; i++) {
        NeXTMODrive *d = &s->drive[i];

        if (!d->spiraling) {
            continue;
        }
        any = true;
        if (d->seeking) {
            continue;
        }
        /* the selected drive is connected to the formatter */
        if (i == s->dnum) {
            if (!fmt_ready(s)) {
                continue;
            }
            fmt_io(s, (d->head_pos << 8) | d->sec_offset);
        }
        d->sec_offset++;
        d->head_pos += d->sec_offset / MO_SEC_PER_TRACK;
        d->sec_offset %= MO_SEC_PER_TRACK;
    }
    if (any) {
        timer_in_us(s->io_timer, SECTOR_IO_DELAY);
    }
}

static void mo_stop(NeXTMOState *s)
{
    NeXTMODrive *d = cur(s);

    d->enabled = false;
    d->spinning = false;
    d->spiraling = false;
    d->seeking = false;
    d->head = NO_HEAD;
    d->head_pos = 0;
    d->ho_head_pos = 0;
    d->sec_offset = 0;
    d->dstat = d->estat = d->hstat = 0;
    d->attn = false;
    mo_set_signals(s, false, false, s->dnum);
}

static void mo_start(NeXTMOState *s)
{
    NeXTMODrive *d = cur(s);

    if (d->connected && !d->enabled) {
        d->enabled = true;
        d->dstat = DS_RESET;
        mo_set_signals_delayed(s, true, false, 500000);
    }
}

/* ---- media ---- */

static void next_mo_insert(NeXTMOState *s, int drive)
{
    NeXTMODrive *d = &s->drive[drive];

    d->inserted = d->blk && blk_is_inserted(d->blk);
    if (!d->inserted) {
        return;
    }
    d->protect = !blk_is_writable(d->blk);
    d->dstat |= DS_INSERT;
    d->spinning = false;
    d->spiraling = false;
    mo_set_signals(s, true, false, drive);
}

static void next_mo_change_media(NeXTMOState *s, int drive, bool load)
{
    if (load) {
        next_mo_insert(s, drive);
    } else {
        mo_eject_disk(s, drive);
    }
}

static void next_mo_change_media0(void *opaque, bool load, Error **errp)
{
    next_mo_change_media(opaque, 0, load);
}

static void next_mo_change_media1(void *opaque, bool load, Error **errp)
{
    next_mo_change_media(opaque, 1, load);
}

static const BlockDevOps next_mo_block_ops[NEXT_MO_DRIVES] = {
    { .change_media_cb = next_mo_change_media0 },
    { .change_media_cb = next_mo_change_media1 },
};

/* ---- registers ---- */

static MemTxResult next_mo_read(void *opaque, hwaddr addr, uint64_t *data,
                                unsigned size, MemTxAttrs attrs)
{
    NeXTMOState *s = opaque;
    unsigned reg = addr & 0x1f;

    switch (reg) {
    case MO_TRACKH:
        *data = s->tracknumh;
        break;
    case MO_TRACKL:
        *data = s->tracknuml;
        break;
    case MO_SECTOR:
        *data = s->sector_num & MOSEC_NUM_MASK;
        break;
    case MO_COUNT:
        *data = s->sector_count & 0xFF;
        break;
    case MO_INTSTATUS:
        *data = s->intstatus;
        if (!(s->intstatus & MOINT_CMD_COMPL)) {
            s->busy_unseen = false;
        }
        break;
    case MO_INTMASK:
        *data = s->intmask;
        break;
    case MO_CSR2:
        *data = s->csr2;
        break;
    case MO_CSR1:
        *data = s->csr1;
        break;
    case MO_CSRH:
        *data = s->csrh;
        break;
    case MO_CSRL:
        *data = s->csrl;
        break;
    case MO_ERRSTAT:
        *data = s->err_stat;
        break;
    case MO_ECCCNT:
        *data = s->ecc_cnt;
        break;
    case MO_INIT:
        *data = s->init;
        break;
    case MO_FORMAT:
        *data = s->format;
        break;
    case MO_MARK:
        *data = s->mark;
        break;
    case MO_REG0F:
        *data = s->reg0f;
        break;
    default:
        if (reg >= MO_FLAG0 && reg <= MO_FLAG6) {
            *data = s->flag[reg - MO_FLAG0];
            break;
        }
        return MEMTX_DECODE_ERROR;
    }
    trace_next_mo_reg_read(reg, *data);
    return MEMTX_OK;
}

static MemTxResult next_mo_write(void *opaque, hwaddr addr, uint64_t data,
                                 unsigned size, MemTxAttrs attrs)
{
    NeXTMOState *s = opaque;
    unsigned reg = addr & 0x1f;
    uint8_t val = data;

    trace_next_mo_reg_write(reg, val);
    switch (reg) {
    case MO_TRACKH:
        s->tracknumh = val;
        break;
    case MO_TRACKL:
        s->tracknuml = val;
        break;
    case MO_SECTOR:
        s->sector_num = val & MOSEC_NUM_MASK;
        s->sector_incr = (val & MOSEC_INCR_MASK) >> 4;
        break;
    case MO_COUNT:
        s->sector_count = val ? val : 0x100;
        break;
    case MO_INTSTATUS:
        s->intstatus &= ~(val & MOINT_OSP_MASK);
        if (val & MOINT_RESET) {
            mo_stop(s);
        } else {
            mo_start(s);
        }
        osp_set_interrupts(s);
        break;
    case MO_INTMASK:
        s->intmask = val;
        osp_set_interrupts(s);
        break;
    case MO_CSR2:
        s->csr2 = val;
        osp_formatter_cmd2(s);
        break;
    case MO_CSR1:
        s->csr1 = val;
        osp_formatter_cmd(s);
        break;
    case MO_CSRH:
        s->csrh = val;
        break;
    case MO_CSRL:
        s->csrl = val;
        mo_drive_cmd(s, (s->csrh << 8) | s->csrl);
        break;
    case MO_ERRSTAT:
    case MO_ECCCNT:
        break;
    case MO_INIT:
        s->init = val;
        break;
    case MO_FORMAT:
        s->format = val;
        break;
    case MO_MARK:
        s->mark = val;
        break;
    case MO_REG0F:
        s->reg0f = val;
        break;
    default:
        if (reg >= MO_FLAG0 && reg <= MO_FLAG6) {
            s->flag[reg - MO_FLAG0] = val;
            break;
        }
        return MEMTX_DECODE_ERROR;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps next_mo_ops = {
    .read_with_attrs = next_mo_read,
    .write_with_attrs = next_mo_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ---- device ---- */

static void next_mo_reset_hold(Object *obj, ResetType type)
{
    NeXTMOState *s = NEXT_MO(obj);

    s->tracknumh = s->tracknuml = s->sector_num = s->sector_incr = 0;
    s->sector_count = 0;
    s->intstatus = s->intmask = s->csr2 = s->csr1 = 0;
    s->csrh = s->csrl = s->err_stat = s->ecc_cnt = 0;
    s->init = s->format = s->mark = s->reg0f = 0;
    memset(s->flag, 0, sizeof(s->flag));

    timer_del(s->signal_timer);
    timer_del(s->io_timer);
    timer_del(s->ecc_timer);
    s->delayed_drive = -1;
    s->delayed_compl = s->delayed_attn = false;
    s->busy_unseen = false;

    for (s->dnum = 0; s->dnum < NEXT_MO_DRIVES; s->dnum++) {
        NeXTMODrive *d = cur(s);
        bool was_ejected = d->connected && !d->inserted;

        d->connected = false;
        d->inserted = false;
        mo_stop(s);
        if (d->blk) {
            d->connected = true;
            d->enabled = true;
            d->complete = true;
            /* a disk the guest ejected stays out */
            if (!was_ejected) {
                next_mo_insert(s, s->dnum);
            }
        }
    }
    s->dnum = 0;
    osp_select(s, 0);

    s->fmt_mode = FMT_MODE_IDLE;
    s->ecc_state = ECC_STATE_DONE;
    s->ecc_repeat = false;
    s->write_timing = false;
    s->sector_timer = 0;
    s->eccin = 0;
    s->eccout = 1;
    ecc_clear_buffer(s);
    s->dma_size = s->dma_limit = 0;
}

static void next_mo_realize(DeviceState *dev, Error **errp)
{
    NeXTMOState *s = NEXT_MO(dev);

    if (!s->dma) {
        error_setg(errp, "next-mo needs the dma link");
        return;
    }
    for (int i = 0; i < NEXT_MO_DRIVES; i++) {
        BlockBackend *blk = s->drive[i].blk;

        if (!blk) {
            continue;
        }
        blk_set_dev_ops(blk, &next_mo_block_ops[i], s);
        if (blk_supports_write_perm(blk) &&
            blk_set_perm(blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0) {
            return;
        }
    }
    next_dma_set_client(s->dma, NEXT_DMA_DISK, NULL, next_mo_dma_initbuf, s);
    s->signal_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   next_mo_signal_timer, s);
    s->io_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, next_mo_io, s);
    s->ecc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, next_mo_ecc_io, s);
}

static void next_mo_init(Object *obj)
{
    NeXTMOState *s = NEXT_MO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mem, obj, &next_mo_ops, s, "next.mo", 0x2000);
    sysbus_init_mmio(sbd, &s->mem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription next_mo_buffer_vmstate = {
    .name = "next-mo/buffer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(data, NeXTMOBuffer, NEXT_MO_SECTOR_DISK),
        VMSTATE_UINT32(size, NeXTMOBuffer),
        VMSTATE_UINT32(limit, NeXTMOBuffer),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription next_mo_drive_vmstate = {
    .name = "next-mo/drive",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(status, NeXTMODrive),
        VMSTATE_UINT16(dstat, NeXTMODrive),
        VMSTATE_UINT16(estat, NeXTMODrive),
        VMSTATE_UINT16(hstat, NeXTMODrive),
        VMSTATE_UINT8(head, NeXTMODrive),
        VMSTATE_UINT32(head_pos, NeXTMODrive),
        VMSTATE_UINT32(ho_head_pos, NeXTMODrive),
        VMSTATE_UINT32(sec_offset, NeXTMODrive),
        VMSTATE_BOOL(spinning, NeXTMODrive),
        VMSTATE_BOOL(spiraling, NeXTMODrive),
        VMSTATE_BOOL(seeking, NeXTMODrive),
        VMSTATE_BOOL(attn, NeXTMODrive),
        VMSTATE_BOOL(complete, NeXTMODrive),
        VMSTATE_BOOL(protect, NeXTMODrive),
        VMSTATE_BOOL(inserted, NeXTMODrive),
        VMSTATE_BOOL(enabled, NeXTMODrive),
        VMSTATE_BOOL(connected, NeXTMODrive),
        VMSTATE_END_OF_LIST()
    },
};

static int next_mo_post_load(void *opaque, int version_id)
{
    NeXTMOState *s = opaque;

    for (int i = 0; i < 2; i++) {
        if (s->ecc_buf[i].limit > NEXT_MO_SECTOR_DISK ||
            s->ecc_buf[i].size > s->ecc_buf[i].limit) {
            return -EINVAL;
        }
    }
    if ((s->eccin & ~1) || (s->eccout & ~1) ||
        s->dma_limit < 0 || s->dma_limit > NEXT_MO_DMA_BURST ||
        s->dma_size < 0 || s->dma_size > s->dma_limit ||
        s->dnum < 0 || s->dnum >= NEXT_MO_DRIVES) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription next_mo_vmstate = {
    .name = "next-mo",
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = next_mo_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(tracknumh, NeXTMOState),
        VMSTATE_UINT8(tracknuml, NeXTMOState),
        VMSTATE_UINT8(sector_num, NeXTMOState),
        VMSTATE_UINT8(sector_incr, NeXTMOState),
        VMSTATE_INT32(sector_count, NeXTMOState),
        VMSTATE_UINT8(intstatus, NeXTMOState),
        VMSTATE_UINT8(intmask, NeXTMOState),
        VMSTATE_UINT8(csr2, NeXTMOState),
        VMSTATE_UINT8(csr1, NeXTMOState),
        VMSTATE_UINT8(csrh, NeXTMOState),
        VMSTATE_UINT8(csrl, NeXTMOState),
        VMSTATE_UINT8(err_stat, NeXTMOState),
        VMSTATE_UINT8(ecc_cnt, NeXTMOState),
        VMSTATE_UINT8(init, NeXTMOState),
        VMSTATE_UINT8(format, NeXTMOState),
        VMSTATE_UINT8(mark, NeXTMOState),
        VMSTATE_UINT8(reg0f, NeXTMOState),
        VMSTATE_UINT8_ARRAY(flag, NeXTMOState, 7),
        VMSTATE_INT32(fmt_mode, NeXTMOState),
        VMSTATE_BOOL(write_timing, NeXTMOState),
        VMSTATE_INT32(sector_timer, NeXTMOState),
        VMSTATE_STRUCT_ARRAY(ecc_buf, NeXTMOState, 2, 1,
                             next_mo_buffer_vmstate, NeXTMOBuffer),
        VMSTATE_INT32(eccin, NeXTMOState),
        VMSTATE_INT32(eccout, NeXTMOState),
        VMSTATE_INT32(ecc_mode, NeXTMOState),
        VMSTATE_INT32(ecc_state, NeXTMOState),
        VMSTATE_BOOL(ecc_repeat, NeXTMOState),
        VMSTATE_UINT8_ARRAY(dma_buf, NeXTMOState, NEXT_MO_DMA_BURST),
        VMSTATE_INT32(dma_size, NeXTMOState),
        VMSTATE_INT32(dma_limit, NeXTMOState),
        VMSTATE_STRUCT_ARRAY(drive, NeXTMOState, NEXT_MO_DRIVES, 1,
                             next_mo_drive_vmstate, NeXTMODrive),
        VMSTATE_INT32(dnum, NeXTMOState),
        VMSTATE_BOOL(delayed_compl, NeXTMOState),
        VMSTATE_BOOL(delayed_attn, NeXTMOState),
        VMSTATE_INT32(delayed_drive, NeXTMOState),
        VMSTATE_BOOL(busy_unseen, NeXTMOState),
        VMSTATE_INT64(busy_deadline, NeXTMOState),
        VMSTATE_TIMER_PTR(signal_timer, NeXTMOState),
        VMSTATE_TIMER_PTR(io_timer, NeXTMOState),
        VMSTATE_TIMER_PTR(ecc_timer, NeXTMOState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property next_mo_properties[] = {
    DEFINE_PROP_DRIVE("drive0", NeXTMOState, drive[0].blk),
    DEFINE_PROP_DRIVE("drive1", NeXTMOState, drive[1].blk),
    DEFINE_PROP_LINK("dma", NeXTMOState, dma, TYPE_NEXT_DMA, NeXTDMAState *),
};

static void next_mo_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "NeXT magneto-optical disk controller";
    dc->realize = next_mo_realize;
    dc->vmsd = &next_mo_vmstate;
    device_class_set_props(dc, next_mo_properties);
    rc->phases.hold = next_mo_reset_hold;
}

static const TypeInfo next_mo_info = {
    .name = TYPE_NEXT_MO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NeXTMOState),
    .instance_init = next_mo_init,
    .class_init = next_mo_class_init,
};

static void next_mo_register_types(void)
{
    type_register_static(&next_mo_info);
}

type_init(next_mo_register_types)
