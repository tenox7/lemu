/*
 * MCP: record a console as a video (see qapi/mcp.json, record)
 *
 * A display listener learns which parts of the screen change.  A timer
 * on the guest's clock takes a frame every 1/fps second of guest time,
 * so the video never stutters when the host is busy, and a paused VM
 * records nothing.  The frames are ZMBV, lossless, in AVI files: the
 * main loop works out which 16x16 blocks changed, and a writer thread
 * compresses them and writes them out.
 *
 * The sound comes from the audio capture interface, as for the sound
 * tool.  The audio layer hands over only what the sound devices play,
 * so each piece of it goes where the guest clock says it belongs, and
 * the gaps stay silent: sound and picture stay together.
 *
 * Two ways leave the waits out: freeze pauses the VM between MCP calls
 * (through a hook in the MCP monitor), and skip-still stops the video's
 * clock while nothing moves and nothing sounds.
 *
 * Everything but the writer thread runs in the main loop, with the BQL.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "monitor/mcp.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-mcp.h"
#include "qemu/audio.h"
#include "qemu/audio-capture.h"
#include "qemu/coroutine.h"
#include "qemu/main-loop.h"
#include "qemu/queue.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "qom/object.h"
#include "system/runstate.h"
#include "system/system.h"
#include "ui/avi.h"
#include "ui/console.h"
#include "ui/png.h"
#include "ui/record.h"
#include "ui/surface.h"
#include "ui-mcp.h"
#include "vgafont.h"

#define REC_FPS             30
#define REC_FPS_MAX         60
#define REC_ZLIB_LEVEL      4
#define REC_KEY_S           10          /* a key frame this often */
#define REC_FILE_MAX        (1024 * 1024 * 1024)
#define REC_QUEUE_MAX       (256 * 1024 * 1024)
#define REC_GAP_MAX_S       10          /* a longer jump is a cut */
#define REC_AUDIO_RATE      44100
#define REC_AUDIO_RING      65536       /* frames, a power of 2 */
#define REC_AUDIO_LAG_MS    100         /* audio that may still come */
#define REC_AUDIO_DRIFT_MS  20          /* nudge a sound this far off */
#define REC_LOUD            104         /* -50 dBFS, of 32768 */
#define REC_ACTIVE_PIXELS   64
#define REC_BLINK_MAX       32
#define REC_BLINK_SLOTS     8
#define REC_THUMB_W         240
#define REC_THUMBS          32
#define REC_SHEET           16
#define REC_SHEET_COLUMNS   4
#define REC_HOLD_MAX_MS     60000
#define REC_CALLS_MAX       20000
#define REC_MARKS_MAX       10000
#define REC_CAPTION_MS      3000

/* ZMBV's pixels: blue, green, red, unused, in this order in memory */
#if HOST_BIG_ENDIAN
#define REC_FORMAT          PIXMAN_b8g8r8x8
#define REC_RGB             0xffffff00u
#else
#define REC_FORMAT          PIXMAN_x8r8g8b8
#define REC_RGB             0x00ffffffu
#endif

typedef struct Recorder Recorder;

typedef enum RecJobType {
    REC_JOB_FILE,               /* go on in a new file */
    REC_JOB_VIDEO,
    REC_JOB_AUDIO,
    REC_JOB_END,
} RecJobType;

typedef struct RecJob {
    RecJobType type;
    bool key;
    GByteArray *data;           /* NULL: a frame that repeats the last */
    AviFile *avi;
    ZmbvEncoder *zmbv;
    QSIMPLEQ_ENTRY(RecJob) node;
} RecJob;

typedef struct RecFile {
    char *path;
    int width, height;
    int64_t first_frame;
    int64_t frames;
} RecFile;

typedef struct RecEar {
    Recorder *rec;
    char *id;
    AudioBackend *be;           /* NULL once the audio layer dropped it */
    CaptureVoiceOut *cap;
    int64_t pos;                /* where its next frame goes; -1: anew */
} RecEar;

typedef struct RecBox {
    int x, y, w, h;             /* empty when w or h is 0 */
} RecBox;

typedef struct RecBlink {
    RecBox box;
    uint32_t a, b;
    int64_t last;
} RecBlink;

typedef struct RecThumb {
    int64_t ms;
    pixman_image_t *img;
} RecThumb;

struct Recorder {
    DisplayChangeListener dcl;
    QemuConsole *con;
    char *base;                 /* the path, less .avi */
    int fps;
    bool freeze;
    int64_t skip_still_ns;

    /* time */
    QEMUTimer *timer;
    int64_t last_v;             /* guest clock at the last tick */
    int64_t rec_ns;             /* the video's clock */
    int64_t next_frame;
    bool skipping;
    int64_t skip_v;             /* guest clock: when the skip began */
    int64_t skipped_ns;
    int64_t active_v;           /* guest clock: the last activity */

    /* the screen */
    int width, height;          /* of the file being written */
    pixman_image_t *out;        /* the screen as the video shows it */
    pixman_image_t *prev;       /* the last frame written */
    pixman_image_t *scratch;
    RecBox dirty;               /* may differ between screen and @out */
    RecBox enc;                 /* may differ between @out and @prev */
    bool key_due;
    int64_t last_key;
    RecBlink blink[REC_BLINK_SLOTS];

    /* the pointer, for cards that leave it to the UI */
    pixman_image_t *cursor;     /* premultiplied */
    int hot_x, hot_y;
    int mouse_x, mouse_y;
    bool mouse_on;
    RecBox cursor_box;          /* where it is in @out */

    /* sound */
    GPtrArray *ears;
    int32_t *ring;              /* REC_AUDIO_RING stereo frames, mixed */
    int64_t flushed;            /* audio frames handed to the writer */
    int64_t loud_v;             /* guest clock: the last loud sound */

    /* the output */
    GPtrArray *files;           /* RecFile */
    int64_t frames, dropped;
    QList *marks, *calls, *cuts;
    int n_calls;
    GHashTable *open_calls;     /* coroutine -> its entry in @calls */
    int n_chapters, n_captions;

    /* freeze */
    bool paused_by_us;
    int holding;
    int64_t paused_real_ms;     /* when we paused it */
    int64_t paused_ms;          /* in all */
    int pauses;
    VMChangeStateEntry *vmstate;
    Notifier exit;

    /* the contact sheet */
    RecThumb thumbs[REC_THUMBS];
    int n_thumbs;
    int64_t thumb_every_ms, next_thumb_ms;

    /* the writer */
    QemuThread thread;
    QemuMutex lock;
    QemuCond cond;
    QSIMPLEQ_HEAD(, RecJob) jobs;
    size_t queued;              /* bytes of frames and sound waiting */
    uint64_t bytes_done;        /* in the files it finished */
    uint64_t file_bytes;        /* in the file it writes */
    char *error;
    bool writer_done;
    bool failed;                /* the main loop saw @error */
};

static Recorder *recorder;

bool qemu_record_active(void)
{
    return recorder;
}

bool qemu_record_frozen(void)
{
    return recorder && recorder->paused_by_us && !runstate_is_running();
}

static int64_t rec_real_ms(void)
{
    return qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
}

static int64_t rec_frame_ns(Recorder *r, int64_t frame)
{
    return muldiv64(frame, NANOSECONDS_PER_SECOND, r->fps);
}

static int64_t rec_sample(int64_t ns)
{
    return muldiv64(ns, REC_AUDIO_RATE, NANOSECONDS_PER_SECOND);
}

/* The video's clock now, between ticks */
static int64_t rec_time(Recorder *r)
{
    int64_t dv = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - r->last_v;

    if (r->skipping || dv < 0) {
        return r->rec_ns;
    }
    return r->rec_ns + MIN(dv, (int64_t)REC_GAP_MAX_S * NANOSECONDS_PER_SECOND);
}

static void rec_time_str(GString *s, int64_t ms)
{
    int64_t sec = ms / 1000;

    if (sec >= 3600) {
        g_string_append_printf(s, "%" PRId64 ":%02" PRId64 ":%02" PRId64,
                               sec / 3600, sec / 60 % 60, sec % 60);
    } else {
        g_string_append_printf(s, "%" PRId64 ":%02" PRId64, sec / 60,
                               sec % 60);
    }
}

/* A length for people: 4.2 s, 1:05 or 1:02:03 */
static void rec_len_str(GString *s, int64_t ms)
{
    if (ms < 60000) {
        g_string_append_printf(s, "%.1f s", ms / 1000.0);
    } else {
        rec_time_str(s, ms);
    }
}

/* Boxes */

static bool rec_box_empty(const RecBox *b)
{
    return b->w <= 0 || b->h <= 0;
}

