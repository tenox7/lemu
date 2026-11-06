/*
 * MCP monitor: the Model Context Protocol, for AI agents
 *
 * The third monitor type, next to HMP and QMP.  It speaks JSON-RPC 2.0,
 * one message per line, as MCP's stdio transport does.  Tools are QAPI
 * commands with the feature 'mcp' (see qapi/mcp.json and the generated
 * qapi-mcp-tools.c), plus the hand-written 'hmp' and 'qmp' tools (in
 * mcp-common.c, which the lemu-mcp bridge shares).
 *
 * Each tools/call runs in its own coroutine, so a call that waits for
 * the screen does not block the guest or other calls.  Cancelling a
 * call, or disconnecting, stops its input and releases every key and
 * button it pressed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/aio.h"
#include "monitor-internal.h"
#ifdef CONFIG_HMP
#include "monitor-hmp-internal.h"
#include "disas/disas.h"
#include "hw/core/cpu.h"
#include "qapi/qapi-commands-misc.h"
#endif
#include "monitor/mcp.h"
#include "monitor/mcp-common.h"
#include "monitor/mcp-tools.h"
#include "qapi/error.h"
#include "qapi/qapi-emit-events.h"
#include "qapi/qmp-registry.h"
#include "qemu/coroutine.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "qobject/qnull.h"
#include "qobject/qnum.h"
#include "qobject/qstring.h"
#include "qom/object_interfaces.h"
#include "system/registry.h"
#include "trace.h"

#define MCP_EVENTS_MAX          32
#define MCP_PROGRESS_MS         5000
#define MCP_STEPS_MAX           50
#define MCP_STEP_SUMMARY_MAX    2000    /* of a step that is not the last */

typedef struct McpCall {
    MonitorMCP *mon;
    QObject *id;
    char *tool;
    char *step;                 /* steps: "(2 of 5: key)" while it runs */
    QDict *args;
    QObject *progress_token;
    QEMUTimer *progress_timer;
    int64_t start_ms;
    bool discard;               /* cancelled or client gone: no reply */
    bool quiet_runstate;        /* no STOP or RESUME events meanwhile */
    QemuInputCtx ctx;
    QTAILQ_ENTRY(McpCall) node;
} McpCall;

typedef struct McpEvent {
    char *name;
    int64_t ms;                 /* the newest one, if repeated */
    int count;
} McpEvent;

struct MonitorMCP {
    Monitor parent_obj;
    JSONMessageParser parser;
    bool initialized;
    QTAILQ_HEAD(, McpCall) calls;
    McpEvent events[MCP_EVENTS_MAX];
    int n_events;
};

struct MonitorMCPClass {
    MonitorClass parent_class;
};

OBJECT_DEFINE_TYPE(MonitorMCP, monitor_mcp, MONITOR_MCP, MONITOR);

/* The call running in each coroutine, for mcp_current_input_ctx() */
static GHashTable *mcp_calls_by_co;

static void mcp_handle_message(void *opaque, QObject *msg, Error *err);

static McpCall *mcp_current_call(void)
{
    if (!mcp_calls_by_co || !qemu_in_coroutine()) {
        return NULL;
    }
    return g_hash_table_lookup(mcp_calls_by_co, qemu_coroutine_self());
}

QemuInputCtx *mcp_current_input_ctx(void)
{
    McpCall *call = mcp_current_call();

    return call ? &call->ctx : NULL;
}

void mcp_quiet_runstate_events(void)
{
    McpCall *call = mcp_current_call();

    if (call) {
        call->quiet_runstate = true;
    }
}

/* What is left of the MCP_WAIT_MAX_MS that one call may take in all */
static int64_t mcp_call_time_left(McpCall *call)
{
    int64_t used = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - call->start_ms;

    return MAX(MCP_WAIT_MAX_MS - used, 0);
}

int64_t mcp_wait_max_ms(void)
{
    McpCall *call = mcp_current_call();

    return call ? mcp_call_time_left(call) : MCP_WAIT_MAX_MS;
}

#ifdef CONFIG_HMP
typedef struct McpDisas {
    CPUState *cpu;
    uint64_t pc;
    int count;
} McpDisas;

/* The monitor writes \r\n line ends; agents read \n */
static char *mcp_strip_cr(char *s)
{
    char *in, *out;

    for (in = out = s; *in; in++) {
        if (*in != '\r') {
            *out++ = *in;
        }
    }
    *out = '\0';
    return s;
}

static void mcp_print_registers(MonitorHMP *hmp, void *opaque)
{
    cpu_dump_state(opaque, NULL, 0);
}

static void mcp_print_disas(MonitorHMP *hmp, void *opaque)
{
    McpDisas *d = opaque;

    monitor_disas(hmp, d->cpu, d->pc, d->count, false);
}

/*
 * What @fn prints, with a scratch HMP monitor as the current one, the
 * way human-monitor-command works; cpu_dump_state() prints there too.
 */
