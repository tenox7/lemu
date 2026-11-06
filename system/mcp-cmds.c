/*
 * VM commands for MCP clients (see qapi/mcp.json)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev.h"
#include "hw/core/status-text.h"
#include "migration/snapshot.h"
#include "monitor/mcp.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-block.h"
#include "qapi/qapi-commands-control.h"
#include "qapi/qapi-commands-machine.h"
#include "qapi/qapi-commands-mcp.h"
#include "qapi/qapi-commands-misc.h"
#include "qemu/coroutine.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "system/mcp-serial.h"
#include "system/runstate.h"
#include "system/system.h"
#include "ui/console.h"
#include "ui/input.h"
#ifdef CONFIG_PIXMAN
#include "ui/pointer.h"
#endif

#define STATS_DEFAULT_MS    5000
#define STATS_MIN_MS        100
#define STATS_MAX_MS        100000

static McpConsoleInfo *mcp_console_info(QemuConsole *con)
{
    McpConsoleInfo *ci = g_new0(McpConsoleInfo, 1);
    Object *dev = object_property_get_link(OBJECT(con), "device", NULL);
    QemuCursorState cs;

    ci->index = qemu_console_get_index(con);
    if (dev && DEVICE(dev)->id) {
        ci->id = g_strdup(DEVICE(dev)->id);
    }
    ci->head = qemu_console_get_head(con);
    ci->graphic = qemu_console_is_graphic(con);
    ci->width = qemu_console_get_width(con, 0);
    ci->height = qemu_console_get_height(con, 0);
    ci->mouse = g_strdup(qemu_input_mouse_kind(con));
    ci->hw_cursor = qemu_console_hw_cursor_state(con, &cs);
    return ci;
}

/*
 * snapshot=on puts a temporary qcow2 overlay on top; report the image
 * under it, since that is the one the user knows.
 */
static bool mcp_is_snapshot(BlockDeviceInfo *bdi)
{
    g_autofree char *base = g_path_get_basename(bdi->file);

    return bdi->backing_file && !strcmp(bdi->drv, "qcow2") &&
           g_str_has_prefix(base, "vl.");
}

static McpDiskInfo *mcp_disk_info(BlockInfo *bi)
{
    McpDiskInfo *di = g_new0(McpDiskInfo, 1);

    di->device = g_strdup(*bi->device ? bi->device :
                          bi->qdev ? bi->qdev : "");
    di->removable = bi->removable;
    if (bi->has_tray_open) {
        di->has_tray_open = true;
        di->tray_open = bi->tray_open;
    }
    if (bi->inserted) {
        BlockDeviceInfo *bdi = bi->inserted;

        di->snapshot = mcp_is_snapshot(bdi);
        di->file = g_strdup(di->snapshot ? bdi->backing_file : bdi->file);
        di->read_only = bdi->ro;
    }
    return di;
}

static McpDiskInfoList *mcp_disks(bool removable_only, int *count)
{
    g_autoptr(BlockInfoList) blocks = qmp_query_block(false, false, NULL);
    McpDiskInfoList *head = NULL, **tail = &head;
    BlockInfoList *b;

    *count = 0;
    for (b = blocks; b; b = b->next) {
        if (removable_only && !b->value->removable) {
            continue;
        }
        if (!b->value->removable && !b->value->inserted) {
            continue;
        }
        QAPI_LIST_APPEND(tail, mcp_disk_info(b->value));
        (*count)++;
    }
    return head;
}

static void mcp_status_one(Object *obj, const char *text, void *opaque)
{
    strList ***tail = opaque;
    DeviceState *dev = (DeviceState *)object_dynamic_cast(obj, TYPE_DEVICE);
    const char *name = dev && dev->id ? dev->id : object_get_typename(obj);

    QAPI_LIST_APPEND(*tail, g_strdup_printf("%s: %s", name, text));
}