static void rec_box_add(RecBox *b, int x, int y, int w, int h)
{
    int x1, y1;

    if (w <= 0 || h <= 0) {
        return;
    }
    if (rec_box_empty(b)) {
        *b = (RecBox) { x, y, w, h };
        return;
    }
    x1 = MAX(b->x + b->w, x + w);
    y1 = MAX(b->y + b->h, y + h);
    b->x = MIN(b->x, x);
    b->y = MIN(b->y, y);
    b->w = x1 - b->x;
    b->h = y1 - b->y;
}

static void rec_box_clip(RecBox *b, int w, int h)
{
    int x1 = MIN(b->x + b->w, w), y1 = MIN(b->y + b->h, h);

    b->x = MAX(b->x, 0);
    b->y = MAX(b->y, 0);
    b->w = MAX(x1 - b->x, 0);
    b->h = MAX(y1 - b->y, 0);
}

/* The writer thread */

static void rec_job_free(RecJob *j)
{
    if (j->data) {
        g_byte_array_unref(j->data);
    }
    g_free(j);
}

static void rec_push(Recorder *r, RecJob *j)
{
    qemu_mutex_lock(&r->lock);
    r->queued += j->data ? j->data->len : 0;
    QSIMPLEQ_INSERT_TAIL(&r->jobs, j, node);
    qemu_cond_signal(&r->cond);
    qemu_mutex_unlock(&r->lock);
}

static void rec_writer_error(Recorder *r, const char *msg)
{
    qemu_mutex_lock(&r->lock);
    if (!r->error) {
        r->error = g_strdup(msg);
    }
    qemu_mutex_unlock(&r->lock);
}

static void *rec_writer(void *opaque)
{
    Recorder *r = opaque;
    g_autoptr(GByteArray) frame = g_byte_array_new();
    AviFile *avi = NULL;
    ZmbvEncoder *zmbv = NULL;
    int64_t synced = rec_real_ms();
    bool end = false;

    while (!end) {
        RecJob *j;
        size_t bytes;

        qemu_mutex_lock(&r->lock);
        while (QSIMPLEQ_EMPTY(&r->jobs)) {
            qemu_cond_wait(&r->cond, &r->lock);
        }
        j = QSIMPLEQ_FIRST(&r->jobs);
        QSIMPLEQ_REMOVE_HEAD(&r->jobs, node);
        qemu_mutex_unlock(&r->lock);
        bytes = j->data ? j->data->len : 0;

        switch (j->type) {
        case REC_JOB_FILE:
        case REC_JOB_END:
            if (avi) {
                Error *err = NULL;
                uint64_t n = avi_bytes(avi);

                if (!avi_close(avi, &err)) {
                    rec_writer_error(r, error_get_pretty(err));
                    error_free(err);
                }
                zmbv_encoder_free(zmbv);
                avi = NULL;
                zmbv = NULL;
                qemu_mutex_lock(&r->lock);
                r->bytes_done += n;
                r->file_bytes = 0;
                qemu_mutex_unlock(&r->lock);
            }
            avi = j->avi;
            zmbv = j->zmbv;
            end = j->type == REC_JOB_END;
            synced = rec_real_ms();
            break;
        case REC_JOB_VIDEO:
            if (!avi) {
                break;
            }
            if (!zmbv_encode(zmbv, j->key, j->data ? j->data->data : NULL,
                             bytes, frame)) {
                rec_writer_error(r, "can't compress a frame");
                break;
            }
            avi_add_video(avi, frame->data, frame->len, j->key);
            break;
        case REC_JOB_AUDIO:
            if (avi) {
                avi_add_audio(avi, (const int16_t *)j->data->data, bytes / 4);
            }
            break;
        }
        if (avi && avi_error(avi)) {
            rec_writer_error(r, avi_error(avi));
        } else if (avi && rec_real_ms() - synced >= 2000) {
            avi_sync(avi);
            synced = rec_real_ms();
        }

        qemu_mutex_lock(&r->lock);
        r->queued -= bytes;
        r->file_bytes = avi ? avi_bytes(avi) : 0;
        qemu_mutex_unlock(&r->lock);
        rec_job_free(j);
    }

    qemu_mutex_lock(&r->lock);
    r->writer_done = true;
    qemu_mutex_unlock(&r->lock);
    return NULL;
}

/* Sound */

/* Hand the mixed sound up to audio frame @upto to the writer */
static void rec_audio_flush(Recorder *r, int64_t upto)
{
    while (r->ears->len && r->flushed < upto) {
        int64_t n = MIN(upto - r->flushed, REC_AUDIO_RING / 4);
        RecJob *j = g_new0(RecJob, 1);
        int16_t *out;
        int64_t i;

        j->type = REC_JOB_AUDIO;
        j->data = g_byte_array_sized_new(n * 4);
        g_byte_array_set_size(j->data, n * 4);
        out = (int16_t *)j->data->data;
        for (i = 0; i < n; i++) {
            int32_t *v = &r->ring[((r->flushed + i) &
                                   (REC_AUDIO_RING - 1)) * 2];

            out[i * 2] = MIN(MAX(v[0], INT16_MIN), INT16_MAX);
            out[i * 2 + 1] = MIN(MAX(v[1], INT16_MIN), INT16_MAX);
            v[0] = v[1] = 0;
        }
        rec_push(r, j);
        r->flushed += n;
    }
}

/* Sound that the sound devices played into @opaque's audiodev */
static void rec_capture(void *opaque, const void *buf, int size)
{
    RecEar *ear = opaque;
    Recorder *r = ear->rec;
    const int16_t *x = buf;
    int n = size / 4;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t lag = REC_AUDIO_RATE * REC_AUDIO_LAG_MS / 1000;
    int64_t drift = REC_AUDIO_RATE * REC_AUDIO_DRIFT_MS / 1000;
    int lo[2] = { INT_MAX, INT_MAX }, hi[2] = { INT_MIN, INT_MIN };
    int64_t start;
    int i, c;

    for (i = 0; i < n; i++) {
        for (c = 0; c < 2; c++) {
            lo[c] = MIN(lo[c], x[i * 2 + c]);
            hi[c] = MAX(hi[c], x[i * 2 + c]);
        }
    }
    /* loud: the swing, not the level, so a DC offset is not a sound */
    if (n && (hi[0] - lo[0] >= 2 * REC_LOUD || hi[1] - lo[1] >= 2 * REC_LOUD)) {
        r->loud_v = now;
    }
    if (r->skipping || r->failed || !r->files->len) {
        ear->pos = -1;
        return;
    }

    /* it plays now: it ends at the video's clock now */
    start = rec_sample(rec_time(r)) - n;
    if (ear->pos < 0 || ear->pos > start + lag || ear->pos < start - lag) {
        ear->pos = start;
    } else if (ear->pos > start + drift) {
        ear->pos--;
    } else if (ear->pos < start - drift) {
        ear->pos++;
    }
    for (i = 0; i < n; i++) {
        int64_t p = ear->pos + i;
        int32_t *v;

        if (p < r->flushed || p >= r->flushed + REC_AUDIO_RING) {
            continue;
        }
        v = &r->ring[(p & (REC_AUDIO_RING - 1)) * 2];
        v[0] += x[i * 2];
        v[1] += x[i * 2 + 1];
    }
    ear->pos += n;
}

static void rec_notify(void *opaque, audcnotification_e cmd)
{
}

static void rec_destroy(void *opaque)
{
    RecEar *ear = opaque;

    ear->cap = NULL;
    ear->be = NULL;
}

static void rec_ears_close(Recorder *r)
{
    guint i;

    for (i = 0; r->ears && i < r->ears->len; i++) {
        RecEar *ear = g_ptr_array_index(r->ears, i);

        if (ear->be && ear->cap) {
            audio_be_del_capture(ear->be, ear->cap, ear);
        }
        g_free(ear->id);
        g_free(ear);
    }
    g_ptr_array_set_size(r->ears, 0);
}

static int rec_find_audiodev(Object *child, void *opaque)
{
    Object *be = object_dynamic_cast(child, TYPE_AUDIO_BACKEND);

    if (be) {
        g_ptr_array_add(opaque, be);
    }
    return 0;
}

