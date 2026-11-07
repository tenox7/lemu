/*
 * MCP: hear the guest's sound (see qapi/mcp.json, sound)
 *
 * It listens through the audio capture interface, as VNC's audio does:
 * to what the sound devices play into each audiodev, mixed, whatever
 * the guest and the device.  It cuts that into 10 ms windows.  A window
 * is loud when a channel's level, less any DC offset, reaches the
 * threshold; loud windows with no pause longer than SOUND_GAP_MS
 * between them make one sound.
 *
 * Listening costs nothing while the guest is silent: the audio layer
 * calls it only with audio that a sound device plays.  All of it runs
 * in the main loop, with the BQL.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include <math.h>

#include "monitor/mcp.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-mcp.h"
#include "qemu/audio.h"
#include "qemu/audio-capture.h"
#include "qemu/coroutine.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "system/runstate.h"
#include "ui/input.h"

#define SOUND_RATE          44100
#define SOUND_CHANNELS      2
#define SOUND_WINDOW        (SOUND_RATE / 100)      /* frames in 10 ms */
#define SOUND_WINDOW_NS     (10 * SCALE_MS)
#define SOUND_GAP_MS        150
#define SOUND_KEEP          1000    /* the latest sounds it keeps */
#define SOUND_LIST          20      /* the most sounds it lists */
#define SOUND_POLL_MS       20
#define SOUND_TIMEOUT_MS    30000
#define SOUND_QUIET_MS      1000
#define SOUND_QUIET_MAX_MS  60000
#define SOUND_THRESHOLD_DB  -50
#define SOUND_FULL          32768.0

/* An audiodev it listens to */
typedef struct McpSoundEar {
    char *id;
    char *driver;
    AudioBackend *be;           /* NULL once the audio layer dropped it */
    CaptureVoiceOut *cap;
    unsigned index;
    int64_t played;             /* frames it heard, sound or silence */

    /* the window being summed up */
    int n;
    int64_t sum[SOUND_CHANNELS];
    int64_t sumsq[SOUND_CHANNELS];
    int min[SOUND_CHANNELS];
    int max[SOUND_CHANNELS];
} McpSoundEar;

/* A sound: loud windows close together */
typedef struct McpSoundSeg {
    int64_t start_ns;           /* virtual clock: its first loud window */
    int64_t end_ns;             /* and the end of its last one */
    double peak;                /* its loudest moment, of full scale */
    double energy;              /* mean square, of full scale, x frames */
    int64_t frames;             /* in its loud windows */
    unsigned channels;          /* a bit for each channel it was loud on */
    unsigned ears;              /* a bit for each ear that heard it */
} McpSoundSeg;

static struct {
    bool on;
    unsigned generation;        /* counts the starts */
    int threshold_db;
    double threshold;           /* the threshold as RMS, of full scale */
    int64_t since_ns;           /* virtual clock: when it began listening */
    int64_t cursor_ns;          /* and when the wait before returned */
    double loudest_quiet;       /* RMS of the loudest window under it */
    GPtrArray *ears;
    GArray *segs;               /* McpSoundSeg: the latest SOUND_KEEP */
    int64_t dropped;            /* older sounds it no longer keeps */
} mcp_sound;

static void mcp_sound_window_reset(McpSoundEar *ear)
{
    int c;

    ear->n = 0;
    for (c = 0; c < SOUND_CHANNELS; c++) {
        ear->sum[c] = 0;
        ear->sumsq[c] = 0;
        ear->min[c] = INT_MAX;
        ear->max[c] = INT_MIN;
    }
}

static McpSoundSeg *mcp_sound_latest(void)
{
    GArray *a = mcp_sound.segs;

    return a && a->len ? &g_array_index(a, McpSoundSeg, a->len - 1) : NULL;
}

