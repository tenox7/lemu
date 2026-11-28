/*
 * Block driver for AaruFormat images, through libaaruformat
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * AaruFormat is the image format of the Aaru Data Preservation Suite. Hard
 * disk images (block media) can be read and written; CD and DVD images
 * (optical discs) are read-only, with their real track list and raw
 * sectors given to the CD-ROM devices.
 *
 * libaaruformat opens images by path with stdio, so the driver keeps the
 * usual "file" child for probing, locking and permissions, and opens the
 * same local file again through the library. Library calls block and are
 * not thread-safe, so each one runs in the thread pool while holding the
 * driver's lock.
 *
 * Writes go to the end of the image (AaruFormat is copy-on-write), and a
 * flush writes a checkpoint the image can always be opened from. Use
 * "qemu-img convert" to make a compact copy.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-visit-block-core.h"
#include "qobject/qdict.h"
#include "qobject/qobject.h"
#include "qapi/qobject-input-visitor.h"
#include "block/block-io.h"
#include "block/block-global-state.h"
#include "block/block_int.h"
#include "block/cdrom-image.h"
#include "block/qdict.h"
#include "block/thread-pool.h"
#include "qemu/coroutine.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/memalign.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "migration/blocker.h"
#include "qemu-version.h"

#include "aaruformat-lib.h"

/* Biggest sector the library stores, plus room for a raw CD sector */
#define AARUFORMAT_MAX_SECTOR 65536

#define AARUFORMAT_OPT_SECTOR_SIZE      "sector_size"
#define AARUFORMAT_OPT_COMPRESS         "compress"
#define AARUFORMAT_OPT_COMPRESSION_TYPE "compression_type"
#define AARUFORMAT_OPT_DEDUPLICATE      "deduplicate"
#define AARUFORMAT_OPT_TABLE_SHIFT      "table_shift"
#define AARUFORMAT_OPT_ERROR_RECOVERY   "error_recovery"

#define AARUFORMAT_RT_COMPRESS         "compress"
#define AARUFORMAT_RT_COMPRESSION_TYPE "compression-type"
#define AARUFORMAT_RT_DEDUPLICATE      "deduplicate"
#define AARUFORMAT_RT_CHECKPOINT       "checkpoint-interval"

/* Seconds after a write without a flush before a checkpoint is written */
#define AARUFORMAT_DEFAULT_CHECKPOINT  5

typedef struct BDRVAaruFormatState {
    BlockDriverState *bs;
    CoMutex lock;
    AaruLib *ctx;           /* libaaruformat image */
    char *path;             /* local file the library opened */
    bool writable;
    bool optical;
    uint32_t sector_size;   /* bytes of a sector as the guest sees it */
    uint64_t sectors;
    uint32_t media_type;
    uint32_t cluster_size;
    char application[65];
    char application_version[33];
    bool error_recovery;    /* the image has error recovery data */
    bool compress;
    bool zstd;
    bool deduplicate;
    bool compressed_writes; /* compression turned on by a compressed write */
    uint8_t *scratch;       /* AARUFORMAT_MAX_SECTOR bytes, used under lock */
    BlockCdToc *toc;        /* optical discs only */
    Error *migration_blocker;
    /*
     * Many old guests never flush, and nothing written since the last
     * checkpoint can be found again if QEMU stops: write one on our own
     * some seconds after a write.
     */
    QEMUTimer *checkpoint_timer;
    int64_t checkpoint_ns;  /* 0: only when the guest flushes */
    bool dirty;             /* written since the last checkpoint */
} BDRVAaruFormatState;

/*
 * Compression asked for when creating an image only reaches the data written
 * by the same process right afterwards (qemu-img convert opens the new image
 * once it is created), because AaruFormat does not keep that setting.
 */
typedef struct AaruFormatPendingCreate {
    char *path;
    bool compress;
    bool zstd;
    bool deduplicate;
    uint8_t recovery;
    QLIST_ENTRY(AaruFormatPendingCreate) next;
} AaruFormatPendingCreate;

static QLIST_HEAD(, AaruFormatPendingCreate) aaruformat_pending =
    QLIST_HEAD_INITIALIZER(aaruformat_pending);
static QemuMutex aaruformat_pending_lock;

static void __attribute__((constructor)) aaruformat_pending_init(void)
{
    qemu_mutex_init(&aaruformat_pending_lock);
}

static void aaruformat_pending_add(const char *path, bool compress, bool zstd,
                                   bool deduplicate, uint8_t recovery)
{
    AaruFormatPendingCreate *p = g_new0(AaruFormatPendingCreate, 1);

    p->recovery = recovery;
    p->path = g_strdup(path);
    p->compress = compress;
    p->zstd = zstd;
    p->deduplicate = deduplicate;

    qemu_mutex_lock(&aaruformat_pending_lock);
    QLIST_INSERT_HEAD(&aaruformat_pending, p, next);
    qemu_mutex_unlock(&aaruformat_pending_lock);
}