/* Listen to every audiodev; say which in @s */
static void rec_ears_open(Recorder *r, GString *s)
{
    static const struct audio_capture_ops ops = {
        .notify = rec_notify,
        .capture = rec_capture,
        .destroy = rec_destroy,
    };
    struct audsettings as = {
        .freq = REC_AUDIO_RATE,
        .nchannels = 2,
        .fmt = AUDIO_FORMAT_S16,
        .big_endian = HOST_BIG_ENDIAN,
    };
    Object *root = object_resolve_path_component(object_get_root(),
                                                 "audiodevs");
    g_autoptr(GPtrArray) bes = g_ptr_array_new();
    guint i;

    if (root) {
        object_child_foreach(root, rec_find_audiodev, bes);
    }
    for (i = 0; i < bes->len; i++) {
        AudioBackend *be = g_ptr_array_index(bes, i);
        RecEar *ear;

        if (!AUDIO_BACKEND_GET_CLASS(be)->add_capture) {
            continue;
        }
        ear = g_new0(RecEar, 1);
        ear->rec = r;
        ear->be = be;
        ear->pos = -1;
        ear->id = g_strdup(audio_be_get_id(be));
        ear->cap = audio_be_add_capture(be, &as, &ops, ear);
        if (!ear->cap) {
            g_free(ear->id);
            g_free(ear);
            continue;
        }
        g_ptr_array_add(r->ears, ear);
    }
    if (!r->ears->len) {
        g_string_append(s, bes->len ? " without sound (the audiodev's mixing"
                        " engine is off)" : " without sound (the VM has no"
                        " audiodev)");
        return;
    }
    g_string_append(s, " with the sound of ");
    for (i = 0; i < r->ears->len; i++) {
        RecEar *ear = g_ptr_array_index(r->ears, i);

        g_string_append_printf(s, "%saudiodev '%s'",
                               !i ? "" : i + 1 < r->ears->len ? ", "
                                                              : " and ",
                               ear->id);
    }
    r->ring = g_new0(int32_t, REC_AUDIO_RING * 2);
}

/* The screen */

/* Is a change of @box from @before to @after a blinking text cursor? */
static bool rec_is_blink(Recorder *r, const RecBox *box, uint32_t before,
                         uint32_t after, int64_t now)
{
    RecBlink *e, *oldest = &r->blink[0];
    int i;

    for (i = 0; i < REC_BLINK_SLOTS; i++) {
        e = &r->blink[i];
        if (e->last && !memcmp(&e->box, box, sizeof(*box))) {
            bool blink = (e->a == before && e->b == after) ||
                         (e->a == after && e->b == before);

            e->a = before;
            e->b = after;
            e->last = now;
            return blink;
        }
        if (e->last < oldest->last) {
            oldest = e;
        }
    }
    *oldest = (RecBlink) { *box, before, after, now };
    return false;
}

static uint32_t rec_hash(pixman_image_t *img, int ox, int oy,
                         const RecBox *b)
{
    uint32_t *data = pixman_image_get_data(img);
    int stride = pixman_image_get_stride(img) / 4;
    uint32_t h = 2166136261u;
    int x, y;

    for (y = b->y; y < b->y + b->h; y++) {
        for (x = b->x; x < b->x + b->w; x++) {
            h = (h ^ (data[(y - oy) * stride + (x - ox)] & REC_RGB))
                * 16777619u;
        }
    }
    return h;
}

static void rec_cursor_box(Recorder *r, RecBox *b)
{
    *b = (RecBox) { 0 };
    if (r->cursor && r->mouse_on) {
        *b = (RecBox) { r->mouse_x - r->hot_x, r->mouse_y - r->hot_y,
                        pixman_image_get_width(r->cursor),
                        pixman_image_get_height(r->cursor) };
        rec_box_clip(b, r->width, r->height);
    }
}

/*
 * Bring @out up to date with the screen.  Returns how many pixels
 * changed since the last time, less a blinking text cursor.
 */
static int rec_refresh(Recorder *r)
{
    DisplaySurface *s = qemu_console_surface(r->con);
    RecBox box = r->dirty, now_cursor, change = { 0 };
    uint32_t *od, *sd;
    int ostride, sstride, x, y, count = 0;
    int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1;

    if (!s || !r->out || surface_width(s) != r->width ||
        surface_height(s) != r->height) {
        return 0;
    }
    rec_cursor_box(r, &now_cursor);
    if (memcmp(&now_cursor, &r->cursor_box, sizeof(now_cursor))) {
        rec_box_add(&box, r->cursor_box.x, r->cursor_box.y,
                    r->cursor_box.w, r->cursor_box.h);
        rec_box_add(&box, now_cursor.x, now_cursor.y, now_cursor.w,
                    now_cursor.h);
    }
    rec_box_clip(&box, r->width, r->height);
    r->dirty = (RecBox) { 0 };
    if (rec_box_empty(&box)) {
        return 0;
    }

    /* the screen and the pointer in @scratch, at 0,0 */
    pixman_image_composite(PIXMAN_OP_SRC, s->image, NULL, r->scratch,
                           box.x, box.y, 0, 0, 0, 0, box.w, box.h);
    if (!rec_box_empty(&now_cursor)) {
        pixman_image_composite(PIXMAN_OP_OVER, r->cursor, NULL, r->scratch,
                               0, 0, 0, 0,
                               r->mouse_x - r->hot_x - box.x,
                               r->mouse_y - r->hot_y - box.y,
                               pixman_image_get_width(r->cursor),
                               pixman_image_get_height(r->cursor));
    }
    r->cursor_box = now_cursor;

    od = pixman_image_get_data(r->out);
    ostride = pixman_image_get_stride(r->out) / 4;
    sd = pixman_image_get_data(r->scratch);
    sstride = pixman_image_get_stride(r->scratch) / 4;
    for (y = 0; y < box.h; y++) {
        uint32_t *orow = od + (box.y + y) * ostride + box.x;
        uint32_t *srow = sd + y * sstride;

        if (!memcmp(orow, srow, box.w * 4)) {
            continue;
        }
        for (x = 0; x < box.w; x++) {
            if ((orow[x] ^ srow[x]) & REC_RGB) {
                count++;
                x0 = MIN(x0, x);
                y0 = MIN(y0, y);
                x1 = MAX(x1, x + 1);
                y1 = MAX(y1, y + 1);
            }
        }
    }
    if (count) {
        change = (RecBox) { box.x + x0, box.y + y0, x1 - x0, y1 - y0 };
        if (change.w <= REC_BLINK_MAX && change.h <= REC_BLINK_MAX &&
            rec_is_blink(r, &change, rec_hash(r->out, 0, 0, &change),
                         rec_hash(r->scratch, box.x, box.y, &change),
                         r->last_v)) {
            count = 0;
        }
    }
    pixman_image_composite(PIXMAN_OP_SRC, r->scratch, NULL, r->out,
                           0, 0, 0, 0, box.x, box.y, box.w, box.h);
    rec_box_add(&r->enc, box.x, box.y, box.w, box.h);
    return count;
}

/* The width and height the screen is shown at, for old text modes */
static void rec_display_shape(int w, int h, int *dw, int *dh)
{
    if (h <= 400 && w * 2 >= h * 3) {
        /* 720x400, 640x350, 320x200: a 4:3 monitor's pixels are tall */
        *dw = 4;
        *dh = 3;
    } else {
        *dw = w;
        *dh = h;
    }
}

static void rec_thumb(Recorder *r)
{
    int64_t ms = rec_frame_ns(r, r->next_frame) / SCALE_MS;
    pixman_transform_t xf;
    RecThumb *t;
    int dw, dh, tw = REC_THUMB_W, th;
    int i;

    if (!r->out || ms < r->next_thumb_ms) {
        return;
    }
    if (r->n_thumbs == REC_THUMBS) {
        /* keep every other one, and take them half as often */
        for (i = 0; i < REC_THUMBS; i++) {
            if (i % 2) {
                pixman_image_unref(r->thumbs[i].img);
            } else {
                r->thumbs[i / 2] = r->thumbs[i];
            }
        }
        r->n_thumbs = REC_THUMBS / 2;
        r->thumb_every_ms *= 2;
    }
    rec_display_shape(r->width, r->height, &dw, &dh);
    th = MAX(tw * dh / dw, 1);
    t = &r->thumbs[r->n_thumbs++];
    t->ms = ms;
    t->img = pixman_image_create_bits(PIXMAN_x8r8g8b8, tw, th, NULL, 0);
    pixman_transform_init_scale(&xf,
                                pixman_double_to_fixed((double)r->width / tw),
                                pixman_double_to_fixed((double)r->height / th));
    pixman_image_set_transform(r->out, &xf);
    pixman_image_set_filter(r->out, PIXMAN_FILTER_GOOD, NULL, 0);
    pixman_image_composite(PIXMAN_OP_SRC, r->out, NULL, t->img,
                           0, 0, 0, 0, 0, 0, tw, th);
    pixman_image_set_transform(r->out, NULL);
    pixman_image_set_filter(r->out, PIXMAN_FILTER_NEAREST, NULL, 0);
    r->next_thumb_ms = ms + r->thumb_every_ms;
}

static void rec_push_video(Recorder *r, bool key, GByteArray *data)
{
    RecJob *j = g_new0(RecJob, 1);
    RecFile *f = g_ptr_array_index(r->files, r->files->len - 1);

    j->type = REC_JOB_VIDEO;
    j->key = key;
    j->data = data;
    rec_push(r, j);
    r->frames++;
    f->frames++;
}

