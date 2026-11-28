/*
 * QEMU Macintosh floppy disk controller emulator (SWIM2)
 *
 * Copyright (c) 2014-2018 Laurent Vivier <laurent@vivier.eu>
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * The SWIM2 of the Quadra 610/650/800 and Centris (in the IOSB) with
 * Sony MFD-75W SuperDrives.  The registers, the FIFO, the mode bits and
 * the drive's sense lines and commands follow MAME's
 * src/devices/machine/swim2.cpp and src/devices/imagedev/floppy.cpp
 * (BSD-3-Clause, copyright-holders: Olivier Galibert).
 *
 * The data path works on bytes, not flux: the track under the head is
 * built from the image as the GCR nibbles or MFM bytes the chip would
 * frame (swim-track.c), handed to the CPU as fast as it reads, and
 * decoded back into sectors when a write ends.  A format that writes
 * another layout (800K GCR to 720K MFM, say) resizes the raw image.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "system/block-backend.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "hw/block/block.h"
#include "hw/block/swim.h"
#include "hw/core/qdev-properties.h"
#include "swim-track.h"
#include "trace.h"

QEMU_BUILD_BUG_ON(SWIM_TRACK_BUF != SWIM_TRACK_MAX);

#define REG_SHIFT               9

/* Registers, (address >> 9) & 7 */
#define R_DATA                  0
#define R_MARK                  1
#define R_ERROR                 2       /* write: CRC */
#define R_PARAM                 3
#define R_PHASES                4
#define R_SETUP                 5
#define R_MODE                  6       /* write: mode clear */
#define R_HANDSHAKE             7       /* write: mode set */

/* Mode */
#define MODE_CLFIFO             0x01
#define MODE_ENBL1              0x02
#define MODE_ENBL2              0x04
#define MODE_ACTION             0x08
#define MODE_WRITE              0x10
#define MODE_HEDSEL             0x20
#define MODE_ISM                0x40
#define MODE_MOTON              0x80

/* Setup */
#define SETUP_GCR_READ          0x04
#define SETUP_GCR_WRITE         0x40

/* Handshake */
#define HS_MARK                 0x01
#define HS_CRC_ERROR            0x02
#define HS_SENSE                0x08
#define HS_ERROR                0x20
#define HS_DAT2BYTE             0x40
#define HS_DAT1BYTE             0x80

/* Error */
#define ERR_UNDERRUN            0x01
#define ERR_MARK_IN_DATA        0x02
#define ERR_OVERRUN             0x04

/* FIFO entries: a byte and its flags */
#define FIFO_EMPTY              0xffff
#define FIFO_MARK               SWIM_MARK
#define FIFO_CRC                0x200
#define FIFO_CRC0               0x400

static const char *const read_names[8] = {
    "data", "mark", "error", "param", "phases", "setup", "mode", "handshake"
};

static const char *const write_names[8] = {
    "data", "mark", "crc", "param", "phases", "setup", "mode0", "mode1"
};

static const char *const drive_cmd_names[16] = {
    "dir+1", "step", "motor on", "end eject",
    "dir-1", "-", "motor off", "eject",
    "-", "mfm", "-", "-",
    "clear dskchg", "gcr", "-", "-"
};

/* Drive */

static bool drive_has_disk(FDrive *d)
{
    return d->blk && !d->tray_open && blk_is_inserted(d->blk);
}

static bool drive_writable(FDrive *d)
{
    return drive_has_disk(d) && blk_is_writable(d->blk);
}

static int64_t drive_rev_ns(FDrive *d)
{
    return NANOSECONDS_PER_SECOND * 60 /
           swim_track_rpm(d->track_enc, d->cyl);
}

static int64_t drive_ns_per_byte(FDrive *d)
{
    return MAX(drive_rev_ns(d) / d->track_len, 1);
}

/*
 * Move the disk on by the time that went by, while the controller neither
 * reads nor writes.  During a transfer, swim_sync() moves it a byte at a
 * time instead.
 */
static void drive_spin(FDrive *d)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t ns, n;

    if (!d->motor || d->track_len <= 0) {
        d->pos_time = now;
        return;
    }
    ns = drive_ns_per_byte(d);
    n = (now - d->pos_time) / ns;
    if (n > 0) {
        d->pos = (d->pos + n) % d->track_len;
        d->pos_time += n * ns;
    }
}

static void drive_set_layout(FDrive *d)
{
    int64_t size = drive_has_disk(d) ? blk_getlength(d->blk) : 0;

    d->layout = swim_layout_from_size(size);
    d->hd = d->layout == SWIM_LAYOUT_NONE ? size > 819200
                                          : swim_layout_hd(d->layout);
    trace_swim_media(d->unit, swim_layout_name(d->layout), size);
}