/* A window of @ear is full; it ended at @end_ns */
static void mcp_sound_window(McpSoundEar *ear, int64_t end_ns)
{
    int64_t start_ns = end_ns - SOUND_WINDOW_NS;
    double var[SOUND_CHANNELS], peak = 0, level = 0, square = 0;
    unsigned loud = 0;
    int n = ear->n, c;
    McpSoundSeg *seg;

    for (c = 0; c < SOUND_CHANNELS; c++) {
        double mean = (double)ear->sum[c] / n;
        double rms;

        /* less the mean, so that a DC offset is not a sound */
        var[c] = MAX((double)ear->sumsq[c] / n - mean * mean, 0);
        rms = sqrt(var[c]) / SOUND_FULL;
        level = MAX(level, rms);
        if (rms >= mcp_sound.threshold) {
            loud |= 1u << c;
            peak = MAX(peak, MAX(ear->max[c] - mean, mean - ear->min[c]) /
                             SOUND_FULL);
            square += var[c] / (SOUND_FULL * SOUND_FULL);
        }
    }
    mcp_sound_window_reset(ear);
    if (!loud) {
        mcp_sound.loudest_quiet = MAX(mcp_sound.loudest_quiet, level);
        return;
    }
    square /= ctpop32(loud);

    seg = mcp_sound_latest();
    if (!seg || start_ns > seg->end_ns + SOUND_GAP_MS * SCALE_MS) {
        McpSoundSeg fresh = { .start_ns = start_ns, .end_ns = end_ns };

        if (mcp_sound.segs->len == SOUND_KEEP) {
            g_array_remove_index(mcp_sound.segs, 0);
            mcp_sound.dropped++;
        }
        g_array_append_val(mcp_sound.segs, fresh);
        seg = mcp_sound_latest();
    }
    seg->start_ns = MIN(seg->start_ns, start_ns);
    seg->end_ns = MAX(seg->end_ns, end_ns);
    seg->peak = MAX(seg->peak, peak);
    seg->energy += square * n;
    seg->frames += n;
    seg->channels |= loud;
    seg->ears |= 1u << MIN(ear->index, 31);
}

/* Audio that the sound devices played into @opaque's audiodev */
static void mcp_sound_capture(void *opaque, const void *buf, int size)
{
    McpSoundEar *ear = opaque;
    const int16_t *x = buf;
    int frames = size / (SOUND_CHANNELS * sizeof(int16_t));
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int i, c;

    for (i = 0; i < frames; i++, x += SOUND_CHANNELS) {
        for (c = 0; c < SOUND_CHANNELS; c++) {
            int v = x[c];

            ear->sum[c] += v;
            ear->sumsq[c] += v * v;
            ear->min[c] = MIN(ear->min[c], v);
            ear->max[c] = MAX(ear->max[c], v);
        }
        if (++ear->n == SOUND_WINDOW) {
            /* the frames after this one played after it */
            mcp_sound_window(ear, now - muldiv64(frames - 1 - i,
                                                 NANOSECONDS_PER_SECOND,
                                                 SOUND_RATE));
        }
    }
    ear->played += frames;
}

static void mcp_sound_notify(void *opaque, audcnotification_e cmd)
{
    /* it goes by the audio that comes instead */
}

/* The audio layer dropped the capture: we deleted it, or QEMU exits */
static void mcp_sound_destroy(void *opaque)
{
    McpSoundEar *ear = opaque;

    ear->cap = NULL;
    ear->be = NULL;
}

static void mcp_sound_close(void)
{
    guint i;

    for (i = 0; mcp_sound.ears && i < mcp_sound.ears->len; i++) {
        McpSoundEar *ear = g_ptr_array_index(mcp_sound.ears, i);

        if (ear->be && ear->cap) {
            audio_be_del_capture(ear->be, ear->cap, ear);
        }
        g_free(ear->id);
        g_free(ear->driver);
        g_free(ear);
    }
    g_clear_pointer(&mcp_sound.ears, g_ptr_array_unref);
    g_clear_pointer(&mcp_sound.segs, g_array_unref);
    mcp_sound.on = false;
}