static bool aaruformat_pending_take(const char *path, bool *compress,
                                    bool *zstd, bool *deduplicate,
                                    uint8_t *recovery)
{
    AaruFormatPendingCreate *p;
    bool found = false;

    qemu_mutex_lock(&aaruformat_pending_lock);
    QLIST_FOREACH(p, &aaruformat_pending, next) {
        if (!strcmp(p->path, path)) {
            *compress = p->compress;
            *zstd = p->zstd;
            *deduplicate = p->deduplicate;
            *recovery = p->recovery;
            QLIST_REMOVE(p, next);
            g_free(p->path);
            g_free(p);
            found = true;
            break;
        }
    }
    qemu_mutex_unlock(&aaruformat_pending_lock);

    return found;
}

static int aaruformat_probe(const uint8_t *buf, int buf_size,
                            const char *filename)
{
    return aaru_lib_probe(buf, buf_size) ? 100 : 0;
}

/* The library opens the image itself, so only a local file will do */
static const char *aaruformat_local_path(BlockDriverState *file,
                                         Error **errp)
{
    if (!file->drv || !file->drv->protocol_name ||
        strcmp(file->drv->protocol_name, "file") != 0) {
        error_setg(errp, "AaruFormat images must be local files");
        return NULL;
    }

    return file->filename;
}

static char *aaruformat_options_string(bool compress, bool zstd,
                                       bool deduplicate, int table_shift)
{
    GString *str = g_string_new(NULL);

    g_string_append_printf(str, "compress=%s;deduplicate=%s",
                           compress ? "true" : "false",
                           deduplicate ? "true" : "false");
    if (compress) {
        g_string_append_printf(str, ";zstd=%s", zstd ? "true" : "false");
    }
    if (table_shift >= 0) {
        g_string_append_printf(str, ";table_shift=%d", table_shift);
    }

    return g_string_free(str, false);
}

/* Thread pool calls */

typedef struct AaruFormatIO {
    BDRVAaruFormatState *s;
    uint64_t sector;
    uint64_t count;
    uint8_t *buf;
    uint8_t *subchannel;
} AaruFormatIO;

/* Puts the 2048 bytes of user data of a CD or DVD sector in @out */
static void aaruformat_user_data(const uint8_t *data, uint32_t length,
                                 uint8_t *out)
{
    static const uint8_t sync[12] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
    };

    if (length == 2048) {
        memcpy(out, data, 2048);
        return;
    }

    /* Stored as a whole raw sector: mode 1, or mode 2 form 1 */
    if (length == 2352 && !memcmp(data, sync, sizeof(sync))) {
        if (data[15] == 1) {
            memcpy(out, data + 16, 2048);
            return;
        }
        if (data[15] == 2 && !(data[18] & 0x20)) {
            memcpy(out, data + 24, 2048);
            return;
        }
    }

    /* Audio, mode 2 form 2: no 2048-byte user data */
    memset(out, 0, 2048);
}

static int aaruformat_read_worker(void *opaque)
{
    AaruFormatIO *io = opaque;
    BDRVAaruFormatState *s = io->s;

    for (uint64_t i = 0; i < io->count; i++) {
        uint8_t *out = io->buf + i * s->sector_size;
        uint8_t *dst = s->optical ? s->scratch : out;
        uint32_t length = s->optical ? AARUFORMAT_MAX_SECTOR : s->sector_size;
        int ret;

        ret = aaru_lib_read_sector(s->ctx, io->sector + i, dst, &length);
        if (ret == AARU_LIB_NOT_DUMPED) {
            memset(out, 0, s->sector_size);
            continue;
        }
        if (ret < 0) {
            return -EIO;
        }

        if (s->optical) {
            aaruformat_user_data(s->scratch, length, out);
        } else if (length < s->sector_size) {
            memset(out + length, 0, s->sector_size - length);
        }
    }

    return 0;
}

static int aaruformat_write_worker(void *opaque)
{
    AaruFormatIO *io = opaque;
    BDRVAaruFormatState *s = io->s;

    for (uint64_t i = 0; i < io->count; i++) {
        int ret = aaru_lib_write_sector(s->ctx, io->sector + i,
                                        io->buf + i * s->sector_size,
                                        s->sector_size);
        if (ret < 0) {
            return -EIO;
        }
    }

    return 0;
}

static int aaruformat_flush_worker(void *opaque)
{
    AaruFormatIO *io = opaque;

    return aaru_lib_flush(io->s->ctx) < 0 ? -EIO : 0;
}

static int aaruformat_compress_worker(void *opaque)
{
    AaruFormatIO *io = opaque;

    return aaru_lib_set_compression(io->s->ctx, true, io->s->zstd) < 0 ?
           -EIO : 0;
}

static int aaruformat_raw_worker(void *opaque)
{
    AaruFormatIO *io = opaque;
    BDRVAaruFormatState *s = io->s;
    uint32_t length = AARUFORMAT_MAX_SECTOR;
    int ret;

    ret = aaru_lib_read_sector_long(s->ctx, io->sector, s->scratch, &length);
    if (ret == AARU_LIB_NOT_DUMPED) {
        memset(io->buf, 0, BLOCK_CD_RAW_SECTOR);
    } else if (ret < 0 || length != BLOCK_CD_RAW_SECTOR) {
        return -EIO;
    } else {
        memcpy(io->buf, s->scratch, BLOCK_CD_RAW_SECTOR);
    }

    /* An image without subchannel still gives the sector */
    if (io->subchannel &&
        aaru_lib_read_subchannel(s->ctx, io->sector, io->subchannel) < 0) {
        memset(io->subchannel, 0, BLOCK_CD_SUBCHANNEL);
        return BLOCK_CD_NO_SUBCHANNEL;
    }

    return 0;
}

