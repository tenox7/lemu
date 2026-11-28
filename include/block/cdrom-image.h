/*
 * CD images with tracks and raw sectors, for the CD-ROM devices
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Most images hold only the 2048-byte user data of a single data track.
 * Image formats that keep the whole disc (such as AaruFormat) also give the
 * CD-ROM devices the real track list and each raw 2352-byte sector, with its
 * subchannel when the image has it, so a guest sees the disc as it was.
 */

#ifndef BLOCK_CDROM_IMAGE_H
#define BLOCK_CDROM_IMAGE_H

#define BLOCK_CD_SECTOR      2048
#define BLOCK_CD_RAW_SECTOR  2352
#define BLOCK_CD_SUBCHANNEL  96
#define BLOCK_CD_MAX_TRACKS  99

/* Raw reads return this (not an error) when the image has no subchannel */
#define BLOCK_CD_NO_SUBCHANNEL 1

typedef enum BlockCdTrackMode {
    BLOCK_CD_TRACK_AUDIO,
    BLOCK_CD_TRACK_MODE1,
    BLOCK_CD_TRACK_MODE2,       /* mode 2 without forms */
    BLOCK_CD_TRACK_MODE2_FORM1,
    BLOCK_CD_TRACK_MODE2_FORM2,
} BlockCdTrackMode;

/* Disc type, as in the first track descriptor of a full TOC */
#define BLOCK_CD_DISC_CDDA_CDROM 0x00
#define BLOCK_CD_DISC_CDI        0x10
#define BLOCK_CD_DISC_XA         0x20

typedef struct BlockCdTrack {
    uint8_t number;   /* 1..99 */
    uint8_t session;  /* 1-based */
    uint8_t control;  /* Q subchannel control nibble (0x04: data track) */
    uint8_t mode;     /* BlockCdTrackMode */
    int64_t start;    /* first LBA, included */
    int64_t end;      /* last LBA, included */
    int64_t pregap;   /* sectors of pre-gap before start */
} BlockCdTrack;

typedef struct BlockCdToc {
    int n_tracks;                 /* in disc order */
    uint8_t first_session;
    uint8_t last_session;
    uint8_t disc_type;            /* BLOCK_CD_DISC_* */
    int64_t leadout;              /* LBA of the lead-out */
    BlockCdTrack tracks[BLOCK_CD_MAX_TRACKS];
} BlockCdToc;

static inline bool block_cd_track_is_audio(const BlockCdTrack *t)
{
    return t->mode == BLOCK_CD_TRACK_AUDIO;
}

/* The track holding @lba, or NULL if it is in no track */
static inline const BlockCdTrack *block_cd_toc_find(const BlockCdToc *toc,
                                                    int64_t lba)
{
    for (int i = 0; i < toc->n_tracks; i++) {
        if (lba >= toc->tracks[i].start && lba <= toc->tracks[i].end) {
            return &toc->tracks[i];
        }
    }
    return NULL;
}

#endif