static void rec_fail(Recorder *r, const char *msg)
{
    rec_writer_error(r, msg);
    r->failed = true;
}

/* Go on in a new file of @w x @h pixels, from frame r->next_frame */
static bool rec_new_file(Recorder *r, int w, int h, Error **errp)
{
    RecFile *f;
    RecJob *j;
    g_autofree char *path = NULL;
    AviFile *avi;
    ZmbvEncoder *zmbv;

    path = r->files->len ? g_strdup_printf("%s-%u.avi", r->base,
                                           r->files->len + 1)
                         : g_strdup_printf("%s.avi", r->base);
    avi = avi_create(path, w, h, r->fps, r->ears->len ? REC_AUDIO_RATE : 0,
                     errp);
    if (!avi) {
        return false;
    }
    zmbv = zmbv_encoder_new(w, h, REC_ZLIB_LEVEL);
    if (!zmbv) {
        avi_close(avi, NULL);
        unlink(path);
        error_setg(errp, "can't start the video compressor");
        return false;
    }

    /* the sound of the file before ends where its frames end */
    if (r->files->len) {
        rec_audio_flush(r, rec_sample(rec_frame_ns(r, r->next_frame)));
    } else {
        r->flushed = rec_sample(rec_frame_ns(r, r->next_frame));
        if (r->ring) {
            memset(r->ring, 0, sizeof(int32_t) * REC_AUDIO_RING * 2);
        }
    }

    f = g_new0(RecFile, 1);
    f->path = g_steal_pointer(&path);
    f->width = w;
    f->height = h;
    f->first_frame = r->next_frame;
    g_ptr_array_add(r->files, f);
    j = g_new0(RecJob, 1);
    j->type = REC_JOB_FILE;
    j->avi = avi;
    j->zmbv = zmbv;
    rec_push(r, j);

    g_clear_pointer(&r->out, pixman_image_unref);
    g_clear_pointer(&r->prev, pixman_image_unref);
    g_clear_pointer(&r->scratch, pixman_image_unref);
    r->out = pixman_image_create_bits(REC_FORMAT, w, h, NULL, w * 4);
    r->prev = pixman_image_create_bits(REC_FORMAT, w, h, NULL, w * 4);
    r->scratch = pixman_image_create_bits(REC_FORMAT, w, h, NULL, w * 4);
    r->width = w;
    r->height = h;
    r->dirty = (RecBox) { 0, 0, w, h };
    r->enc = (RecBox) { 0 };
    r->cursor_box = (RecBox) { 0 };
    r->key_due = true;
    memset(r->blink, 0, sizeof(r->blink));
    return true;
}

/*
 * Write frame r->next_frame: what the screen shows now if @fresh, else
 * the frame before again.
 */
static void rec_frame(Recorder *r, bool fresh)
{
    DisplaySurface *s = qemu_console_surface(r->con);
    GByteArray *d;
    int box[4];
    bool full;

    if (fresh && s && !r->failed) {
        int w = surface_width(s), h = surface_height(s);

        qemu_mutex_lock(&r->lock);
        full = r->file_bytes >= REC_FILE_MAX;
        qemu_mutex_unlock(&r->lock);
        if (!r->files->len || w != r->width || h != r->height || full) {
            Error *err = NULL;

            if (!rec_new_file(r, w, h, &err)) {
                rec_fail(r, error_get_pretty(err));
                error_free(err);
                return;
            }
            rec_refresh(r);
        }
    }
    if (!r->files->len || r->failed) {
        return;
    }
    rec_thumb(r);
    if (!fresh || !s) {
        rec_push_video(r, false, NULL);
        return;
    }

    if (r->key_due ||
        r->next_frame - r->last_key >= (int64_t)r->fps * REC_KEY_S) {
        size_t len = (size_t)r->width * r->height * 4;

        d = g_byte_array_sized_new(len);
        g_byte_array_append(d, (uint8_t *)pixman_image_get_data(r->out), len);
        memcpy(pixman_image_get_data(r->prev), d->data, len);
        rec_push_video(r, true, d);
        r->last_key = r->next_frame;
        r->key_due = false;
        r->enc = (RecBox) { 0 };
        return;
    }
    if (rec_box_empty(&r->enc)) {
        rec_push_video(r, false, NULL);
        return;
    }
    qemu_mutex_lock(&r->lock);
    full = r->queued > REC_QUEUE_MAX;
    qemu_mutex_unlock(&r->lock);
    if (full) {
        /* the disk can't keep up: show the frame before longer */
        r->dropped++;
        rec_push_video(r, false, NULL);
        return;
    }
    d = g_byte_array_new();
    if (!zmbv_delta(r->width, r->height, r->width,
                    pixman_image_get_data(r->prev),
                    pixman_image_get_data(r->out), r->enc.x, r->enc.y,
                    r->enc.w, r->enc.h, 0xffffffff, d, box)) {
        /* nothing changed after all */
        g_clear_pointer(&d, g_byte_array_unref);
    }
    r->enc = (RecBox) { 0 };
    rec_push_video(r, false, d);
}

/* Write the frames that are due by the video's clock */
static void rec_frames_due(Recorder *r)
{
    while (!r->failed && rec_frame_ns(r, r->next_frame) <= r->rec_ns) {
        rec_frame(r, rec_frame_ns(r, r->next_frame + 1) > r->rec_ns);
        r->next_frame++;
    }
}

static void rec_cut(Recorder *r, int64_t skipped_ns, const char *why)
{
    QDict *c = qdict_new();

    qdict_put_int(c, "at-ms", r->rec_ns / SCALE_MS);
    qdict_put_int(c, "skipped-ms", skipped_ns / SCALE_MS);
    qdict_put_str(c, "why", why);
    qlist_append(r->cuts, c);
}

/* Is anything going on: a call, a hold, a change or a sound? */
static bool rec_active(Recorder *r, int64_t now)
{
    int64_t last = MAX(r->active_v, r->loud_v);

    return mcp_calls_running() || r->holding ||
           now - last < r->skip_still_ns;
}

static void rec_tick(void *opaque)
{
    Recorder *r = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t dv = now - r->last_v;
    int64_t gap = (int64_t)REC_GAP_MAX_S * NANOSECONDS_PER_SECOND;
    int64_t due;

    r->last_v = now;
    qemu_mutex_lock(&r->lock);
    r->failed |= r->error != NULL;
    qemu_mutex_unlock(&r->lock);
    if (r->failed) {
        return;
    }

    qemu_console_hw_update(r->con);
    if (rec_refresh(r) >= REC_ACTIVE_PIXELS || mcp_calls_running() ||
        r->holding) {
        r->active_v = now;
    }

    if (r->skipping) {
        if (now - MAX(r->active_v, r->loud_v) < r->skip_still_ns) {
            /* something happens again: show it from now on */
            r->skipping = false;
            r->skipped_ns += now - r->skip_v;
            rec_cut(r, now - r->skip_v, "still");
            r->rec_ns = MAX(r->rec_ns, rec_frame_ns(r, r->next_frame));
        }
    } else {
        if (dv > gap) {
            /* the guest clock jumped: icount without sleep, or a load */
            rec_cut(r, dv, "clock");
            dv = rec_frame_ns(r, 1);
        }
        r->rec_ns += MAX(dv, 0);
        if (r->skip_still_ns && !rec_active(r, now)) {
            r->skipping = true;
            r->skip_v = now;
        }
    }

    if (!r->skipping) {
        rec_frames_due(r);
        rec_audio_flush(r, rec_sample(r->rec_ns) -
                           REC_AUDIO_RATE * REC_AUDIO_LAG_MS / 1000);
        due = rec_frame_ns(r, r->next_frame) - r->rec_ns;
    } else {
        due = rec_frame_ns(r, 1);
    }
    if (!r->failed) {
        timer_mod(r->timer, now + MAX(due, 1));
    }
}

static void rec_vm_state(void *opaque, bool running, RunState state)
{
    Recorder *r = opaque;

    if (running) {
        /* the guest clock may have moved while it was stopped (loadvm) */
        r->last_v = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (r->paused_by_us) {
            r->paused_ms += rec_real_ms() - r->paused_real_ms;
            r->paused_by_us = false;
        }
        if (!r->failed) {
            timer_mod(r->timer, r->last_v);
        }
    }
}

/* Display listener */

static void rec_gfx_update(DisplayChangeListener *dcl,
                           int x, int y, int w, int h)
{
    Recorder *r = container_of(dcl, Recorder, dcl);

    rec_box_add(&r->dirty, x, y, w, h);
}

static void rec_gfx_switch(DisplayChangeListener *dcl,
                           DisplaySurface *surface)
{
    Recorder *r = container_of(dcl, Recorder, dcl);

    if (surface) {
        rec_box_add(&r->dirty, 0, 0, surface_width(surface),
                    surface_height(surface));
    }
}

