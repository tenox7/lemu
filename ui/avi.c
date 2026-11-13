/*
 * AVI files with ZMBV video and PCM audio, for the recorder
 *
 * A plain RIFF AVI with an idx1 index, as ffmpeg writes one, kept
 * under 1 GiB by the caller.  Its headers are rewritten now and then,
 * so a file cut short by a crash still plays.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "ui/avi.h"

#include <zlib.h>

#define AVIIF_KEYFRAME      0x10
#define AVIF_HASINDEX       0x10
#define AVIF_ISINTERLEAVED  0x100
#define AVIF_TRUSTCKTYPE    0x800

typedef struct AviIndex {
    uint32_t fourcc, flags, offset, size;
} AviIndex;

struct AviFile {
    FILE *f;
    char *error;
    int fps;
    int audio_rate;
    long pos;                   /* where the next chunk goes */
    long movi;                  /* the 'movi' of LIST movi */
    GArray *index;              /* AviIndex */
    uint32_t video_frames;
    uint32_t audio_frames;
    uint32_t video_max;         /* biggest chunk of each */
    uint32_t audio_max;
    /* header fields that change, by their offset in the file */
    long riff_size, total_frames, buffer_size;
    long video_length, video_buffer;
    long audio_length, audio_buffer;
    long movi_size;
};

static void put32(GByteArray *b, uint32_t v)
{
    uint32_t le = cpu_to_le32(v);

    g_byte_array_append(b, (uint8_t *)&le, 4);
}

static void put16(GByteArray *b, uint16_t v)
{
    uint16_t le = cpu_to_le16(v);

    g_byte_array_append(b, (uint8_t *)&le, 2);
}

static void put4cc(GByteArray *b, const char *cc)
{
    g_byte_array_append(b, (const uint8_t *)cc, 4);
}

/* Begin a chunk or LIST: returns the offset of its size, for list_end */
static long list_begin(GByteArray *b, const char *cc, const char *type)
{
    long at;

    put4cc(b, cc);
    at = b->len;
    put32(b, 0);
    if (type) {
        put4cc(b, type);
    }
    return at;
}

static void list_end(GByteArray *b, long at)
{
    uint32_t le = cpu_to_le32(b->len - at - 4);

    memcpy(b->data + at, &le, 4);
}

static void avi_fail(AviFile *a, const char *what)
{
    if (!a->error) {
        a->error = g_strdup_printf("%s: %s", what, g_strerror(errno));
    }
}

static bool avi_patch(AviFile *a, long at, uint32_t v)
{
    uint32_t le = cpu_to_le32(v);

    if (fseek(a->f, at, SEEK_SET) || fwrite(&le, 4, 1, a->f) != 1) {
        avi_fail(a, "can't update the video file");
        return false;
    }
    return true;
}

