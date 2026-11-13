/*
 * Pointer model: put the guest's mouse pointer on a given pixel
 *
 * Old guests have relative mice with acceleration, so "move to x,y"
 * has no direct meaning there.  Three ways to get there anyway:
 *
 * - absolute: the device takes absolute positions.  Exact.
 * - closed loop: the display card reports where it draws its hardware
 *   cursor.  Send a move, read the cursor back, correct, repeat.
 *   Acceleration does not matter.
 * - open loop: push the pointer into the top-left corner, where guests
 *   clamp it, then step to the target in moves small enough to stay
 *   below guest acceleration, paced so the mouse device does not merge
 *   them into bigger moves.  The first open loop move measures how the
 *   guest moves the pointer, by finding it on the screen: the biggest
 *   step it does not speed up (some speed up any move bigger than one
 *   count), and how many pixels a count moves (some scale all motion,
 *   so 100 counts may move 35 pixels or 200).
 *
 * The model forgets the position when something else moves the mouse,
 * when the screen size changes, and on reset.
 *
 * Natural motion, for videos, moves the pointer the way a hand moves a
 * mouse, on top of any of the three: it speeds up, slows down before
 * the target (minimum jerk) and bends a little on the way, in about
 * the time Fitts' law gives a person.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qapi/error.h"
#include "qemu/queue.h"
#include "qemu/timer.h"
#include "system/reset.h"
#include "ui/frames.h"
#include "ui/pointer.h"
#include "trace.h"

#define POINTER_HOME_PACKET     100     /* counts per homing packet */
#define POINTER_HOME_ROUNDS     8       /* closed loop: homing rounds at most */
#define POINTER_STALE_MS        10000
#define POINTER_LOOP_CAP        40      /* largest closed-loop move */
#define POINTER_LOOP_MAX        300     /* closed-loop iterations */
#define POINTER_POLL_MS         10
#define POINTER_SETTLE_MS       150
#define POINTER_IMAGE_HOLD_MS   60      /* a new cursor image holds this long */
#define POINTER_IMAGE_SETTLE_MS 600     /* and settles in this time at most */
#define POINTER_CAL_DIST        48      /* counts each step test covers */
#define POINTER_CAL_SLACK       2       /* px two travels may differ */
#define POINTER_CAL_MIN_PX      4       /* px the step test must travel */
#define POINTER_CAL_MAX_MOVES   150     /* moves of the gain test at most */
#define POINTER_DRIFT_SLACK     1.0     /* px of drift that homing must save */
#define POINTER_GAIN_MIN        0.05    /* px per count we believe */
#define POINTER_GAIN_MAX        20.0
#define POINTER_GAIN_FIRST      0.25    /* the first homing covers this */
#define POINTER_SHAPES          16      /* cursor images it keeps hotspots of */
#define POINTER_PROBES          4       /* closed loop: steps sent again */
#define POINTER_HOT_HOMINGS     2       /* closed loop: homings in one move */
#define POINTER_GLIDE_TICK_MS   16      /* natural: time between moves */
#define POINTER_GLIDE_MIN_MS    150     /* natural: the shortest move */
#define POINTER_GLIDE_MAX_MS    2500    /* natural: the longest, by default */
#define POINTER_GLIDE_BOW       40      /* natural: px it bends at most */

/* Closed loop: the hotspot of a cursor image, from its top-left corner */
typedef struct PointerHot {
    uint32_t shape;
    int x, y;
} PointerHot;

typedef struct QemuPointer {
    QemuConsole *con;
    bool known;
    int x, y;
    int64_t sync_ms;
    int fb_w, fb_h;
    uint64_t foreign;
    QemuPointerMethod method;
    bool unknown_told;          /* qemu_pointer_unknown_once() said so */
    /*
     * Closed loop: the hotspot of each cursor image seen.  A guest
     * may load another image, with its hotspot elsewhere, for each
     * window.
     */
    PointerHot hot[POINTER_SHAPES];
    int n_hot;
    int next_hot;               /* the one to replace when all are used */
    /* open loop: the biggest step the guest takes as it is; 0: unknown */
    bool cal_done;
    int cal_step;
    double gain_x, gain_y;      /* px one count moves; 0: unknown */
    double err_x, err_y;        /* how far off the gain may be, as a part */
    double drift;               /* px the position may be off, from that */
    int cal_w, cal_h;           /* the screen size it was measured at */
    /*
     * closed loop: px a count moved, across and down, in steps whose
     * bigger axis had n counts; 0: not seen.  A guest that speeds up
     * big steps moves them further.
     */
    float step_gain[2][POINTER_LOOP_CAP + 1];
    QLIST_ENTRY(QemuPointer) node;
} QemuPointer;

static QLIST_HEAD(, QemuPointer) pointers = QLIST_HEAD_INITIALIZER(pointers);

static void pointer_reset(void *opaque)
{
    QemuPointer *p;

    QLIST_FOREACH(p, &pointers, node) {
        p->known = false;
        p->n_hot = 0;
        p->cal_done = false;    /* another guest may boot */
        memset(p->step_gain, 0, sizeof(p->step_gain));
        p->unknown_told = false;
    }
}

static QemuPointer *pointer_find(QemuConsole *con, bool create)
{
    QemuPointer *p;

    QLIST_FOREACH(p, &pointers, node) {
        if (p->con == con) {
            return p;
        }
    }
    if (!create) {
        return NULL;
    }
    if (QLIST_EMPTY(&pointers)) {
        qemu_register_reset(pointer_reset, NULL);
    }
    p = g_new0(QemuPointer, 1);
    p->con = con;
    QLIST_INSERT_HEAD(&pointers, p, node);
    return p;
}

static bool pointer_valid(QemuPointer *p, int w, int h)
{
    return p->known && p->fb_w == w && p->fb_h == h &&
           p->foreign == qemu_input_foreign_mouse_count();
}

static void pointer_set(QemuPointer *p, int x, int y, int w, int h,
                        QemuPointerMethod method)
{
    p->known = true;
    p->unknown_told = false;
    p->drift = 0;
    p->x = x;
    p->y = y;
    p->fb_w = w;
    p->fb_h = h;
    p->method = method;
    p->sync_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    p->foreign = qemu_input_foreign_mouse_count();
}

void qemu_pointer_opts_init(QemuPointerOpts *o)
{
    o->method = QEMU_POINTER_AUTO;
    o->home = QEMU_POINTER_HOME_AUTO;
    o->step_px = 4;
    o->step_auto = true;
    o->step_delay_ms = 20;
    o->natural = false;
    o->duration_ms = 0;
}

/* Natural motion: how long a person takes to move @d px */
static int pointer_glide_ms(double d, const QemuPointerOpts *o)
{
    if (o->duration_ms > 0) {
        return o->duration_ms;
    }
    return MAX(POINTER_GLIDE_MIN_MS,
               MIN(POINTER_GLIDE_MAX_MS, (int)(250 + 120 * log2(1 + d / 24))));
}

/*
 * Natural motion: where the hand is at @t (0 to 1) of its way from
 * @x0, @y0 to @x1, @y1, on a @w x @h screen.  Minimum jerk: it starts
 * and stops gently, fastest halfway.  Like a hand turning at the
 * wrist, it bends upward, by 6% of the distance at most.
 */