static void rec_mouse_set(DisplayChangeListener *dcl, int x, int y,
                          bool on)
{
    Recorder *r = container_of(dcl, Recorder, dcl);

    r->mouse_x = x;
    r->mouse_y = y;
    r->mouse_on = on;
}

static void rec_cursor_define(DisplayChangeListener *dcl, QEMUCursor *c)
{
    Recorder *r = container_of(dcl, Recorder, dcl);
    uint32_t *d;
    int i, stride;

    g_clear_pointer(&r->cursor, pixman_image_unref);
    if (!c || !c->width || !c->height) {
        return;
    }
    r->cursor = pixman_image_create_bits(PIXMAN_a8r8g8b8, c->width,
                                         c->height, NULL, 0);
    d = pixman_image_get_data(r->cursor);
    stride = pixman_image_get_stride(r->cursor) / 4;
    for (i = 0; i < c->width * c->height; i++) {
        uint32_t v = c->data[i];
        uint32_t a = v >> 24;

        /* pixman wants it premultiplied */
        d[(i / c->width) * stride + i % c->width] =
            (a << 24) |
            ((((v >> 16) & 0xff) * a / 255) << 16) |
            ((((v >> 8) & 0xff) * a / 255) << 8) |
            ((v & 0xff) * a / 255);
    }
    r->hot_x = c->hot_x;
    r->hot_y = c->hot_y;
    /* a new image in the same place: draw it again */
    rec_box_add(&r->dirty, r->cursor_box.x, r->cursor_box.y,
                r->cursor_box.w, r->cursor_box.h);
    r->cursor_box = (RecBox) { 0 };
}

static const DisplayChangeListenerOps rec_ops = {
    .dpy_name           = "record",
    .dpy_gfx_update     = rec_gfx_update,
    .dpy_gfx_switch     = rec_gfx_switch,
    .dpy_mouse_set      = rec_mouse_set,
    .dpy_cursor_define  = rec_cursor_define,
};

/* Freeze: the VM runs only while an MCP call runs */

typedef struct RecWake {
    Coroutine *co;
} RecWake;

static void rec_resume_bh(void *opaque)
{
    RecWake *w = opaque;
    Recorder *r = recorder;

    if (r && r->paused_by_us) {
        /* here, not in rec_vm_state(), which is gone once it stops */
        r->paused_ms += rec_real_ms() - r->paused_real_ms;
        r->paused_by_us = false;
        if (runstate_check(RUN_STATE_PAUSED)) {
            mcp_quiet_runstate(true);
            vm_start();
            mcp_quiet_runstate(false);
        }
    }
    aio_co_wake(w->co);
}

/* Let the VM run again if we paused it; back when it runs */
static void coroutine_fn rec_co_resume(Recorder *r)
{
    RecWake w = { .co = qemu_coroutine_self() };

    if (!r->paused_by_us) {
        return;
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), rec_resume_bh, &w);
    qemu_coroutine_yield();
}

static void rec_pause_bh(void *opaque)
{
    Recorder *r = recorder;

    if (!r || !r->freeze || r->holding || mcp_calls_running() ||
        !runstate_is_running()) {
        return;
    }
    mcp_quiet_runstate(true);
    vm_stop(RUN_STATE_PAUSED);
    mcp_quiet_runstate(false);
    r->paused_by_us = true;
    r->paused_real_ms = rec_real_ms();
    r->pauses++;
}

/* Tools that only look: they leave a frozen VM paused */
static bool rec_looks(const char *tool)
{
    static const char *const look[] = {
        "screenshot", "frames", "screen-text", "vm-info", "vm-list",
        "vm-stats", "record", NULL
    };
    int i;

    for (i = 0; look[i]; i++) {
        if (!strcmp(tool, look[i])) {
            return true;
        }
    }
    return false;
}

static void coroutine_fn rec_call_hook(void *opaque, const char *tool,
                                       QDict *args, bool begin, bool top)
{
    Recorder *r = opaque;
    Coroutine *co = qemu_coroutine_self();
    bool steps = top && !strcmp(tool, "steps");

    /* the log: each tool once, the steps of a steps call one by one */
    if (!steps && begin && r->n_calls < REC_CALLS_MAX) {
        QDict *c = qdict_new();

        qdict_put_int(c, "at-ms", rec_time(r) / SCALE_MS);
        qdict_put_str(c, "tool", tool);
        if (args && qdict_size(args)) {
            qdict_put(c, "args", qdict_clone_shallow(args));
        }
        qlist_append(r->calls, qobject_ref(c));
        g_hash_table_replace(r->open_calls, co, c);
        r->n_calls++;
    } else if (!steps && !begin) {
        QDict *c = g_hash_table_lookup(r->open_calls, co);

        if (c) {
            qdict_put_int(c, "end-ms", rec_time(r) / SCALE_MS);
            g_hash_table_remove(r->open_calls, co);
        }
    }

    if (!r->freeze) {
        return;
    }
    if (begin && !steps && !rec_looks(tool)) {
        rec_co_resume(r);
    } else if (!begin && top) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(), rec_pause_bh, NULL);
    }
}

/* What stop leaves behind */

static void rec_ms_str(GString *s, int64_t ms)
{
    g_string_append_printf(s, "%02" PRId64 ":%02" PRId64 ":%02" PRId64
                           ",%03" PRId64, ms / 3600000, ms / 60000 % 60,
                           ms / 1000 % 60, ms % 1000);
}

static char *rec_chapters(Recorder *r)
{
    GString *s = g_string_new(NULL);
    QListEntry *e;
    bool first = true;

    QLIST_FOREACH_ENTRY(r->marks, e) {
        QDict *m = qobject_to(QDict, qlist_entry_obj(e));
        int64_t ms = qdict_get_int(m, "at-ms");

        if (strcmp(qdict_get_str(m, "kind"), "chapter")) {
            continue;
        }
        if (first && ms >= 1000) {
            /* YouTube wants the first chapter at 0:00 */
            g_string_append(s, "0:00 Start\n");
        }
        rec_time_str(s, first ? (ms < 1000 ? 0 : ms) : ms);
        g_string_append_printf(s, " %s\n", qdict_get_try_str(m, "text"));
        first = false;
    }
    return g_string_free(s, false);
}

static bool rec_write_srt(Recorder *r, const char *path, Error **errp)
{
    g_autoptr(GString) s = g_string_new(NULL);
    g_autoptr(GError) gerr = NULL;
    QListEntry *e;
    int n = 0;

    QLIST_FOREACH_ENTRY(r->marks, e) {
        QDict *m = qobject_to(QDict, qlist_entry_obj(e));
        int64_t ms = qdict_get_int(m, "at-ms");

        if (strcmp(qdict_get_str(m, "kind"), "caption")) {
            continue;
        }
        g_string_append_printf(s, "%d\n", ++n);
        rec_ms_str(s, ms);
        g_string_append(s, " --> ");
        rec_ms_str(s, ms + qdict_get_int(m, "duration-ms"));
        g_string_append_printf(s, "\n%s\n\n", qdict_get_try_str(m, "text"));
    }
    if (!g_file_set_contents(path, s->str, s->len, &gerr)) {
        error_setg(errp, "can't write %s: %s", path, gerr->message);
        return false;
    }
    return true;
}

static int64_t rec_length_ms(Recorder *r)
{
    return rec_frame_ns(r, r->next_frame) / SCALE_MS;
}

static int64_t rec_file_ms(Recorder *r, RecFile *f)
{
    return (rec_frame_ns(r, f->first_frame + f->frames) -
            rec_frame_ns(r, f->first_frame)) / SCALE_MS;
}

static bool rec_write_sidecar(Recorder *r, const char *path, Error **errp)
{
    g_autoptr(QDict) d = qdict_new();
    g_autoptr(GError) gerr = NULL;
    QList *files = qlist_new();
    GString *json;
    bool ok;
    guint i;

    for (i = 0; i < r->files->len; i++) {
        RecFile *f = g_ptr_array_index(r->files, i);
        QDict *fd = qdict_new();

        qdict_put_str(fd, "file", f->path);
        qdict_put_int(fd, "width", f->width);
        qdict_put_int(fd, "height", f->height);
        qdict_put_int(fd, "start-ms",
                      rec_frame_ns(r, f->first_frame) / SCALE_MS);
        qdict_put_int(fd, "length-ms", rec_file_ms(r, f));
        qdict_put_int(fd, "frames", f->frames);
        qlist_append(files, fd);
    }
    qdict_put_str(d, "recorder", "lemu record");
    qdict_put_int(d, "fps", r->fps);
    qdict_put_int(d, "length-ms", rec_length_ms(r));
    qdict_put_int(d, "frames", r->frames);
    qdict_put_int(d, "dropped-frames", r->dropped);
    if (r->ring) {
        qdict_put_int(d, "audio-rate", REC_AUDIO_RATE);
    }
    qdict_put_bool(d, "freeze", r->freeze);
    qdict_put_int(d, "skip-still-ms", r->skip_still_ns / SCALE_MS);
    qdict_put(d, "files", files);
    qdict_put(d, "marks", qobject_ref(r->marks));
    qdict_put(d, "cuts", qobject_ref(r->cuts));
    qdict_put(d, "calls", qobject_ref(r->calls));
    json = qobject_to_json_pretty(QOBJECT(d), true);
    g_string_append_c(json, '\n');
    ok = g_file_set_contents(path, json->str, json->len, &gerr);
    if (!ok) {
        error_setg(errp, "can't write %s: %s", path, gerr->message);
    }
    g_string_free(json, true);
    return ok;
}