AviFile *avi_create(const char *path, int width, int height, int fps,
                    int audio_rate, Error **errp)
{
    g_autoptr(GByteArray) h = g_byte_array_new();
    AviFile *a;
    long riff, hdrl, strl, strf, chunk;
    int fd;

    fd = qemu_create(path, O_WRONLY | O_EXCL | O_BINARY, 0644, errp);
    if (fd < 0) {
        return NULL;
    }
    a = g_new0(AviFile, 1);
    a->f = fdopen(fd, "wb");
    if (!a->f) {
        error_setg_errno(errp, errno, "can't open %s", path);
        close(fd);
        g_free(a);
        return NULL;
    }
    a->fps = fps;
    a->audio_rate = audio_rate;
    a->index = g_array_new(false, false, sizeof(AviIndex));

    riff = list_begin(h, "RIFF", "AVI ");
    a->riff_size = riff;
    hdrl = list_begin(h, "LIST", "hdrl");

    chunk = list_begin(h, "avih", NULL);
    put32(h, 1000000 / fps);                /* microseconds a frame */
    put32(h, 0);                            /* max bytes a second */
    put32(h, 0);                            /* padding granularity */
    put32(h, AVIF_HASINDEX | AVIF_ISINTERLEAVED | AVIF_TRUSTCKTYPE);
    a->total_frames = h->len;
    put32(h, 0);
    put32(h, 0);                            /* initial frames */
    put32(h, audio_rate ? 2 : 1);           /* streams */
    a->buffer_size = h->len;
    put32(h, 0);
    put32(h, width);
    put32(h, height);
    put32(h, 0);
    put32(h, 0);
    put32(h, 0);
    put32(h, 0);
    list_end(h, chunk);

    strl = list_begin(h, "LIST", "strl");
    chunk = list_begin(h, "strh", NULL);
    put4cc(h, "vids");
    put4cc(h, "ZMBV");
    put32(h, 0);                            /* flags */
    put16(h, 0);                            /* priority */
    put16(h, 0);                            /* language */
    put32(h, 0);                            /* initial frames */
    put32(h, 1);                            /* scale */
    put32(h, fps);                          /* rate */
    put32(h, 0);                            /* start */
    a->video_length = h->len;
    put32(h, 0);
    a->video_buffer = h->len;
    put32(h, 0);
    put32(h, 0xffffffff);                   /* quality */
    put32(h, 0);                            /* sample size */
    put16(h, 0);
    put16(h, 0);
    put16(h, width);
    put16(h, height);
    list_end(h, chunk);
    strf = list_begin(h, "strf", NULL);     /* BITMAPINFOHEADER */
    put32(h, 40);
    put32(h, width);
    put32(h, height);
    put16(h, 1);                            /* planes */
    put16(h, 24);                           /* bits, as DOSBox has it */
    put4cc(h, "ZMBV");
    put32(h, width * height * 3);
    put32(h, 0);
    put32(h, 0);
    put32(h, 0);
    put32(h, 0);
    list_end(h, strf);
    list_end(h, strl);

    if (audio_rate) {
        strl = list_begin(h, "LIST", "strl");
        chunk = list_begin(h, "strh", NULL);
        put4cc(h, "auds");
        put32(h, 1);
        put32(h, 0);
        put16(h, 0);
        put16(h, 0);
        put32(h, 0);
        put32(h, 4);                        /* scale: bytes a frame */
        put32(h, audio_rate * 4);           /* rate: bytes a second */
        put32(h, 0);
        a->audio_length = h->len;
        put32(h, 0);
        a->audio_buffer = h->len;
        put32(h, 0);
        put32(h, 0xffffffff);
        put32(h, 4);                        /* sample size */
        put16(h, 0);
        put16(h, 0);
        put16(h, 0);
        put16(h, 0);
        list_end(h, chunk);
        strf = list_begin(h, "strf", NULL); /* WAVEFORMATEX */
        put16(h, 1);                        /* PCM */
        put16(h, 2);                        /* channels */
        put32(h, audio_rate);
        put32(h, audio_rate * 4);
        put16(h, 4);                        /* block align */
        put16(h, 16);                       /* bits */
        list_end(h, strf);
        list_end(h, strl);
    }
    list_end(h, hdrl);

    a->movi_size = list_begin(h, "LIST", "movi");
    a->movi = h->len - 4;
    list_end(h, a->movi_size);
    list_end(h, riff);

    if (fwrite(h->data, h->len, 1, a->f) != 1) {
        error_setg_errno(errp, errno, "can't write %s", path);
        fclose(a->f);
        unlink(path);
        g_array_unref(a->index);
        g_free(a);
        return NULL;
    }
    a->pos = h->len;
    return a;
}

static bool avi_chunk(AviFile *a, const char *cc, const void *data,
                      size_t len, uint32_t flags)
{
    static const uint8_t pad;
    uint8_t head[8];
    uint32_t le = cpu_to_le32(len);
    AviIndex e = {
        .fourcc = ldl_le_p(cc),
        .flags = flags,
        .offset = a->pos - a->movi,
        .size = len,
    };

    if (a->error) {
        return false;
    }
    memcpy(head, cc, 4);
    memcpy(head + 4, &le, 4);
    if (fwrite(head, 8, 1, a->f) != 1 ||
        (len && fwrite(data, len, 1, a->f) != 1) ||
        ((len & 1) && fwrite(&pad, 1, 1, a->f) != 1)) {
        avi_fail(a, "can't write the video file");
        return false;
    }
    a->pos += 8 + len + (len & 1);
    g_array_append_val(a->index, e);
    return true;
}

bool avi_add_video(AviFile *a, const void *data, size_t len, bool key)
{
    if (!avi_chunk(a, "00dc", data, len, key ? AVIIF_KEYFRAME : 0)) {
        return false;
    }
    a->video_frames++;
    a->video_max = MAX(a->video_max, len);
    return true;
}