static void pointer_glide_at(int x0, int y0, int x1, int y1, double t,
                             int w, int h, int *x, int *y)
{
    double s = t * t * t * (10 - t * (15 - 6 * t));
    double dx = x1 - x0, dy = y1 - y0, d = hypot(dx, dy);
    double bow = d > 0 ? MIN(0.06 * d, POINTER_GLIDE_BOW) *
                         4 * s * (1 - s) / d : 0;

    if (dx < 0) {
        bow = -bow;             /* (dy, -dx) * bow points up */
    }

    *x = MAX(0, MIN(w - 1, (int)lround(x0 + dx * s + dy * bow)));
    *y = MAX(0, MIN(h - 1, (int)lround(y0 + dy * s - dx * bow)));
}

const char *qemu_pointer_method_str(QemuPointerMethod m)
{
    switch (m) {
    case QEMU_POINTER_ABSOLUTE:
        return "absolute";
    case QEMU_POINTER_CLOSED_LOOP:
        return "closed-loop";
    case QEMU_POINTER_OPEN_LOOP:
        return "open-loop";
    default:
        return "auto";
    }
}

bool qemu_pointer_get(QemuConsole *con, int *x, int *y,
                      QemuPointerMethod *method)
{
    QemuPointer *p = pointer_find(con, false);

    if (!p || !pointer_valid(p, qemu_console_get_width(con, 0),
                             qemu_console_get_height(con, 0))) {
        return false;
    }
    *x = p->x;
    *y = p->y;
    *method = p->method;
    return true;
}

bool qemu_pointer_unknown_once(QemuConsole *con)
{
    QemuPointer *p = pointer_find(con, true);

    if (pointer_valid(p, qemu_console_get_width(con, 0),
                      qemu_console_get_height(con, 0))) {
        p->unknown_told = false;    /* tell again once it is lost */
        return false;
    }
    if (p->unknown_told) {
        return false;
    }
    p->unknown_told = true;
    return true;
}

void qemu_pointer_forget(QemuConsole *con)
{
    QemuPointer *p = pointer_find(con, false);

    if (p) {
        p->known = false;
        p->n_hot = 0;
    }
}

/* The hotspot of the cursor image @shape, if it was measured */
static bool pointer_hot_get(QemuPointer *p, uint32_t shape, PointerHot *hot)
{
    int i;

    for (i = 0; i < p->n_hot; i++) {
        if (p->hot[i].shape == shape) {
            *hot = p->hot[i];
            return true;
        }
    }
    return false;
}

static void pointer_hot_set(QemuPointer *p, const PointerHot *hot,
                            const char *how)
{
    int i;

    for (i = 0; i < p->n_hot; i++) {
        if (p->hot[i].shape == hot->shape) {
            break;
        }
    }
    if (i == p->n_hot) {
        if (p->n_hot < POINTER_SHAPES) {
            p->n_hot++;
        } else {
            i = p->next_hot;
            p->next_hot = (p->next_hot + 1) % POINTER_SHAPES;
        }
    }
    p->hot[i] = *hot;
    trace_pointer_hotspot(qemu_console_get_index(p->con), hot->shape,
                          hot->x, hot->y, how);
}

static int clamp_step(int d, int cap)
{
    return MAX(-cap, MIN(cap, d));
}

/* Push toward a corner, twice the screen for a guest moving @gain px/count */
static void pointer_add_homing(QemuInputBatch *b, int w, int h,
                               int dir, double gain, int delay_ms)
{
    int n = DIV_ROUND_UP((int)(2 * MAX(w, h) / MAX(gain, POINTER_GAIN_MIN)),
                         POINTER_HOME_PACKET);
    int i;

    for (i = 0; i < n; i++) {
        qemu_input_batch_move(b, INPUT_EVENT_KIND_REL,
                              dir * POINTER_HOME_PACKET,
                              dir * POINTER_HOME_PACKET);
        qemu_input_batch_delay(b, delay_ms);
    }
}

static void pointer_add_abs(QemuInputBatch *b, int x, int y, int w, int h)
{
    qemu_input_batch_move(b, INPUT_EVENT_KIND_ABS,
                          qemu_input_scale_axis(x, 0, MAX(w - 1, 1),
                                                INPUT_EVENT_ABS_MIN,
                                                INPUT_EVENT_ABS_MAX),
                          qemu_input_scale_axis(y, 0, MAX(h - 1, 1),
                                                INPUT_EVENT_ABS_MIN,
                                                INPUT_EVENT_ABS_MAX));
}

static bool coroutine_fn pointer_co_absolute(QemuPointer *p,
                                             QemuInputCtx *ctx, int x, int y,
                                             int w, int h,
                                             const QemuPointerOpts *o,
                                             QemuPointerResult *res,
                                             Error **errp)
{
    QemuInputBatch *b = qemu_input_batch_new(p->con);

    /* natural: glide from where it is; from nowhere known, it jumps */
    if (o->natural && pointer_valid(p, w, h) && (p->x != x || p->y != y)) {
        int x0 = p->x, y0 = p->y, i, n, gx, gy;

        n = DIV_ROUND_UP(pointer_glide_ms(hypot(x - x0, y - y0), o),
                         POINTER_GLIDE_TICK_MS);
        for (i = 1; i < n; i++) {
            pointer_glide_at(x0, y0, x, y, (double)i / n, w, h, &gx, &gy);
            pointer_add_abs(b, gx, gy, w, h);
            qemu_input_batch_delay(b, POINTER_GLIDE_TICK_MS);
            res->steps++;
        }
    }
    pointer_add_abs(b, x, y, w, h);
    if (!qemu_input_co_run(ctx, b, errp)) {
        p->known = false;
        return false;
    }
    pointer_set(p, x, y, w, h, QEMU_POINTER_ABSOLUTE);
    res->method = QEMU_POINTER_ABSOLUTE;
    res->steps++;
    return true;
}

#define POINTER_WIGGLE          12      /* counts for wiggle-and-diff */
#define POINTER_WIGGLE_MAX_BOX  128     /* a bigger change is not the pointer */
#define POINTER_CELL            8       /* px per cell, to group changes */
#define POINTER_CELL_GAP        2       /* cells between changes of a group */
#define POINTER_SHIFT_MAX       48      /* px the wiggle may move the pointer */
#define POINTER_MOVED_MIN       4       /* pixels that must move with it */

/* Two screens, before and after the wiggle */
typedef struct PointerDiff {
    int w, h, sa, sb;
    uint32_t *da, *db;
} PointerDiff;

static inline uint32_t pointer_old(const PointerDiff *d, int x, int y)
{
    return d->da[y * d->sa + x] & 0xffffff;
}

static inline uint32_t pointer_new(const PointerDiff *d, int x, int y)
{
    return d->db[y * d->sb + x] & 0xffffff;
}

/*
 * How many changed pixels in @x0,@y0-@x1,@y1 show up in the new screen
 * moved down and right by one amount, as the pointer does when the
 * mouse moves that way.  A pixel counts only if it differs from the old
 * one where it lands, so background landing on background, as when a
 * text cursor blinks or a digit of a clock changes, mostly does not.
 */
static int pointer_moved_score(const PointerDiff *d, int x0, int y0,
                               int x1, int y1)
{
    int best = 0, dx, dy, x, y;

    for (dy = 0; dy <= POINTER_SHIFT_MAX; dy++) {
        for (dx = !dy; dx <= POINTER_SHIFT_MAX; dx++) {
            int n = 0;

            for (y = y0; y <= y1 && y + dy < d->h; y++) {
                for (x = x0; x <= x1 && x + dx < d->w; x++) {
                    uint32_t old = pointer_old(d, x, y);

                    n += old != pointer_new(d, x, y) &&
                         old == pointer_new(d, x + dx, y + dy) &&
                         old != pointer_old(d, x + dx, y + dy);
                }
            }
            best = MAX(best, n);
        }
    }
    return best;
}