static bool drive_resize(FDrive *d, SwimLayout l)
{
    Error *err = NULL;
    uint64_t perm, shared;
    int ret;

    /* Take the resize permission only for as long as it takes */
    blk_get_perm(d->blk, &perm, &shared);
    ret = blk_set_perm(d->blk, perm | BLK_PERM_RESIZE, shared, &err);
    if (ret == 0) {
        ret = blk_truncate(d->blk, swim_layout_size(l), true,
                           PREALLOC_MODE_OFF, 0, &err);
        blk_set_perm(d->blk, perm, shared, NULL);
    }
    if (ret < 0) {
        qemu_log_mask(LOG_UNIMP, "swim: cannot make the image %s: %s\n",
                      swim_layout_name(l), error_get_pretty(err));
        error_free(err);
        return false;
    }
    trace_swim_layout(d->unit, swim_layout_name(d->layout),
                      swim_layout_name(l));
    d->layout = l;
    return true;
}

/* The layout a track the guest wrote asks for, from its sectors */
static SwimLayout drive_wanted_layout(FDrive *d, const SwimSector *secs,
                                      int n)
{
    int maxsec = 0;
    bool ds = d->track_head == 1;

    for (int i = 0; i < n; i++) {
        if (secs[i].cyl != d->track_cyl) {
            continue;
        }
        maxsec = MAX(maxsec, secs[i].sector);
        ds |= secs[i].format & 0x20;
    }

    if (d->track_enc == SWIM_ENC_GCR) {
        if (d->hd) {
            return d->layout;           /* the SuperDrive does no GCR on HD */
        }
        return ds ? SWIM_LAYOUT_GCR800 : SWIM_LAYOUT_GCR400;
    }
    if (maxsec > 9) {
        return d->hd ? SWIM_LAYOUT_MFM1440 : d->layout;
    }
    if (d->layout == SWIM_LAYOUT_MFM360 && d->track_head == 0) {
        return SWIM_LAYOUT_MFM360;
    }
    return SWIM_LAYOUT_MFM720;
}

/* Store the sectors of a track the guest wrote */
static void drive_flush(FDrive *d)
{
    g_autofree SwimSector *secs = NULL;
    SwimLayout want;
    int n, stored = 0;

    if (!d->track_dirty) {
        return;
    }
    d->track_dirty = false;
    if (!drive_writable(d)) {
        return;
    }

    secs = g_new(SwimSector, SWIM_MAX_SPT * 2);
    n = swim_track_decode(d->track, d->track_len, d->track_enc, secs,
                          SWIM_MAX_SPT * 2);
    if (!n) {
        trace_swim_track_flush(d->unit, d->track_cyl, d->track_head, 0,
                               swim_layout_name(d->layout));
        return;
    }

    want = drive_wanted_layout(d, secs, n);
    if (want != d->layout && !drive_resize(d, want)) {
        return;
    }
    if (swim_layout_enc(d->layout) != d->track_enc) {
        return;
    }

    for (int i = 0; i < n; i++) {
        int idx = d->track_enc == SWIM_ENC_GCR ? secs[i].sector
                                               : secs[i].sector - 1;
        int64_t off;

        if (secs[i].cyl != d->track_cyl || secs[i].head != d->track_head) {
            continue;
        }
        off = swim_layout_offset(d->layout, d->track_cyl, d->track_head, idx);
        if (off < 0) {
            continue;
        }
        if (blk_pwrite(d->blk, off, SWIM_SECTOR, secs[i].data, 0) < 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "swim: write error at %" PRId64 "\n", off);
            continue;
        }
        stored++;
    }
    trace_swim_track_flush(d->unit, d->track_cyl, d->track_head, stored,
                           swim_layout_name(d->layout));
}

/* Make the track under the head the one in the buffer */
static void drive_load_track(FDrive *d, bool head)
{
    g_autofree uint8_t *data = NULL;
    SwimEnc enc = swim_layout_enc(d->layout);
    int spt = swim_layout_spt(d->layout, d->cyl);
    int old_len = d->track_len;
    int64_t off;

    if (!drive_has_disk(d)) {
        d->track_valid = false;
        d->track_dirty = false;
        d->track_len = 0;
        return;
    }
    if (d->track_valid && d->track_cyl == d->cyl && d->track_head == head) {
        return;
    }
    drive_flush(d);

    d->stop_valid = false;
    d->track_valid = true;
    d->track_cyl = d->cyl;
    d->track_head = head;

    off = swim_layout_offset(d->layout, d->cyl, head, 0);
    data = g_malloc0(SWIM_MAX_SPT * SWIM_SECTOR);
    if (off >= 0 && blk_pread(d->blk, off, spt * SWIM_SECTOR, data, 0) >= 0) {
        d->track_enc = enc;
        d->track_len = swim_track_build(d->track, d->layout, d->cyl, head,
                                        data);
    } else {
        /* No such track in this layout: an unformatted one */
        if (enc == SWIM_ENC_NONE) {
            enc = d->mfm ? SWIM_ENC_MFM : SWIM_ENC_GCR;
        }
        d->track_enc = enc;
        d->track_len = swim_track_blank(d->track, enc, d->hd, d->cyl);
    }

    /* Keep the angle the disk was at */
    if (old_len > 0) {
        d->pos = (uint64_t)d->pos * d->track_len / old_len % d->track_len;
    } else {
        d->pos = 0;
    }
    trace_swim_track_load(d->unit, d->cyl, head, swim_layout_name(d->layout),
                          d->track_len);
}