static char *mcp_print(void (*fn)(MonitorHMP *hmp, void *opaque),
                       void *opaque)
{
    MonitorHMP *hmp = MONITOR_HMP(object_new(TYPE_MONITOR_HMP));
    Monitor *old = monitor_set_cur(qemu_coroutine_self(), MONITOR(hmp));
    char *out;

    fn(hmp, opaque);
    monitor_set_cur(qemu_coroutine_self(), old);
    WITH_QEMU_LOCK_GUARD(&hmp->parent_obj.mon_lock) {
        out = g_strdup(hmp->parent_obj.outbuf->str);
    }
    object_unref(hmp);
    return mcp_strip_cr(out);
}

char *mcp_cpu_registers(CPUState *cpu)
{
    return mcp_print(mcp_print_registers, cpu);
}

char *mcp_cpu_disas(CPUState *cpu, uint64_t pc, int count)
{
    McpDisas d = { .cpu = cpu, .pc = pc, .count = count };

    return mcp_print(mcp_print_disas, &d);
}

char *mcp_cpu_memory(CPUState *cpu, uint64_t addr, int words, bool physical)
{
    g_autofree char *cmd = g_strdup_printf("%s /%dwx 0x%" PRIx64,
                                           physical ? "xp" : "x", words,
                                           addr);
    char *out = qmp_human_monitor_command(cmd, true, cpu->cpu_index, NULL);

    return out ? mcp_strip_cr(out) : NULL;
}
#else
char *mcp_cpu_registers(CPUState *cpu)
{
    return NULL;
}

char *mcp_cpu_disas(CPUState *cpu, uint64_t pc, int count)
{
    return NULL;
}

char *mcp_cpu_memory(CPUState *cpu, uint64_t addr, int words, bool physical)
{
    return NULL;
}
#endif

static void mcp_send(MonitorMCP *mcp, QDict *msg)
{
    Monitor *mon = MONITOR(mcp);
    GString *json = qobject_to_json(QOBJECT(msg));

    trace_mcp_send(mcp, json->len);
    WITH_QEMU_LOCK_GUARD(&mon->mon_lock) {
        g_string_append_len(mon->outbuf, json->str, json->len);
        g_string_append_c(mon->outbuf, '\n');
        monitor_flush_locked(mon);
    }
    g_string_free(json, true);
}

static QDict *mcp_message(QObject *id)
{
    QDict *msg = qdict_new();

    qdict_put_str(msg, "jsonrpc", "2.0");
    if (id) {
        qdict_put_obj(msg, "id", qobject_ref(id));
    }
    return msg;
}

/* Send @result (taking the reference) as the reply to request @id */
static void mcp_reply(MonitorMCP *mcp, QObject *id, QObject *result)
{
    QDict *msg = mcp_message(id);

    qdict_put_obj(msg, "result", result);
    mcp_send(mcp, msg);
    qobject_unref(msg);
}

static void mcp_reply_error(MonitorMCP *mcp, QObject *id, int code,
                            const char *text, QObject *data)
{
    QDict *msg = mcp_message(NULL);
    QDict *error = qdict_new();

    qdict_put_obj(msg, "id", id ? qobject_ref(id) : QOBJECT(qnull()));
    qdict_put_int(error, "code", code);
    qdict_put_str(error, "message", text);
    if (data) {
        qdict_put_obj(error, "data", data);
    }
    qdict_put(msg, "error", error);
    mcp_send(mcp, msg);
    qobject_unref(msg);
}

/* Events */

static void monitor_mcp_emit_event(Monitor *mon, QAPIEvent event,
                                   QDict *qdict)
{
    MonitorMCP *mcp = MONITOR_MCP(mon);
    const char *name = QAPIEvent_str(event);
    McpCall *call;

    if (event == QAPI_EVENT_STOP || event == QAPI_EVENT_RESUME) {
        QTAILQ_FOREACH(call, &mcp->calls, node) {
            if (call->quiet_runstate) {
                return;
            }
        }
    }

    /* a guest that sets its clock every second sends a stream of these */
    if (mcp->n_events &&
        !strcmp(mcp->events[mcp->n_events - 1].name, name)) {
        mcp->events[mcp->n_events - 1].count++;
        mcp->events[mcp->n_events - 1].ms =
            qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        return;
    }
    if (mcp->n_events == MCP_EVENTS_MAX) {
        g_free(mcp->events[0].name);
        memmove(&mcp->events[0], &mcp->events[1],
                sizeof(mcp->events[0]) * (MCP_EVENTS_MAX - 1));
        mcp->n_events--;
    }
    mcp->events[mcp->n_events].name = g_strdup(name);
    mcp->events[mcp->n_events].ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    mcp->events[mcp->n_events].count = 1;
    mcp->n_events++;
}