/*
 * Find the pointer in what changed between @a and @b while the mouse
 * moved down and right.  Changes near each other make a group, and the
 * pointer's group is the one whose pixels moved with the mouse, so a
 * blinking text cursor elsewhere is not taken for part of it.  True
 * with the top-left corner of the group; else @why says what was seen.
 */
static bool pointer_find_moved(pixman_image_t *a, pixman_image_t *b,
                               int *px, int *py, char *why, size_t why_size)
{
    PointerDiff d = {
        .w = pixman_image_get_width(a),
        .h = pixman_image_get_height(a),
        .sa = pixman_image_get_stride(a) / 4,
        .sb = pixman_image_get_stride(b) / 4,
        .da = pixman_image_get_data(a),
        .db = pixman_image_get_data(b),
    };
    int gw = DIV_ROUND_UP(d.w, POINTER_CELL);
    int gh = DIV_ROUND_UP(d.h, POINTER_CELL);
    g_autofree int *cell = NULL;        /* 0: same, -1: changed, n: group */
    g_autofree int *queue = NULL;
    int groups = 0, big = 0, best = -1, x, y, i;

    if (d.w != pixman_image_get_width(b) || d.h != pixman_image_get_height(b)) {
        g_strlcpy(why, "the screen size changed while the mouse moved",
                  why_size);
        return false;
    }
    cell = g_new0(int, gw * gh);
    queue = g_new(int, gw * gh);
    for (y = 0; y < d.h; y++) {
        for (x = 0; x < d.w; x++) {
            if (pointer_old(&d, x, y) != pointer_new(&d, x, y)) {
                cell[(y / POINTER_CELL) * gw + x / POINTER_CELL] = -1;
            }
        }
    }
    for (i = 0; i < gw * gh; i++) {
        int head = 0, tail = 0, cx0, cy0, cx1, cy1, score;
        int x0 = d.w, y0 = d.h, x1 = -1, y1 = -1;

        if (cell[i] != -1) {
            continue;
        }
        cell[i] = ++groups;
        queue[tail++] = i;
        cx0 = cx1 = i % gw;
        cy0 = cy1 = i / gw;
        while (head < tail) {
            int c = queue[head++], nx, ny;

            for (ny = MAX(0, c / gw - POINTER_CELL_GAP);
                 ny <= MIN(gh - 1, c / gw + POINTER_CELL_GAP); ny++) {
                for (nx = MAX(0, c % gw - POINTER_CELL_GAP);
                     nx <= MIN(gw - 1, c % gw + POINTER_CELL_GAP); nx++) {
                    if (cell[ny * gw + nx] == -1) {
                        cell[ny * gw + nx] = groups;
                        queue[tail++] = ny * gw + nx;
                        cx0 = MIN(cx0, nx);
                        cx1 = MAX(cx1, nx);
                        cy0 = MIN(cy0, ny);
                        cy1 = MAX(cy1, ny);
                    }
                }
            }
        }
        /* the box of the group's changed pixels */
        for (y = cy0 * POINTER_CELL;
             y < MIN(d.h, (cy1 + 1) * POINTER_CELL); y++) {
            for (x = cx0 * POINTER_CELL;
                 x < MIN(d.w, (cx1 + 1) * POINTER_CELL); x++) {
                if (cell[(y / POINTER_CELL) * gw + x / POINTER_CELL] ==
                    groups && pointer_old(&d, x, y) != pointer_new(&d, x, y)) {
                    x0 = MIN(x0, x);
                    y0 = MIN(y0, y);
                    x1 = MAX(x1, x);
                    y1 = MAX(y1, y);
                }
            }
        }
        if (x1 - x0 >= POINTER_WIGGLE_MAX_BOX ||
            y1 - y0 >= POINTER_WIGGLE_MAX_BOX) {
            big++;
            continue;
        }
        score = pointer_moved_score(&d, x0, y0, x1, y1);
        if (score > best) {
            best = score;
            *px = x0;
            *py = y0;
        }
    }

    /* one small change that did not move as such may be a pointer too */
    if (best >= POINTER_MOVED_MIN || (best >= 0 && groups == 1)) {
        return true;
    }
    if (!groups) {
        g_strlcpy(why, "nothing on the screen moved with the mouse; the guest"
                  " may draw no pointer here", why_size);
    } else if (big) {
        g_strlcpy(why, "the screen changed too much while the mouse moved to"
                  " find the pointer; try again when it is still", why_size);
    } else {
        snprintf(why, why_size, "%d places on the screen changed while the"
                 " mouse moved, and none moved with it; the pointer may have"
                 " changed its shape there", groups);
    }
    return false;
}

static bool coroutine_fn pointer_co_nudge(QemuInputCtx *ctx, QemuConsole *con,
                                          int dx, int dy, Error **errp)
{
    QemuInputBatch *b = qemu_input_batch_new(con);

    qemu_input_batch_move(b, INPUT_EVENT_KIND_REL, dx, dy);
    qemu_input_batch_delay(b, POINTER_SETTLE_MS);
    return qemu_input_co_run(ctx, b, errp);
}

/*
 * Find the pointer without the card's cursor: from the display's cursor
 * if it reports one, else nudge the mouse and look at what moved; the
 * old pointer image's top-left corner is the top-left of the change.
 */
static bool coroutine_fn pointer_co_find(QemuConsole *con, QemuInputCtx *ctx,
                                         int *x, int *y, const char **how,
                                         Error **errp)
{
    char why[160];
    bool on;
    int i;

    if (qemu_frames_get_hw_cursor(qemu_console_frames(con), x, y, &on) &&
        on) {
        *how = "display cursor";
        return true;
    }
    /* look twice, in case something else changed just then */
    for (i = 0; i < 2; i++) {
        g_autoptr(pixman_image_t) before = NULL;
        g_autoptr(pixman_image_t) after = NULL;

        before = qemu_console_co_capture(con, NULL, 1.0, errp);
        if (!before ||
            !pointer_co_nudge(ctx, con, POINTER_WIGGLE, POINTER_WIGGLE, errp)) {
            return false;
        }
        after = qemu_console_co_capture(con, NULL, 1.0, errp);
        if (!after ||
            !pointer_co_nudge(ctx, con, -POINTER_WIGGLE, -POINTER_WIGGLE,
                              errp)) {
            return false;
        }
        if (pointer_find_moved(before, after, x, y, why, sizeof(why))) {
            *how = "screen diff, pointer image corner (approximate)";
            return true;
        }
    }
    error_setg(errp, "%s", why);
    return false;
}

static bool pointer_cal_valid(QemuPointer *p, int w, int h)
{
    return p->cal_done && p->cal_w == w && p->cal_h == h;
}

/* px per count on one axis: as measured, else 1 */
static double pointer_gain(QemuPointer *p, int w, int h, bool vertical)
{
    double g = vertical ? p->gain_y : p->gain_x;

    return pointer_cal_valid(p, w, h) && g > 0 ? g : 1.0;
}