/* ffmpeg filters that make file @f 3840x2160 */
static void rec_render_filter(GString *s, RecFile *f)
{
    int dw, dh, w, h, n;

    rec_display_shape(f->width, f->height, &dw, &dh);
    if ((int64_t)dw * 2160 >= (int64_t)dh * 3840) {
        w = 3840;
        h = (int)(3840LL * dh / dw) & ~1;
    } else {
        h = 2160;
        w = (int)(2160LL * dw / dh) & ~1;
    }
    /* sharp pixels: blow it up whole first, then scale down smoothly */
    n = MAX(DIV_ROUND_UP(w, f->width), DIV_ROUND_UP(h, f->height));
    if (n > 1) {
        g_string_append_printf(s, "scale=iw*%d:ih*%d:flags=neighbor,", n, n);
    }
    g_string_append_printf(s, "scale=%d:%d:flags=area,pad=3840:2160:%d:%d,"
                           "setsar=1", w, h, (3840 - w) / 2, (2160 - h) / 2);
}

static char *rec_render(Recorder *r)
{
    GString *s = g_string_new("ffmpeg");
    g_autofree char *mp4 = g_strdup_printf("%s.mp4", r->base);
    g_autofree char *q = g_shell_quote(mp4);
    bool sound = r->ring != NULL;
    guint i;

    for (i = 0; i < r->files->len; i++) {
        RecFile *f = g_ptr_array_index(r->files, i);
        g_autofree char *p = g_shell_quote(f->path);

        g_string_append_printf(s, " -i %s", p);
    }
    if (r->files->len == 1) {
        g_string_append(s, " -vf '");
        rec_render_filter(s, g_ptr_array_index(r->files, 0));
        g_string_append(s, "'");
    } else {
        g_string_append(s, " -filter_complex '");
        for (i = 0; i < r->files->len; i++) {
            g_string_append_printf(s, "[%u:v]", i);
            rec_render_filter(s, g_ptr_array_index(r->files, i));
            g_string_append_printf(s, "[v%u];", i);
        }
        for (i = 0; i < r->files->len; i++) {
            g_string_append_printf(s, sound ? "[v%u][%u:a]" : "[v%u]", i, i);
        }
        g_string_append_printf(s, "concat=n=%u:v=1:a=%d[v]%s' -map '[v]'%s",
                               r->files->len, sound, sound ? "[a]" : "",
                               sound ? " -map '[a]'" : "");
    }
    g_string_append(s, " -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p");
    if (sound) {
        g_string_append(s, " -c:a aac -b:a 192k -ar 48000");
    }
    g_string_append_printf(s, " -movflags +faststart %s", q);
    return g_string_free(s, false);
}

static void rec_text(pixman_image_t *img, int x, int y, const char *str)
{
    uint32_t *d = pixman_image_get_data(img);
    int stride = pixman_image_get_stride(img) / 4;
    int w = pixman_image_get_width(img), h = pixman_image_get_height(img);
    int i, j;

    for (; *str; str++, x += FONT_WIDTH) {
        const uint8_t *g = &vgafont16[(uint8_t)*str * FONT_HEIGHT];

        for (j = 0; j < FONT_HEIGHT; j++) {
            for (i = 0; i < FONT_WIDTH; i++) {
                if ((g[j] & (0x80 >> i)) && x + i < w && y + j < h) {
                    d[(y + j) * stride + x + i] = 0xffffffff;
                }
            }
        }
    }
}

/* Up to REC_SHEET frames from all through the video, with their times */
static McpImage *rec_sheet(Recorder *r, Error **errp)
{
    g_autoptr(pixman_image_t) sheet = NULL;
    pixman_color_t bg = { 0x2000, 0x2000, 0x2000, 0xffff };
    pixman_rectangle16_t all;
    int pick[REC_SHEET];
    int n = MIN(r->n_thumbs, REC_SHEET), cols, rows, cell_h = 0;
    int gap = 6, label = FONT_HEIGHT + 4, w, h, i;

    if (!n) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        pick[i] = n == 1 ? 0 : i * (r->n_thumbs - 1) / (n - 1);
        cell_h = MAX(cell_h,
                     pixman_image_get_height(r->thumbs[pick[i]].img));
    }
    cols = MIN(n, REC_SHEET_COLUMNS);
    rows = DIV_ROUND_UP(n, cols);
    w = gap + cols * (REC_THUMB_W + gap);
    h = gap + rows * (cell_h + label + gap);
    sheet = pixman_image_create_bits(PIXMAN_x8r8g8b8, w, h, NULL, 0);
    all = (pixman_rectangle16_t) { 0, 0, w, h };
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, sheet, &bg, 1, &all);
    for (i = 0; i < n; i++) {
        RecThumb *t = &r->thumbs[pick[i]];
        int x = gap + (i % cols) * (REC_THUMB_W + gap);
        int y = gap + (i / cols) * (cell_h + label + gap);
        g_autoptr(GString) s = g_string_new(NULL);

        pixman_image_composite(PIXMAN_OP_SRC, t->img, NULL, sheet, 0, 0, 0, 0,
                               x, y, pixman_image_get_width(t->img),
                               pixman_image_get_height(t->img));
        rec_time_str(s, t->ms);
        rec_text(sheet, x, y + cell_h + 2, s->str);
    }
    return mcp_image_encode(sheet, w, h, NULL, 1.0, errp);
}

/* Start and stop */

static void rec_exit(Notifier *n, void *data);

static void rec_free(Recorder *r)
{
    guint i;
    int t;

    for (i = 0; i < r->files->len; i++) {
        RecFile *f = g_ptr_array_index(r->files, i);

        g_free(f->path);
        g_free(f);
    }
    g_ptr_array_unref(r->files);
    g_ptr_array_unref(r->ears);
    for (t = 0; t < r->n_thumbs; t++) {
        pixman_image_unref(r->thumbs[t].img);
    }
    g_clear_pointer(&r->out, pixman_image_unref);
    g_clear_pointer(&r->prev, pixman_image_unref);
    g_clear_pointer(&r->scratch, pixman_image_unref);
    g_clear_pointer(&r->cursor, pixman_image_unref);
    qobject_unref(r->marks);
    qobject_unref(r->calls);
    qobject_unref(r->cuts);
    g_hash_table_unref(r->open_calls);
    qemu_mutex_destroy(&r->lock);
    qemu_cond_destroy(&r->cond);
    g_free(r->ring);
    g_free(r->error);
    g_free(r->base);
    g_free(r);
}

/*
 * Stop taking frames and sound, write the last of it, and tell the
 * writer to finish.  The VM stays as it is.  At @exit, QEMU is gone
 * but for the writer: only finish the files.
 */
static void rec_end(Recorder *r, bool exit)
{
    RecJob *j;

    timer_del(r->timer);
    if (exit) {
        rec_audio_flush(r, rec_sample(rec_frame_ns(r, r->next_frame)));
        j = g_new0(RecJob, 1);
        j->type = REC_JOB_END;
        rec_push(r, j);
        return;
    }
    if (!r->failed && r->files->len) {
        /* the screen as it is now is the last frame */
        r->rec_ns = rec_time(r);
        rec_frames_due(r);
        qemu_console_hw_update(r->con);
        rec_refresh(r);
        rec_frame(r, true);
        r->next_frame++;
        rec_audio_flush(r, rec_sample(rec_frame_ns(r, r->next_frame)));
    }
    rec_ears_close(r);
    qemu_console_unregister_listener(&r->dcl);
    timer_free(r->timer);
    r->timer = NULL;
    qemu_del_vm_change_state_handler(r->vmstate);
    mcp_set_call_hook(NULL, NULL);
    qemu_remove_exit_notifier(&r->exit);
    j = g_new0(RecJob, 1);
    j->type = REC_JOB_END;
    rec_push(r, j);
}