/* The events since the previous call, as a list; NULL if none */
static QList *mcp_take_events(MonitorMCP *mcp)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    QList *list;
    int i;

    if (!mcp->n_events) {
        return NULL;
    }
    list = qlist_new();
    for (i = 0; i < mcp->n_events; i++) {
        QDict *e = qdict_new();

        qdict_put_str(e, "event", mcp->events[i].name);
        qdict_put_int(e, "ms-ago", now - mcp->events[i].ms);
        if (mcp->events[i].count > 1) {
            qdict_put_int(e, "count", mcp->events[i].count);
        }
        qlist_append(list, e);
        g_free(mcp->events[i].name);
    }
    mcp->n_events = 0;
    return list;
}

/* Tool results */

static QDict *mcp_text_block(const char *text)
{
    QDict *b = qdict_new();

    qdict_put_str(b, "type", "text");
    qdict_put_str(b, "text", text);
    return b;
}

static QDict *mcp_tool_result(QList *content, bool is_error)
{
    QDict *r = qdict_new();

    qdict_put(r, "content", content);
    qdict_put_bool(r, "isError", is_error);
    return r;
}

static QDict *mcp_tool_error(const char *text)
{
    QList *content = qlist_new();

    qlist_append(content, mcp_text_block(text));
    return mcp_tool_result(content, true);
}

/*
 * Add an image block to @content for each of @images; the images
 * without their data, for the JSON block
 */
static QList *mcp_image_blocks(QList *content, QList *images)
{
    QList *meta = qlist_new();
    QListEntry *le;

    QLIST_FOREACH_ENTRY(images, le) {
        QDict *img = qobject_to(QDict, qlist_entry_obj(le));
        QDict *block = qdict_new();
        QDict *m = qdict_clone_shallow(img);

        qdict_put_str(block, "type", "image");
        qdict_put_str(block, "data", qdict_get_str(img, "data"));
        qdict_put_str(block, "mimeType", qdict_get_str(img, "mime-type"));
        qlist_append(content, block);
        qdict_del(m, "data");
        qdict_del(m, "mime-type");
        qlist_append(meta, m);
    }
    return meta;
}

/*
 * Turn an McpResult-based return value into MCP content: the summary
 * as text, each image as an image block, all else as one JSON block.
 */
static QDict *mcp_content_from_result(MonitorMCP *mcp, QObject *ret)
{
    QDict *dict = qobject_to(QDict, ret);
    QList *content = qlist_new();
    g_autoptr(QDict) rest = qdict_new();
    QList *events = mcp_take_events(mcp);
    const QDictEntry *e;

    if (!dict) {
        GString *json = qobject_to_json(ret);

        qlist_append(content, mcp_text_block(json->str));
        g_string_free(json, true);
        if (events) {
            qdict_put(rest, "events", events);
            json = qobject_to_json(QOBJECT(rest));
            qlist_append(content, mcp_text_block(json->str));
            g_string_free(json, true);
        }
        return mcp_tool_result(content, false);
    }

    if (qdict_haskey(dict, "summary")) {
        qlist_append(content,
                     mcp_text_block(qdict_get_str(dict, "summary")));
    }
    for (e = qdict_first(dict); e; e = qdict_next(dict, e)) {
        const char *key = qdict_entry_key(e);

        if (!strcmp(key, "summary")) {
            continue;
        }
        if (strcmp(key, "images")) {
            qdict_put_obj(rest, key, qobject_ref(qdict_entry_value(e)));
            continue;
        }
        qdict_put(rest, "images",
                  mcp_image_blocks(content, qobject_to(QList,
                                                       qdict_entry_value(e))));
    }
    if (events) {
        qdict_put(rest, "events", events);
    }
    if (qdict_size(rest)) {
        GString *json = qobject_to_json(QOBJECT(rest));

        qlist_append(content, mcp_text_block(json->str));
        g_string_free(json, true);
    }
    return mcp_tool_result(content, false);
}

/* Run QMP command @execute with @args from the call's coroutine */
static QDict *coroutine_fn mcp_dispatch(McpCall *call, const char *execute,
                                        QDict *args)
{
    QDict *req = qdict_new();
    QDict *rsp;

    qdict_put_str(req, "execute", execute);
    qdict_put(req, "arguments", qobject_ref(args));
    rsp = qmp_dispatch(&qmp_commands, QOBJECT(req), false,
                       MONITOR(call->mon));
    qobject_unref(req);
    return rsp;
}

static QDict *mcp_error_from_rsp(QDict *rsp)
{
    QDict *error = qdict_get_qdict(rsp, "error");

    return mcp_tool_error(error ? qdict_get_try_str(error, "desc")
                                : "unknown error");
}

static void mcp_progress_tick(void *opaque)
{
    McpCall *call = opaque;
    QDict *msg = mcp_message(NULL);
    QDict *params = qdict_new();
    int64_t s = (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - call->start_ms)
                / 1000;
    g_autofree char *text = g_strdup_printf("%s%s%s running for %" PRId64
                                            " s", call->tool,
                                            call->step ? " " : "",
                                            call->step ? call->step : "", s);

    if (call->discard) {
        qobject_unref(msg);
        qobject_unref(params);
        return;
    }
    qdict_put_str(msg, "method", "notifications/progress");
    qdict_put_obj(params, "progressToken",
                  qobject_ref(call->progress_token));
    qdict_put_int(params, "progress", s);
    qdict_put_str(params, "message", text);
    qdict_put(msg, "params", params);
    mcp_send(call->mon, msg);
    qobject_unref(msg);
    timer_mod(call->progress_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + MCP_PROGRESS_MS);
}

