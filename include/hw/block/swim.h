/*
 * QEMU Macintosh floppy disk controller emulator (SWIM2)
 *
 * Copyright (c) 2014-2018 Laurent Vivier <laurent@vivier.eu>
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#ifndef SWIM_H
#define SWIM_H

#include "hw/block/block.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define SWIM_MAX_FD            2
#define SWIM_TRACK_BUF         12800    /* SWIM_TRACK_MAX of swim-track.h */

typedef struct SWIMCtrl SWIMCtrl;

#define TYPE_SWIM_DRIVE "swim-drive"
OBJECT_DECLARE_SIMPLE_TYPE(SWIMDrive, SWIM_DRIVE)

struct SWIMDrive {
    DeviceState qdev;
    int32_t     unit;
    BlockConf   conf;
};

#define TYPE_SWIM_BUS "swim-bus"
OBJECT_DECLARE_SIMPLE_TYPE(SWIMBus, SWIM_BUS)

struct SWIMBus {
    BusState bus;
    struct SWIMCtrl *ctrl;
};

/* A Sony MFD-75W SuperDrive */
typedef struct FDrive {
    SWIMCtrl *swimctrl;
    BlockBackend *blk;
    BlockConf *conf;
    int unit;

    /* Drive */
    uint8_t cyl;
    bool dir;                   /* true: step towards track 0 */
    bool motor;
    bool mfm;                   /* MFM mode (else GCR) */
    bool dskchg;                /* true once the guest cleared disk-changed */
    bool strb;                  /* last LSTRB */
    bool tray_open;             /* ejected: no disk */

    /* Medium */
    int32_t layout;             /* SwimLayout of the image */
    bool hd;                    /* HD media (else DD) */

    /* The track under the head */
    bool track_valid;
    bool track_dirty;
    uint8_t track_cyl;
    uint8_t track_head;
    int32_t track_enc;          /* SwimEnc */
    int32_t track_len;
    uint32_t pos;
    int64_t pos_time;           /* QEMU_CLOCK_VIRTUAL of pos, when idle */
    bool stop_valid;            /* the last transfer ended on this track */
    uint32_t stop_pos;          /* where it ended */
    int64_t stop_time;          /* and when */
    uint16_t track[SWIM_TRACK_BUF];
} FDrive;

struct SWIMCtrl {
    MemoryRegion swim;
    FDrive drives[SWIM_MAX_FD];
    SWIMBus bus;

    uint8_t mode;
    uint8_t setup;
    uint8_t phases;
    uint8_t param[4];
    uint8_t param_idx;
    uint8_t error;
    uint16_t fifo[2];
    uint8_t fifo_pos;
    uint16_t crc;
    bool hdsel;                 /* VIA1 PA5: SEL of the selected drive */

    /* Data path */
    bool reading;
    bool writing;
    bool synced;                /* MFM read found a mark */
    uint8_t gcr_sr;             /* GCR write: bits not yet a nibble */
    int64_t xfer_time;          /* QEMU_CLOCK_VIRTUAL the next byte is due */
    uint32_t write_start;       /* where the write began on the track */
    uint32_t write_count;       /* bytes it put there */
};

#define TYPE_SWIM "swim"
OBJECT_DECLARE_SIMPLE_TYPE(Swim, SWIM)

struct Swim {
    SysBusDevice parent_obj;
    SWIMCtrl     ctrl;
};
#endif