/* Step @n counts down and right, @step at a time; where did it land? */
static bool coroutine_fn pointer_co_cal_move(QemuPointer *p,
                                             QemuInputCtx *ctx, int n,
                                             int step,
                                             const QemuPointerOpts *o,
                                             int *x, int *y, Error **errp)
{
    QemuInputBatch *b = qemu_input_batch_new(p->con);
    const char *how;
    int i;

    for (i = 0; i < n / step; i++) {
        qemu_input_batch_move(b, INPUT_EVENT_KIND_REL, step, step);
        qemu_input_batch_delay(b, o->step_delay_ms);
    }
    qemu_input_batch_delay(b, POINTER_SETTLE_MS);
    return qemu_input_co_run(ctx, b, errp) &&
           pointer_co_find(p->con, ctx, x, y, &how, errp);
}

/*
 * Did @px over @n counts go about as far per count as @d1 px over
 * POINTER_CAL_DIST?  If not, the pointer found was not the pointer, or
 * the guest clamped it at an edge.
 */
static bool pointer_cal_agrees(int px, int n, int d1)
{
    double g = (double)px / n, g1 = (double)d1 / POINTER_CAL_DIST;

    return fabs(g - g1) <= g1 * (3.0 / d1 + 0.05);
}

/*
 * @px over @n counts, as a simple fraction such as 1/2 or 3 if one is
 * as close as the landings can tell (a pixel each).  @err is how far
 * off it may be, as a part of it: none for a simple fraction.
 */
static double pointer_cal_gain(int px, int n, double *err)
{
    double g = (double)px / n;
    int q;

    *err = 2.0 / MAX(px, 1);
    for (q = 1; q <= 4; q++) {
        double f = (double)lround(g * q) / q;

        if (f > 0 && fabs(f - g) <= *err * g) {
            *err = 0;
            g = f;
            break;
        }
    }
    return MAX(POINTER_GAIN_MIN, MIN(POINTER_GAIN_MAX, g));
}

/*
 * Measure how the guest moves the pointer: the biggest step that
 * travels as far as steps of one count (so the guest does not speed it
 * up), and how many pixels a count moves.  From the corner it steps down
 * and right, finding the pointer after each part.  Travels are
 * differences of landings found the same way, so the hotspot's place in
 * the pointer image cancels out.  If the pointer cannot be found, the
 * default step stays, a count is taken as a pixel, and @why says what
 * went wrong.  False only if cancelled.
 */
static bool coroutine_fn pointer_co_calibrate(QemuPointer *p,
                                              QemuInputCtx *ctx,
                                              int w, int h,
                                              const QemuPointerOpts *o,
                                              char *why, size_t why_size,
                                              Error **errp)
{
    static const int steps[] = { 4, 2 };
    QemuInputBatch *b = qemu_input_batch_new(p->con);
    Error *local_err = NULL;
    int ax, ay, bx, by, x, y, d1x, d1y, i, n, step = 1;
    int skip_x = 0, skip_y = 0, skip_n = 0; /* travels of steps sped up */
    int since_a = POINTER_CAL_DIST;         /* counts from A to B */

    p->cal_done = true;
    p->cal_w = w;
    p->cal_h = h;
    p->cal_step = 0;
    p->gain_x = p->gain_y = 0;
    p->err_x = p->err_y = 0;

    /* A lands POINTER_CAL_DIST counts from the corner, B as far again */
    pointer_add_homing(b, w, h, -1, POINTER_GAIN_FIRST, o->step_delay_ms);
    if (!qemu_input_co_run(ctx, b, &local_err) ||
        !pointer_co_cal_move(p, ctx, POINTER_CAL_DIST, 1, o, &ax, &ay,
                             &local_err) ||
        !pointer_co_cal_move(p, ctx, POINTER_CAL_DIST, 1, o, &bx, &by,
                             &local_err)) {
        goto out;
    }
    d1x = bx - ax;
    d1y = by - ay;
    if (d1x < POINTER_CAL_MIN_PX || d1y < POINTER_CAL_MIN_PX) {
        error_setg(&local_err, "%d counts moved the pointer only %d,%d px",
                   POINTER_CAL_DIST, d1x, d1y);
        goto out;
    }

    for (i = 0; i < ARRAY_SIZE(steps); i++) {
        bool same;

        if (steps[i] > MAX(1, o->step_px)) {
            continue;
        }
        if (!pointer_co_cal_move(p, ctx, POINTER_CAL_DIST, steps[i], o,
                                 &x, &y, &local_err)) {
            goto out;
        }
        same = abs(x - bx - d1x) <= POINTER_CAL_SLACK &&
               abs(y - by - d1y) <= POINTER_CAL_SLACK;
        if (!same) {
            skip_x += x - bx;
            skip_y += y - by;
            skip_n += POINTER_CAL_DIST;
        }
        since_a += POINTER_CAL_DIST;
        bx = x;
        by = y;
        if (same) {
            step = steps[i];
            break;
        }
    }
    p->cal_step = step;

    /*
     * Go on to 3/4 of the smaller side for a longer travel, so the gain
     * is good to a fraction of a percent; at most POINTER_CAL_MAX_MOVES
     * moves, so a slow guest does not take long.
     */
    n = (int)MIN((w * 0.75 - bx) * POINTER_CAL_DIST / d1x,
                 (h * 0.75 - by) * POINTER_CAL_DIST / d1y);
    n = MIN(n, POINTER_CAL_MAX_MOVES * step) / step * step;
    if (n >= POINTER_CAL_DIST) {
        if (pointer_co_cal_move(p, ctx, n, step, o, &x, &y, &local_err) &&
            pointer_cal_agrees(x - bx, n, d1x) &&
            pointer_cal_agrees(y - by, n, d1y)) {
            since_a += n;
            bx = x;
            by = y;
        } else if (ctx->cancelled) {
            goto out;
        } else {
            g_clear_pointer(&local_err, error_free); /* the shorter one */
        }
    }
    p->gain_x = pointer_cal_gain(bx - ax - skip_x, since_a - skip_n,
                                 &p->err_x);
    p->gain_y = pointer_cal_gain(by - ay - skip_y, since_a - skip_n,
                                 &p->err_y);

out:
    trace_pointer_calibrate(qemu_console_get_index(p->con), p->cal_step,
                            (int)lround(p->gain_x * 1000),
                            (int)lround(p->gain_y * 1000),
                            local_err ? error_get_pretty(local_err) : "");
    if (local_err && ctx->cancelled) {
        error_propagate(errp, local_err);
        return false;
    }
    if (local_err) {
        p->cal_step = 0;
        g_strlcpy(why, error_get_pretty(local_err), why_size);
    }
    error_free(local_err);
    return true;
}

/*
 * Natural motion with a relative mouse: @nx, @ny counts that take the
 * pointer from @fx, @fy to @x, @y, sent along the hand's path, one move
 * a tick.  A move holds @step counts at most, so the guest does not
 * speed it up; what that holds back goes with the next ones.  Returns
 * the number of moves.
 */