static void mcp_call_free(McpCall *call)
{
    qemu_input_ctx_end(&call->ctx);
    QTAILQ_REMOVE(&call->mon->calls, call, node);
    if (call->progress_timer) {
        timer_free(call->progress_timer);
    }
    qobject_unref(call->id);
    qobject_unref(call->args);
    qobject_unref(call->progress_token);
    g_free(call->tool);
    g_free(call->step);
    object_unref(OBJECT(call->mon));
    g_free(call);
}

/*
 * Run tool @tool with @args: a QMP command, or the qmp or hmp tool.
 * The QMP response, or NULL and *@error if the call cannot run.
 */
static QDict *coroutine_fn mcp_run_tool(McpCall *call, const char *tool,
                                        QDict *args, char **error)
{
    if (!strcmp(tool, "qmp")) {
        const char *execute = qdict_get_try_str(args, "execute");
        QDict *qargs = qdict_get_qdict(args, "arguments");
        g_autoptr(QDict) empty = qdict_new();

        if (!execute) {
            *error = g_strdup("'execute' is missing");
            return NULL;
        }
        if (!strcmp(execute, "qmp_capabilities")) {
            *error = g_strdup("qmp_capabilities is not needed here");
            return NULL;
        }
        return mcp_dispatch(call, execute, qargs ? qargs : empty);
    }
#ifdef CONFIG_HMP
    if (!strcmp(tool, "hmp")) {
        const char *cmd = qdict_get_try_str(args, "command");
        g_autoptr(QDict) hargs = qdict_new();

        if (!cmd) {
            *error = g_strdup("'command' is missing");
            return NULL;
        }
        qdict_put_str(hargs, "command-line", cmd);
        return mcp_dispatch(call, "human-monitor-command", hargs);
    }
#endif
    return mcp_dispatch(call, tool, args);
}

/* hmp output; the monitor writes \r\n line ends */
static GString *mcp_hmp_text(QString *text)
{
    GString *s = g_string_new(NULL);
    const char *p;

    for (p = qstring_get_str(text); *p; p++) {
        if (p[0] != '\r' || p[1] != '\n') {
            g_string_append_c(s, *p);
        }
    }
    return s;
}

static void mcp_add_json_block(QList *content, QDict *rest)
{
    GString *json = qobject_to_json(QOBJECT(rest));

    qlist_append(content, mcp_text_block(json->str));
    g_string_free(json, true);
}

/* The tool result for a QMP response */
static QDict *mcp_result_from_rsp(MonitorMCP *mcp, QDict *rsp)
{
    QObject *ret = qdict_get(rsp, "return");
    QString *text = qobject_to(QString, ret);
    g_autoptr(GString) s = NULL;
    QList *content, *events;

    if (qdict_haskey(rsp, "error")) {
        return mcp_error_from_rsp(rsp);
    }
    if (!text) {
        return mcp_content_from_result(mcp, ret);
    }
    /* hmp: plain text */
    content = qlist_new();
    events = mcp_take_events(mcp);
    s = mcp_hmp_text(text);
    qlist_append(content, mcp_text_block(s->len ? s->str : "(no output)"));
    if (events) {
        g_autoptr(QDict) rest = qdict_new();

        qdict_put(rest, "events", events);
        mcp_add_json_block(content, rest);
    }
    return mcp_tool_result(content, false);
}

/* steps */

static bool mcp_tool_exists(const char *name);

static bool mcp_steps_check(QList *steps, char **error)
{
    QListEntry *e;
    int i = 0;

    if (!steps || qlist_empty(steps)) {
        *error = g_strdup("'steps' needs a list of at least one step");
        return false;
    }
    if (qlist_size(steps) > MCP_STEPS_MAX) {
        *error = g_strdup_printf("at most %d steps in one call",
                                 MCP_STEPS_MAX);
        return false;
    }
    QLIST_FOREACH_ENTRY(steps, e) {
        QDict *step = qobject_to(QDict, qlist_entry_obj(e));
        const char *tool = step ? qdict_get_try_str(step, "tool") : NULL;
        QObject *args = step ? qdict_get(step, "args") : NULL;
        QObject *vm = qobject_to(QDict, args)
                      ? qdict_get(qobject_to(QDict, args), "vm") : NULL;

        i++;
        if (!tool) {
            *error = g_strdup_printf("step %d has no 'tool'", i);
            return false;
        }
        if (args && !qobject_to(QDict, args)) {
            *error = g_strdup_printf("step %d: 'args' must be an object", i);
            return false;
        }
        if (vm && !lemu_registry_names_me(vm)) {
            *error = g_strdup_printf("step %d: its vm is not this VM; all"
                                     " steps run on %s (pid %d), so make one"
                                     " steps call per VM", i,
                                     lemu_registry_name(), getpid());
            return false;
        }
        if (!strcmp(tool, "steps")) {
            *error = g_strdup_printf("step %d: steps cannot hold steps", i);
            return false;
        }
        if (!mcp_tool_exists(tool)) {
            *error = g_strdup_printf("step %d: unknown tool '%s'%s", i, tool,
                                     !strcmp(tool, "vm-list") ||
                                     !strcmp(tool, "vm-wait")
                                     ? " here (the bridge's own tools "
                                       "cannot be steps)" : "");
            return false;
        }
    }
    return true;
}

