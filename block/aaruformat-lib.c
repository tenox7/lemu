/*
 * The parts of libaaruformat the AaruFormat block driver uses
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This is the only file that includes libaaruformat's headers: they declare
 * names QEMU also uses, so no QEMU header is included here.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-declaration"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Wundef"
#include <aaruformat.h>
#pragma GCC diagnostic pop

#include "aaruformat-lib.h"

size_t aaru_lib_probe_size(void)
{
    return sizeof(AaruHeader);
}

bool aaru_lib_probe(const uint8_t *buf, size_t size)
{
    AaruHeader header;

    if (size < sizeof(header)) {
        return false;
    }

    /* AaruFormat is little-endian, as are the hosts LEMU runs on */
    memcpy(&header, buf, sizeof(header));

    return (header.identifier == AARU_MAGIC ||
            header.identifier == DIC_MAGIC) &&
           header.imageMajorVersion <= AARUF_VERSION;
}

static void copy_string(char *dst, size_t dst_size, const char *src,
                        size_t src_size)
{
    size_t n = strnlen(src, src_size);

    if (n >= dst_size) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

AaruLib *aaru_lib_open(const char *path, bool writable, const char *options,
                       AaruLibInfo *info)
{
    aaruformat_context *ctx = aaruf_open(path, writable, options);
    ImageInfo image_info;

    if (!ctx) {
        return NULL;
    }

    if (aaruf_get_image_info(ctx, &image_info) != AARUF_STATUS_OK) {
        aaruf_close(ctx);
        errno = EINVAL;
        return NULL;
    }

    memset(info, 0, sizeof(*info));
    switch (image_info.MetadataMediaType) {
    case BlockMedia:
        info->media = AARU_LIB_MEDIA_BLOCK;
        break;
    case OpticalDisc:
        info->media = AARU_LIB_MEDIA_OPTICAL;
        break;
    default:
        info->media = AARU_LIB_MEDIA_OTHER;
        break;
    }
    info->is_tape = ctx->is_tape;
    info->media_type = image_info.MediaType;
    info->sector_size = image_info.SectorSize;
    info->sectors = image_info.Sectors;
    info->data_shift = ctx->user_data_ddt_header.dataShift;
    info->error_recovery =
        !!(ctx->header.featureCompatibleRo & AARU_FEATURE_ROCOMPAT_ERASURE);
    copy_string(info->application, sizeof(info->application),
                image_info.Application, sizeof(image_info.Application));
    copy_string(info->application_version, sizeof(info->application_version),
                image_info.ApplicationVersion,
                sizeof(image_info.ApplicationVersion));

    return (AaruLib *)ctx;
}

int aaru_lib_close(AaruLib *img)
{
    return aaruf_close(img) == 0 ? AARU_LIB_OK : AARU_LIB_ERROR;
}

int aaru_lib_create(const char *path, uint32_t media_type,
                    uint32_t sector_size, uint64_t sectors,
                    const char *options, const char *application,
                    uint8_t major, uint8_t minor, uint8_t recovery_percent)
{
    void *ctx = aaruf_create(path, media_type, sector_size, sectors, 0, 0,
                             options, (const uint8_t *)application,
                             (uint8_t)strlen(application), major, minor,
                             false);

    uint8_t *zero;
    int ret = AARU_LIB_OK;

    if (!ctx) {
        return AARU_LIB_ERROR;
    }

    if (recovery_percent &&
        aaruf_set_erasure_coding_auto(ctx, recovery_percent) < 0) {
        aaruf_close(ctx);
        return AARU_LIB_ERROR;
    }

    /*
     * The sector size is only recorded with the first sector written, and
     * an image opened without one has none: write sector 0, as zeros, so
     * the size is known.
     */
    zero = calloc(1, sector_size);
    if (!zero || aaruf_write_sector(ctx, 0, false, zero, SectorStatusDumped,
                                    sector_size) < 0) {
        ret = AARU_LIB_ERROR;
    }
    free(zero);

    if (aaruf_close(ctx) != 0) {
        ret = AARU_LIB_ERROR;
    }

    return ret;
}

static int read_result(int32_t ret)
{
    if (ret == AARUF_STATUS_SECTOR_NOT_DUMPED) {
        return AARU_LIB_NOT_DUMPED;
    }
    /* Other positive statuses still give the data (sector with errors...) */
    return ret < 0 ? AARU_LIB_ERROR : AARU_LIB_OK;
}

int aaru_lib_read_sector(AaruLib *img, uint64_t lba, uint8_t *buf,
                         uint32_t *length)
{
    uint8_t status = 0;

    return read_result(aaruf_read_sector(img, lba, false, buf, length,
                                         &status));
}

int aaru_lib_read_sector_long(AaruLib *img, uint64_t lba, uint8_t *buf,
                              uint32_t *length)
{
    uint8_t status = 0;

    return read_result(aaruf_read_sector_long(img, lba, false, buf, length,
                                              &status));
}

int aaru_lib_read_subchannel(AaruLib *img, uint64_t lba, uint8_t *buf)
{
    uint32_t length = 96;

    if (aaruf_read_sector_tag(img, lba, false, buf, &length,
                              kSectorTagCdSubchannel) != AARUF_STATUS_OK ||
        length != 96) {
        return AARU_LIB_ERROR;
    }

    return AARU_LIB_OK;
}

int aaru_lib_write_sector(AaruLib *img, uint64_t lba, const uint8_t *buf,
                          uint32_t length)
{
    return aaruf_write_sector(img, lba, false, buf, SectorStatusDumped,
                              length) < 0 ? AARU_LIB_ERROR : AARU_LIB_OK;
}

int aaru_lib_flush(AaruLib *img)
{
    return aaruf_flush(img) < 0 ? AARU_LIB_ERROR : AARU_LIB_OK;
}

int aaru_lib_set_compression(AaruLib *img, bool enabled, bool zstd)
{
    return aaruf_set_compression(img, enabled, zstd) < 0 ? AARU_LIB_ERROR :
                                                            AARU_LIB_OK;
}

int aaru_lib_set_error_recovery(AaruLib *img, uint8_t percent)
{
    return aaruf_set_erasure_coding_auto(img, percent) < 0 ? AARU_LIB_ERROR :
                                                              AARU_LIB_OK;
}

int aaru_lib_get_tracks(AaruLib *img, AaruLibTrack **tracks, int *count)
{
    size_t length = 0;
    TrackEntry *entries;
    int32_t ret;
    size_t n;

    *tracks = NULL;
    *count = 0;

    ret = aaruf_get_tracks(img, NULL, &length);
    if (ret == AARUF_ERROR_TRACK_NOT_FOUND ||
        ret == AARUF_ERROR_INCORRECT_MEDIA_TYPE ||
        (ret == AARUF_ERROR_BUFFER_TOO_SMALL && length == 0)) {
        return AARU_LIB_OK;
    }
    if (ret != AARUF_ERROR_BUFFER_TOO_SMALL) {
        return ret == AARUF_STATUS_OK ? AARU_LIB_OK : AARU_LIB_ERROR;
    }

    entries = malloc(length);
    if (!entries) {
        return AARU_LIB_ERROR;
    }
    if (aaruf_get_tracks(img, (uint8_t *)entries, &length) != AARUF_STATUS_OK) {
        free(entries);
        return AARU_LIB_ERROR;
    }

    n = length / sizeof(TrackEntry);
    *tracks = calloc(n ? n : 1, sizeof(AaruLibTrack));
    if (!*tracks) {
        free(entries);
        return AARU_LIB_ERROR;
    }

    for (size_t i = 0; i < n; i++) {
        AaruLibTrack *t = &(*tracks)[i];

        t->sequence = entries[i].sequence;
        t->session = entries[i].session;
        t->flags = entries[i].flags;
        t->start = entries[i].start;
        t->end = entries[i].end;
        t->pregap = entries[i].pregap;

        switch (entries[i].type) {
        case kTrackTypeAudio:
            t->type = AARU_LIB_TRACK_AUDIO;
            break;
        case kTrackTypeCdMode1:
            t->type = AARU_LIB_TRACK_MODE1;
            break;
        case kTrackTypeCdMode2Formless:
            t->type = AARU_LIB_TRACK_MODE2;
            break;
        case kTrackTypeCdMode2Form1:
            t->type = AARU_LIB_TRACK_MODE2_FORM1;
            break;
        case kTrackTypeCdMode2Form2:
            t->type = AARU_LIB_TRACK_MODE2_FORM2;
            break;
        default:
            t->type = AARU_LIB_TRACK_DATA;
            break;
        }
    }

    free(entries);
    *count = (int)n;
    return AARU_LIB_OK;
}