static int pointer_add_glide_rel(QemuInputBatch *b, int fx, int fy,
                                 int x, int y, int nx, int ny,
                                 double gx, double gy, int step,
                                 int w, int h, const QemuPointerOpts *o)
{
    int tick = o->step_delay_ms > 0 ? o->step_delay_ms
                                     : POINTER_GLIDE_TICK_MS;
    /*
     * The hand is fastest halfway, at 1.875 times its mean speed; a bit
     * longer than the steps need lets it keep close to its path there
     */
    int steps_ms = DIV_ROUND_UP(MAX(abs(nx), abs(ny)), step) * tick;
    int ms = MAX(pointer_glide_ms(hypot(x - fx, y - fy), o),
                 steps_ms * 5 / 4);
    int n = MAX(1, DIV_ROUND_UP(ms, tick));
    int sx = 0, sy = 0, moves = 0, i;

    for (i = 1; i <= n || sx != nx || sy != ny; i++) {
        int cx = nx, cy = ny;

        if (i < n) {
            int px, py;

            pointer_glide_at(fx, fy, x, y, (double)i / n, w, h, &px, &py);
            cx = (int)lround((px - fx) / gx);
            cy = (int)lround((py - fy) / gy);
        }
        cx = clamp_step(cx - sx, step);
        cy = clamp_step(cy - sy, step);
        if (cx || cy) {
            qemu_input_batch_move(b, INPUT_EVENT_KIND_REL, cx, cy);
            moves++;
        }
        qemu_input_batch_delay(b, tick);
        sx += cx;
        sy += cy;
    }
    return moves;
}

/* Step from the model's position (homing first if needed) to @x, @y */
static bool coroutine_fn pointer_co_open_loop(QemuPointer *p,
                                              QemuInputCtx *ctx,
                                              int x, int y, int w, int h,
                                              const QemuPointerOpts *o,
                                              QemuPointerResult *res,
                                              Error **errp)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    bool valid = pointer_valid(p, w, h);
    int step = MAX(1, o->step_px);
    QemuInputBatch *b;
    double gx, gy, ex, ey, drift;
    int fx, fy, nx, ny;

    switch (o->home) {
    case QEMU_POINTER_HOME_ALWAYS:
        res->homed = true;
        break;
    case QEMU_POINTER_HOME_NEVER:
        if (!valid) {
            error_setg(errp, "pointer position is unknown; move with home "
                       "set to auto or always");
            return false;
        }
        break;
    default:
        /*
         * A natural move trusts an old position: homing would show on
         * the video, and a recording that freezes the guest between
         * tool calls makes every position old
         */
        res->homed = !valid ||
                     (!o->natural && now - p->sync_ms > POINTER_STALE_MS);
        break;
    }

    if (o->step_auto && o->home != QEMU_POINTER_HOME_NEVER &&
        !pointer_cal_valid(p, w, h)) {
        if (!pointer_co_calibrate(p, ctx, w, h, o, res->cal_error,
                                  sizeof(res->cal_error), errp)) {
            p->known = false;
            return false;
        }
        res->calibrated = true;
        res->homed = true;      /* it left the pointer somewhere else */
    }
    if (o->step_auto && pointer_cal_valid(p, w, h) && p->cal_step) {
        step = MIN(step, p->cal_step);
    }
    gx = pointer_gain(p, w, h, false);
    gy = pointer_gain(p, w, h, true);
    res->step_px = step;
    res->gain_x = gx;
    res->gain_y = gy;
    ex = pointer_cal_valid(p, w, h) ? p->err_x : 0;
    ey = pointer_cal_valid(p, w, h) ? p->err_y : 0;
    /* moves from moves add up the gain's error; from the corner they don't */
    drift = x * ex + y * ey;
    if (!res->homed) {
        double chained = p->drift + abs(x - p->x) * ex + abs(y - p->y) * ey;

        if (o->home == QEMU_POINTER_HOME_AUTO &&
            chained > drift + POINTER_DRIFT_SLACK) {
            res->homed = true;
        } else {
            drift = chained;
        }
    }

    b = qemu_input_batch_new(p->con);
    if (res->homed) {
        pointer_add_homing(b, w, h, -1, MIN(gx, gy), o->step_delay_ms);
        fx = fy = 0;
    } else {
        fx = p->x;
        fy = p->y;
    }
    /*
     * The guest moves the pointer gx, gy px per count, so it lands on
     * the nearest pixel it can reach
     */
    nx = (int)lround((x - fx) / gx);
    ny = (int)lround((y - fy) / gy);
    x = MAX(0, MIN(w - 1, (int)lround(fx + nx * gx)));
    y = MAX(0, MIN(h - 1, (int)lround(fy + ny * gy)));
    if (o->natural && (nx || ny)) {
        res->steps += pointer_add_glide_rel(b, fx, fy, x, y, nx, ny, gx, gy,
                                            step, w, h, o);
        nx = ny = 0;
    }
    while (nx || ny) {
        int sx = clamp_step(nx, step);
        int sy = clamp_step(ny, step);

        qemu_input_batch_move(b, INPUT_EVENT_KIND_REL, sx, sy);
        qemu_input_batch_delay(b, o->step_delay_ms);
        nx -= sx;
        ny -= sy;
        res->steps++;
    }
    if (!qemu_input_co_run(ctx, b, errp)) {
        p->known = false;
        return false;
    }
    pointer_set(p, x, y, w, h, QEMU_POINTER_OPEN_LOOP);
    p->drift = drift;
    res->method = QEMU_POINTER_OPEN_LOOP;
    return true;
}

static bool cursor_equal(const QemuCursorState *a, const QemuCursorState *b)
{
    return a->x == b->x && a->y == b->y && a->visible == b->visible &&
           a->shape == b->shape;
}

/*
 * Sleep until the card's cursor moves away from @old and settles: holds
 * still for one poll.  A new image, or a cursor shown or hidden, must
 * hold still longer, since the guest may load the image first and move
 * it later.  NT 3.51 on the G364 loads it, then waits for the next frame
 * (every 30 ms) before it moves it.
 */
static bool coroutine_fn pointer_co_await_cursor(QemuPointer *p,
                                                 QemuInputCtx *ctx,
                                                 const QemuCursorState *old,
                                                 QemuCursorState *cs)
{
    bool moved = false;
    int waited = 0, still = 0;

    for (;;) {
        QemuCursorState prev = *cs;
        bool image;

        qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME,
                                  POINTER_POLL_MS * SCALE_MS);
        if (ctx->cancelled) {
            return false;
        }
        waited += POINTER_POLL_MS;
        qemu_console_hw_cursor_state(p->con, cs);
        still = cursor_equal(&prev, cs) ? still + POINTER_POLL_MS : 0;
        image = cs->shape != old->shape || cs->visible != old->visible;
        if (moved &&
            still >= (image ? POINTER_IMAGE_HOLD_MS : POINTER_POLL_MS)) {
            break;          /* moved, then held still */
        }
        if (!cursor_equal(old, cs)) {
            moved = true;
        }
        if (waited >= (moved && image ? POINTER_IMAGE_SETTLE_MS :
                                        POINTER_SETTLE_MS)) {
            break;
        }
    }
    return moved;
}

/*
 * Natural motion: wait until @deadline_ms (one tick after the step) and
 * until the cursor has moved, so the steps come at an even pace and the
 * video shows the same number of them in every frame
 */
static bool coroutine_fn pointer_co_await_tick(QemuPointer *p,
                                               QemuInputCtx *ctx,
                                               const QemuCursorState *old,
                                               QemuCursorState *cs,
                                               int64_t deadline_ms)
{
    int64_t give_up = deadline_ms + POINTER_SETTLE_MS;

    for (;;) {
        int64_t now;

        qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME,
                                  2 * SCALE_MS);
        if (ctx->cancelled) {
            return false;
        }
        qemu_console_hw_cursor_state(p->con, cs);
        if (cs->shape != old->shape || cs->visible != old->visible) {
            /* a new image: let it settle as after any step */
            return pointer_co_await_cursor(p, ctx, old, cs);
        }
        now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        if (now >= deadline_ms && !cursor_equal(old, cs)) {
            return true;
        }
        if (now >= give_up) {
            return false;
        }
    }
}

