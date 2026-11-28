/*
 * Macintosh floppy tracks as the SWIM2 sees them: GCR nibbles and MFM bytes
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * The GCR encoding follows MAME's src/lib/formats/flopimg.cpp
 * (BSD-3-Clause, copyright-holders: Olivier Galibert).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_BLOCK_SWIM_TRACK_H
#define HW_BLOCK_SWIM_TRACK_H

/* A track is an array of these: a byte, plus a flag for an MFM mark */
#define SWIM_MARK           0x100

/* Longest track: MFM HD is 12500 bytes, GCR zone 0 about 9700 nibbles */
#define SWIM_TRACK_MAX      12800

#define SWIM_CYLS           80
#define SWIM_SECTOR         512
#define SWIM_MAX_SPT        24

typedef enum SwimEnc {
    SWIM_ENC_NONE,
    SWIM_ENC_GCR,
    SWIM_ENC_MFM,
} SwimEnc;

/* How the sectors of a raw image are laid out; the file size tells which */
typedef enum SwimLayout {
    SWIM_LAYOUT_NONE,
    SWIM_LAYOUT_GCR400,         /* Mac 400K, one side */
    SWIM_LAYOUT_GCR800,         /* Mac 800K */
    SWIM_LAYOUT_MFM360,         /* DOS 360K, one side, 3.5" */
    SWIM_LAYOUT_MFM720,         /* DOS 720K */
    SWIM_LAYOUT_MFM1440,        /* 1.44M, HFS or DOS */
} SwimLayout;

typedef struct SwimSector {
    uint8_t cyl;
    uint8_t head;
    uint8_t sector;             /* GCR: from 0; MFM: the ID, from 1 */
    uint8_t size;               /* MFM size code N; GCR: 2 */
    uint8_t format;             /* GCR format byte; MFM: 0 */
    uint8_t data[SWIM_SECTOR];
} SwimSector;

const char *swim_layout_name(SwimLayout l);
SwimLayout swim_layout_from_size(int64_t size);
int64_t swim_layout_size(SwimLayout l);
SwimEnc swim_layout_enc(SwimLayout l);
int swim_layout_heads(SwimLayout l);
bool swim_layout_hd(SwimLayout l);

/* Sectors per track of @l on cylinder @cyl */
int swim_layout_spt(SwimLayout l, int cyl);

/*
 * Byte offset in the image of sector @idx (from 0, also for MFM) of
 * @cyl/@head, or -1 when there is no such sector.
 */
int64_t swim_layout_offset(SwimLayout l, int cyl, int head, int idx);

/* Length of a track in bytes (MFM) or nibbles (GCR) */
int swim_track_len(SwimEnc enc, bool hd, int cyl);

/* RPM of the disk for a track of @enc on @cyl */
int swim_track_rpm(SwimEnc enc, int cyl);

/*
 * Build track @cyl/@head of layout @l into @buf from @data, the sectors of
 * the track in order (swim_layout_spt() * 512 bytes).  Returns the length.
 */
int swim_track_build(uint16_t *buf, SwimLayout l, int cyl, int head,
                     const uint8_t *data);

/* An unformatted track of @enc: sync only, no headers */
int swim_track_blank(uint16_t *buf, SwimEnc enc, bool hd, int cyl);

/*
 * Find the sectors with a good header and a good data field on a track.
 * Where a sector has more than one data field (a write that landed a
 * little off the old one), the last good one wins.  Returns how many were
 * stored in @out, at most @max.
 */
int swim_track_decode(const uint16_t *buf, int len, SwimEnc enc,
                      SwimSector *out, int max);

/* CRC-CCITT of one byte, MSB first */
uint16_t swim_crc_byte(uint16_t crc, uint8_t b);

/* CRC after the three A1 marks of an MFM address mark */
#define SWIM_CRC_AFTER_MARKS    0xcdb4

#endif