/* Give the buffer a blank track of @enc for a format to write over */
static void drive_reformat_track(FDrive *d, SwimEnc enc)
{
    int old_len = d->track_len;

    d->track_enc = enc;
    d->track_len = swim_track_blank(d->track, enc, d->hd, d->cyl);
    d->pos = old_len > 0 ?
             (uint64_t)d->pos * d->track_len / old_len % d->track_len : 0;
}

static void drive_eject(FDrive *d)
{
    drive_flush(d);
    trace_swim_eject(d->unit);
    if (drive_has_disk(d)) {
        blk_eject(d->blk, true);
    }
    /* Let the next medium be read-only, as when the tray opens */
    if (d->blk) {
        blk_set_perm(d->blk, 0, BLK_PERM_ALL, NULL);
    }
    d->tray_open = true;
    d->dskchg = false;
    d->track_valid = false;
    d->track_len = 0;
}

/* The drive's sense line for register @reg (CA0-CA2, SEL) */
static bool drive_sense(FDrive *d, int reg, bool head)
{
    bool disk = drive_has_disk(d);

    switch (reg) {
    case 0x0:                           /* step direction */
        return d->dir;
    case 0x1:                           /* step done */
        return true;
    case 0x2:                           /* motor off */
        return !d->motor;
    case 0x3:                           /* disk changed */
        return !d->dskchg;
    case 0x4:
    case 0xc:                           /* index, low during the pulse */
        if (!disk || !d->motor) {
            return true;
        }
        drive_load_track(d, head);
        if (!d->swimctrl->reading && !d->swimctrl->writing) {
            drive_spin(d);
        }
        return d->track_len <= 0 || d->pos >= d->track_len / 50;
    case 0x5:                           /* SuperDrive */
        return true;
    case 0x6:                           /* double-sided */
        return true;
    case 0x7:                           /* no drive */
        return false;
    case 0x8:                           /* no disk */
        return !disk;
    case 0x9:                           /* not write-protected */
        return drive_writable(d);
    case 0xa:                           /* not on track 0 */
        return d->cyl != 0;
    case 0xb:                           /* tach, 120 edges a turn */
        if (!disk || !d->motor) {
            return false;
        }
        drive_load_track(d, head);
        if (!d->swimctrl->reading && !d->swimctrl->writing) {
            drive_spin(d);
        }
        return d->track_len > 0 &&
               ((uint64_t)d->pos * 120 / d->track_len) & 1;
    case 0xd:                           /* MFM mode */
        return d->mfm;
    case 0xe:                           /* not ready */
        return !(disk && d->motor);
    case 0xf:                           /* 2M media (DD), not HD */
        return disk && !d->hd;
    default:
        return false;
    }
}