/*
 * Push the pointer into the bottom-right corner until the card's cursor
 * stays put, so a guest that scales mouse motion down gets there too.
 * @cs is where the cursor ends.
 */
static bool coroutine_fn pointer_co_corner_cursor(QemuPointer *p,
                                                  QemuInputCtx *ctx,
                                                  int w, int h,
                                                  const QemuPointerOpts *o,
                                                  QemuCursorState *cs,
                                                  Error **errp)
{
    int i;

    qemu_console_hw_cursor_state(p->con, cs);
    for (i = 0; i < POINTER_HOME_ROUNDS; i++) {
        QemuInputBatch *b = qemu_input_batch_new(p->con);
        QemuCursorState old = *cs;

        pointer_add_homing(b, w, h, 1, 1.0, o->step_delay_ms);
        if (!qemu_input_co_run(ctx, b, errp)) {
            return false;
        }
        if (!pointer_co_await_cursor(p, ctx, &old, cs)) {
            break;
        }
    }
    if (ctx->cancelled) {
        error_setg(errp, "cancelled");
        return false;
    }
    return true;
}

/*
 * Learn the hotspot of the cursor image in the bottom-right corner:
 * guests clamp the hotspot to the last pixel, so there it shows how far
 * the hotspot is from the image's corner.
 */
static bool coroutine_fn pointer_co_home_hot(QemuPointer *p, QemuInputCtx *ctx,
                                             int w, int h,
                                             const QemuPointerOpts *o,
                                             QemuCursorState *cs,
                                             PointerHot *hot, Error **errp)
{
    if (!pointer_co_corner_cursor(p, ctx, w, h, o, cs, errp)) {
        return false;
    }
    hot->shape = cs->shape;
    hot->x = (w - 1) - cs->x;
    hot->y = (h - 1) - cs->y;
    if (hot->x < 0 || hot->x >= MAX(cs->w, 1) ||
        hot->y < 0 || hot->y >= MAX(cs->h, 1)) {
        hot->x = hot->y = 0;
    }
    pointer_hot_set(p, hot, "bottom-right corner");
    return true;
}

/*
 * Could the guest have stopped the pointer short at the screen's edge
 * on the step @sx, @sy that ended in the cursor image @cs?  The pointer
 * is somewhere in the image.
 */
static bool pointer_maybe_at_edge(const QemuCursorState *cs, int sx, int sy,
                                  int w, int h)
{
    return (sx < 0 && cs->x <= 0) ||
           (sx > 0 && cs->x + MAX(cs->w, 1) >= w) ||
           (sy < 0 && cs->y <= 0) ||
           (sy > 0 && cs->y + MAX(cs->h, 1) >= h);
}

/*
 * The step @sx, @sy took the pointer from where @old showed it, with
 * the hotspot @from, to where the guest shows another cursor image,
 * @cs, whose hotspot is not known: the guest loaded that image for the
 * window the pointer went into.  The image moved by the step plus the
 * difference of the two hotspots, so the hotspot is known once it is
 * known how far the step alone went.  Send the step again until the
 * image is the same twice in a row, which shows that, or a known one;
 * then each image on the way gets its hotspot.  Near the screen's
 * edges, where a step may go less far, it learns nothing.  Returns 1
 * with @hot set if the image at the end has a known hotspot, 0 if not,
 * -1 on error.
 */
static int coroutine_fn pointer_co_learn_hot(QemuPointer *p, QemuInputCtx *ctx,
                                             const QemuCursorState *old,
                                             const PointerHot *from,
                                             int sx, int sy, int w, int h,
                                             QemuCursorState *cs,
                                             PointerHot *hot,
                                             QemuPointerResult *res,
                                             Error **errp)
{
    QemuCursorState on[POINTER_PROBES + 1];     /* the image after each step */
    int x0 = old->x + from->x, y0 = old->y + from->y;
    int n = 1, dx = 0, dy = 0, i;
    bool learn = false;
    PointerHot k;

    on[0] = *cs;
    for (;;) {
        QemuInputBatch *b;

        if (pointer_maybe_at_edge(cs, sx, sy, w, h)) {
            break;
        }
        if (n > 1 && cs->shape == on[n - 2].shape) {
            /* the same image twice: it went as far as the pointer */
            dx = cs->x - on[n - 2].x;
            dy = cs->y - on[n - 2].y;
            learn = true;
            break;
        }
        if (n > 1 && pointer_hot_get(p, cs->shape, &k)) {
            /* a known image: @n equal steps took the pointer here */
            dx = cs->x + k.x - x0;
            dy = cs->y + k.y - y0;
            learn = !(dx % n) && !(dy % n);
            dx /= n;
            dy /= n;
            break;
        }
        if (n > POINTER_PROBES) {
            break;
        }
        b = qemu_input_batch_new(p->con);
        qemu_input_batch_move(b, INPUT_EVENT_KIND_REL, sx, sy);
        if (!qemu_input_co_run(ctx, b, errp)) {
            return -1;
        }
        res->steps++;
        if (!pointer_co_await_cursor(p, ctx, &on[n - 1], cs)) {
            if (ctx->cancelled) {
                error_setg(errp, "cancelled");
                return -1;
            }
            return 0;
        }
        trace_pointer_cursor(qemu_console_get_index(p->con), sx, sy,
                             cs->x, cs->y, cs->visible, cs->shape);
        if (!cs->visible) {
            return 0;
        }
        on[n++] = *cs;
    }

    /* the pointer went @dx, @dy with each step: measure each new image */
    for (i = 0; learn && i < n; i++) {
        PointerHot m = {
            .shape = on[i].shape,
            .x = x0 + (i + 1) * dx - on[i].x,
            .y = y0 + (i + 1) * dy - on[i].y,
        };

        if (pointer_hot_get(p, m.shape, &k)) {
            continue;
        }
        if (m.x < 0 || m.x >= MAX(on[i].w, 1) ||
            m.y < 0 || m.y >= MAX(on[i].h, 1)) {
            trace_pointer_hotspot(qemu_console_get_index(p->con), m.shape,
                                  m.x, m.y, "outside the image: dropped");
            continue;
        }
        pointer_hot_set(p, &m, "step sent again");
    }
    return pointer_hot_get(p, cs->shape, hot) ? 1 : 0;
}

/* Closed loop: px a count moves on @axis in steps of @m counts, if seen */
static double pointer_step_gain_seen(QemuPointer *p, int axis, int m)
{
    int i;

    /* the nearest size seen, the smaller one first */
    for (i = 0; i < POINTER_LOOP_CAP; i++) {
        if (m - i >= 1 && p->step_gain[axis][m - i] > 0) {
            return p->step_gain[axis][m - i];
        }
        if (m + i <= POINTER_LOOP_CAP && p->step_gain[axis][m + i] > 0) {
            return p->step_gain[axis][m + i];
        }
    }
    return 0;
}

static double pointer_step_gain(QemuPointer *p, int axis, int m)
{
    double g = pointer_step_gain_seen(p, axis, m);

    if (!g) {
        g = pointer_step_gain_seen(p, !axis, m);
    }
    return g ? g : 1.0;
}