McpVmInfo *qmp_vm_info(Error **errp)
{
    McpVmInfo *vi = g_new0(McpVmInfo, 1);
    McpConsoleInfoList **ctail = &vi->consoles;
    strList **stail = &vi->serial;
    strList **dtail = &vi->device_status, *d;
    g_autoptr(GString) s = g_string_new(NULL);
    QemuConsole *con, *dflt = qemu_console_lookup_default();
    int i, ndisks;

    vi->name = g_strdup(qemu_name ? qemu_name : "");
    vi->machine = g_strdup(current_machine ?
                           MACHINE_GET_CLASS(current_machine)->name : "");
    vi->status = g_strdup(RunState_str(runstate_get()));
    vi->running = runstate_is_running();

    for (i = 0; (con = qemu_console_lookup_by_index(i)); i++) {
        QAPI_LIST_APPEND(ctail, mcp_console_info(con));
    }
    if (dflt) {
        vi->has_default_console = true;
        vi->default_console = qemu_console_get_index(dflt);
#ifdef CONFIG_PIXMAN
        {
            QemuPointerMethod m;
            int x, y;

            vi->pointer = g_new0(McpPointer, 1);
            if (qemu_pointer_get(dflt, &x, &y, &m)) {
                vi->pointer->known = true;
                vi->pointer->has_x = vi->pointer->has_y = true;
                vi->pointer->x = x;
                vi->pointer->y = y;
                vi->pointer->method = g_strdup(qemu_pointer_method_str(m));
            }
        }
#endif
    }
    vi->disks = mcp_disks(false, &ndisks);
    mcp_serial_list(stail);
    status_text_foreach(mcp_status_one, &dtail);

    g_string_printf(s, "%s, machine %s, %s", *vi->name ? vi->name : "VM",
                    vi->machine, vi->status);
    if (dflt) {
        g_string_append_printf(s, ", screen %dx%d",
                               qemu_console_get_width(dflt, 0),
                               qemu_console_get_height(dflt, 0));
    }
    g_string_append_printf(s, ", %d disk%s", ndisks, ndisks == 1 ? "" : "s");
    for (d = vi->device_status; d; d = d->next) {
        g_string_append_printf(s, "; %s", d->value);
    }
    vi->summary = g_strdup(s->str);
    return vi;
}

/* Host CPU time that this process has used, user and system, in us */
static int64_t mcp_cpu_time_us(void)
{
#ifdef _WIN32
    FILETIME create, end, kernel, user;
    ULARGE_INTEGER k, u;

    if (!GetProcessTimes(GetCurrentProcess(), &create, &end, &kernel,
                         &user)) {
        return 0;
    }
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return (k.QuadPart + u.QuadPart) / 10;
#else
    struct rusage ru;

    if (getrusage(RUSAGE_SELF, &ru) < 0) {
        return 0;
    }
    return (int64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 +
           ru.ru_utime.tv_usec + ru.ru_stime.tv_usec;
#endif
}

McpVmStats *coroutine_fn qmp_vm_stats(bool has_interval_ms,
                                      int64_t interval_ms, Error **errp)
{
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_current_input_ctx();
    g_autoptr(GString) s = g_string_new(NULL);
    g_autofree int *halted = NULL;
    McpVmStats *st;
    McpVcpuStatsList **tail;
    int64_t start, now, end, cpu_us, ms, tenths;
    int ncpus = 0, samples = 0, running = 0;
    CPUState *cpu;

    if (!ctx) {
        qemu_input_ctx_init(&local, NULL);
        ctx = &local;
    }
    interval_ms = has_interval_ms ? interval_ms : STATS_DEFAULT_MS;
    interval_ms = MIN(MAX(interval_ms, STATS_MIN_MS), STATS_MAX_MS);
    CPU_FOREACH(cpu) {
        ncpus = MAX(ncpus, cpu->cpu_index + 1);
    }
    halted = g_new0(int, MAX(ncpus, 1));

    cpu_us = mcp_cpu_time_us();
    start = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    end = start + interval_ms;
    for (;;) {
        samples++;
        if (runstate_is_running()) {
            running++;
            CPU_FOREACH(cpu) {
                if (cpu->cpu_index < ncpus && qatomic_read(&cpu->halted)) {
                    halted[cpu->cpu_index]++;
                }
            }
        }
        now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        if (now >= end || ctx->cancelled) {
            break;
        }
        /* uneven steps, so that a guest timer can't keep pace with them */
        qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME,
                                  MIN(g_random_int_range(5, 16), end - now)
                                  * SCALE_MS);
    }
    if (ctx->cancelled) {
        error_setg(errp, "cancelled");
        return NULL;
    }
    cpu_us = mcp_cpu_time_us() - cpu_us;
    ms = MAX(now - start, 1);
    tenths = (cpu_us + ms / 2) / ms;

    st = g_new0(McpVmStats, 1);
    st->interval_ms = ms;
    st->cpu_ms = cpu_us / 1000;
    st->cpu_percent = tenths / 10.0;
    st->paused_percent = (samples - running) * 100 / samples;
    tail = &st->vcpus;
    CPU_FOREACH(cpu) {
        McpVcpuStats *v = g_new0(McpVcpuStats, 1);

        v->index = cpu->cpu_index;
        if (running && cpu->cpu_index < ncpus) {
            v->halted_percent = halted[cpu->cpu_index] * 100 / running;
        }
        QAPI_LIST_APPEND(tail, v);
    }

    g_string_printf(s, "over %.1f s, QEMU used %.1f%% host CPU (100%% = one"
                    " core)", ms / 1000.0, tenths / 10.0);
    if (!running) {
        g_string_append(s, "; the VM was paused, so its CPUs did not run");
    } else if (st->vcpus && !st->vcpus->next) {
        g_string_append_printf(s, "; CPU %" PRId64 " was halted %" PRId64
                               "%% of the time", st->vcpus->value->index,
                               st->vcpus->value->halted_percent);
    } else {
        McpVcpuStatsList *v;

        g_string_append(s, "; halted:");
        for (v = st->vcpus; v; v = v->next) {
            g_string_append_printf(s, "%s CPU %" PRId64 " %" PRId64 "%%",
                                   v == st->vcpus ? "" : ",",
                                   v->value->index,
                                   v->value->halted_percent);
        }
    }
    if (running && st->paused_percent) {
        g_string_append_printf(s, "; paused %" PRId64 "%% of the time",
                               st->paused_percent);
    }
    st->summary = g_strdup(s->str);
    return st;
}

