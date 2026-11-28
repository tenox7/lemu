/*
 * The parts of libaaruformat the AaruFormat block driver uses
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * libaaruformat's headers declare names QEMU also uses (ImageInfo, DIR...),
 * so only aaruformat-lib.c includes them, and the driver goes through this
 * small interface. Like the library, none of it is thread-safe: calls on
 * one image must not run at the same time.
 */

#ifndef BLOCK_AARUFORMAT_LIB_H
#define BLOCK_AARUFORMAT_LIB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct AaruLib AaruLib;

typedef enum AaruLibMedia {
    AARU_LIB_MEDIA_BLOCK,
    AARU_LIB_MEDIA_OPTICAL,
    AARU_LIB_MEDIA_OTHER,
} AaruLibMedia;

typedef struct AaruLibInfo {
    AaruLibMedia media;
    bool is_tape;
    uint32_t media_type;     /* AaruFormat MediaType number */
    uint32_t sector_size;    /* biggest sector stored */
    uint64_t sectors;
    uint8_t data_shift;      /* sectors per data block, as a power of two */
    bool error_recovery;     /* has erasure coding (error recovery) data */
    char application[65];
    char application_version[33];
} AaruLibInfo;

typedef enum AaruLibTrackType {
    AARU_LIB_TRACK_AUDIO,
    AARU_LIB_TRACK_DATA,
    AARU_LIB_TRACK_MODE1,
    AARU_LIB_TRACK_MODE2,
    AARU_LIB_TRACK_MODE2_FORM1,
    AARU_LIB_TRACK_MODE2_FORM2,
} AaruLibTrackType;

typedef struct AaruLibTrack {
    uint8_t sequence;
    uint8_t type;            /* AaruLibTrackType */
    uint8_t session;
    uint8_t flags;
    int64_t start;
    int64_t end;
    int64_t pregap;
} AaruLibTrack;

/* Results of the read functions */
#define AARU_LIB_OK          0
#define AARU_LIB_NOT_DUMPED  1   /* never written: no data */
#define AARU_LIB_ERROR       (-1)

/* Media type for hard disks */
#define AARU_LIB_GENERIC_HDD 2

/* True if the header in @buf is an AaruFormat image this library opens */
bool aaru_lib_probe(const uint8_t *buf, size_t size);
/* Bytes aaru_lib_probe() needs */
size_t aaru_lib_probe_size(void);

/* Returns NULL with errno set on failure */
AaruLib *aaru_lib_open(const char *path, bool writable, const char *options,
                       AaruLibInfo *info);
int aaru_lib_close(AaruLib *img);

/*
 * Creates the image with sector 0 written as zeros, so its sector size is
 * recorded. @recovery_percent (1-100) adds error recovery data, as Aaru's
 * --error-recovery does; 0 adds none.
 */
int aaru_lib_create(const char *path, uint32_t media_type,
                    uint32_t sector_size, uint64_t sectors,
                    const char *options, const char *application,
                    uint8_t major, uint8_t minor, uint8_t recovery_percent);

/* *length: buffer size in, bytes read out */
int aaru_lib_read_sector(AaruLib *img, uint64_t lba, uint8_t *buf,
                         uint32_t *length);
int aaru_lib_read_sector_long(AaruLib *img, uint64_t lba, uint8_t *buf,
                              uint32_t *length);
/* 96 bytes; AARU_LIB_ERROR if the image has no subchannel */
int aaru_lib_read_subchannel(AaruLib *img, uint64_t lba, uint8_t *buf);

int aaru_lib_write_sector(AaruLib *img, uint64_t lba, const uint8_t *buf,
                          uint32_t length);
int aaru_lib_flush(AaruLib *img);
int aaru_lib_set_compression(AaruLib *img, bool enabled, bool zstd);
/*
 * Adds error recovery data (erasure coding) for the data written from now
 * on, before anything is written. It covers what this session writes: a
 * later session that writes to the image drops it.
 */
int aaru_lib_set_error_recovery(AaruLib *img, uint8_t percent);

/* free()-able array in *tracks; 0 tracks if the image has no track list */
int aaru_lib_get_tracks(AaruLib *img, AaruLibTrack **tracks, int *count);

#endif