static int coroutine_fn aaruformat_run(ThreadPoolFunc *func, AaruFormatIO *io)
{
    int ret;

    qemu_co_mutex_lock(&io->s->lock);
    ret = thread_pool_submit_co(func, io);
    qemu_co_mutex_unlock(&io->s->lock);

    return ret;
}

/* Opening */

static QemuOptsList aaruformat_runtime_opts = {
    .name = "aaruformat",
    .head = QTAILQ_HEAD_INITIALIZER(aaruformat_runtime_opts.head),
    .desc = {
        {
            .name = AARUFORMAT_RT_COMPRESS,
            .type = QEMU_OPT_BOOL,
            .help = "Compress the data written to the image",
        },
        {
            .name = AARUFORMAT_RT_COMPRESSION_TYPE,
            .type = QEMU_OPT_STRING,
            .help = "Compression for the written data (lzma, zstd)",
        },
        {
            .name = AARUFORMAT_RT_DEDUPLICATE,
            .type = QEMU_OPT_BOOL,
            .help = "Store identical sectors once",
        },
        {
            .name = AARUFORMAT_RT_CHECKPOINT,
            .type = QEMU_OPT_NUMBER,
            .help = "Seconds after a write before the image is checkpointed "
                    "if the guest does not flush (0: never)",
        },
        { /* end of list */ }
    },
};

/* Checkpoints */

static int coroutine_fn aaruformat_checkpoint(BDRVAaruFormatState *s)
{
    AaruFormatIO io = { .s = s };
    int ret;

    if (!s->writable || !s->dirty) {
        return 0;
    }

    /* writes coming in while it runs make it dirty again */
    s->dirty = false;
    ret = aaruformat_run(aaruformat_flush_worker, &io);
    if (ret < 0) {
        s->dirty = true;
    }
    return ret;
}

static void coroutine_fn aaruformat_checkpoint_entry(void *opaque)
{
    BDRVAaruFormatState *s = opaque;

    aaruformat_checkpoint(s);
    bdrv_dec_in_flight(s->bs);
}

static void aaruformat_checkpoint_cb(void *opaque)
{
    BDRVAaruFormatState *s = opaque;
    Coroutine *co = qemu_coroutine_create(aaruformat_checkpoint_entry, s);

    bdrv_inc_in_flight(s->bs);
    aio_co_enter(bdrv_get_aio_context(s->bs), co);
}

/* After a write: a checkpoint some seconds later, unless one is due */
static void aaruformat_written(BDRVAaruFormatState *s)
{
    s->dirty = true;
    if (s->checkpoint_timer && s->checkpoint_ns &&
        !timer_pending(s->checkpoint_timer)) {
        timer_mod(s->checkpoint_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + s->checkpoint_ns);
    }
}

static void aaruformat_detach_aio_context(BlockDriverState *bs)
{
    BDRVAaruFormatState *s = bs->opaque;

    if (s->checkpoint_timer) {
        timer_free(s->checkpoint_timer);
        s->checkpoint_timer = NULL;
    }
}

static void aaruformat_attach_aio_context(BlockDriverState *bs,
                                          AioContext *new_context)
{
    BDRVAaruFormatState *s = bs->opaque;

    /* the realtime clock, so a paused VM is checkpointed too */
    s->checkpoint_timer = aio_timer_new(new_context, QEMU_CLOCK_REALTIME,
                                        SCALE_NS, aaruformat_checkpoint_cb, s);
    if (s->dirty && s->checkpoint_ns) {
        timer_mod(s->checkpoint_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + s->checkpoint_ns);
    }
}

/* A drain waits for nothing: the checkpoint due is written now */
static void aaruformat_drain_begin(BlockDriverState *bs)
{
    BDRVAaruFormatState *s = bs->opaque;

    if (s->checkpoint_timer && timer_pending(s->checkpoint_timer)) {
        Coroutine *co = qemu_coroutine_create(aaruformat_checkpoint_entry, s);

        timer_del(s->checkpoint_timer);
        bdrv_inc_in_flight(bs);
        aio_co_enter(bdrv_get_aio_context(bs), co);
    }
}

static bool aaruformat_parse_compression(const char *str, bool *zstd,
                                         Error **errp)
{
    if (!str || !strcmp(str, "lzma")) {
        *zstd = false;
    } else if (!strcmp(str, "zstd")) {
        *zstd = true;
    } else {
        error_setg(errp, "Unknown AaruFormat compression '%s' "
                   "(use lzma or zstd)", str);
        return false;
    }
    return true;
}