/* ide-cd, scsi-cd and friends */
static bool mcp_is_cdrom(BlockInfo *bi)
{
    g_autofree char *path = NULL;
    Object *o;

    if (!bi->qdev) {
        return false;
    }
    path = bi->qdev[0] == '/' ? g_strdup(bi->qdev) :
           g_strdup_printf("/machine/peripheral/%s", bi->qdev);
    o = object_resolve_path(path, NULL);
    return o && strstr(object_get_typename(o), "-cd");
}

static bool mcp_find_drive(const char *device, BlockInfo **out,
                           BlockInfoList *blocks, Error **errp)
{
    g_autoptr(GString) names = g_string_new(NULL);
    BlockInfoList *b;

    for (b = blocks; b; b = b->next) {
        BlockInfo *bi = b->value;

        if (!bi->removable) {
            continue;
        }
        if (!strcmp(bi->device, device) ||
            (bi->qdev && !strcmp(bi->qdev, device))) {
            *out = bi;
            return true;
        }
        g_string_append_printf(names, "%s%s", names->len ? ", " : "",
                               *bi->device ? bi->device : bi->qdev);
    }
    error_setg(errp, "no removable drive '%s'; drives: %s", device,
               names->len ? names->str : "none");
    return false;
}

McpMediaResult *qmp_media(McpMediaAction action, const char *device,
                          const char *file, const char *format,
                          bool has_read_only, bool read_only,
                          bool has_force, bool force, Error **errp)
{
    ERRP_GUARD();
    g_autoptr(BlockInfoList) blocks = NULL;
    g_autoptr(GString) s = g_string_new(NULL);
    McpMediaResult *res;
    BlockInfo *bi = NULL;
    int n;

    if (action != MCP_MEDIA_ACTION_LIST) {
        const char *dev, *id;

        if (!device) {
            error_setg(errp, "'device' is needed; use action \"list\" "
                       "to see the drives");
            return NULL;
        }
        blocks = qmp_query_block(false, false, NULL);
        if (!mcp_find_drive(device, &bi, blocks, errp)) {
            return NULL;
        }
        /* a -device drive has no backend name; use its qdev id */
        dev = *bi->device ? bi->device : NULL;
        id = dev ? NULL : bi->qdev;

        if (action == MCP_MEDIA_ACTION_INSERT) {
            bool ro = has_read_only ? read_only : mcp_is_cdrom(bi);
            BlockdevChangeReadOnlyMode mode =
                ro ? BLOCKDEV_CHANGE_READ_ONLY_MODE_READ_ONLY
                   : BLOCKDEV_CHANGE_READ_ONLY_MODE_READ_WRITE;

            if (!file) {
                error_setg(errp, "'file' is needed to insert a medium");
                return NULL;
            }
            if (!g_file_test(file, G_FILE_TEST_EXISTS)) {
                error_setg(errp, "file '%s' does not exist", file);
                return NULL;
            }
            qmp_blockdev_change_medium(dev, id, file, format, has_force,
                                       force, true, mode, errp);
            if (*errp) {
                return NULL;
            }
            g_string_printf(s, "inserted %s into %s%s", file, device,
                            ro ? " (read-only)" : "");
        } else {
            qmp_eject(dev, id, has_force, force, errp);
            if (*errp) {
                return NULL;
            }
            g_string_printf(s, "ejected %s", device);
        }
    }

    res = g_new0(McpMediaResult, 1);
    res->drives = mcp_disks(true, &n);
    if (action == MCP_MEDIA_ACTION_LIST) {
        McpDiskInfoList *l;

        g_string_printf(s, "%d removable drive%s", n, n == 1 ? "" : "s");
        for (l = res->drives; l; l = l->next) {
            g_string_append_printf(s, "%s %s: %s", l == res->drives ? ":" : ";",
                                   l->value->device,
                                   l->value->file ? l->value->file : "empty");
        }
    }
    res->summary = g_strdup(s->str);
    return res;
}