/* A command on the rising edge of LSTRB */
static void drive_command(FDrive *d, int reg)
{
    trace_swim_drive_cmd(d->unit, reg, drive_cmd_names[reg]);

    switch (reg) {
    case 0x0:
        d->dir = false;
        break;
    case 0x1:
        if (d->dir) {
            if (d->cyl > 0) {
                d->cyl--;
            }
        } else if (d->cyl < SWIM_CYLS - 1) {
            d->cyl++;
        }
        trace_swim_step(d->unit, d->cyl);
        break;
    case 0x2:
        if (!d->motor) {
            d->motor = true;
            d->pos_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        break;
    case 0x4:
        d->dir = true;
        break;
    case 0x6:
        drive_flush(d);
        d->motor = false;
        break;
    case 0x7:
        drive_eject(d);
        break;
    case 0x9:
        d->mfm = true;
        break;
    case 0xc:
        d->dskchg = true;
        break;
    case 0xd:
        d->mfm = false;
        break;
    default:
        break;
    }
}

static void drive_phases(FDrive *d, uint8_t phases, bool head)
{
    bool strb = phases & 0x08;

    if (strb && !d->strb) {
        drive_command(d, (phases & 7) | (head ? 8 : 0));
    }
    d->strb = strb;
}

/* Controller */

static FDrive *swim_drive(SWIMCtrl *s)
{
    int devsel = s->mode & MODE_MOTON ? (s->mode >> 1) & 3 : 0;
    FDrive *d;

    if (devsel != 1 && devsel != 2) {
        return NULL;
    }
    d = &s->drives[devsel - 1];
    return d->blk ? d : NULL;
}

static uint8_t swim_phases_out(SWIMCtrl *s)
{
    return s->phases & (s->phases >> 4) & 0x0f;
}

static void fifo_clear(SWIMCtrl *s)
{
    s->fifo_pos = 0;
    s->crc = SWIM_CRC_AFTER_MARKS;
}

static bool fifo_push(SWIMCtrl *s, uint16_t v)
{
    if (s->fifo_pos == 2) {
        return false;
    }
    s->fifo[s->fifo_pos++] = v;
    return true;
}

static uint16_t fifo_pop(SWIMCtrl *s)
{
    uint16_t r;

    if (s->fifo_pos == 0) {
        return FIFO_EMPTY;
    }
    r = s->fifo[0];
    s->fifo[0] = s->fifo[1];
    s->fifo_pos--;
    return r;
}

/*
 * Transfers run at the speed of the disk: the controller shifts a byte
 * every swim_ns_per_byte().  xfer_time is when the next byte is due.
 *
 * - Writing, as on the chip, a byte goes out whether or not a drive takes
 *   it.  The chip would underrun when the FIFO runs empty; here the disk
 *   waits for the guest instead, since a guest under TCG can be late by
 *   more than a byte, and the drivers end their writes themselves.
 * - Reading, a byte is only there when a disk turns under the head.  When
 *   the guest falls behind, the disk waits instead of dropping bytes,
 *   unless the guest leaves it for a whole turn.
 */
static FDrive *swim_xfer_drive(SWIMCtrl *s)
{
    FDrive *d = swim_drive(s);

    if (!d || !drive_has_disk(d) || !d->motor) {
        return NULL;
    }
    drive_load_track(d, s->hdsel);
    return d->track_len > 0 ? d : NULL;
}

static int64_t swim_ns_per_byte(SWIMCtrl *s, FDrive *d)
{
    if (d) {
        return MAX(drive_rev_ns(d) / d->track_len, 1);
    }
    /* 500 kbit/s, or 250 kbit/s with FCLK/2 */
    return s->setup & 0x08 ? 32000 : 16000;
}

static void drive_advance(FDrive *d)
{
    d->pos = (d->pos + 1) % d->track_len;
}

/* The next byte under the head while reading, or false when none is due */
static bool swim_read_next(SWIMCtrl *s, FDrive *d, int64_t now,
                           uint16_t *out)
{
    SwimEnc want = s->setup & SETUP_GCR_READ ? SWIM_ENC_GCR : SWIM_ENC_MFM;
    bool match = d->track_enc == want && d->mfm == (want == SWIM_ENC_MFM);
    int64_t ns = swim_ns_per_byte(s, d);
    uint16_t v;

    if (want == SWIM_ENC_GCR) {
        if (s->xfer_time > now) {
            return false;
        }
        /* Without GCR under the head, the shift register frames noise */
        *out = match ? d->track[d->pos] & 0xff : 0xff;
        drive_advance(d);
        s->xfer_time += ns;
        return true;
    }

    /* MFM: nothing until the clock locks on a mark */
    while (s->xfer_time <= now && !s->synced) {
        if (match && (d->track[d->pos] & SWIM_MARK)) {
            s->synced = true;
            break;
        }
        drive_advance(d);
        s->xfer_time += ns;
    }
    if (!s->synced || s->xfer_time > now) {
        return false;
    }
    v = d->track[d->pos];
    drive_advance(d);
    s->xfer_time += ns;
    if (v & SWIM_MARK) {
        s->crc = SWIM_CRC_AFTER_MARKS;
    } else {
        s->crc = swim_crc_byte(s->crc, v);
        if (!s->crc) {
            v |= FIFO_CRC0;
        }
    }
    *out = v;
    return true;
}

static void swim_put(SWIMCtrl *s, FDrive *d, uint16_t v)
{
    if (d) {
        d->track[d->pos] = v;
        drive_advance(d);
        s->write_count++;
    }
}

/*
 * A format writes the index mark (C2 C2 C2 FC) right after the index.  A
 * guest that comes late finds the disk further on than a real one would
 * be, so move what this write put down so far to the start of the track.
 */
static void swim_format_at_index(SWIMCtrl *s, FDrive *d)
{
    uint16_t head[256];
    uint32_t n = s->write_count;

    if (!d || n >= ARRAY_SIZE(head) || s->write_start == 0) {
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        head[i] = d->track[(s->write_start + i) % d->track_len];
    }
    for (uint32_t i = 0; i < n; i++) {
        d->track[(s->write_start + i) % d->track_len] = 0x4e;
    }
    memcpy(d->track, head, n * sizeof(head[0]));
    d->pos = n;
    s->write_start = 0;
}

/* One FIFO entry goes onto the disk, if a drive writes it (@d) */
static void swim_commit(SWIMCtrl *s, FDrive *d, uint16_t v)
{
    SwimEnc enc = s->setup & SETUP_GCR_WRITE ? SWIM_ENC_GCR : SWIM_ENC_MFM;

    if (d && (!drive_writable(d) || (enc == SWIM_ENC_GCR && d->hd))) {
        d = NULL;
    }
    if (d) {
        if (d->track_enc != enc) {
            drive_reformat_track(d, enc);
        }
        d->track_dirty = true;
    }

    if (enc == SWIM_ENC_GCR) {
        /* The bits as the read shift register would frame them */
        for (int i = 7; i >= 0; i--) {
            s->gcr_sr = (s->gcr_sr << 1) | ((v >> i) & 1);
            if (s->gcr_sr & 0x80) {
                swim_put(s, d, s->gcr_sr);
                s->gcr_sr = 0;
            }
        }
        return;
    }

    if (v & FIFO_CRC) {
        uint16_t crc = s->crc;

        swim_put(s, d, crc >> 8);
        swim_put(s, d, crc & 0xff);
        s->crc = swim_crc_byte(swim_crc_byte(crc, crc >> 8), crc & 0xff);
    } else if (v & FIFO_MARK) {
        if ((v & 0xff) == 0xc2) {
            swim_format_at_index(s, d);
        }
        swim_put(s, d, SWIM_MARK | (v & 0xff));
        s->crc = SWIM_CRC_AFTER_MARKS;
    } else {
        swim_put(s, d, v & 0xff);
        s->crc = swim_crc_byte(s->crc, v);
    }
}

/* Bring the FIFO and the disk up to now */
static void swim_sync(SWIMCtrl *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    FDrive *d;
    uint16_t v;

    if (s->reading) {
        d = swim_xfer_drive(s);
        if (!d) {
            s->xfer_time = now;
            return;
        }
        if (now - s->xfer_time > drive_rev_ns(d)) {
            s->xfer_time = now;
        }
        /*
         * One byte at a time: the chip is faster than the guest, and the
         * mark and CRC bits of the handshake describe the newest byte.
         */
        if (s->fifo_pos == 0 && swim_read_next(s, d, now, &v)) {
            fifo_push(s, v);
        }
        d->pos_time = now;
    } else if (s->writing) {
        d = swim_xfer_drive(s);
        while (s->fifo_pos > 0 && s->xfer_time <= now) {
            v = fifo_pop(s);
            swim_commit(s, d, v);
            s->xfer_time += swim_ns_per_byte(s, d) * (v & FIFO_CRC ? 2 : 1);
        }
        if (s->fifo_pos == 0 && s->xfer_time < now) {
            s->xfer_time = now;         /* the disk waits for the guest */
        }
        if (d) {
            d->pos_time = now;
        }
    }
}

static void swim_write_byte(SWIMCtrl *s, uint16_t v)
{
    swim_sync(s);
    if (!fifo_push(s, v) && !s->error) {
        s->error |= ERR_OVERRUN;
    }
}

static void swim_start(SWIMCtrl *s, bool write)
{
    FDrive *d = swim_drive(s);

    if (d && drive_has_disk(d)) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        drive_load_track(d, s->hdsel);
        /*
         * Right after a transfer, go on where it ended: the guest reads a
         * header and then writes its data field, and under TCG it can be
         * later than the gap after the header allows.
         */
        if (d->stop_valid && now - d->stop_time < drive_rev_ns(d)) {
            d->pos = d->stop_pos;
            d->pos_time = now;
        } else {
            drive_spin(d);
        }
        trace_swim_rw(write ? "write start" : "read start", d->unit, d->cyl,
                      s->hdsel, d->pos);
    }
    s->xfer_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (write) {
        s->writing = true;
        s->gcr_sr = 0;
        s->write_start = d && d->track_len > 0 ? d->pos : 0;
        s->write_count = 0;
    } else {
        s->reading = true;
        s->synced = false;
    }
    swim_sync(s);
}