static int aaruformat_load_toc(BDRVAaruFormatState *s, Error **errp)
{
    AaruLibTrack *tracks = NULL;
    BlockCdToc *toc;
    int count = 0;

    if (aaru_lib_get_tracks(s->ctx, &tracks, &count) < 0) {
        error_setg(errp, "Could not read the track list of the image");
        return -EIO;
    }

    /* A DVD, or a disc with no track list: one data track */
    s->toc = NULL;
    if (count == 0) {
        free(tracks);
        return 0;
    }

    toc = g_new0(BlockCdToc, 1);
    toc->first_session = 0xff;

    for (int i = 0; i < count && toc->n_tracks < BLOCK_CD_MAX_TRACKS; i++) {
        BlockCdTrack *t;

        /* Track 0 (hidden before track 1) and lead-in/out are not listed */
        if (tracks[i].sequence < 1 || tracks[i].sequence > 99 ||
            tracks[i].start < 0) {
            continue;
        }

        t = &toc->tracks[toc->n_tracks++];
        t->number = tracks[i].sequence;
        t->session = tracks[i].session ? tracks[i].session : 1;
        t->start = tracks[i].start;
        t->end = tracks[i].end;
        t->pregap = tracks[i].pregap;

        switch (tracks[i].type) {
        case AARU_LIB_TRACK_AUDIO:
            t->mode = BLOCK_CD_TRACK_AUDIO;
            break;
        case AARU_LIB_TRACK_MODE1:
        case AARU_LIB_TRACK_DATA:
            t->mode = BLOCK_CD_TRACK_MODE1;
            break;
        case AARU_LIB_TRACK_MODE2_FORM1:
            t->mode = BLOCK_CD_TRACK_MODE2_FORM1;
            toc->disc_type = BLOCK_CD_DISC_XA;
            break;
        case AARU_LIB_TRACK_MODE2_FORM2:
            t->mode = BLOCK_CD_TRACK_MODE2_FORM2;
            toc->disc_type = BLOCK_CD_DISC_XA;
            break;
        default:
            t->mode = BLOCK_CD_TRACK_MODE2;
            toc->disc_type = BLOCK_CD_DISC_XA;
            break;
        }

        /* Q control: the stored flags, data tracks always marked as such */
        t->control = tracks[i].flags & 0x0f;
        if (t->mode != BLOCK_CD_TRACK_AUDIO) {
            t->control |= 0x04;
        }

        toc->first_session = MIN(toc->first_session, t->session);
        toc->last_session = MAX(toc->last_session, t->session);
    }

    free(tracks);

    if (toc->n_tracks == 0) {
        g_free(toc);
        s->toc = NULL;
        return 0;
    }

    /* Tracks in disc order */
    for (int i = 1; i < toc->n_tracks; i++) {
        for (int j = i; j > 0 &&
             toc->tracks[j].start < toc->tracks[j - 1].start; j--) {
            BlockCdTrack tmp = toc->tracks[j];
            toc->tracks[j] = toc->tracks[j - 1];
            toc->tracks[j - 1] = tmp;
        }
    }

    toc->leadout = toc->tracks[toc->n_tracks - 1].end + 1;
    s->toc = toc;
    return 0;
}

/* Opens the library context for the current mode and reads the image info */
static int aaruformat_open_ctx(BDRVAaruFormatState *s, bool writable,
                               Error **errp)
{
    g_autofree char *options = NULL;
    AaruLibInfo info;

    if (writable) {
        options = aaruformat_options_string(s->compress, s->zstd,
                                            s->deduplicate, -1);
    }

    s->ctx = aaru_lib_open(s->path, writable, options, &info);
    if (!s->ctx) {
        error_setg(errp, "Could not open AaruFormat image '%s' (error %d)",
                   s->path, errno);
        return -EINVAL;
    }

    if (info.is_tape) {
        error_setg(errp, "AaruFormat tape images are not supported");
        goto fail;
    }

    s->media_type = info.media_type;
    s->sectors = info.sectors;
    pstrcpy(s->application, sizeof(s->application), info.application);
    pstrcpy(s->application_version, sizeof(s->application_version),
            info.application_version);
    s->error_recovery = info.error_recovery;

    switch (info.media) {
    case AARU_LIB_MEDIA_BLOCK:
        s->optical = false;
        s->sector_size = info.sector_size;
        if (s->sector_size == 0 || s->sector_size > AARUFORMAT_MAX_SECTOR ||
            !is_power_of_2(s->sector_size)) {
            error_setg(errp, "Unsupported AaruFormat sector size %u",
                       s->sector_size);
            goto fail;
        }
        break;
    case AARU_LIB_MEDIA_OPTICAL:
        s->optical = true;
        s->sector_size = BLOCK_CD_SECTOR;
        if (writable) {
            error_setg(errp, "AaruFormat optical disc images are read-only");
            aaru_lib_close(s->ctx);
            s->ctx = NULL;
            return -EACCES;
        }
        break;
    default:
        error_setg(errp, "Only AaruFormat hard disk, CD and DVD images are "
                   "supported");
        goto fail;
    }

    s->cluster_size = s->sector_size << info.data_shift;
    s->writable = writable;
    return 0;

fail:
    aaru_lib_close(s->ctx);
    s->ctx = NULL;
    return -ENOTSUP;
}