bool avi_add_audio(AviFile *a, const int16_t *samples, size_t frames)
{
    size_t len = frames * 4;
    g_autofree int16_t *le = NULL;

    if (!frames) {
        return true;
    }
#if HOST_BIG_ENDIAN
    {
        size_t i;

        le = g_new(int16_t, frames * 2);
        for (i = 0; i < frames * 2; i++) {
            le[i] = cpu_to_le16(samples[i]);
        }
        samples = le;
    }
#endif
    if (!avi_chunk(a, "01wb", samples, len, AVIIF_KEYFRAME)) {
        return false;
    }
    a->audio_frames += frames;
    a->audio_max = MAX(a->audio_max, len);
    return true;
}

/* Rewrite the header fields that changed, then go back to the end */
static bool avi_headers(AviFile *a, long end)
{
    bool ok = avi_patch(a, a->riff_size, end - 8) &&
           avi_patch(a, a->total_frames, a->video_frames) &&
           avi_patch(a, a->buffer_size, MAX(a->video_max, a->audio_max)) &&
           avi_patch(a, a->video_length, a->video_frames) &&
           avi_patch(a, a->video_buffer, a->video_max) &&
           (!a->audio_rate ||
            (avi_patch(a, a->audio_length, a->audio_frames) &&
             avi_patch(a, a->audio_buffer, a->audio_max))) &&
           avi_patch(a, a->movi_size, a->pos - a->movi);

    if (ok && fseek(a->f, end, SEEK_SET)) {
        avi_fail(a, "can't write the video file");
        ok = false;
    }
    return ok;
}

bool avi_sync(AviFile *a)
{
    if (a->error) {
        return false;
    }
    if (!avi_headers(a, a->pos) || fflush(a->f)) {
        avi_fail(a, "can't write the video file");
        return false;
    }
    return true;
}

bool avi_close(AviFile *a, Error **errp)
{
    g_autoptr(GByteArray) idx = g_byte_array_new();
    long end;
    bool ok;
    guint i;

    if (!a->error) {
        put4cc(idx, "idx1");
        put32(idx, a->index->len * 16);
        for (i = 0; i < a->index->len; i++) {
            AviIndex *e = &g_array_index(a->index, AviIndex, i);

            put32(idx, e->fourcc);
            put32(idx, e->flags);
            put32(idx, e->offset);
            put32(idx, e->size);
        }
        end = a->pos + idx->len;
        if (fwrite(idx->data, idx->len, 1, a->f) != 1) {
            avi_fail(a, "can't write the video index");
        } else {
            avi_headers(a, end);
        }
    }
    if (fclose(a->f) && !a->error) {
        avi_fail(a, "can't write the video file");
    }
    ok = !a->error;
    if (!ok) {
        error_setg(errp, "%s", a->error);
    }
    g_array_unref(a->index);
    g_free(a->error);
    g_free(a);
    return ok;
}

uint64_t avi_bytes(AviFile *a)
{
    return a->pos;
}

const char *avi_error(AviFile *a)
{
    return a->error;
}

/* ZMBV */

#define ZMBV_KEYFRAME   1
#define ZMBV_FMT_32BPP  8

struct ZmbvEncoder {
    int width, height;
    z_stream zs;
    size_t table;               /* the motion table, padded */
    uint8_t *zeros;             /* the delta of a frame that repeats */
};

static size_t zmbv_table_size(int width, int height)
{
    int bx = DIV_ROUND_UP(width, ZMBV_BLOCK);
    int by = DIV_ROUND_UP(height, ZMBV_BLOCK);

    return ROUND_UP((size_t)bx * by * 2, 4);
}

ZmbvEncoder *zmbv_encoder_new(int width, int height, int level)
{
    ZmbvEncoder *z = g_new0(ZmbvEncoder, 1);

    z->width = width;
    z->height = height;
    z->table = zmbv_table_size(width, height);
    z->zeros = g_malloc0(z->table);
    if (deflateInit(&z->zs, level) != Z_OK) {
        g_free(z->zeros);
        g_free(z);
        return NULL;
    }
    return z;
}

void zmbv_encoder_free(ZmbvEncoder *z)
{
    if (z) {
        deflateEnd(&z->zs);
        g_free(z->zeros);
        g_free(z);
    }
}