static void swim_stop(SWIMCtrl *s, FDrive *d, bool write)
{
    if (write) {
        s->writing = false;
        if (d) {
            trace_swim_rw("write end", d->unit, d->cyl, s->hdsel, d->pos);
            drive_flush(d);
        }
    } else {
        s->reading = false;
        s->synced = false;
    }
    if (d) {
        d->pos_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        d->stop_valid = d->track_valid;
        d->stop_pos = d->pos;
        d->stop_time = d->pos_time;
    }
}

static void swim_mode_changed(SWIMCtrl *s, uint8_t old_mode)
{
    uint8_t old_save = s->mode;
    FDrive *old, *now;
    bool want_w = (s->mode & (MODE_ACTION | MODE_WRITE)) ==
                  (MODE_ACTION | MODE_WRITE);
    bool want_r = (s->mode & (MODE_ACTION | MODE_WRITE)) == MODE_ACTION;

    s->mode = old_mode;
    old = swim_drive(s);
    s->mode = old_save;
    now = swim_drive(s);

    if (s->mode & MODE_CLFIFO) {
        fifo_clear(s);
    }

    /* A transfer ends when it stops or its drive is deselected */
    if (s->writing && (!want_w || old != now)) {
        swim_stop(s, old, true);
    }
    if (s->reading && (!want_r || old != now)) {
        swim_stop(s, old, false);
    }
    if (old != now && now) {
        drive_phases(now, swim_phases_out(s), s->hdsel);
    }
    if (want_w && !s->writing) {
        swim_start(s, true);
    }
    if (want_r && !s->reading) {
        swim_start(s, false);
    }
}