static void rec_exit(Notifier *n, void *data)
{
    Recorder *r = container_of(n, Recorder, exit);
    g_autofree char *json = g_strdup_printf("%s.json", r->base);

    rec_end(r, true);
    qemu_thread_join(&r->thread);
    rec_write_sidecar(r, json, NULL);
    recorder = NULL;
}

static McpRecord *rec_result(Recorder *r)
{
    McpRecord *res = g_new0(McpRecord, 1);
    McpRecordFileList **tail = &res->files;
    guint i;

    res->recording = recorder == r;
    res->has_length_ms = true;
    res->length_ms = rec_length_ms(r);
    res->has_frames = true;
    res->frames = r->frames;
    res->has_dropped_frames = true;
    res->dropped_frames = r->dropped;
    res->has_bytes = true;
    qemu_mutex_lock(&r->lock);
    res->bytes = r->bytes_done + r->file_bytes;
    qemu_mutex_unlock(&r->lock);
    res->has_sound = true;
    res->sound = r->ring != NULL;
    res->has_marks = true;
    res->marks = qlist_size(r->marks);
    if (r->skip_still_ns) {
        res->has_skipped_ms = true;
        res->skipped_ms = (r->skipped_ns +
                           (r->skipping ? r->last_v - r->skip_v : 0)) /
                          SCALE_MS;
    }
    if (r->freeze) {
        res->has_paused_ms = true;
        res->paused_ms = r->paused_ms;
    }
    for (i = 0; i < r->files->len; i++) {
        RecFile *f = g_ptr_array_index(r->files, i);
        McpRecordFile *mf = g_new0(McpRecordFile, 1);

        mf->file = g_strdup(f->path);
        mf->width = f->width;
        mf->height = f->height;
        mf->start_ms = rec_frame_ns(r, f->first_frame) / SCALE_MS;
        mf->length_ms = rec_file_ms(r, f);
        QAPI_LIST_APPEND(tail, mf);
        res->has_files = true;
    }
    return res;
}

static void rec_size_str(GString *s, uint64_t bytes)
{
    if (bytes < 10 * 1024 * 1024) {
        g_string_append_printf(s, "%.1f MB", bytes / 1048576.0);
    } else {
        g_string_append_printf(s, "%" PRIu64 " MB", bytes / 1048576);
    }
}

static void rec_files_str(GString *s, Recorder *r)
{
    guint i;

    for (i = 0; i < r->files->len && i < 3; i++) {
        RecFile *f = g_ptr_array_index(r->files, i);

        g_string_append_printf(s, "%s%s", i ? ", " : "", f->path);
    }
    if (r->files->len > 3) {
        g_string_append_printf(s, " and %u more", r->files->len - 3);
    }
}

static char *rec_default_base(void)
{
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *name = g_date_time_format(now,
                                               "lemu-take-%Y%m%d-%H%M%S");

    return g_canonicalize_filename(name, NULL);
}

static McpRecord *rec_start(QObject *console, int64_t head, const char *file,
                            int64_t fps, bool audio, bool freeze,
                            int64_t skip_still_ms, Error **errp)
{
    static const char *const ext[] = { ".avi", "-2.avi", ".json", ".srt",
                                       ".mp4", NULL };
    g_autoptr(GString) s = g_string_new(NULL);
    Recorder *r;
    QemuConsole *con;
    DisplaySurface *surface;
    McpRecord *res;
    g_autofree char *label = NULL;
    g_autofree char *base = NULL;
    Error *err = NULL;
    int i;

    if (recorder) {
        RecFile *f = g_ptr_array_index(recorder->files, 0);

        error_setg(errp, "it records already, to %s: stop that first",
                   f ? f->path : recorder->base);
        return NULL;
    }
    if (fps < 1 || fps > REC_FPS_MAX) {
        error_setg(errp, "fps must be from 1 to %d", REC_FPS_MAX);
        return NULL;
    }
    if (skip_still_ms < 0 || skip_still_ms > 3600000) {
        error_setg(errp, "skip-still-ms must be from 0 to 3600000");
        return NULL;
    }
    con = mcp_console(console, head, errp);
    if (!con) {
        return NULL;
    }
    surface = qemu_console_surface(con);
    if (!surface) {
        error_setg(errp, "this console has no screen to record");
        return NULL;
    }
    base = file ? g_canonicalize_filename(file, NULL) : rec_default_base();
    if (g_str_has_suffix(base, ".avi")) {
        base[strlen(base) - 4] = 0;
    }
    for (i = 0; ext[i]; i++) {
        g_autofree char *p = g_strconcat(base, ext[i], NULL);

        if (g_file_test(p, G_FILE_TEST_EXISTS)) {
            error_setg(errp, "%s exists: give another file", p);
            return NULL;
        }
    }

    r = g_new0(Recorder, 1);
    r->con = con;
    r->base = g_steal_pointer(&base);
    r->fps = fps;
    r->freeze = freeze;
    r->skip_still_ns = skip_still_ms * SCALE_MS;
    r->files = g_ptr_array_new();
    r->ears = g_ptr_array_new();
    r->marks = qlist_new();
    r->calls = qlist_new();
    r->cuts = qlist_new();
    r->open_calls = g_hash_table_new(NULL, NULL);
    r->thumb_every_ms = 1000;
    qemu_mutex_init(&r->lock);
    qemu_cond_init(&r->cond);
    QSIMPLEQ_INIT(&r->jobs);
    r->last_v = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    r->active_v = r->last_v;

    label = qemu_console_get_label(con);
    g_string_append_printf(s, "Recording console %d (%s, %dx%d) at %d fps",
                           qemu_console_get_index(con), label,
                           surface_width(surface), surface_height(surface),
                           r->fps);
    if (audio) {
        rec_ears_open(r, s);
    } else {
        g_string_append(s, " without sound");
    }

    /* the first file now, so a bad path fails here */
    if (!rec_new_file(r, surface_width(surface), surface_height(surface),
                      &err)) {
        error_propagate(errp, err);
        rec_ears_close(r);
        rec_free(r);
        return NULL;
    }
    qemu_thread_create(&r->thread, "record", rec_writer, r,
                       QEMU_THREAD_JOINABLE);
    recorder = r;

    qemu_console_register_listener(con, &r->dcl, &rec_ops);
    /* its own timer takes the frames: don't speed up the UI's */
    qemu_console_listener_set_refresh(&r->dcl, GUI_REFRESH_INTERVAL_IDLE);
    r->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rec_tick, r);
    r->vmstate = qemu_add_vm_change_state_handler(rec_vm_state, r);
    r->exit.notify = rec_exit;
    qemu_add_exit_notifier(&r->exit);
    mcp_set_call_hook(rec_call_hook, r);

    /* the first frame now */
    qemu_console_hw_update(con);
    rec_refresh(r);
    rec_frame(r, true);
    r->next_frame++;
    timer_mod(r->timer, r->last_v + rec_frame_ns(r, 1));

    g_string_append_printf(s, " to %s.", ((RecFile *)g_ptr_array_index(
                                              r->files, 0))->path);
    if (freeze) {
        g_string_append(s, " The VM runs only during MCP calls (freeze):"
                        " a screenshot shows no progress, so wait with a"
                        " tool's then, wait-screen or a mark's hold-ms.");
    }
    if (r->skip_still_ns) {
        g_string_append_printf(s, " Times when nothing moves or sounds for"
                               " %" PRId64 " ms are left out.",
                               skip_still_ms);
    }
    if (!runstate_is_running()) {
        g_string_append(s, " The VM is paused: it records when it runs.");
    }
    g_string_append(s, " Mark chapters and captions with record mark;"
                    " end with record stop.");
    res = rec_result(r);
    res->summary = g_string_free(g_steal_pointer(&s), false);
    return res;
}