/* Why a step that ran did not get what it waited for, or NULL */
static const char *mcp_step_unmet(QDict *ret)
{
    QDict *wait = qdict_get_qdict(ret, "wait");

    if (!qdict_get_try_bool(ret, "matched", true)) {
        return "no match";
    }
    if (wait && !qdict_get_try_bool(wait, "matched", true)) {
        return "the wait timed out";
    }
    if (!qdict_get_try_bool(ret, "done", true)) {
        return "the command did not end";
    }
    if (!qdict_get_try_bool(ret, "reached", true)) {
        return "the CPU did not get there";
    }
    return NULL;
}

/* Append @text, cut to about @max bytes at a character boundary */
static void mcp_append_cut(GString *s, const char *text, size_t max)
{
    if (strlen(text) <= max) {
        g_string_append(s, text);
        return;
    }
    while (max && (text[max] & 0xc0) == 0x80) {
        max--;
    }
    g_string_append_len(s, text, max);
    g_string_append(s, " [...]");
}

/* One line for a step that ran; its images go to *@images */
static char *mcp_step_line(int i, const char *tool, QObject *ret,
                           QList **images)
{
    QString *str = qobject_to(QString, ret);
    QDict *dict = qobject_to(QDict, ret);
    g_autoptr(GString) s = NULL;

    if (str) {
        s = mcp_hmp_text(str);
        return g_strdup_printf("%d. %s: %s", i, tool,
                               s->len ? s->str : "(no output)");
    }
    if (!dict) {
        s = qobject_to_json(ret);
        return g_strdup_printf("%d. %s: %s", i, tool, s->str);
    }
    if (qdict_get_qlist(dict, "images")) {
        qobject_unref(*images);
        *images = qobject_ref(qdict_get_qlist(dict, "images"));
    }
    return g_strdup_printf("%d. %s: %s", i, tool,
                           qdict_haskey(dict, "summary")
                           ? qdict_get_str(dict, "summary") : "done");
}

/*
 * Run the steps in order, until one fails or does not get what it
 * waits for.  One call: they share its cancel, its events and its
 * MCP_WAIT_MAX_MS.
 */