static int mcp_sound_find(Object *child, void *opaque)
{
    Object *be = object_dynamic_cast(child, TYPE_AUDIO_BACKEND);

    if (be) {
        g_ptr_array_add(opaque, be);
    }
    return 0;
}

/* Begin listening to every audiodev; say which in @s */
static bool mcp_sound_open(int threshold_db, GString *s, Error **errp)
{
    static const struct audio_capture_ops ops = {
        .notify = mcp_sound_notify,
        .capture = mcp_sound_capture,
        .destroy = mcp_sound_destroy,
    };
    struct audsettings as = {
        .freq = SOUND_RATE,
        .nchannels = SOUND_CHANNELS,
        .fmt = AUDIO_FORMAT_S16,
        .big_endian = HOST_BIG_ENDIAN,
    };
    Object *root = object_resolve_path_component(object_get_root(),
                                                 "audiodevs");
    g_autoptr(GPtrArray) bes = g_ptr_array_new();
    g_autoptr(GString) deaf = g_string_new(NULL);
    guint i;

    if (root) {
        object_child_foreach(root, mcp_sound_find, bes);
    }
    if (!bes->len) {
        error_setg(errp, "this VM has no sound to hear: it has no audiodev"
                   " for a sound device to play into (start it with"
                   " -audiodev, and give its sound device audiodev=)");
        return false;
    }

    mcp_sound_close();
    mcp_sound.ears = g_ptr_array_new();
    mcp_sound.segs = g_array_new(false, true, sizeof(McpSoundSeg));
    mcp_sound.dropped = 0;
    mcp_sound.loudest_quiet = 0;
    mcp_sound.threshold_db = threshold_db;
    mcp_sound.threshold = pow(10, threshold_db / 20.0);
    for (i = 0; i < bes->len; i++) {
        AudioBackend *be = g_ptr_array_index(bes, i);
        const char *type = object_get_typename(OBJECT(be));
        McpSoundEar *ear = g_new0(McpSoundEar, 1);

        ear->id = g_strdup(audio_be_get_id(be));
        ear->driver = g_strdup(g_str_has_prefix(type, "audio-") ? type + 6
                                                                : type);
        ear->be = be;
        ear->index = mcp_sound.ears->len;
        mcp_sound_window_reset(ear);
        if (AUDIO_BACKEND_GET_CLASS(be)->add_capture) {
            ear->cap = audio_be_add_capture(be, &as, &ops, ear);
        }
        if (!ear->cap) {
            g_string_append_printf(deaf, "%s'%s'", deaf->len ? ", " : "",
                                   ear->id);
            g_free(ear->id);
            g_free(ear->driver);
            g_free(ear);
            continue;
        }
        g_ptr_array_add(mcp_sound.ears, ear);
    }
    if (!mcp_sound.ears->len) {
        mcp_sound_close();
        error_setg(errp, "it can't hear audiodev %s: its mixing engine is"
                   " off (out.mixing-engine=off)", deaf->str);
        return false;
    }
    mcp_sound.on = true;
    mcp_sound.generation++;
    mcp_sound.since_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    mcp_sound.cursor_ns = mcp_sound.since_ns;

    g_string_append(s, "Listening to ");
    for (i = 0; i < mcp_sound.ears->len; i++) {
        McpSoundEar *ear = g_ptr_array_index(mcp_sound.ears, i);

        g_string_append_printf(s, "%saudiodev '%s' (%s)",
                               !i ? "" : i + 1 < mcp_sound.ears->len
                                         ? ", " : " and ",
                               ear->id, ear->driver);
    }
    g_string_append_printf(s, "; audio quieter than %d dBFS counts as"
                           " silence.", threshold_db);
    if (deaf->len) {
        g_string_append_printf(s, " It can't hear audiodev %s: its mixing"
                               " engine is off (out.mixing-engine=off).",
                               deaf->str);
    }
    if (!runstate_is_running()) {
        g_string_append(s, " The VM is paused: it plays no sound until it"
                        " runs.");
    }
    return true;
}