McpResult *qmp_vm_power(McpPowerAction action, Error **errp)
{
    ERRP_GUARD();
    McpResult *res;
    const char *what;

    switch (action) {
    case MCP_POWER_ACTION_PAUSE:
        qmp_stop(errp);
        what = "paused";
        break;
    case MCP_POWER_ACTION_RESUME:
        qmp_cont(errp);
        what = "resumed";
        break;
    case MCP_POWER_ACTION_RESET:
        qmp_system_reset(errp);
        what = "reset";
        break;
    case MCP_POWER_ACTION_POWERDOWN:
        qmp_system_powerdown(errp);
        what = "power button pressed; the guest decides what to do";
        break;
    case MCP_POWER_ACTION_QUIT:
        qmp_quit(errp);
        what = "quitting; this connection will close";
        break;
    default:
        g_assert_not_reached();
    }
    if (*errp) {
        return NULL;
    }
    res = g_new0(McpResult, 1);
    res->summary = g_strdup(what);
    return res;
}

/* The snapshots of the first disk that can hold them */
static McpSnapshotInfoList *mcp_snapshots(int *count)
{
    g_autoptr(BlockInfoList) blocks = qmp_query_block(false, false, NULL);
    McpSnapshotInfoList *head = NULL, **tail = &head;
    BlockInfoList *b;

    *count = 0;
    for (b = blocks; b; b = b->next) {
        BlockDeviceInfo *bdi = b->value->inserted;
        SnapshotInfoList *s;

        if (!bdi || bdi->ro || !bdi->image ||
            strcmp(bdi->image->format, "qcow2")) {
            continue;
        }
        for (s = bdi->image->snapshots; s; s = s->next) {
            McpSnapshotInfo *si = g_new0(McpSnapshotInfo, 1);
            g_autoptr(GDateTime) dt =
                g_date_time_new_from_unix_local(s->value->date_sec);

            si->name = g_strdup(s->value->name);
            si->date = dt ? g_date_time_format(dt, "%Y-%m-%d %H:%M:%S")
                          : g_strdup("?");
            si->vm_state_size = s->value->vm_state_size;
            QAPI_LIST_APPEND(tail, si);
            (*count)++;
        }
        break;
    }
    return head;
}

McpSnapshotResult *qmp_vm_snapshot(McpSnapshotAction action,
                                   const char *name, Error **errp)
{
    McpSnapshotResult *res;
    g_autofree char *done = NULL;
    McpSnapshotInfoList *l;
    int n;

    if (action != MCP_SNAPSHOT_ACTION_LIST && (!name || !*name)) {
        error_setg(errp, "'name' is needed to %s a snapshot",
                   McpSnapshotAction_str(action));
        return NULL;
    }
    switch (action) {
    case MCP_SNAPSHOT_ACTION_SAVE:
        if (!save_snapshot(name, true, NULL, false, NULL, errp)) {
            return NULL;
        }
        done = g_strdup_printf("saved snapshot '%s'", name);
        break;
    case MCP_SNAPSHOT_ACTION_LOAD: {
        RunState saved = runstate_get();
        bool ok;

        vm_stop(RUN_STATE_RESTORE_VM);
        ok = load_snapshot(name, NULL, false, NULL, errp);
        if (ok) {
            load_snapshot_resume(saved);
        } else if (saved == RUN_STATE_RUNNING) {
            vm_start();
        }
        if (!ok) {
            return NULL;
        }
#ifdef CONFIG_PIXMAN
        {
            QemuConsole *con;
            int i;

            /* the guest's pointer is back where it was then */
            for (i = 0; (con = qemu_console_lookup_by_index(i)); i++) {
                qemu_pointer_forget(con);
            }
        }
#endif
        done = g_strdup_printf("loaded snapshot '%s'; the VM is %s", name,
                               RunState_str(runstate_get()));
        break;
    }
    case MCP_SNAPSHOT_ACTION_DELETE:
        if (!delete_snapshot(name, false, NULL, errp)) {
            return NULL;
        }
        done = g_strdup_printf("deleted snapshot '%s'", name);
        break;
    default:
        break;
    }

    res = g_new0(McpSnapshotResult, 1);
    res->snapshots = mcp_snapshots(&n);
    if (done) {
        res->summary = g_strdup_printf("%s; %d snapshot%s", done, n,
                                       n == 1 ? "" : "s");
    } else {
        GString *s = g_string_new(NULL);

        g_string_printf(s, "%d snapshot%s", n, n == 1 ? "" : "s");
        for (l = res->snapshots; l; l = l->next) {
            g_string_append_printf(s, "%s %s (%s)",
                                   l == res->snapshots ? ":" : ";",
                                   l->value->name, l->value->date);
        }
        res->summary = g_string_free(s, false);
    }
    return res;
}