static QDict *coroutine_fn mcp_steps(McpCall *call, QDict *args)
{
    QList *steps = qdict_get_qlist(args, "steps");
    g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(QList) images = NULL;     /* the last ones taken */
    g_autoptr(QDict) last = NULL;       /* the last step's result */
    g_autoptr(GString) text = g_string_new(NULL);
    g_autoptr(QDict) rest = qdict_new();
    g_autofree char *error = NULL;
    const char *unmet = NULL;
    bool failed = false;
    bool late = false;  /* the last wait was cut short by the time limit */
    QList *content, *events;
    QListEntry *e;
    int n, i = 0;
    int again = 0;      /* the time limit came before this step */
    guint j;

    if (!mcp_steps_check(steps, &error)) {
        return mcp_tool_error(error);
    }
    n = qlist_size(steps);
    QLIST_FOREACH_ENTRY(steps, e) {
        QDict *step = qobject_to(QDict, qlist_entry_obj(e));
        const char *tool = qdict_get_str(step, "tool");
        QDict *sargs = qdict_get_qdict(step, "args");
        g_autoptr(QDict) a = sargs ? qdict_clone_shallow(sargs) : qdict_new();
        g_autofree char *err = NULL;
        QDict *rsp, *qerr;
        char *line;

        if (call->discard) {
            unmet = "cancelled";
            break;
        }
        if (i && !mcp_call_time_left(call)) {
            unmet = "25 minutes is the most one call may take";
            again = i + 1;
            break;
        }
        i++;
        qdict_del(a, "vm");     /* the bridge's, checked to be this VM */
        g_free(call->step);
        call->step = g_strdup_printf("(%d of %d: %s)", i, n, tool);
        call->quiet_runstate = false;
        rsp = mcp_run_tool(call, tool, a, &err);
        qerr = rsp ? qdict_get_qdict(rsp, "error") : NULL;
        if (!rsp || qerr) {
            g_ptr_array_add(lines, g_strdup_printf(
                "%d. %s failed: %s", i, tool,
                err ? err : qdict_get_try_str(qerr, "desc")));
            qobject_unref(rsp);
            failed = true;
            break;
        }
        line = mcp_step_line(i, tool, qdict_get(rsp, "return"), &images);
        g_ptr_array_add(lines, g_strchomp(line));
        qobject_unref(last);
        last = qobject_to(QDict, qdict_get(rsp, "return"));
        if (last) {
            qobject_ref(last);
            unmet = mcp_step_unmet(last);
        }
        qobject_unref(rsp);
        if (unmet) {
            late = !mcp_call_time_left(call);
            break;
        }
    }
    g_clear_pointer(&call->step, g_free);

    if (failed) {
        g_string_printf(text, "step %d of %d failed", i, n);
    } else if (unmet && i < n) {
        g_string_printf(text, "stopped after step %d of %d: %s", i, n, unmet);
    } else if (unmet) {
        g_string_printf(text, "step %d of %d: %s", i, n, unmet);
    } else if (n == 1) {
        g_string_printf(text, "ran the 1 step");
    } else {
        g_string_printf(text, "ran all %d steps", n);
    }
    if (late) {
        /* its own summary says how to go on */
        g_string_append(text, ", at the 25-minute limit of one call");
    }
    if (i < n) {
        g_string_append(text, ", so the rest did not run");
    }
    if (again) {
        g_string_append_printf(text, "; call steps again from step %d",
                               again);
    }
    for (j = 0; j < lines->len; j++) {
        g_string_append_c(text, '\n');
        mcp_append_cut(text, g_ptr_array_index(lines, j),
                       j + 1 < lines->len ? MCP_STEP_SUMMARY_MAX : SIZE_MAX);
    }
    content = qlist_new();
    qlist_append(content, mcp_text_block(text->str));
    if (images) {
        qdict_put(rest, "images", mcp_image_blocks(content, images));
    }
    qdict_put_int(rest, "steps-done", failed ? i - 1 : i);
    qdict_put_int(rest, "steps", n);
    if (last && !failed) {
        /* what the last step said, but its summary and images */
        QDict *l = qdict_clone_shallow(last);

        qdict_del(l, "summary");
        qdict_del(l, "images");
        qdict_put(rest, "last", l);
    }
    events = mcp_take_events(call->mon);
    if (events) {
        qdict_put(rest, "events", events);
    }
    mcp_add_json_block(content, rest);
    return mcp_tool_result(content, failed);
}

static void coroutine_fn mcp_call_co(void *opaque)
{
    McpCall *call = opaque;
    MonitorMCP *mcp = call->mon;
    QDict *result = NULL;
    QDict *rsp = NULL;

    g_hash_table_insert(mcp_calls_by_co, qemu_coroutine_self(), call);
    trace_mcp_tool_start(mcp, call->tool);

    if (!strcmp(call->tool, "steps")) {
        result = mcp_steps(call, call->args);
    } else {
        g_autofree char *error = NULL;

        rsp = mcp_run_tool(call, call->tool, call->args, &error);
        result = rsp ? mcp_result_from_rsp(mcp, rsp) : mcp_tool_error(error);
    }
    qobject_unref(rsp);

    g_hash_table_remove(mcp_calls_by_co, qemu_coroutine_self());
    trace_mcp_tool_done(mcp, call->tool, call->discard,
                        qdict_get_bool(result, "isError"));
    if (!call->discard) {
        mcp_reply(mcp, call->id, QOBJECT(result));
    } else {
        qobject_unref(result);
    }
    mcp_call_free(call);
}

/* Requests */

static bool mcp_tool_exists(const char *name)
{
    const McpToolDef *t;
    const QmpCommand *cmd;

    if (!strcmp(name, "qmp") || !strcmp(name, "steps")) {
        return true;
    }
#ifdef CONFIG_HMP
    if (!strcmp(name, "hmp")) {
        return true;
    }
#endif
    for (t = mcp_tools; t->name; t++) {
        if (!strcmp(t->name, name)) {
            cmd = qmp_find_command(&qmp_commands, name);
            return cmd && cmd->enabled;
        }
    }
    return false;
}

static QList *mcp_tools_list(void)
{
    QList *list = qlist_new();
    const McpToolDef *t;

    for (t = mcp_tools; t->name; t++) {
        if (mcp_tool_exists(t->name)) {
            qlist_append_obj(list, qobject_from_json(t->json, &error_abort));
        }
    }
#ifdef CONFIG_HMP
    qlist_append_obj(list, qobject_from_json(mcp_tool_hmp, &error_abort));
#endif
    qlist_append_obj(list, qobject_from_json(mcp_tool_qmp, &error_abort));
    qlist_append_obj(list, qobject_from_json(mcp_tool_steps, &error_abort));
    return list;
}