static void mcp_sound_time(GString *s, int64_t ms)
{
    if (ms < 1000) {
        g_string_append_printf(s, "%" PRId64 " ms", ms);
    } else if (ms < 10000) {
        g_string_append_printf(s, "%.2f s", ms / 1000.0);
    } else if (ms < 100000) {
        g_string_append_printf(s, "%.1f s", ms / 1000.0);
    } else {
        g_string_append_printf(s, "%.0f s", ms / 1000.0);
    }
}

/* @x of full scale in dB, to a tenth */
static double mcp_sound_db(double x)
{
    return round(20 * log10(MAX(x, 1e-6)) * 10) / 10;
}

static int64_t mcp_sound_played_ms(void)
{
    int64_t played = 0;
    guint i;

    for (i = 0; i < mcp_sound.ears->len; i++) {
        McpSoundEar *ear = g_ptr_array_index(mcp_sound.ears, i);

        played = MAX(played, ear->played);
    }
    return muldiv64(played, 1000, SOUND_RATE);
}

/* The sounds from index @from on: in @s, a line each, and in @r */
static void mcp_sound_list(GString *s, McpSound *r, guint from)
{
    GArray *a = mcp_sound.segs;
    McpSoundHeardList **tail = &r->sounds;
    guint i;

    r->has_sounds = true;
    for (i = from; i < a->len; i++) {
        McpSoundSeg *seg = &g_array_index(a, McpSoundSeg, i);
        McpSoundHeard *h;

        if (i - from == SOUND_LIST) {
            g_string_append_printf(s, "\n... and %u more", a->len - i);
            break;
        }
        h = g_new0(McpSoundHeard, 1);
        h->at_ms = MAX(seg->start_ns - mcp_sound.since_ns, 0) / SCALE_MS;
        h->length_ms = (seg->end_ns - seg->start_ns) / SCALE_MS;
        h->peak_db = mcp_sound_db(seg->peak);
        h->average_db = mcp_sound_db(sqrt(seg->energy / seg->frames));
        if (SOUND_CHANNELS == 2 && seg->channels != 3) {
            h->channel = g_strdup(seg->channels == 1 ? "left" : "right");
        }
        if (mcp_sound.ears->len > 1) {
            McpSoundEar *ear = g_ptr_array_index(mcp_sound.ears,
                                                 ctz32(seg->ears));

            h->audiodev = g_strdup(ear->id);
        }
        QAPI_LIST_APPEND(tail, h);

        g_string_append(s, "\nat ");
        mcp_sound_time(s, h->at_ms);
        g_string_append(s, ": ");
        mcp_sound_time(s, h->length_ms);
        g_string_append_printf(s, " long, loudest %.1f dBFS, average"
                               " %.1f dBFS", h->peak_db, h->average_db);
        if (h->channel) {
            g_string_append_printf(s, ", %s channel only", h->channel);
        }
        if (h->audiodev) {
            g_string_append_printf(s, ", audiodev '%s'", h->audiodev);
        }
    }
}

/* Why it heard nothing: what the sound devices played */
static void mcp_sound_silence(GString *s)
{
    int64_t played_ms = mcp_sound_played_ms();

    if (!played_ms) {
        g_string_append(s, " Since listening began, the sound devices"
                        " played nothing into the audiodev.");
        return;
    }
    g_string_append(s, " Since listening began, the sound devices played ");
    mcp_sound_time(s, played_ms);
    if (mcp_sound.loudest_quiet == 0) {
        g_string_append(s, " of audio, all of it silence.");
    } else {
        g_string_append_printf(s, " of audio, all of it quieter than %d"
                               " dBFS (the loudest moment was %.1f dBFS).",
                               mcp_sound.threshold_db,
                               mcp_sound_db(mcp_sound.loudest_quiet));
    }
}