static int aaruformat_open(BlockDriverState *bs, QDict *options, int flags,
                           Error **errp)
{
    BDRVAaruFormatState *s = bs->opaque;
    QemuOpts *opts = NULL;
    const char *path;
    uint8_t recovery = 0;
    bool writable;
    int ret;

    ret = bdrv_open_file_child(NULL, options, "file", bs, errp);
    if (ret < 0) {
        return ret;
    }

    GRAPH_RDLOCK_GUARD_MAINLOOP();

    path = aaruformat_local_path(bs->file->bs, errp);
    if (!path) {
        return -EINVAL;
    }
    s->path = g_strdup(path);

    /* What the image was just created with, unless given now */
    s->compress = false;
    s->zstd = false;
    s->deduplicate = true;
    aaruformat_pending_take(s->path, &s->compress, &s->zstd, &s->deduplicate,
                            &recovery);

    opts = qemu_opts_create(&aaruformat_runtime_opts, NULL, 0, &error_abort);
    if (!qemu_opts_absorb_qdict(opts, options, errp)) {
        ret = -EINVAL;
        goto fail;
    }
    s->bs = bs;
    s->checkpoint_ns = qemu_opt_get_number(opts, AARUFORMAT_RT_CHECKPOINT,
                                           AARUFORMAT_DEFAULT_CHECKPOINT) *
                       NANOSECONDS_PER_SECOND;
    s->compress = qemu_opt_get_bool(opts, AARUFORMAT_RT_COMPRESS, s->compress);
    s->deduplicate = qemu_opt_get_bool(opts, AARUFORMAT_RT_DEDUPLICATE,
                                       s->deduplicate);
    if (qemu_opt_get(opts, AARUFORMAT_RT_COMPRESSION_TYPE) &&
        !aaruformat_parse_compression(qemu_opt_get(opts,
                                      AARUFORMAT_RT_COMPRESSION_TYPE),
                                      &s->zstd, errp)) {
        ret = -EINVAL;
        goto fail;
    }

    qemu_co_mutex_init(&s->lock);
    s->scratch = g_malloc(AARUFORMAT_MAX_SECTOR);

    writable = !!(flags & BDRV_O_RDWR);
    ret = aaruformat_open_ctx(s, writable, errp);
    if (ret == -EACCES) {
        /* An optical disc opened for writing: read-only if allowed */
        error_free(*errp);
        *errp = NULL;
        ret = bdrv_apply_auto_read_only(bs, "AaruFormat optical disc images "
                                        "are read-only", errp);
        if (ret < 0) {
            goto fail;
        }
        ret = aaruformat_open_ctx(s, false, errp);
    }
    if (ret < 0) {
        goto fail;
    }

    if (s->writable && recovery) {
        /*
         * Just created with error recovery: it covers what this session
         * writes, so the zero sector 0 of the creation goes in again.
         */
        uint8_t *zero = g_malloc0(s->sector_size);

        if (aaru_lib_set_error_recovery(s->ctx, recovery) < 0 ||
            aaru_lib_write_sector(s->ctx, 0, zero, s->sector_size) < 0) {
            g_free(zero);
            error_setg(errp, "Could not add error recovery to the image");
            ret = -EIO;
            goto fail;
        }
        g_free(zero);
        s->error_recovery = true;
    } else if (s->writable && s->error_recovery) {
        warn_report("AaruFormat image '%s' has error recovery data, which "
                    "writing to it drops; qemu-img convert -o "
                    "error_recovery=N makes a new copy with it", s->path);
    }

    if (s->optical) {
        ret = aaruformat_load_toc(s, errp);
        if (ret < 0) {
            goto fail;
        }
    }

    bs->total_sectors = s->sectors * s->sector_size / BDRV_SECTOR_SIZE;

    error_setg(&s->migration_blocker, "The aaruformat format used by node "
               "'%s' does not support live migration",
               bdrv_get_device_or_node_name(bs));
    ret = migrate_add_blocker_normal(&s->migration_blocker, errp);
    if (ret < 0) {
        goto fail;
    }

    aaruformat_attach_aio_context(bs, bdrv_get_aio_context(bs));

    qemu_opts_del(opts);
    return 0;

fail:
    qemu_opts_del(opts);
    if (s->ctx) {
        aaru_lib_close(s->ctx);
        s->ctx = NULL;
    }
    g_free(s->toc);
    s->toc = NULL;
    g_free(s->scratch);
    s->scratch = NULL;
    g_free(s->path);
    s->path = NULL;
    return ret;
}

static void aaruformat_close(BlockDriverState *bs)
{
    BDRVAaruFormatState *s = bs->opaque;

    aaruformat_detach_aio_context(bs);

    if (s->ctx) {
        aaru_lib_close(s->ctx);
        s->ctx = NULL;
    }
    g_free(s->toc);
    g_free(s->scratch);
    g_free(s->path);
    migrate_del_blocker(&s->migration_blocker);
}

static void aaruformat_refresh_limits(BlockDriverState *bs, Error **errp)
{
    BDRVAaruFormatState *s = bs->opaque;

    bs->bl.request_alignment = s->sector_size;
}

/*
 * Changing between read-only and read-write closes the library context and
 * opens it again in the new mode; the new one is opened before the old one
 * is closed, so a failure leaves the image as it was.
 */
typedef struct AaruFormatReopen {
    AaruLib *old_ctx;
    bool old_writable;
} AaruFormatReopen;

static int aaruformat_reopen_prepare(BDRVReopenState *state,
                                     BlockReopenQueue *queue, Error **errp)
{
    BDRVAaruFormatState *s = state->bs->opaque;
    bool writable = !!(state->flags & BDRV_O_RDWR);
    AaruFormatReopen *r;
    int ret;

    if (writable == s->writable) {
        return 0;
    }

    if (writable && s->optical) {
        error_setg(errp, "AaruFormat optical disc images are read-only");
        return -EACCES;
    }

    /* Leaving read-write: make what was written visible to the new context */
    if (s->writable && aaru_lib_flush(s->ctx) < 0) {
        error_setg(errp, "Could not flush the AaruFormat image");
        return -EIO;
    }

    r = g_new0(AaruFormatReopen, 1);
    r->old_ctx = s->ctx;
    r->old_writable = s->writable;

    ret = aaruformat_open_ctx(s, writable, errp);
    if (ret < 0) {
        s->ctx = r->old_ctx;
        s->writable = r->old_writable;
        g_free(r);
        return ret;
    }

    state->opaque = r;
    return 0;
}