static McpRecord *coroutine_fn rec_mark(McpRecordMarkKind kind,
                                        const char *text,
                                        ScreenRegion *region,
                                        int64_t duration_ms,
                                        int64_t hold_ms, Error **errp)
{
    Recorder *r = recorder;
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx;
    g_autoptr(GString) s = g_string_new(NULL);
    QDict *m;
    McpRecord *res;
    int64_t at;

    if (!r) {
        error_setg(errp, "it does not record: call record start first");
        return NULL;
    }
    if (kind != MCP_RECORD_MARK_KIND_ZOOM && (!text || !*text)) {
        error_setg(errp, "a %s needs text", McpRecordMarkKind_str(kind));
        return NULL;
    }
    if (kind == MCP_RECORD_MARK_KIND_ZOOM &&
        (!region || region->w <= 0 || region->h <= 0)) {
        error_setg(errp, "a zoom needs a region");
        return NULL;
    }
    if (duration_ms < 100 || duration_ms > 600000) {
        error_setg(errp, "duration-ms must be from 100 to 600000");
        return NULL;
    }
    if (hold_ms < 0 || hold_ms > REC_HOLD_MAX_MS) {
        error_setg(errp, "hold-ms must be from 0 to %d", REC_HOLD_MAX_MS);
        return NULL;
    }
    if (qlist_size(r->marks) >= REC_MARKS_MAX) {
        error_setg(errp, "a recording takes up to %d marks", REC_MARKS_MAX);
        return NULL;
    }

    at = rec_time(r) / SCALE_MS;
    m = qdict_new();
    qdict_put_int(m, "at-ms", at);
    qdict_put_str(m, "kind", McpRecordMarkKind_str(kind));
    if (text) {
        qdict_put_str(m, "text", text);
    }
    if (kind == MCP_RECORD_MARK_KIND_CAPTION ||
        kind == MCP_RECORD_MARK_KIND_ZOOM) {
        qdict_put_int(m, "duration-ms", duration_ms);
    }
    if (region) {
        QDict *rg = qdict_new();

        qdict_put_int(rg, "x", region->x);
        qdict_put_int(rg, "y", region->y);
        qdict_put_int(rg, "w", region->w);
        qdict_put_int(rg, "h", region->h);
        qdict_put(m, "region", rg);
    }
    qlist_append(r->marks, m);
    r->n_chapters += kind == MCP_RECORD_MARK_KIND_CHAPTER;
    r->n_captions += kind == MCP_RECORD_MARK_KIND_CAPTION;

    g_string_append_printf(s, "Marked a %s at ", McpRecordMarkKind_str(kind));
    rec_len_str(s, at);
    if (hold_ms) {
        int64_t until = at + hold_ms;
        int64_t give_up = rec_real_ms() + hold_ms + 60000;

        ctx = mcp_ctx(&local);
        r->holding++;
        rec_co_resume(r);
        while (recorder == r && !ctx->cancelled &&
               rec_time(r) / SCALE_MS < until && rec_real_ms() < give_up) {
            qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME,
                                      10 * SCALE_MS);
        }
        r->holding--;
        if (recorder != r) {
            error_setg(errp, "the recording ended during the hold");
            return NULL;
        }
        g_string_append_printf(s, ", then recorded %" PRId64 " ms more",
                               rec_time(r) / SCALE_MS - at);
        if (!runstate_is_running() && !r->paused_by_us) {
            g_string_append(s, " (the VM is paused)");
        }
    }
    g_string_append_c(s, '.');
    res = rec_result(r);
    res->summary = g_string_free(g_steal_pointer(&s), false);
    return res;
}

static McpRecord *rec_status(Error **errp)
{
    Recorder *r = recorder;
    g_autoptr(GString) s = g_string_new(NULL);
    McpRecord *res;

    if (!r) {
        res = g_new0(McpRecord, 1);
        res->recording = false;
        res->summary = g_strdup("It does not record.");
        return res;
    }
    res = rec_result(r);
    g_string_append(s, "Recording: ");
    rec_len_str(s, rec_time(r) / SCALE_MS);
    g_string_append_printf(s, " so far, %" PRId64 " frames", r->frames);
    if (r->dropped) {
        g_string_append_printf(s, " (%" PRId64 " dropped: the disk is too"
                               " slow)", r->dropped);
    }
    g_string_append(s, ", ");
    rec_size_str(s, res->bytes);
    g_string_append(s, " in ");
    rec_files_str(s, r);
    if (r->skipping) {
        g_string_append(s, "; nothing moves now, so it leaves this out");
    }
    if (r->paused_by_us) {
        g_string_append(s, "; the VM waits paused for the next call");
    }
    qemu_mutex_lock(&r->lock);
    if (r->error) {
        g_string_append_printf(s, "; it stopped writing: %s", r->error);
    }
    qemu_mutex_unlock(&r->lock);
    g_string_append_c(s, '.');
    res->summary = g_string_free(g_steal_pointer(&s), false);
    return res;
}

static McpRecord *coroutine_fn rec_stop(Error **errp)
{
    Recorder *r = recorder;
    g_autoptr(GString) s = g_string_new(NULL);
    g_autofree char *json = NULL;
    g_autofree char *srt = NULL;
    McpRecord *res;
    McpImage *sheet;
    Error *err = NULL;
    bool done = false;

    if (!r) {
        error_setg(errp, "it does not record");
        return NULL;
    }
    rec_end(r, false);
    /* the VM runs on as it did before the recording */
    rec_co_resume(r);
    recorder = NULL;
    while (!done) {
        qemu_mutex_lock(&r->lock);
        done = r->writer_done;
        qemu_mutex_unlock(&r->lock);
        if (!done) {
            qemu_co_sleep_ns(QEMU_CLOCK_REALTIME, 10 * SCALE_MS);
        }
    }
    qemu_thread_join(&r->thread);

    res = rec_result(r);
    json = g_strdup_printf("%s.json", r->base);
    if (rec_write_sidecar(r, json, &err)) {
        res->sidecar = g_strdup(json);
    }
    if (r->n_captions) {
        srt = g_strdup_printf("%s.srt", r->base);
        if (rec_write_srt(r, srt, err ? NULL : &err)) {
            res->subtitles = g_strdup(srt);
        }
    }
    res->render = rec_render(r);
    if (r->n_chapters) {
        res->chapters = rec_chapters(r);
    }

    g_string_append(s, "Recorded ");
    rec_len_str(s, res->length_ms);
    g_string_append_printf(s, " (%" PRId64 " frames at %d fps) %s sound, ",
                           r->frames, r->fps, r->ring ? "with" : "without");
    rec_size_str(s, res->bytes);
    g_string_append_printf(s, " in %u file%s: ", r->files->len,
                           r->files->len == 1 ? "" : "s");
    rec_files_str(s, r);
    g_string_append_c(s, '.');
    if (r->dropped) {
        g_string_append_printf(s, " %" PRId64 " frames came too late for the"
                               " disk and show the frame before.",
                               r->dropped);
    }
    if (r->error) {
        g_string_append_printf(s, " It stopped writing early: %s.", r->error);
    }
    if (res->has_skipped_ms && res->skipped_ms) {
        g_string_append(s, " It left out ");
        rec_len_str(s, res->skipped_ms);
        g_string_append(s, " where nothing moved.");
    }
    if (r->pauses == 1) {
        g_string_append(s, " The VM waited paused between calls once.");
    } else if (r->pauses) {
        g_string_append_printf(s, " The VM waited paused between calls %d"
                               " times.", r->pauses);
    }
    g_string_append(s, " To make the video for YouTube, run the command in"
                    " render.");
    if (r->n_chapters) {
        g_string_append(s, " Paste chapters into the video's description.");
    }
    if (res->subtitles) {
        g_string_append_printf(s, " Upload %s as its subtitles.",
                               res->subtitles);
    }
    if (res->sidecar) {
        g_string_append_printf(s, " %s has the marks, the calls and the"
                               " cuts.", res->sidecar);
    }
    if (err) {
        g_string_append_printf(s, " But: %s.", error_get_pretty(err));
        error_free(err);
    }
    sheet = rec_sheet(r, NULL);
    if (sheet) {
        QAPI_LIST_PREPEND(res->images, sheet);
        res->has_images = true;
    }
    res->summary = g_string_free(g_steal_pointer(&s), false);
    rec_free(r);
    return res;
}

McpRecord *coroutine_fn qmp_record(McpRecordAction action, const char *file,
                                   QObject *console, bool has_head,
                                   int64_t head, bool has_fps, int64_t fps,
                                   bool has_audio, bool audio,
                                   bool has_freeze, bool freeze,
                                   bool has_skip_still_ms,
                                   int64_t skip_still_ms,
                                   bool has_kind, McpRecordMarkKind kind,
                                   const char *text, ScreenRegion *region,
                                   bool has_duration_ms, int64_t duration_ms,
                                   bool has_hold_ms, int64_t hold_ms,
                                   Error **errp)
{
    switch (action) {
    case MCP_RECORD_ACTION_START:
        return rec_start(console, has_head ? head : 0, file,
                         has_fps ? fps : REC_FPS, has_audio ? audio : true,
                         has_freeze && freeze,
                         has_skip_still_ms ? skip_still_ms : 0, errp);
    case MCP_RECORD_ACTION_MARK:
        return rec_mark(has_kind ? kind : MCP_RECORD_MARK_KIND_CHAPTER, text,
                        region, has_duration_ms ? duration_ms : REC_CAPTION_MS,
                        has_hold_ms ? hold_ms : 0, errp);
    case MCP_RECORD_ACTION_STATUS:
        return rec_status(errp);
    case MCP_RECORD_ACTION_STOP:
        return rec_stop(errp);
    default:
        g_assert_not_reached();
    }
}