static uint64_t swim_read(void *opaque, hwaddr addr, unsigned size)
{
    SWIMCtrl *s = opaque;
    int reg = (addr >> REG_SHIFT) & 7;
    uint16_t r;
    uint8_t value = 0xff;

    switch (reg) {
    case R_DATA:
        swim_sync(s);
        r = fifo_pop(s);
        if (!s->error) {
            if (r == FIFO_EMPTY) {
                s->error |= ERR_OVERRUN;
            } else if (r & FIFO_MARK) {
                s->error |= ERR_MARK_IN_DATA;
            }
        }
        value = r;
        break;
    case R_MARK:
        swim_sync(s);
        r = fifo_pop(s);
        if (!s->error && r == FIFO_EMPTY) {
            s->error |= ERR_OVERRUN;
        }
        value = r;
        break;
    case R_ERROR:
        value = s->error;
        s->error = 0;
        break;
    case R_PARAM:
        value = s->param_idx & 1 ? 0 : s->param[s->param_idx] & 0xf0;
        s->param_idx = (s->param_idx + 1) & 3;
        break;
    case R_PHASES:
        value = s->phases;
        break;
    case R_SETUP:
        value = s->setup;
        break;
    case R_MODE:
        value = s->mode;
        break;
    case R_HANDSHAKE: {
        FDrive *d = swim_drive(s);

        swim_sync(s);
        value = 0;
        if (s->fifo_pos > 0) {
            if (s->fifo[s->fifo_pos - 1] & FIFO_MARK) {
                value |= HS_MARK;
            }
            if (!(s->fifo[s->fifo_pos - 1] & FIFO_CRC0)) {
                value |= HS_CRC_ERROR;
            }
        }
        if (!d || drive_sense(d, (swim_phases_out(s) & 7) |
                                 (s->hdsel ? 8 : 0), s->hdsel)) {
            value |= HS_SENSE;
        }
        if (s->error) {
            value |= HS_ERROR;
        }
        if (s->mode & MODE_WRITE) {
            if (s->fifo_pos == 0) {
                value |= HS_DAT1BYTE | HS_DAT2BYTE;
            } else if (s->fifo_pos == 1) {
                value |= HS_DAT1BYTE;
            }
        } else {
            if (s->fifo_pos == 2) {
                value |= HS_DAT1BYTE | HS_DAT2BYTE;
            } else if (s->fifo_pos == 1) {
                value |= HS_DAT1BYTE;
            }
        }
        break;
    }
    }

    trace_swim_read(read_names[reg], value);
    return size == 2 ? (value << 8) | value : value;
}

static void swim_write(void *opaque, hwaddr addr, uint64_t value,
                       unsigned size)
{
    SWIMCtrl *s = opaque;
    int reg = (addr >> REG_SHIFT) & 7;
    uint8_t old_mode = s->mode;
    uint8_t v = size == 2 ? value >> 8 : value;
    FDrive *d;

    trace_swim_write(write_names[reg], v);
    swim_sync(s);

    switch (reg) {
    case R_DATA:
        swim_write_byte(s, v);
        break;
    case R_MARK:
        swim_write_byte(s, FIFO_MARK | v);
        break;
    case R_ERROR:
        swim_write_byte(s, FIFO_CRC);
        break;
    case R_PARAM:
        s->param[s->param_idx] = v;
        s->param_idx = (s->param_idx + 1) & 3;
        break;
    case R_PHASES:
        s->phases = v;
        d = swim_drive(s);
        if (d) {
            drive_phases(d, swim_phases_out(s), s->hdsel);
        }
        break;
    case R_SETUP:
        s->setup = v;
        break;
    case R_MODE:
        s->mode = (s->mode & ~v) | MODE_ISM;
        s->param_idx = 0;
        swim_mode_changed(s, old_mode);
        break;
    case R_HANDSHAKE:
        s->mode |= v;
        swim_mode_changed(s, old_mode);
        break;
    }
}

static const MemoryRegionOps swim_ops = {
    .read = swim_read,
    .write = swim_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
};

/* VIA1 PA5: the SEL line of the drives */
static void swim_hdsel(void *opaque, int n, int level)
{
    SWIMCtrl *s = &SWIM(opaque)->ctrl;

    s->hdsel = level;
}

/* Media */

static void swim_change_cb(void *opaque, bool load, Error **errp)
{
    FDrive *drive = opaque;

    drive_flush(drive);
    drive->track_valid = false;
    drive->track_len = 0;
    drive->dskchg = false;

    if (!load) {
        drive->tray_open = true;
        blk_set_perm(drive->blk, 0, BLK_PERM_ALL, &error_abort);
    } else {
        drive->tray_open = false;
        if (!blkconf_apply_backend_options(drive->conf,
                                           !blk_supports_write_perm(drive->blk),
                                           true, errp)) {
            return;
        }
    }
    drive_set_layout(drive);
}