static McpSound *mcp_sound_result(bool listening)
{
    McpSound *r = g_new0(McpSound, 1);

    r->listening = listening;
    r->has_listened_ms = true;
    r->listened_ms = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                      mcp_sound.since_ns) / SCALE_MS;
    r->has_played_ms = true;
    r->played_ms = mcp_sound_played_ms();
    return r;
}

static McpSound *coroutine_fn mcp_sound_wait(int64_t timeout_ms,
                                             int64_t quiet_ms, Error **errp)
{
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_current_input_ctx();
    g_autoptr(GString) s = g_string_new(NULL);
    g_autoptr(GString) opened = NULL;
    int64_t start = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t now_ns, waited;
    bool heard, matched = false, paused = false;
    unsigned generation;
    McpSoundSeg *seg;
    McpSound *r;
    guint from, n;

    if (!ctx) {
        qemu_input_ctx_init(&local, NULL);
        ctx = &local;
    }
    if (!mcp_sound.on) {
        opened = g_string_new(NULL);
        if (!mcp_sound_open(SOUND_THRESHOLD_DB, opened, errp)) {
            return NULL;
        }
    }
    generation = mcp_sound.generation;
    for (;;) {
        now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        seg = mcp_sound_latest();
        heard = seg && seg->end_ns > mcp_sound.cursor_ns;
        if (heard && (!quiet_ms ||
                      now_ns - seg->end_ns >= quiet_ms * SCALE_MS)) {
            matched = true;
            break;
        }
        if (!runstate_is_running()) {
            paused = true;
            break;
        }
        waited = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - start;
        if (waited >= timeout_ms || ctx->cancelled) {
            break;
        }
        qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME,
                                  MIN(timeout_ms - waited, SOUND_POLL_MS) *
                                  SCALE_MS);
        if (!mcp_sound.on || mcp_sound.generation != generation) {
            error_setg(errp, "another call started or stopped listening"
                       " while this one waited");
            return NULL;
        }
    }
    if (ctx->cancelled) {
        error_setg(errp, "cancelled");
        return NULL;
    }
    waited = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - start;

    /* the sounds that played after the wait before */
    for (from = mcp_sound.segs->len; from > 0; from--) {
        if (g_array_index(mcp_sound.segs, McpSoundSeg,
                          from - 1).end_ns <= mcp_sound.cursor_ns) {
            break;
        }
    }
    n = mcp_sound.segs->len - from;
    if (matched && quiet_ms) {
        g_string_append_printf(s, "Heard %u sound%s, then quiet for ", n,
                               n == 1 ? "" : "s");
        mcp_sound_time(s, (now_ns - seg->end_ns) / SCALE_MS);
        g_string_append_c(s, ':');
    } else if (matched) {
        g_string_append_printf(s, "Heard %u sound%s; the last one plays"
                               " now (wait with quiet-ms to hear it out):",
                               n, n == 1 ? "" : "s");
    } else if (paused) {
        g_string_append(s, "The VM is paused, so it plays no sound: resume"
                        " it, then wait again.");
        if (heard) {
            g_string_append_printf(s, " Before it paused, it heard %u"
                                   " sound%s:", n, n == 1 ? "" : "s");
        }
    } else if (heard) {
        g_string_append_printf(s, "Heard %u sound%s, but no quiet of ", n,
                               n == 1 ? "" : "s");
        mcp_sound_time(s, quiet_ms);
        g_string_append(s, " after the last one in ");
        mcp_sound_time(s, waited);
        g_string_append_c(s, ':');
    } else {
        g_string_append(s, "No sound in ");
        mcp_sound_time(s, waited);
        g_string_append_c(s, '.');
        if (from || mcp_sound.dropped) {
            int64_t before = from + mcp_sound.dropped;

            g_string_append_printf(s, " (It heard %" PRId64 " sound%s"
                                   " before: stop lists them.)", before,
                                   before == 1 ? "" : "s");
        }
        mcp_sound_silence(s);
    }

    r = mcp_sound_result(true);
    r->has_matched = true;
    r->matched = matched;
    if (heard) {
        mcp_sound_list(s, r, from);
    }
    if (opened) {
        g_string_append_printf(s, "\n(It was not listening before this"
                               " call, so it heard only what played after"
                               " it. %s)", opened->str);
    }
    mcp_sound.cursor_ns = now_ns;
    r->summary = g_string_free(g_steal_pointer(&s), false);
    return r;
}