static void aaruformat_reopen_commit(BDRVReopenState *state)
{
    AaruFormatReopen *r = state->opaque;

    if (r) {
        aaru_lib_close(r->old_ctx);
        g_free(r);
        state->opaque = NULL;
    }
}

static void aaruformat_reopen_abort(BDRVReopenState *state)
{
    BDRVAaruFormatState *s = state->bs->opaque;
    AaruFormatReopen *r = state->opaque;

    if (r) {
        aaru_lib_close(s->ctx);
        s->ctx = r->old_ctx;
        s->writable = r->old_writable;
        g_free(r);
        state->opaque = NULL;
    }
}

/* I/O */

static int coroutine_fn GRAPH_RDLOCK
aaruformat_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes,
                     QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    BDRVAaruFormatState *s = bs->opaque;
    AaruFormatIO io = {
        .s = s,
        .sector = offset / s->sector_size,
        .count = bytes / s->sector_size,
    };
    int ret;

    assert(QEMU_IS_ALIGNED(offset | bytes, s->sector_size));

    io.buf = qemu_try_blockalign(bs, bytes);
    if (!io.buf) {
        return -ENOMEM;
    }

    ret = aaruformat_run(aaruformat_read_worker, &io);
    if (ret == 0) {
        qemu_iovec_from_buf(qiov, 0, io.buf, bytes);
    }

    qemu_vfree(io.buf);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
aaruformat_co_pwritev(BlockDriverState *bs, int64_t offset, int64_t bytes,
                      QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    BDRVAaruFormatState *s = bs->opaque;
    AaruFormatIO io = {
        .s = s,
        .sector = offset / s->sector_size,
        .count = bytes / s->sector_size,
    };
    int ret;

    assert(QEMU_IS_ALIGNED(offset | bytes, s->sector_size));

    if (!s->writable || s->optical) {
        return -EACCES;
    }

    io.buf = qemu_try_blockalign(bs, bytes);
    if (!io.buf) {
        return -ENOMEM;
    }

    qemu_iovec_to_buf(qiov, 0, io.buf, bytes);
    ret = aaruformat_run(aaruformat_write_worker, &io);
    aaruformat_written(s);

    qemu_vfree(io.buf);
    return ret;
}

/*
 * "qemu-img convert -c" writes compressed clusters: compression is turned on
 * for the image with the first of them, and the data goes in as any other.
 */