static bool swim_is_tray_open(void *opaque)
{
    FDrive *drive = opaque;

    return drive->tray_open;
}

/* The host ejects the disk: the paper clip in the hole */
static void swim_eject_request_cb(void *opaque, bool force)
{
    drive_eject(opaque);
}

static const BlockDevOps swim_block_ops = {
    .change_media_cb = swim_change_cb,
    .is_tray_open = swim_is_tray_open,
    .eject_request_cb = swim_eject_request_cb,
};

static const Property swim_drive_properties[] = {
    DEFINE_PROP_INT32("unit", SWIMDrive, unit, -1),
    DEFINE_BLOCK_PROPERTIES(SWIMDrive, conf),
};

static void swim_drive_realize(DeviceState *qdev, Error **errp)
{
    SWIMDrive *dev = SWIM_DRIVE(qdev);
    SWIMBus *bus = SWIM_BUS(qdev->parent_bus);
    FDrive *drive;
    int ret;

    if (dev->unit == -1) {
        for (dev->unit = 0; dev->unit < SWIM_MAX_FD; dev->unit++) {
            drive = &bus->ctrl->drives[dev->unit];
            if (!drive->blk) {
                break;
            }
        }
    }

    if (dev->unit >= SWIM_MAX_FD) {
        error_setg(errp, "Can't create floppy unit %d, bus supports "
                   "only %d units", dev->unit, SWIM_MAX_FD);
        return;
    }

    drive = &bus->ctrl->drives[dev->unit];
    if (drive->blk) {
        error_setg(errp, "Floppy unit %d is in use", dev->unit);
        return;
    }

    if (!dev->conf.blk) {
        /* Anonymous BlockBackend for an empty drive */
        dev->conf.blk = blk_new(qemu_get_aio_context(), 0, BLK_PERM_ALL);
        ret = blk_attach_dev(dev->conf.blk, qdev);
        assert(ret == 0);
    }

    if (!blkconf_blocksizes(&dev->conf, errp)) {
        return;
    }

    if (dev->conf.logical_block_size != 512 ||
        dev->conf.physical_block_size != 512)
    {
        error_setg(errp, "Physical and logical block size must "
                   "be 512 for floppy");
        return;
    }

    /*
     * rerror/werror aren't supported by fdc and therefore not even registered
     * with qdev. So set the defaults manually before they are used in
     * blkconf_apply_backend_options().
     */
    dev->conf.rerror = BLOCKDEV_ON_ERROR_AUTO;
    dev->conf.werror = BLOCKDEV_ON_ERROR_AUTO;

    /* Resizable: a format into another layout changes the image size */
    if (!blkconf_apply_backend_options(&dev->conf,
                                       !blk_supports_write_perm(dev->conf.blk),
                                       true, errp)) {
        return;
    }

    /*
     * 'enospc' is the default for -drive, 'report' is what blk_new() gives us
     * for empty drives.
     */
    if (blk_get_on_error(dev->conf.blk, 0) != BLOCKDEV_ON_ERROR_ENOSPC &&
        blk_get_on_error(dev->conf.blk, 0) != BLOCKDEV_ON_ERROR_REPORT) {
        error_setg(errp, "fdc doesn't support drive option werror");
        return;
    }
    if (blk_get_on_error(dev->conf.blk, 1) != BLOCKDEV_ON_ERROR_REPORT) {
        error_setg(errp, "fdc doesn't support drive option rerror");
        return;
    }

    drive->conf = &dev->conf;
    drive->blk = dev->conf.blk;
    drive->swimctrl = bus->ctrl;
    drive->unit = dev->unit;
    drive->mfm = true;
    drive->tray_open = false;
    drive->dskchg = false;
    drive->track_valid = false;

    blk_set_dev_ops(drive->blk, &swim_block_ops, drive);
    drive_set_layout(drive);
}

static void swim_drive_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *k = DEVICE_CLASS(klass);
    k->realize = swim_drive_realize;
    set_bit(DEVICE_CATEGORY_STORAGE, k->categories);
    k->bus_type = TYPE_SWIM_BUS;
    device_class_set_props(k, swim_drive_properties);
    k->desc = "virtual SWIM drive (Apple SuperDrive)";
}

static const TypeInfo swim_drive_info = {
    .name = TYPE_SWIM_DRIVE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SWIMDrive),
    .class_init = swim_drive_class_init,
};

static const TypeInfo swim_bus_info = {
    .name = TYPE_SWIM_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(SWIMBus),
};