static void mcp_initialize(MonitorMCP *mcp, QObject *id, QDict *params)
{
    const char *asked = params ? qdict_get_try_str(params, "protocolVersion")
                               : NULL;
    QDict *result = qdict_new();
    QDict *caps = qdict_new();
    QDict *tools = qdict_new();
    QDict *info = qdict_new();

    qdict_put_str(result, "protocolVersion",
                  mcp_protocol_supported(asked) ? asked
                                                : MCP_PROTOCOL_LATEST);
    qdict_put_bool(tools, "listChanged", false);
    qdict_put(caps, "tools", tools);
    qdict_put(result, "capabilities", caps);
    qdict_put_str(info, "name", "lemu");
    qdict_put_str(info, "version", QEMU_VERSION "-mcp");
    qdict_put(result, "serverInfo", info);
    qdict_put_str(result, "instructions", mcp_instructions);
    mcp->initialized = true;
    mcp_reply(mcp, id, QOBJECT(result));
}

static void mcp_tools_call(MonitorMCP *mcp, QObject *id, QDict *params)
{
    const char *name = params ? qdict_get_try_str(params, "name") : NULL;
    QDict *args = params ? qdict_get_qdict(params, "arguments") : NULL;
    QDict *meta = params ? qdict_get_qdict(params, "_meta") : NULL;
    QObject *token = meta ? qdict_get(meta, "progressToken") : NULL;
    McpCall *call;
    Coroutine *co;

    if (!name || !mcp_tool_exists(name)) {
        g_autofree char *text = g_strdup_printf("unknown tool '%s'",
                                                name ? name : "");
        mcp_reply_error(mcp, id, MCP_INVALID_PARAMS, text, NULL);
        return;
    }

    call = g_new0(McpCall, 1);
    call->mon = mcp;
    object_ref(OBJECT(mcp));
    call->id = qobject_ref(id);
    call->tool = g_strdup(name);
    call->args = args ? qobject_ref(args) : qdict_new();
    call->start_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    qemu_input_ctx_init(&call->ctx, mcp);
    if (token) {
        call->progress_token = qobject_ref(token);
        call->progress_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                            mcp_progress_tick, call);
        timer_mod(call->progress_timer, call->start_ms + MCP_PROGRESS_MS);
    }
    QTAILQ_INSERT_TAIL(&mcp->calls, call, node);

    co = qemu_coroutine_create(mcp_call_co, call);
    aio_co_enter(iohandler_get_aio_context(), co);
}

static void mcp_cancel_call(McpCall *call)
{
    call->discard = true;
    qemu_input_ctx_cancel(&call->ctx);
}

static void mcp_notification(MonitorMCP *mcp, const char *method,
                             QDict *params)
{
    if (!strcmp(method, "notifications/cancelled") && params) {
        QObject *id = qdict_get(params, "requestId");
        McpCall *call;

        QTAILQ_FOREACH(call, &mcp->calls, node) {
            if (id && qobject_is_equal(id, call->id)) {
                mcp_cancel_call(call);
            }
        }
    }
    /* notifications/initialized and others need no action */
}

static void mcp_handle_message(void *opaque, QObject *msg, Error *err)
{
    MonitorMCP *mcp = opaque;
    QDict *dict = qobject_to(QDict, msg);
    QDict *params;
    QObject *id;
    const char *method;
    const char *version;

    if (err) {
        mcp_reply_error(mcp, NULL, MCP_PARSE_ERROR,
                        error_get_pretty(err), NULL);
        error_free(err);
        return;
    }
    if (!dict) {
        mcp_reply_error(mcp, NULL, MCP_INVALID_REQUEST,
                        qobject_type(msg) == QTYPE_QLIST
                        ? "batches are not supported"
                        : "a message must be a JSON object", NULL);
        goto out;
    }

    method = qdict_get_try_str(dict, "method");
    id = qdict_get(dict, "id");
    params = qdict_get_qdict(dict, "params");
    trace_mcp_request(mcp, method ? method : "(response)");

    if (!method) {
        /* a response; lemu never sends requests */
        goto out;
    }
    if (!id) {
        mcp_notification(mcp, method, params);
        goto out;
    }

    version = mcp_meta_protocol(params);
    if (!mcp->initialized && version && !mcp_protocol_supported(version)) {
        QDict *data = qdict_new();

        qdict_put(data, "supported", mcp_protocol_list());
        qdict_put_str(data, "requested", version);
        mcp_reply_error(mcp, id, MCP_UNSUPPORTED_VERSION,
                        "Unsupported protocol version", QOBJECT(data));
        goto out;
    }

    if (!strcmp(method, "initialize")) {
        mcp_initialize(mcp, id, params);
    } else if (!strcmp(method, "ping")) {
        mcp_reply(mcp, id, QOBJECT(qdict_new()));
    } else if (!strcmp(method, "tools/list")) {
        QDict *result = qdict_new();

        qdict_put(result, "tools", mcp_tools_list());
        mcp_reply(mcp, id, QOBJECT(result));
    } else if (!strcmp(method, "tools/call")) {
        mcp_tools_call(mcp, id, params);
    } else {
        g_autofree char *text = g_strdup_printf("unknown method '%s'",
                                                method);
        mcp_reply_error(mcp, id, MCP_METHOD_NOT_FOUND, text, NULL);
    }
out:
    qobject_unref(msg);
}