/*
 * Closed loop, natural motion: the step that moves the pointer nearest
 * to @dx, @dy px away, from how far steps of each size moved it
 */
static void pointer_plan_step(QemuPointer *p, int dx, int dy,
                              int *sx, int *sy)
{
    int major = abs(dy) > abs(dx);
    int e = major ? abs(dy) : abs(dx);
    double best_err = e;
    int best = 0, m, minor;

    for (m = 1; m <= POINTER_LOOP_CAP; m++) {
        double err = fabs(m * pointer_step_gain(p, major, m) - e);

        if (err < best_err) {
            best_err = err;
            best = m;
        }
    }
    minor = clamp_step((int)lround((major ? dx : dy) /
                                   pointer_step_gain(p, !major, best)),
                       best);
    *sx = major ? minor : (dx > 0 ? best : -best);
    *sy = major ? (dy > 0 ? best : -best) : minor;
    if (!best) {
        *sx = *sy = 0;
    }
}

/*
 * Closed loop: remember that the step @sx, @sy moved the cursor by
 * @mx, @my px; @edge_x, @edge_y: it ended at the edge, which may have
 * stopped it
 */
static void pointer_learn_step(QemuPointer *p, int sx, int sy, int mx,
                               int my, bool edge_x, bool edge_y)
{
    int m = MAX(abs(sx), abs(sy));
    int n[2] = { sx, sy }, moved[2] = { mx, my };
    bool edge[2] = { edge_x, edge_y };
    int a;

    if (m < 1 || m > POINTER_LOOP_CAP) {
        return;
    }
    for (a = 0; a < 2; a++) {
        double g = n[a] ? (double)moved[a] / n[a] : 0;

        /* a count or two moves the pointer too little to tell */
        if (edge[a] || abs(n[a]) < MIN(2, m) || g < POINTER_GAIN_MIN ||
            g > POINTER_GAIN_MAX) {
            continue;
        }
        p->step_gain[a][m] = g;
    }
}

static bool coroutine_fn pointer_co_closed_loop(QemuPointer *p,
                                                QemuInputCtx *ctx,
                                                int x, int y, int w, int h,
                                                const QemuPointerOpts *o,
                                                QemuPointerResult *res,
                                                Error **errp)
{
    int capx = POINTER_LOOP_CAP, capy = POINTER_LOOP_CAP;
    int stuck = 0, homings = 0, glide_ms = 0, x0 = 0, y0 = 0, limit;
    int overshot = 0;
    int64_t glide_start = 0;
    QemuCursorState cs;
    PointerHot hot;
    bool hot_known, plan = o->natural;
    int i;

    qemu_console_hw_cursor_state(p->con, &cs);
    hot_known = pointer_hot_get(p, cs.shape, &hot);
    if (!hot_known || o->home == QEMU_POINTER_HOME_ALWAYS) {
        if (!pointer_co_home_hot(p, ctx, w, h, o, &cs, &hot, errp)) {
            return false;
        }
        hot_known = true;
        homings++;
        res->homed = true;
    }
    if (o->natural) {
        /* steer toward a point that moves along the hand's path */
        x0 = cs.x + hot.x;
        y0 = cs.y + hot.y;
        glide_ms = pointer_glide_ms(hypot(x - x0, y - y0), o);
        glide_start = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    }
    limit = POINTER_LOOP_MAX + glide_ms / POINTER_GLIDE_TICK_MS;

    for (i = 0; i < limit; i++) {
        int tx = x, ty = y, dx, dy;
        QemuCursorState old = cs;
        PointerHot from = hot;
        QemuInputBatch *b;
        int sx, sy, ndx, ndy, r;
        int64_t tick_end;

        if (glide_ms) {
            int64_t t = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - glide_start;

            if (t < glide_ms) {
                pointer_glide_at(x0, y0, x, y, (double)t / glide_ms, w, h,
                                 &tx, &ty);
            } else {
                glide_ms = 0;       /* the rest as without it */
            }
        }
        dx = tx - (cs.x + hot.x);
        dy = ty - (cs.y + hot.y);
        if (!dx && !dy && (tx != x || ty != y)) {
            /* on the path: wait for the path to move on */
            qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME,
                                      POINTER_GLIDE_TICK_MS * SCALE_MS);
            if (ctx->cancelled) {
                error_setg(errp, "cancelled");
                p->known = false;
                return false;
            }
            continue;
        }
        if (!dx && !dy) {
            pointer_set(p, x, y, w, h, QEMU_POINTER_CLOSED_LOOP);
            res->method = QEMU_POINTER_CLOSED_LOOP;
            res->hot_guess = !hot_known;
            return true;
        }
        if (plan) {
            /*
             * natural: a step the guest moves as far as the path wants,
             * as far as it is known how the guest speeds steps up
             */
            pointer_plan_step(p, dx, dy, &sx, &sy);
            if (!sx && !sy) {
                if (glide_ms) {
                    /* closer than a count moves: let the path move on */
                    qemu_co_sleep_ns_wakeable(&ctx->sleep,
                                              QEMU_CLOCK_REALTIME,
                                              POINTER_GLIDE_TICK_MS *
                                              SCALE_MS);
                    if (ctx->cancelled) {
                        error_setg(errp, "cancelled");
                        p->known = false;
                        return false;
                    }
                    continue;
                }
                plan = false;
            }
        }
        if (!plan) {
            sx = clamp_step(dx, capx);
            sy = clamp_step(dy, capy);
        }
        b = qemu_input_batch_new(p->con);
        qemu_input_batch_move(b, INPUT_EVENT_KIND_REL, sx, sy);
        tick_end = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                   POINTER_GLIDE_TICK_MS;
        if (!qemu_input_co_run(ctx, b, errp)) {
            p->known = false;
            return false;
        }
        res->steps++;
        if (!(glide_ms ? pointer_co_await_tick(p, ctx, &old, &cs, tick_end)
                       : pointer_co_await_cursor(p, ctx, &old, &cs))) {
            if (ctx->cancelled) {
                error_setg(errp, "cancelled");
                return false;
            }
            if (++stuck >= 3) {
                break;
            }
            continue;
        }
        stuck = 0;
        trace_pointer_cursor(qemu_console_get_index(p->con), sx, sy,
                             cs.x, cs.y, cs.visible, cs.shape);
        if (cs.shape == old.shape && cs.visible && old.visible) {
            int hx = cs.x + hot.x, hy = cs.y + hot.y;

            pointer_learn_step(p, sx, sy, cs.x - old.x, cs.y - old.y,
                               hx <= 0 || hx >= w - 1,
                               hy <= 0 || hy >= h - 1);
        }

        if (cs.shape != old.shape) {
            /* another cursor image: is its hotspot known? */
            r = pointer_hot_get(p, cs.shape, &hot);
            if (!r && hot_known && old.visible && cs.visible) {
                r = pointer_co_learn_hot(p, ctx, &old, &from, sx, sy, w, h,
                                         &cs, &hot, res, errp);
            }
            if (r < 0) {
                p->known = false;
                return false;
            }
            if (r > 0) {
                hot_known = true;
            } else if (homings < POINTER_HOT_HOMINGS) {
                /* measure again, from the corner */
                if (!pointer_co_home_hot(p, ctx, w, h, o, &cs, &hot, errp)) {
                    return false;
                }
                hot_known = true;
                homings++;
                res->homed = true;
                capx = capy = POINTER_LOOP_CAP;
                continue;
            } else {
                /* go on with the hotspot of the old image: a guess */
                hot = from;
                hot.shape = cs.shape;
                hot_known = false;
            }
        }
        /* overshot: the guest accelerated the move, so move less */
        ndx = tx - (cs.x + hot.x);
        ndy = ty - (cs.y + hot.y);
        if ((ndx && (ndx > 0) != (dx > 0)) || (ndy && (ndy > 0) != (dy > 0))) {
            /* planned steps learn from it, but not for ever at the end */
            if (plan && !glide_ms && ++overshot >= 3) {
                plan = false;
            }
        }
        if (!plan && ndx && (ndx > 0) != (dx > 0)) {
            capx = MAX(1, capx / 2);
        }
        if (!plan && ndy && (ndy > 0) != (dy > 0)) {
            capy = MAX(1, capy / 2);
        }
    }

    /* the card's cursor stopped following: finish in open loop from here */
    pointer_set(p, cs.x + hot.x, cs.y + hot.y, w, h, QEMU_POINTER_OPEN_LOOP);
    res->hot_guess = !hot_known;
    {
        QemuPointerOpts oo = *o;

        oo.home = QEMU_POINTER_HOME_NEVER;
        return pointer_co_open_loop(p, ctx, x, y, w, h, &oo, res, errp);
    }
}