static void sysbus_swim_reset(DeviceState *dev)
{
    Swim *sys = SWIM(dev);
    SWIMCtrl *s = &sys->ctrl;

    for (int i = 0; i < SWIM_MAX_FD; i++) {
        FDrive *d = &s->drives[i];

        drive_flush(d);
        d->motor = false;
        d->mfm = true;
        d->strb = false;
        d->dskchg = false;
        d->track_valid = false;
        d->track_len = 0;
    }
    s->mode = MODE_ISM;
    s->setup = 0;
    s->phases = 0xf0;
    memset(s->param, 0, sizeof(s->param));
    s->param_idx = 0;
    s->error = 0;
    fifo_clear(s);
    s->reading = false;
    s->writing = false;
    s->synced = false;
    s->gcr_sr = 0;
}

static void sysbus_swim_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    Swim *sbs = SWIM(obj);
    SWIMCtrl *swimctrl = &sbs->ctrl;

    memory_region_init_io(&swimctrl->swim, obj, &swim_ops, swimctrl,
                          "swim", 0x2000);
    sysbus_init_mmio(sbd, &swimctrl->swim);
    qdev_init_gpio_in_named(DEVICE(obj), swim_hdsel, "headsel", 1);
}

static void sysbus_swim_realize(DeviceState *dev, Error **errp)
{
    Swim *sys = SWIM(dev);
    SWIMCtrl *swimctrl = &sys->ctrl;

    qbus_init(&swimctrl->bus, sizeof(SWIMBus), TYPE_SWIM_BUS, dev, NULL);
    swimctrl->bus.ctrl = swimctrl;
}

static const VMStateDescription vmstate_fdrive = {
    .name = "fdrive",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(cyl, FDrive),
        VMSTATE_BOOL(dir, FDrive),
        VMSTATE_BOOL(motor, FDrive),
        VMSTATE_BOOL(mfm, FDrive),
        VMSTATE_BOOL(dskchg, FDrive),
        VMSTATE_BOOL(strb, FDrive),
        VMSTATE_BOOL(tray_open, FDrive),
        VMSTATE_INT32(layout, FDrive),
        VMSTATE_BOOL(hd, FDrive),
        VMSTATE_BOOL(track_valid, FDrive),
        VMSTATE_BOOL(track_dirty, FDrive),
        VMSTATE_UINT8(track_cyl, FDrive),
        VMSTATE_UINT8(track_head, FDrive),
        VMSTATE_INT32(track_enc, FDrive),
        VMSTATE_INT32(track_len, FDrive),
        VMSTATE_UINT32(pos, FDrive),
        VMSTATE_INT64(pos_time, FDrive),
        VMSTATE_BOOL(stop_valid, FDrive),
        VMSTATE_UINT32(stop_pos, FDrive),
        VMSTATE_INT64(stop_time, FDrive),
        VMSTATE_UINT16_ARRAY(track, FDrive, SWIM_TRACK_BUF),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_swim = {
    .name = "swim",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(mode, SWIMCtrl),
        VMSTATE_UINT8(setup, SWIMCtrl),
        VMSTATE_UINT8(phases, SWIMCtrl),
        VMSTATE_UINT8_ARRAY(param, SWIMCtrl, 4),
        VMSTATE_UINT8(param_idx, SWIMCtrl),
        VMSTATE_UINT8(error, SWIMCtrl),
        VMSTATE_UINT16_ARRAY(fifo, SWIMCtrl, 2),
        VMSTATE_UINT8(fifo_pos, SWIMCtrl),
        VMSTATE_UINT16(crc, SWIMCtrl),
        VMSTATE_BOOL(hdsel, SWIMCtrl),
        VMSTATE_BOOL(reading, SWIMCtrl),
        VMSTATE_BOOL(writing, SWIMCtrl),
        VMSTATE_BOOL(synced, SWIMCtrl),
        VMSTATE_UINT8(gcr_sr, SWIMCtrl),
        VMSTATE_INT64(xfer_time, SWIMCtrl),
        VMSTATE_UINT32(write_start, SWIMCtrl),
        VMSTATE_UINT32(write_count, SWIMCtrl),
        VMSTATE_STRUCT_ARRAY(drives, SWIMCtrl, SWIM_MAX_FD, 2,
                             vmstate_fdrive, FDrive),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sysbus_swim = {
    .name = "SWIM",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(ctrl, Swim, 2, vmstate_swim, SWIMCtrl),
        VMSTATE_END_OF_LIST()
    }
};

static void sysbus_swim_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = sysbus_swim_realize;
    device_class_set_legacy_reset(dc, sysbus_swim_reset);
    dc->vmsd = &vmstate_sysbus_swim;
}

static const TypeInfo sysbus_swim_info = {
    .name          = TYPE_SWIM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Swim),
    .instance_init = sysbus_swim_init,
    .class_init    = sysbus_swim_class_init,
};

static void swim_register_types(void)
{
    type_register_static(&sysbus_swim_info);
    type_register_static(&swim_bus_info);
    type_register_static(&swim_drive_info);
}

type_init(swim_register_types)