/* Monitor plumbing */

static void monitor_mcp_read(void *opaque, const uint8_t *buf, int size)
{
    MonitorMCP *mcp = opaque;

    json_message_parser_feed(&mcp->parser, (const char *)buf, size);
}

static void monitor_mcp_event(void *opaque, QEMUChrEvent event)
{
    MonitorMCP *mcp = opaque;
    McpCall *call;
    int i;

    switch (event) {
    case CHR_EVENT_OPENED:
    case CHR_EVENT_CLOSED:
        /* a new client starts a new session */
        QTAILQ_FOREACH(call, &mcp->calls, node) {
            mcp_cancel_call(call);
        }
        qemu_input_release_owner(mcp);
        json_message_parser_destroy(&mcp->parser);
        json_message_parser_init(&mcp->parser, mcp_handle_message, mcp,
                                 NULL);
        mcp->initialized = false;
        for (i = 0; i < mcp->n_events; i++) {
            g_free(mcp->events[i].name);
        }
        mcp->n_events = 0;
        if (event == CHR_EVENT_CLOSED) {
            WITH_QEMU_LOCK_GUARD(&mcp->parent_obj.mon_lock) {
                g_string_truncate(mcp->parent_obj.outbuf, 0);
            }
        }
        break;
    default:
        break;
    }
}

static bool monitor_mcp_requires_iothread(const Monitor *mon)
{
    return false;
}

static void monitor_mcp_accept_input(Monitor *mon)
{
    WITH_QEMU_LOCK_GUARD(&mon->mon_lock) {
        qemu_chr_fe_accept_input(&mon->chr);
    }
}

static void monitor_mcp_complete(UserCreatable *uc, Error **errp)
{
    MonitorMCP *mcp = MONITOR_MCP(uc);
    UserCreatableClass *ucc_parent =
        USER_CREATABLE_CLASS(
            object_class_get_parent(
                OBJECT_CLASS(MONITOR_MCP_GET_CLASS(mcp))));
    ERRP_GUARD();

    ucc_parent->complete(uc, errp);
    if (*errp) {
        return;
    }
    qemu_chr_fe_set_handlers(&mcp->parent_obj.chr, monitor_can_read,
                             monitor_mcp_read, monitor_mcp_event,
                             NULL, mcp, NULL, true);
    monitor_list_append(&mcp->parent_obj);
}

static bool monitor_mcp_prepare_delete(UserCreatable *uc, Error **errp)
{
    Monitor *mon = MONITOR(uc);
    MonitorMCP *mcp = MONITOR_MCP(uc);
    McpCall *call;

    if (!QTAILQ_EMPTY(&mcp->calls)) {
        QTAILQ_FOREACH(call, &mcp->calls, node) {
            mcp_cancel_call(call);
        }
        error_setg(errp, "the MCP monitor still has calls in flight");
        return false;
    }
    WITH_QEMU_LOCK_GUARD(&monitor_lock) {
        QTAILQ_REMOVE(&mon_list, mon, entry);
    }
    WITH_QEMU_LOCK_GUARD(&mon->mon_lock) {
        monitor_cancel_out_watch(mon);
    }
    qemu_chr_fe_set_handlers(&mon->chr, NULL, NULL, NULL, NULL,
                             NULL, NULL, true);
    qemu_input_release_owner(mcp);
    return true;
}

static void monitor_mcp_init(Object *obj)
{
    MonitorMCP *mcp = MONITOR_MCP(obj);

    QTAILQ_INIT(&mcp->calls);
    json_message_parser_init(&mcp->parser, mcp_handle_message, mcp, NULL);
    if (!mcp_calls_by_co) {
        mcp_calls_by_co = g_hash_table_new(NULL, NULL);
    }
}

static void monitor_mcp_finalize(Object *obj)
{
    MonitorMCP *mcp = MONITOR_MCP(obj);
    int i;

    json_message_parser_destroy(&mcp->parser);
    for (i = 0; i < mcp->n_events; i++) {
        g_free(mcp->events[i].name);
    }
}

static void monitor_mcp_class_init(ObjectClass *cls, const void *data)
{
    MonitorClass *moncls = MONITOR_CLASS(cls);
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(cls);

    moncls->emit_event = monitor_mcp_emit_event;
    moncls->requires_iothread = monitor_mcp_requires_iothread;
    moncls->accept_input = monitor_mcp_accept_input;
    ucc->complete = monitor_mcp_complete;
    ucc->prepare_delete = monitor_mcp_prepare_delete;
}

void monitor_new_mcp(const char *id, const char *chardev_id, Error **errp)
{
    g_autofree char *autoid = id ? NULL : monitor_compat_id();

    object_new_with_props(TYPE_MONITOR_MCP, object_get_objects_root(),
                          id ? id : autoid, errp,
                          "chardev", chardev_id, NULL);
}