bool zmbv_encode(ZmbvEncoder *z, bool key, const void *raw, size_t len,
                 GByteArray *out)
{
    size_t head;

    g_byte_array_set_size(out, 0);
    if (key) {
        static const uint8_t hdr[7] = {
            ZMBV_KEYFRAME, 0, 1, 1, ZMBV_FMT_32BPP, ZMBV_BLOCK, ZMBV_BLOCK
        };

        if (deflateReset(&z->zs) != Z_OK) {
            return false;
        }
        g_byte_array_append(out, hdr, sizeof(hdr));
    } else {
        uint8_t flags = 0;

        g_byte_array_append(out, &flags, 1);
        if (!raw) {
            raw = z->zeros;
            len = z->table;
        }
    }
    head = out->len;
    z->zs.next_in = (Bytef *)raw;
    z->zs.avail_in = len;
    do {
        size_t room = MAX(len / 8, 4096) + 64;
        int ret;

        g_byte_array_set_size(out, out->len + room);
        z->zs.next_out = out->data + out->len - room;
        z->zs.avail_out = room;
        ret = deflate(&z->zs, Z_SYNC_FLUSH);
        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            return false;
        }
        g_byte_array_set_size(out, out->len - z->zs.avail_out);
    } while (z->zs.avail_out == 0 || z->zs.avail_in);
    return out->len > head;
}

int zmbv_delta(int width, int height, int stride, uint32_t *prev,
               const uint32_t *cur, int x, int y, int w, int h,
               uint32_t mask, GByteArray *out, int box[4])
{
    int bx = DIV_ROUND_UP(width, ZMBV_BLOCK);
    size_t table = zmbv_table_size(width, height);
    size_t base = out->len;
    int bx0, by0, bx1, by1, i, j;
    int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1;
    int changed = 0;

    g_byte_array_set_size(out, base + table);
    memset(out->data + base, 0, table);
    x = MAX(x, 0);
    y = MAX(y, 0);
    w = MIN(x + w, width) - x;
    h = MIN(y + h, height) - y;
    if (w <= 0 || h <= 0) {
        return 0;
    }
    bx0 = x / ZMBV_BLOCK;
    by0 = y / ZMBV_BLOCK;
    bx1 = DIV_ROUND_UP(x + w, ZMBV_BLOCK);
    by1 = DIV_ROUND_UP(y + h, ZMBV_BLOCK);

    for (j = by0; j < by1; j++) {
        int py = j * ZMBV_BLOCK;
        int bh = MIN(ZMBV_BLOCK, height - py);

        for (i = bx0; i < bx1; i++) {
            int px = i * ZMBV_BLOCK;
            int bw = MIN(ZMBV_BLOCK, width - px);
            uint32_t *p = prev + (size_t)py * stride + px;
            const uint32_t *c = cur + (size_t)py * stride + px;
            uint32_t *xor;
            int r, k;

            for (r = 0; r < bh; r++) {
                if (memcmp(p + (size_t)r * stride, c + (size_t)r * stride,
                           bw * 4)) {
                    break;
                }
            }
            if (r == bh) {
                continue;
            }
            /* the block changed: flag it and add its XOR */
            out->data[base + ((size_t)j * bx + i) * 2] = 1;
            g_byte_array_set_size(out, out->len + (size_t)bw * bh * 4);
            xor = (uint32_t *)(out->data + out->len - (size_t)bw * bh * 4);
            for (r = 0; r < bh; r++) {
                uint32_t *pr = p + (size_t)r * stride;
                const uint32_t *cr = c + (size_t)r * stride;

                for (k = 0; k < bw; k++) {
                    uint32_t d = pr[k] ^ cr[k];

                    stl_le_p(xor++, d);
                    if (d & mask) {
                        changed++;
                        x0 = MIN(x0, px + k);
                        y0 = MIN(y0, py + r);
                        x1 = MAX(x1, px + k + 1);
                        y1 = MAX(y1, py + r + 1);
                    }
                }
                memcpy(pr, cr, bw * 4);
            }
        }
    }
    if (changed) {
        box[0] = x0;
        box[1] = y0;
        box[2] = x1 - x0;
        box[3] = y1 - y0;
    }
    return changed;
}