static McpSound *mcp_sound_stop(Error **errp)
{
    g_autoptr(GString) s = g_string_new(NULL);
    int64_t all;
    McpSound *r;

    if (!mcp_sound.on) {
        error_setg(errp, "it is not listening: call sound with action"
                   " start first");
        return NULL;
    }
    r = mcp_sound_result(false);
    all =mcp_sound.segs->len + mcp_sound.dropped;
    g_string_append(s, "Listened for ");
    mcp_sound_time(s, r->listened_ms);
    if (!all) {
        g_string_append(s, " and heard no sound.");
        mcp_sound_silence(s);
    } else {
        g_string_append_printf(s, ": heard %" PRId64 " sound%s:", all,
                               all == 1 ? "" : "s");
        if (mcp_sound.dropped) {
            g_string_append_printf(s, " (the first %" PRId64 " are no"
                                   " longer kept)", mcp_sound.dropped);
        }
        mcp_sound_list(s, r, 0);
    }
    g_string_append(s, "\nStopped listening.");
    mcp_sound_close();
    r->summary = g_string_free(g_steal_pointer(&s), false);
    return r;
}

McpSound *coroutine_fn qmp_sound(McpSoundAction action,
                                 bool has_timeout_ms, int64_t timeout_ms,
                                 bool has_quiet_ms, int64_t quiet_ms,
                                 bool has_threshold_db, int64_t threshold_db,
                                 Error **errp)
{
    g_autoptr(GString) s = NULL;
    McpSound *r;

    if ((has_timeout_ms || has_quiet_ms) &&
        action != MCP_SOUND_ACTION_WAIT) {
        error_setg(errp, "timeout-ms and quiet-ms go with wait");
        return NULL;
    }
    if (has_threshold_db && action != MCP_SOUND_ACTION_START) {
        error_setg(errp, "threshold-db goes with start");
        return NULL;
    }
    if (has_threshold_db && (threshold_db < -90 || threshold_db > -10)) {
        error_setg(errp, "threshold-db must be -90 to -10");
        return NULL;
    }
    if (has_quiet_ms && (quiet_ms < 0 || quiet_ms > SOUND_QUIET_MAX_MS)) {
        error_setg(errp, "quiet-ms must be 0 to %d", SOUND_QUIET_MAX_MS);
        return NULL;
    }

    switch (action) {
    case MCP_SOUND_ACTION_START:
        s = g_string_new(mcp_sound.on ? "What it heard before is"
                         " forgotten. " : "");
        if (!mcp_sound_open(has_threshold_db ? threshold_db
                                             : SOUND_THRESHOLD_DB,
                            s, errp)) {
            return NULL;
        }
        g_string_append(s, " Now do the thing that should make a sound,"
                        " then call sound with action wait: it waits for"
                        " a sound and for the quiet after it.");
        r = mcp_sound_result(true);
        r->summary = g_string_free(g_steal_pointer(&s), false);
        return r;
    case MCP_SOUND_ACTION_WAIT:
        timeout_ms = has_timeout_ms ? timeout_ms : SOUND_TIMEOUT_MS;
        timeout_ms = MIN(MAX(timeout_ms, 0), mcp_wait_max_ms());
        return mcp_sound_wait(timeout_ms,
                              has_quiet_ms ? quiet_ms : SOUND_QUIET_MS,
                              errp);
    case MCP_SOUND_ACTION_STOP:
        return mcp_sound_stop(errp);
    default:
        g_assert_not_reached();
    }
}