static int coroutine_fn GRAPH_RDLOCK
aaruformat_co_pwritev_compressed_part(BlockDriverState *bs, int64_t offset,
                                      int64_t bytes, QEMUIOVector *qiov,
                                      size_t qiov_offset)
{
    BDRVAaruFormatState *s = bs->opaque;
    QEMUIOVector local_qiov;
    int ret;

    if (bytes == 0) {
        return 0;
    }

    if (!s->compressed_writes) {
        AaruFormatIO io = { .s = s };

        ret = aaruformat_run(aaruformat_compress_worker, &io);
        if (ret < 0) {
            return ret;
        }
        s->compressed_writes = true;
        s->compress = true;
    }

    qemu_iovec_init_slice(&local_qiov, qiov, qiov_offset, bytes);
    ret = aaruformat_co_pwritev(bs, offset, bytes, &local_qiov, 0);
    qemu_iovec_destroy(&local_qiov);

    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
aaruformat_co_flush_to_os(BlockDriverState *bs)
{
    BDRVAaruFormatState *s = bs->opaque;

    if (s->checkpoint_timer) {
        timer_del(s->checkpoint_timer);
    }
    return aaruformat_checkpoint(s);
}

static int coroutine_fn GRAPH_RDLOCK
aaruformat_co_get_info(BlockDriverState *bs, BlockDriverInfo *bdi)
{
    BDRVAaruFormatState *s = bs->opaque;

    bdi->cluster_size = s->cluster_size;
    return 0;
}

static ImageInfoSpecific *aaruformat_get_specific_info(BlockDriverState *bs,
                                                       Error **errp)
{
    BDRVAaruFormatState *s = bs->opaque;
    ImageInfoSpecificAaruFormat *info = g_new0(ImageInfoSpecificAaruFormat, 1);
    ImageInfoSpecific *spec = g_new0(ImageInfoSpecific, 1);

    info->media_type = s->media_type;
    info->metadata_type = s->optical ? AARU_FORMAT_METADATA_TYPE_OPTICAL :
                                       AARU_FORMAT_METADATA_TYPE_BLOCK;
    info->sector_size = s->sector_size;
    info->sectors = s->sectors;
    info->error_recovery = s->error_recovery;

    if (s->application[0]) {
        info->application = g_strdup(s->application);
    }
    if (s->application_version[0]) {
        info->application_version = g_strdup(s->application_version);
    }

    if (s->toc) {
        info->has_tracks = true;
        info->tracks = s->toc->n_tracks;
    }

    spec->type = IMAGE_INFO_SPECIFIC_KIND_AARUFORMAT;
    spec->u.aaruformat.data = info;
    return spec;
}

/* CD-ROM devices */

static int aaruformat_get_cd_toc(BlockDriverState *bs, BlockCdToc *toc)
{
    BDRVAaruFormatState *s = bs->opaque;

    if (!s->toc) {
        return -ENOTSUP;
    }

    *toc = *s->toc;
    return 0;
}

static int coroutine_fn GRAPH_RDLOCK
aaruformat_co_read_cd_raw(BlockDriverState *bs, int64_t lba, uint8_t *raw,
                          uint8_t *subchannel)
{
    BDRVAaruFormatState *s = bs->opaque;
    AaruFormatIO io = {
        .s = s,
        .sector = lba,
        .buf = raw,
        .subchannel = subchannel,
    };

    if (!s->toc || lba < 0 || lba >= s->sectors) {
        return -ENOTSUP;
    }

    return aaruformat_run(aaruformat_raw_worker, &io);
}

/* Creating */

static int coroutine_fn
aaruformat_co_do_create(BlockdevCreateOptions *options, Error **errp)
{
    BlockdevCreateOptionsAaruFormat *opts = &options->u.aaruformat;
    uint32_t sector_size = opts->has_sector_size ? opts->sector_size : 512;
    int table_shift = opts->has_table_shift ? opts->table_shift : 12;
    bool compress = opts->has_compress && opts->compress;
    bool zstd = opts->has_compression_type &&
                opts->compression_type == AARU_FORMAT_COMPRESSION_ZSTD;
    bool deduplicate = !opts->has_deduplicate || opts->deduplicate;
    uint8_t recovery = opts->has_error_recovery ? opts->error_recovery : 0;
    static const char app[] = "LEMU";
    g_autofree char *options_str = NULL;
    BlockDriverState *bs_file;
    const char *path;
    int ret = 0;

    if (sector_size < 512 || sector_size > 32768 ||
        !is_power_of_2(sector_size)) {
        error_setg(errp, "Sector size must be a power of two between 512 "
                   "and 32768");
        return -EINVAL;
    }

    if (opts->size == 0 || opts->size % sector_size) {
        error_setg(errp, "Image size must be a non-zero multiple of the "
                   "sector size (%u)", sector_size);
        return -EINVAL;
    }

    if (recovery > 100) {
        error_setg(errp, "Error recovery must be a percentage from 0 to 100");
        return -EINVAL;
    }

    if (table_shift > 30) {
        error_setg(errp, "Table shift must be between 0 and 30");
        return -EINVAL;
    }

    bs_file = bdrv_co_open_blockdev_ref(opts->file, errp);
    if (!bs_file) {
        return -EIO;
    }

    bdrv_graph_co_rdlock();
    path = aaruformat_local_path(bs_file, errp);
    bdrv_graph_co_rdunlock();
    if (!path) {
        ret = -EINVAL;
        goto out;
    }

    /* The image is written uncompressed: the compression is for later */
    options_str = aaruformat_options_string(false, false, deduplicate,
                                            table_shift);

    if (aaru_lib_create(path, AARU_LIB_GENERIC_HDD, sector_size,
                        opts->size / sector_size, options_str, app,
                        QEMU_VERSION_MAJOR, QEMU_VERSION_MINOR,
                        recovery) < 0) {
        error_setg(errp, "Could not create AaruFormat image '%s' (error %d)",
                   path, errno);
        ret = -EIO;
        goto out;
    }

    aaruformat_pending_add(path, compress, zstd, deduplicate, recovery);

out:
    bdrv_co_unref(bs_file);
    return ret;
}

static int coroutine_fn
aaruformat_co_create(BlockdevCreateOptions *options, Error **errp)
{
    assert(options->driver == BLOCKDEV_DRIVER_AARUFORMAT);

    return aaruformat_co_do_create(options, errp);
}

static QemuOptsList aaruformat_create_opts;

static int coroutine_fn GRAPH_UNLOCKED
aaruformat_co_create_opts(BlockDriver *drv, const char *filename,
                          QemuOpts *opts, Error **errp)
{
    BlockdevCreateOptions *create_options = NULL;
    BlockDriverState *bs_file = NULL;
    QDict *qdict;
    Visitor *v;
    int ret;

    /* QemuOpts names to the QAPI ones */
    qdict = qemu_opts_to_qdict_filtered(opts, NULL, &aaruformat_create_opts,
                                        true);
    qdict_rename_keys(qdict, (const QDictRenames[]) {
        { AARUFORMAT_OPT_SECTOR_SIZE,      "sector-size" },
        { AARUFORMAT_OPT_COMPRESSION_TYPE, "compression-type" },
        { AARUFORMAT_OPT_TABLE_SHIFT,      "table-shift" },
        { AARUFORMAT_OPT_ERROR_RECOVERY,   "error-recovery" },
        { NULL, NULL },
    }, &error_abort);

    ret = bdrv_co_create_file(filename, opts, true, errp);
    if (ret < 0) {
        goto done;
    }

    bs_file = bdrv_co_open(filename, NULL, NULL,
                           BDRV_O_RDWR | BDRV_O_RESIZE | BDRV_O_PROTOCOL, errp);
    if (!bs_file) {
        ret = -EIO;
        goto done;
    }

    qdict_put_str(qdict, "driver", "aaruformat");
    qdict_put_str(qdict, "file", bs_file->node_name);

    v = qobject_input_visitor_new_flat_confused(qdict, errp);
    if (!v) {
        ret = -EINVAL;
        goto done;
    }
    visit_type_BlockdevCreateOptions(v, NULL, &create_options, errp);
    visit_free(v);
    if (!create_options) {
        ret = -EINVAL;
        goto done;
    }

    /* Silently round the size up to whole sectors */
    if (create_options->u.aaruformat.has_sector_size &&
        is_power_of_2(create_options->u.aaruformat.sector_size)) {
        create_options->u.aaruformat.size =
            ROUND_UP(create_options->u.aaruformat.size,
                     create_options->u.aaruformat.sector_size);
    } else {
        create_options->u.aaruformat.size =
            ROUND_UP(create_options->u.aaruformat.size, 512);
    }

    ret = aaruformat_co_do_create(create_options, errp);

done:
    qobject_unref(qdict);
    qapi_free_BlockdevCreateOptions(create_options);
    bdrv_co_unref(bs_file);
    return ret;
}

static QemuOptsList aaruformat_create_opts = {
    .name = "aaruformat-create-opts",
    .head = QTAILQ_HEAD_INITIALIZER(aaruformat_create_opts.head),
    .desc = {
        {
            .name = BLOCK_OPT_SIZE,
            .type = QEMU_OPT_SIZE,
            .help = "Virtual disk size"
        },
        {
            .name = AARUFORMAT_OPT_SECTOR_SIZE,
            .type = QEMU_OPT_SIZE,
            .help = "Sector size in bytes",
            .def_value_str = "512"
        },
        {
            .name = AARUFORMAT_OPT_COMPRESS,
            .type = QEMU_OPT_BOOL,
            .help = "Compress the data written right after creating the "
                    "image (qemu-img convert)",
            .def_value_str = "off"
        },
        {
            .name = AARUFORMAT_OPT_COMPRESSION_TYPE,
            .type = QEMU_OPT_STRING,
            .help = "Compression for that data: lzma (smaller) or zstd "
                    "(faster)",
            .def_value_str = "lzma"
        },
        {
            .name = AARUFORMAT_OPT_DEDUPLICATE,
            .type = QEMU_OPT_BOOL,
            .help = "Store identical sectors once",
            .def_value_str = "on"
        },
        {
            .name = AARUFORMAT_OPT_TABLE_SHIFT,
            .type = QEMU_OPT_NUMBER,
            .help = "Sectors per secondary deduplication table, as a power "
                    "of two (0 for a single table)",
            .def_value_str = "12"
        },
        {
            .name = AARUFORMAT_OPT_ERROR_RECOVERY,
            .type = QEMU_OPT_NUMBER,
            .help = "Percentage of error recovery data (erasure coding) for "
                    "the data written right after creating the image, as "
                    "Aaru's --error-recovery (0: none)",
            .def_value_str = "0"
        },
        { /* end of list */ }
    }
};

static const char *const aaruformat_strong_runtime_opts[] = {
    NULL
};

static BlockDriver bdrv_aaruformat = {
    .format_name                    = "aaruformat",
    .instance_size                  = sizeof(BDRVAaruFormatState),
    .is_format                      = true,
    .create_opts                    = &aaruformat_create_opts,
    .strong_runtime_opts            = aaruformat_strong_runtime_opts,

    .bdrv_probe                     = aaruformat_probe,
    .bdrv_open                      = aaruformat_open,
    .bdrv_close                     = aaruformat_close,
    .bdrv_child_perm                = bdrv_default_perms,
    .bdrv_refresh_limits            = aaruformat_refresh_limits,
    .bdrv_reopen_prepare            = aaruformat_reopen_prepare,
    .bdrv_reopen_commit             = aaruformat_reopen_commit,
    .bdrv_reopen_abort              = aaruformat_reopen_abort,
    .bdrv_has_zero_init             = bdrv_has_zero_init_1,
    .bdrv_attach_aio_context        = aaruformat_attach_aio_context,
    .bdrv_detach_aio_context        = aaruformat_detach_aio_context,
    .bdrv_drain_begin               = aaruformat_drain_begin,

    .bdrv_co_preadv                 = aaruformat_co_preadv,
    .bdrv_co_pwritev                = aaruformat_co_pwritev,
    .bdrv_co_pwritev_compressed_part = aaruformat_co_pwritev_compressed_part,
    .bdrv_co_flush_to_os            = aaruformat_co_flush_to_os,

    .bdrv_co_create                 = aaruformat_co_create,
    .bdrv_co_create_opts            = aaruformat_co_create_opts,
    .bdrv_co_get_info               = aaruformat_co_get_info,
    .bdrv_get_specific_info         = aaruformat_get_specific_info,

    .bdrv_get_cd_toc                = aaruformat_get_cd_toc,
    .bdrv_co_read_cd_raw            = aaruformat_co_read_cd_raw,
};

static void bdrv_aaruformat_init(void)
{
    bdrv_register(&bdrv_aaruformat);
}

block_init(bdrv_aaruformat_init);
