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
#define POINTER_CAL_DIST        48      /* counts each step test covers */
#define POINTER_CAL_SLACK       2       /* px two travels may differ */
#define POINTER_CAL_MIN_PX      4       /* px the step test must travel */
#define POINTER_CAL_MAX_MOVES   150     /* moves of the gain test at most */
#define POINTER_DRIFT_SLACK     1.0     /* px of drift that homing must save */
#define POINTER_GAIN_MIN        0.05    /* px per count we believe */
#define POINTER_GAIN_MAX        20.0
#define POINTER_GAIN_FIRST      0.25    /* the first homing covers this */

typedef struct QemuPointer {
    QemuConsole *con;
    bool known;
    int x, y;
    int64_t sync_ms;
    int fb_w, fb_h;
    uint64_t foreign;
    QemuPointerMethod method;
    bool unknown_told;          /* qemu_pointer_unknown_once() said so */
    /* closed loop: hotspot minus the cursor image's top-left corner */
    bool has_offset;
    int off_x, off_y;
    /* open loop: the biggest step the guest takes as it is; 0: unknown */
    bool cal_done;
    int cal_step;
    double gain_x, gain_y;      /* px one count moves; 0: unknown */
    double err_x, err_y;        /* how far off the gain may be, as a part */
    double drift;               /* px the position may be off, from that */
    int cal_w, cal_h;           /* the screen size it was measured at */
    QLIST_ENTRY(QemuPointer) node;
} QemuPointer;

static QLIST_HEAD(, QemuPointer) pointers = QLIST_HEAD_INITIALIZER(pointers);

static void pointer_reset(void *opaque)
{
    QemuPointer *p;

    QLIST_FOREACH(p, &pointers, node) {
        p->known = false;
        p->has_offset = false;
        p->cal_done = false;    /* another guest may boot */
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
        p->has_offset = false;
    }
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

static bool coroutine_fn pointer_co_absolute(QemuPointer *p,
                                             QemuInputCtx *ctx, int x, int y,
                                             int w, int h,
                                             QemuPointerResult *res,
                                             Error **errp)
{
    QemuInputBatch *b = qemu_input_batch_new(p->con);

    qemu_input_batch_move(b, INPUT_EVENT_KIND_ABS,
                          qemu_input_scale_axis(x, 0, MAX(w - 1, 1),
                                                INPUT_EVENT_ABS_MIN,
                                                INPUT_EVENT_ABS_MAX),
                          qemu_input_scale_axis(y, 0, MAX(h - 1, 1),
                                                INPUT_EVENT_ABS_MIN,
                                                INPUT_EVENT_ABS_MAX));
    if (!qemu_input_co_run(ctx, b, errp)) {
        return false;
    }
    pointer_set(p, x, y, w, h, QEMU_POINTER_ABSOLUTE);
    res->method = QEMU_POINTER_ABSOLUTE;
    res->steps = 1;
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
        res->homed = !valid || now - p->sync_ms > POINTER_STALE_MS;
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
    return a->x == b->x && a->y == b->y && a->visible == b->visible;
}

/* Sleep until the card's cursor moves away from @old and settles */
static bool coroutine_fn pointer_co_await_cursor(QemuPointer *p,
                                                 QemuInputCtx *ctx,
                                                 const QemuCursorState *old,
                                                 QemuCursorState *cs)
{
    bool moved = false;
    int waited;

    for (waited = 0; waited < POINTER_SETTLE_MS; waited += POINTER_POLL_MS) {
        QemuCursorState prev;

        qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME,
                                  POINTER_POLL_MS * SCALE_MS);
        if (ctx->cancelled) {
            return false;
        }
        prev = *cs;
        qemu_console_hw_cursor_state(p->con, cs);
        if (moved && cursor_equal(&prev, cs)) {
            break;          /* moved, then held still for one poll */
        }
        if (!cursor_equal(old, cs)) {
            moved = true;
        }
    }
    return moved;
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

static bool coroutine_fn pointer_co_closed_loop(QemuPointer *p,
                                                QemuInputCtx *ctx,
                                                int x, int y, int w, int h,
                                                const QemuPointerOpts *o,
                                                QemuPointerResult *res,
                                                Error **errp)
{
    int capx = POINTER_LOOP_CAP, capy = POINTER_LOOP_CAP;
    int stuck = 0;
    QemuCursorState cs;
    int i;

    qemu_console_hw_cursor_state(p->con, &cs);

    if (!p->has_offset || o->home == QEMU_POINTER_HOME_ALWAYS) {
        /*
         * Learn the hotspot: guests clamp the hotspot to the last
         * pixel, so in the bottom-right corner the offset shows.
         */
        if (!pointer_co_corner_cursor(p, ctx, w, h, o, &cs, errp)) {
            return false;
        }
        p->off_x = (w - 1) - cs.x;
        p->off_y = (h - 1) - cs.y;
        if (p->off_x < 0 || p->off_x >= MAX(cs.w, 1) ||
            p->off_y < 0 || p->off_y >= MAX(cs.h, 1)) {
            p->off_x = p->off_y = 0;
        }
        p->has_offset = true;
        res->homed = true;
    }

    for (i = 0; i < POINTER_LOOP_MAX; i++) {
        int dx = x - (cs.x + p->off_x);
        int dy = y - (cs.y + p->off_y);
        QemuCursorState old = cs;
        QemuInputBatch *b;
        int ndx, ndy;

        if (!dx && !dy) {
            pointer_set(p, x, y, w, h, QEMU_POINTER_CLOSED_LOOP);
            res->method = QEMU_POINTER_CLOSED_LOOP;
            return true;
        }
        b = qemu_input_batch_new(p->con);
        qemu_input_batch_move(b, INPUT_EVENT_KIND_REL,
                              clamp_step(dx, capx), clamp_step(dy, capy));
        if (!qemu_input_co_run(ctx, b, errp)) {
            p->known = false;
            return false;
        }
        res->steps++;
        if (!pointer_co_await_cursor(p, ctx, &old, &cs)) {
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
        /* overshot: the guest accelerated the move, so move less */
        ndx = x - (cs.x + p->off_x);
        ndy = y - (cs.y + p->off_y);
        if (ndx && (ndx > 0) != (dx > 0)) {
            capx = MAX(1, capx / 2);
        }
        if (ndy && (ndy > 0) != (dy > 0)) {
            capy = MAX(1, capy / 2);
        }
    }

    /* the card's cursor stopped following: finish in open loop from here */
    pointer_set(p, cs.x + p->off_x, cs.y + p->off_y, w, h,
                QEMU_POINTER_OPEN_LOOP);
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
        ok = pointer_co_absolute(p, ctx, x, y, w, h, res, errp);
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
        if (p->has_offset) {
            pointer_set(p, cs.x + p->off_x, cs.y + p->off_y, w, h,
                        QEMU_POINTER_CLOSED_LOOP);
            *how = "display card cursor";
        } else {
            /* without the learned offset, the image corner is the guess */
            pointer_set(p, cs.x, cs.y, w, h, QEMU_POINTER_CLOSED_LOOP);
            *how = "display card cursor, image corner (approximate)";
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