bool coroutine_fn qemu_pointer_co_move(QemuConsole *con, QemuInputCtx *ctx,
                                       int x, int y,
                                       const QemuPointerOpts *o,
                                       QemuPointerResult *res, Error **errp)
{
    QemuPointer *p = pointer_find(con, true);
    int w = qemu_console_get_width(con, 0);
    int h = qemu_console_get_height(con, 0);
    QemuPointerMethod m = o->method;
    QemuCursorState cs;
    bool ok;

    memset(res, 0, sizeof(*res));
    if (w <= 0 || h <= 0) {
        error_setg(errp, "the console has no screen");
        return false;
    }
    if (x < 0 || y < 0 || x >= w || y >= h) {
        error_setg(errp, "%d,%d is outside the %dx%d screen", x, y, w, h);
        return false;
    }

    if (m == QEMU_POINTER_AUTO) {
        if (qemu_input_is_absolute(con)) {
            m = QEMU_POINTER_ABSOLUTE;
        } else if (qemu_console_hw_cursor_state(con, &cs) && cs.visible) {
            m = QEMU_POINTER_CLOSED_LOOP;
        } else {
            m = QEMU_POINTER_OPEN_LOOP;
        }
    }

    switch (m) {
    case QEMU_POINTER_ABSOLUTE:
        if (!qemu_input_is_absolute(con)) {
            error_setg(errp, "the mouse of this console is relative");
            return false;
        }
        ok = pointer_co_absolute(p, ctx, x, y, w, h, o, res, errp);
        break;
    case QEMU_POINTER_CLOSED_LOOP:
        if (!qemu_console_hw_cursor_state(con, &cs) || !cs.visible) {
            error_setg(errp, "the display card shows no hardware cursor");
            return false;
        }
        ok = pointer_co_closed_loop(p, ctx, x, y, w, h, o, res, errp);
        break;
    default:
        ok = pointer_co_open_loop(p, ctx, x, y, w, h, o, res, errp);
        break;
    }

    res->known = p->known;
    res->x = p->x;
    res->y = p->y;
    trace_pointer_move(qemu_console_get_index(con), x, y,
                       qemu_pointer_method_str(res->method), res->homed,
                       res->steps, ok);
    return ok;
}

bool coroutine_fn qemu_pointer_co_move_by(QemuConsole *con, QemuInputCtx *ctx,
                                          int dx, int dy,
                                          const QemuPointerOpts *o,
                                          QemuPointerResult *res,
                                          Error **errp)
{
    QemuPointer *p = pointer_find(con, true);
    int w = qemu_console_get_width(con, 0);
    int h = qemu_console_get_height(con, 0);
    int step = MAX(1, o->step_px);
    QemuInputBatch *b;
    int nx, ny;

    if (pointer_valid(p, w, h)) {
        QemuPointerOpts oo = *o;

        oo.home = QEMU_POINTER_HOME_NEVER;
        return qemu_pointer_co_move(con, ctx,
                                    MAX(0, MIN(w - 1, p->x + dx)),
                                    MAX(0, MIN(h - 1, p->y + dy)),
                                    &oo, res, errp);
    }

    /* position unknown: just send the steps */
    memset(res, 0, sizeof(*res));
    if (o->step_auto && pointer_cal_valid(p, w, h) && p->cal_step) {
        step = MIN(step, p->cal_step);
    }
    res->step_px = step;
    res->gain_x = pointer_gain(p, w, h, false);
    res->gain_y = pointer_gain(p, w, h, true);
    nx = (int)lround(dx / res->gain_x);
    ny = (int)lround(dy / res->gain_y);
    b = qemu_input_batch_new(con);
    while (nx || ny) {
        int sx = clamp_step(nx, step);
        int sy = clamp_step(ny, step);

        qemu_input_batch_move(b, INPUT_EVENT_KIND_REL, sx, sy);
        qemu_input_batch_delay(b, o->step_delay_ms);
        nx -= sx;
        ny -= sy;
        res->steps++;
    }
    res->method = QEMU_POINTER_OPEN_LOOP;
    return qemu_input_co_run(ctx, b, errp);
}

bool coroutine_fn qemu_pointer_co_locate(QemuConsole *con, QemuInputCtx *ctx,
                                         QemuPointerResult *res,
                                         const char **how, Error **errp)
{
    int w = qemu_console_get_width(con, 0);
    int h = qemu_console_get_height(con, 0);
    QemuPointer *p = pointer_find(con, true);
    QemuCursorState cs;
    int x, y;

    memset(res, 0, sizeof(*res));
    if (qemu_input_is_absolute(con)) {
        if (!pointer_valid(p, w, h)) {
            error_setg(errp, "the mouse is absolute, so it is wherever "
                       "it was last sent; use mouse-move to place it");
            return false;
        }
        *how = "absolute device, last position sent";
        goto found_model;
    }
    if (qemu_console_hw_cursor_state(con, &cs) && cs.visible) {
        PointerHot hot;

        if (pointer_hot_get(p, cs.shape, &hot)) {
            pointer_set(p, cs.x + hot.x, cs.y + hot.y, w, h,
                        QEMU_POINTER_CLOSED_LOOP);
            *how = "display card cursor";
        } else {
            /* without the hotspot, the image corner is the guess */
            pointer_set(p, cs.x, cs.y, w, h, QEMU_POINTER_CLOSED_LOOP);
            *how = p->n_hot ?
                "display card cursor, image corner (approximate: the hotspot"
                " of this cursor image is not measured yet; a mouse-move"
                " measures it)" :
                "display card cursor, image corner (approximate)";
        }
        goto found_model;
    }
    if (!pointer_co_find(con, ctx, &x, &y, how, errp)) {
        return false;
    }
    pointer_set(p, x, y, w, h, QEMU_POINTER_OPEN_LOOP);

found_model:
    res->known = true;
    res->x = p->x;
    res->y = p->y;
    res->method = p->method;
    trace_pointer_move(qemu_console_get_index(con), p->x, p->y, *how,
                       false, 0, true);
    return true;
}
