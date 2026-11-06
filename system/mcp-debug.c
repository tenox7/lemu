/*
 * Debug tools for MCP clients: run to an address, single steps, a
 * profile of where the CPUs run, and memory writes (see qapi/mcp.json).
 * They do what agents used to do with GDB scripts on the GDB stub.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "exec/gdbstub.h"
#include "exec/replay-core.h"
#include "exec/target_page.h"
#include "hw/core/cpu.h"
#include "monitor/mcp.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-mcp.h"
#include "qemu/coroutine.h"
#include "qemu/ctype.h"
#include "qemu/cutils.h"
#include "qemu/main-loop.h"
#include "qemu/bswap.h"
#include "qemu/rcu.h"
#include "qemu/target-info.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/cpus.h"
#include "system/hw_accel.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "system/tcg.h"
#include "ui/input.h"
#ifdef CONFIG_TCG
#include "accel/tcg/cpu-ops.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"
#include "exec/excp-watch.h"
#include "exec/tb-flush.h"
#include "exec/tb-history.h"
#include "exec/watchpoint.h"
#include "qemu/host-utils.h"
#endif

#define WRITE_MAX_BYTES         1024

/* "0x..." is hex, anything else decimal, as qapi/mcp.json says */
static bool mcp_parse_address(const char *s, const char *what,
                              uint64_t *addr, Error **errp)
{
    int base = 10;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
    }
    if (s[0] == '-' || qemu_strtou64(s, NULL, base, addr) < 0) {
        error_setg(errp, "%s '%s' is not a number: write hex with 0x, or "
                   "decimal", what, s);
        return false;
    }
    return true;
}

/*
 * The CPU asked for, else the one that stopped last in a debug tool,
 * else the first.
 */
static int mcp_debug_last_cpu = -1;

static CPUState *mcp_debug_cpu(bool has_cpu, int64_t index, Error **errp)
{
    CPUState *cpu = NULL;

    if (has_cpu) {
        if (index >= 0 && index <= INT_MAX) {
            cpu = qemu_get_cpu(index);
        }
        if (!cpu) {
            error_setg(errp, "there is no CPU %" PRId64, index);
        }
        return cpu;
    }
    if (mcp_debug_last_cpu >= 0) {
        cpu = qemu_get_cpu(mcp_debug_last_cpu);
    }
    cpu = cpu ? cpu : first_cpu;
    if (!cpu) {
        error_setg(errp, "this VM has no CPU");
    }
    return cpu;
}

static char *mcp_hex(const uint8_t *buf, size_t len)
{
    char *s = g_malloc(len * 2 + 1);
    size_t i;

    for (i = 0; i < len; i++) {
        sprintf(s + i * 2, "%02x", buf[i]);
    }
    s[len * 2] = '\0';
    return s;
}

/* Hex bytes, with spaces or commas between groups and 0x before them */
static GByteArray *mcp_parse_bytes(const char *s, Error **errp)
{
    g_autoptr(GByteArray) b = g_byte_array_new();
    g_auto(GStrv) groups = g_strsplit_set(s, " \t\r\n,", -1);
    int i;

    for (i = 0; groups[i]; i++) {
        const char *g = groups[i];
        size_t len, j;

        if (g[0] == '0' && (g[1] == 'x' || g[1] == 'X')) {
            g += 2;
        }
        len = strlen(g);
        if (len % 2) {
            error_setg(errp, "'%s' is not whole bytes: write two hex digits "
                       "for each byte", groups[i]);
            return NULL;
        }
        for (j = 0; j < len; j += 2) {
            int hi = g_ascii_xdigit_value(g[j]);
            int lo = g_ascii_xdigit_value(g[j + 1]);
            uint8_t v = hi << 4 | lo;

            if (hi < 0 || lo < 0) {
                error_setg(errp, "'%s' is not hex", groups[i]);
                return NULL;
            }
            g_byte_array_append(b, &v, 1);
        }
    }
    if (!b->len || b->len > WRITE_MAX_BYTES) {
        error_setg(errp, "give 1 to %d bytes, not %u", WRITE_MAX_BYTES,
                   b->len);
        return NULL;
    }
    return g_steal_pointer(&b);
}

/*
 * Where the bytes at @addr live: the address space, the physical
 * address and how many of @len bytes stay in that page and region.
 * Only RAM and ROM; a device could act on the access.
 */
static bool mcp_memory_chunk(CPUState *cpu, bool physical, uint64_t addr,
                             size_t len, AddressSpace **as, hwaddr *pa,
                             MemTxAttrs *attrs, size_t *n, Error **errp)
{
    MemoryRegion *mr;
    hwaddr xlat, l;

    if (physical) {
        *as = &address_space_memory;
        *pa = addr;
        *attrs = MEMTXATTRS_UNSPECIFIED;
        *n = len;
    } else {
        TranslateForDebugResult t;
        uint64_t page;

        if (!cpu_translate_for_debug(cpu, addr, &t)) {
            error_setg(errp, "0x%" PRIx64 " is not mapped (CPU %d)", addr,
                       cpu->cpu_index);
            return false;
        }
        page = t.lg_page_size < 64 ? 1ULL << t.lg_page_size : 0;
        *as = cpu_get_address_space(cpu, cpu_asidx_from_attrs(cpu, t.attrs));
        *pa = t.physaddr;
        *attrs = t.attrs;
        *n = page ? MIN(len, page - (addr & (page - 1))) : len;
    }

    RCU_READ_LOCK_GUARD();
    l = *n;
    mr = address_space_translate(*as, *pa, &xlat, &l, true, *attrs);
    if (!memory_region_supports_direct_access(mr) ||
        memory_region_is_ram_device(mr)) {
        error_setg(errp, "0x%" PRIx64 " is not RAM or ROM but %s; devices "
                   "are never written", addr, memory_region_name(mr));
        return false;
    }
    *n = MIN(*n, l);
    return true;
}

McpMemoryWriteResult *qmp_memory_write(const char *address,
                                       const char *bytes,
                                       bool has_physical, bool physical,
                                       bool has_cpu, int64_t cpu_index,
                                       Error **errp)
{
    g_autoptr(GByteArray) data = NULL;
    g_autofree uint8_t *before = NULL;
    g_autofree uint8_t *after = NULL;
    McpMemoryWriteResult *res;
    CPUState *cpu = NULL;
    AddressSpace *as;
    MemTxAttrs attrs;
    uint64_t addr;
    hwaddr pa, first_pa = 0;
    size_t done, n;

    physical = has_physical && physical;
    if (!mcp_parse_address(address, "address", &addr, errp)) {
        return NULL;
    }
    data = mcp_parse_bytes(bytes, errp);
    if (!data) {
        return NULL;
    }
    if (!physical) {
        cpu = mcp_debug_cpu(has_cpu, cpu_index, errp);
        if (!cpu) {
            return NULL;
        }
        cpu_synchronize_state(cpu);
    }
    before = g_malloc(data->len);
    after = g_malloc(data->len);

    /* check it all and keep the old bytes, then write */
    for (done = 0; done < data->len; done += n) {
        if (!mcp_memory_chunk(cpu, physical, addr + done, data->len - done,
                              &as, &pa, &attrs, &n, errp)) {
            return NULL;
        }
        if (!done) {
            first_pa = pa;
        }
        address_space_read(as, pa, attrs, before + done, n);
    }
    for (done = 0; done < data->len; done += n) {
        if (!mcp_memory_chunk(cpu, physical, addr + done, data->len - done,
                              &as, &pa, &attrs, &n, errp)) {
            return NULL;
        }
        /* RAM, and ROM too; drops translated code made from the old bytes */
        address_space_write_rom(as, pa, attrs, data->data + done, n);
        address_space_read(as, pa, attrs, after + done, n);
    }

    res = g_new0(McpMemoryWriteResult, 1);
    res->before = mcp_hex(before, data->len);
    res->after = mcp_hex(after, data->len);
    if (physical) {
        res->summary = g_strdup_printf("wrote %u byte%s at physical 0x%"
                                       PRIx64, data->len,
                                       data->len == 1 ? "" : "s", addr);
    } else {
        res->summary = g_strdup_printf("wrote %u byte%s at 0x%" PRIx64
                                       " (physical 0x%" HWADDR_PRIx ")",
                                       data->len, data->len == 1 ? "" : "s",
                                       addr, first_pa);
    }
    if (memcmp(after, data->data, data->len)) {
        g_autofree char *old = res->summary;

        res->summary = g_strdup_printf("%s, but it reads back different "
                                       "bytes (see after)", old);
    }
    return res;
}

#ifdef CONFIG_TCG

#define RUN_DEFAULT_MS          30000
#define RUN_HITS_MAX            1000000
#define RUN_ADDRS_MAX           64
#define RUN_WATCHES_MAX         16
#define RUN_LISTED              8       /* addresses a summary names */
#define STEP_MAX                10000
#define STEP_SHOWN              200
#define STEP_WAIT_MS            1000
#define MEMORY_WORDS            8
#define MEMORY_WORDS_MAX        256
#define MEMORY_RANGES_MAX       16
#define PROFILE_DEFAULT_MS      5000
#define PROFILE_MIN_MS          100
#define PROFILE_TOP             20
#define PROFILE_TOP_MAX         100

/*
 * One debug-run or debug-step at a time.  Its context is woken when the
 * VM stops, so it need not poll.
 */
static struct {
    bool busy;
    QemuInputCtx *ctx;
    QEMUBH *wake_bh;
} mcp_debug;

static void mcp_debug_wake_bh(void *opaque)
{
    if (mcp_debug.ctx) {
        qemu_co_sleep_wake(&mcp_debug.ctx->sleep);
    }
}

/*
 * Wake the tool from a BH, not from here: the VM is in the middle of
 * stopping, and the other handlers have yet to run.
 */
static void mcp_debug_vm_state(void *opaque, bool running, RunState state)
{
    if (!running && mcp_debug.ctx) {
        qemu_bh_schedule(mcp_debug.wake_bh);
    }
}

static bool mcp_debug_begin(QemuInputCtx *ctx, Error **errp)
{
    RunState state = runstate_get();

    if (!tcg_enabled()) {
        error_setg(errp, "the debug tools need TCG (-accel tcg)");
        return false;
    }
    if (replay_mode != REPLAY_MODE_NONE) {
        error_setg(errp, "the debug tools don't work while QEMU records or "
                   "replays");
        return false;
    }
    if (mcp_debug.busy) {
        error_setg(errp, "another debug-run or debug-step is running");
        return false;
    }
    switch (state) {
    case RUN_STATE_RUNNING:
    case RUN_STATE_PAUSED:
    case RUN_STATE_DEBUG:
    case RUN_STATE_PRELAUNCH:
        break;
    default:
        error_setg(errp, "the VM is %s; the debug tools need it running or "
                   "paused", RunState_str(state));
        return false;
    }
    if (!mcp_debug.wake_bh) {
        mcp_debug.wake_bh = qemu_bh_new(mcp_debug_wake_bh, NULL);
        qemu_add_vm_change_state_handler(mcp_debug_vm_state, NULL);
    }
    mcp_debug.busy = true;
    mcp_debug.ctx = ctx;
    mcp_quiet_runstate_events();
    return true;
}

static void mcp_debug_end(void)
{
    mcp_debug.busy = false;
    mcp_debug.ctx = NULL;
}

typedef struct McpDebugBh {
    void (*fn)(void *opaque);
    void *opaque;
    Coroutine *co;
} McpDebugBh;

static void mcp_debug_bh(void *opaque)
{
    McpDebugBh *b = opaque;

    b->fn(b->opaque);
    aio_co_wake(b->co);
}

/*
 * Run @fn in the main loop, outside this coroutine: vm_stop() waits for
 * the vCPUs and flushes the disks.  QMP runs its commands that are not
 * coroutines the same way.
 */
static void coroutine_fn mcp_debug_outside(void (*fn)(void *opaque),
                                           void *opaque)
{
    McpDebugBh b = { fn, opaque, qemu_coroutine_self() };

    aio_bh_schedule_oneshot(qemu_get_aio_context(), mcp_debug_bh, &b);
    qemu_coroutine_yield();
}

static void mcp_debug_stop(void *opaque)
{
    vm_stop(*(RunState *)opaque);
}

static void mcp_debug_start(void *opaque)
{
    vm_start();
}

/* Wait until the VM stops.  False when the time ran out or on cancel. */
static bool coroutine_fn mcp_debug_wait_stop(QemuInputCtx *ctx, int64_t end,
                                             CPUState *step_cpu)
{
    int halted = 0;

    while (runstate_is_running()) {
        int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

        if (ctx->cancelled || now >= end) {
            return false;
        }
        /*
         * A step that halts the CPU waits for an interrupt, and a step
         * keeps interrupts away.
         */
        if (step_cpu) {
            halted = qatomic_read(&step_cpu->halted) ? halted + 1 : 0;
            if (halted > 1) {
                return false;
            }
        }
        qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME,
                                  MIN(step_cpu ? 10 : 1000, end - now)
                                  * SCALE_MS);
    }
    return true;
}

typedef enum {
    STEP_DONE,          /* the VM stopped after one instruction */
    STEP_HALTED,        /* the CPU halted; the VM was stopped */
    STEP_STOPPED,       /* the VM stopped for another reason */
    STEP_CANCELLED,     /* the VM was stopped */
    STEP_NOT_STARTED,
} McpStepResult;

typedef struct McpStep {
    CPUState *cpu;
    int ret;
} McpStep;

/* What gdbstub does to step: only this CPU runs, until its debug stop */
static void mcp_debug_step_start(void *opaque)
{
    McpStep *st = opaque;

    st->ret = vm_prepare_start(true);
    if (st->ret) {
        return;
    }
    cpu_single_step(st->cpu, SSTEP_ENABLE | SSTEP_NOIRQ | SSTEP_NOTIMER);
    cpu_resume(st->cpu);
    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, true);
}

/* One instruction on @cpu, with the VM stopped before and after */
static McpStepResult coroutine_fn mcp_debug_step(QemuInputCtx *ctx,
                                                 CPUState *cpu)
{
    McpStep st = { .cpu = cpu };
    RunState debug = RUN_STATE_DEBUG;
    bool stopped;

    mcp_debug_outside(mcp_debug_step_start, &st);
    if (st.ret) {
        return STEP_NOT_STARTED;
    }
    stopped = mcp_debug_wait_stop(ctx, qemu_clock_get_ms(QEMU_CLOCK_REALTIME)
                                  + STEP_WAIT_MS, cpu);
    if (!stopped) {
        mcp_debug_outside(mcp_debug_stop, &debug);
    }
    cpu_single_step(cpu, 0);
    if (!stopped) {
        return ctx->cancelled ? STEP_CANCELLED : STEP_HALTED;
    }
    return runstate_check(RUN_STATE_DEBUG) ? STEP_DONE : STEP_STOPPED;
}

/* The address of a breakpoint at @addr, as the CPU compares it */
static vaddr mcp_debug_bp_pc(CPUState *cpu, uint64_t addr)
{
    if (cpu->cc->gdb_adjust_breakpoint) {
        return cpu->cc->gdb_adjust_breakpoint(cpu, addr);
    }
    return addr;
}

typedef struct McpWatchSeen {
    CPUState *cpu;
    uint64_t addr;
    uint64_t pc;
    bool write;
} McpWatchSeen;

/* Memory that a debug tool watches */
typedef struct McpWatchArea {
    uint64_t addr;
    uint64_t len;
    int flags;                  /* BP_MEM_READ, BP_MEM_WRITE or both */
} McpWatchArea;

/* An exception a CPU stopped for */
typedef struct McpExcpSeen {
    CPUState *cpu;
    int32_t number;
    uint64_t from;              /* the instruction that took it */
    uint64_t to;                /* the guest's handler */
} McpExcpSeen;

typedef struct McpBreak {
    CPUState *only;
    int n_addrs;                /* the addresses to stop at */
    uint64_t addrs[RUN_ADDRS_MAX];
    bool has_mask;
    uint64_t mask;              /* the bits of each address compared */
    int hits;                   /* CPUs there as the time ran out */
    int n_watches;              /* the watched memory */
    McpWatchArea watches[RUN_WATCHES_MAX];
    int whits;                  /* CPUs that touched it as the time ran out */
    McpWatchSeen seen;
    bool has_excp;              /* the exceptions to stop at */
    CPUExcpWatch excp;          /* each CPU watched gets a copy */
    int ehits;                  /* CPUs that took one as the time ran out */
    McpExcpSeen eseen;
} McpBreak;

/*
 * Which address of @b @cpu stands at, about to run the instruction
 * there, or -1 when none or the CPU is not watched.  A halted CPU there
 * has not got there yet: it goes on with an interrupt.
 */
static int mcp_debug_addr_index(McpBreak *b, CPUState *cpu)
{
    vaddr pc;
    int i;

    if (!b->n_addrs || (b->only && cpu != b->only) || cpu->halted) {
        return -1;
    }
    pc = cpu->cc->get_pc(cpu);
    for (i = 0; i < b->n_addrs; i++) {
        if (((pc ^ mcp_debug_bp_pc(cpu, b->addrs[i])) & b->mask) == 0) {
            return i;
        }
    }
    return -1;
}

static bool mcp_debug_is_at(McpBreak *b, CPUState *cpu)
{
    return mcp_debug_addr_index(b, cpu) >= 0;
}

/* The address a CPU of @b stopped at, as the summary shows it */
static uint64_t mcp_debug_reached(McpBreak *b, CPUState *cpu)
{
    int i = mcp_debug_addr_index(b, cpu);

    return b->has_mask || i < 0 ? cpu->cc->get_pc(cpu) : b->addrs[i];
}

/* Whether debug-trace counts the arrivals by address */
static bool mcp_debug_by_addr(McpBreak *b)
{
    return b->has_mask || b->n_addrs > 1;
}

/*
 * "a", "a or b", "a, b or c"; past RUN_LISTED of them, the first ones
 * and "or 9 more".  @items are freed.
 */
static void mcp_append_list(GString *s, GPtrArray *items, const char *conj)
{
    guint n = items->len;
    guint shown = n > RUN_LISTED ? RUN_LISTED - 1 : n;
    guint i;

    for (i = 0; i < shown; i++) {
        if (i && i == n - 1) {
            g_string_append_printf(s, " %s ", conj);
        } else if (i) {
            g_string_append(s, ", ");
        }
        g_string_append(s, g_ptr_array_index(items, i));
    }
    if (shown < n) {
        g_string_append_printf(s, " %s %u more", conj, n - shown);
    }
    g_ptr_array_free(items, true);
}

/* "0x1000", "0x1000 or 0x2000", then " (mask 0xffff)" */
static void mcp_debug_append_addrs(GString *s, McpBreak *b, const char *conj)
{
    GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
    int i;

    for (i = 0; i < b->n_addrs; i++) {
        g_ptr_array_add(items, g_strdup_printf("0x%" PRIx64, b->addrs[i]));
    }
    mcp_append_list(s, items, conj);
    if (b->has_mask) {
        g_string_append_printf(s, " (mask 0x%" PRIx64 ")", b->mask);
    }
}

/*
 * Breakpoints are checked when a CPU looks for the next block of
 * translated code, and chained blocks don't look: flush them all, so
 * that every way to the addresses looks again.  Watchpoints flush the
 * TLB for their pages themselves.  The VM is stopped.
 */
static void mcp_debug_insert(McpBreak *b)
{
    CPUState *cpu;
    int i;

    CPU_FOREACH(cpu) {
        if (b->only && cpu != b->only) {
            continue;
        }
        for (i = 0; i < b->n_addrs; i++) {
            CPUBreakpoint *bp;

            cpu_breakpoint_insert(cpu, b->addrs[i], BP_GDB, &bp);
            bp->mask = b->mask;
        }
        for (i = 0; i < b->n_watches; i++) {
            McpWatchArea *w = &b->watches[i];

            cpu_watchpoint_insert(cpu, w->addr, w->len, BP_GDB | w->flags,
                                  NULL);
        }
        if (b->has_excp) {
            g_free(cpu->excp_watch);
            cpu->excp_watch = g_memdup2(&b->excp, sizeof(b->excp));
        }
    }
    if (b->n_addrs) {
        tb_flush__exclusive_or_serial();
    }
}

/*
 * Whether @cpu stopped for an exception, and which in *@seen.  That
 * uses it up.
 */
static bool mcp_excp_take(McpBreak *b, CPUState *cpu, McpExcpSeen *seen)
{
    CPUExcpWatch *w = cpu->excp_watch;

    if (!b->has_excp || !w || !w->hit) {
        return false;
    }
    seen->cpu = cpu;
    seen->number = w->number;
    seen->from = w->from;
    seen->to = w->to;
    w->hit = false;
    return true;
}

/*
 * Our watchpoint on @cpu, found again by what it watches: a GDB client
 * that detaches removes all BP_GDB watchpoints, ours too
 */
static CPUWatchpoint *mcp_watch_find(McpWatchArea *w, CPUState *cpu)
{
    CPUWatchpoint *wp;

    QTAILQ_FOREACH(wp, &cpu->watchpoints, entry) {
        if (wp->vaddr == w->addr && wp->len == w->len &&
            (wp->flags & ~BP_WATCHPOINT_HIT) == (BP_GDB | w->flags)) {
            return wp;
        }
    }
    return NULL;
}

/* Whether @wp on @cpu is one of the watchpoints of @b */
static bool mcp_watch_ours(McpBreak *b, CPUState *cpu, CPUWatchpoint *wp)
{
    int i;

    for (i = 0; i < b->n_watches; i++) {
        if (mcp_watch_find(&b->watches[i], cpu) == wp) {
            return true;
        }
    }
    return false;
}

/* A CPU's hit is dealt with: it must not stop the VM again */
static void mcp_watch_clear_hit(CPUState *cpu, CPUWatchpoint *wp)
{
    wp->flags &= ~BP_WATCHPOINT_HIT;
    cpu->watchpoint_hit = NULL;
    /* raised as the access ended; a single step may have stopped first */
    cpu_reset_interrupt(cpu, CPU_INTERRUPT_DEBUG);
}

/*
 * Whether @cpu touched the watched memory, and what it did in *@seen.
 * Its hit is cleared, as gdbstub does.
 */
static bool mcp_watch_take(McpBreak *b, CPUState *cpu, McpWatchSeen *seen)
{
    CPUWatchpoint *wp = cpu->watchpoint_hit;

    if (!wp || !mcp_watch_ours(b, cpu, wp)) {
        return false;
    }
    seen->cpu = cpu;
    seen->addr = wp->hitaddr;
    seen->pc = wp->hitpc;
    seen->write = wp->flags & BP_WATCHPOINT_HIT_WRITE;
    mcp_watch_clear_hit(cpu, wp);
    return true;
}

/* How many CPUs touched it, with what the first of them did */
static int mcp_watch_hits(McpBreak *b, McpWatchSeen *seen)
{
    McpWatchSeen s;
    CPUState *cpu;
    int n = 0;

    CPU_FOREACH(cpu) {
        if (mcp_watch_take(b, cpu, &s) && !n++) {
            *seen = s;
        }
    }
    return n;
}

/* Our breakpoint at @addr on @cpu, found again by what it compares */
static CPUBreakpoint *mcp_bp_find(McpBreak *b, uint64_t addr, CPUState *cpu)
{
    vaddr pc = mcp_debug_bp_pc(cpu, addr);
    CPUBreakpoint *bp;

    QTAILQ_FOREACH(bp, &cpu->breakpoints, entry) {
        if (bp->pc == pc && bp->mask == b->mask && bp->flags == BP_GDB) {
            return bp;
        }
    }
    return NULL;
}

static void mcp_debug_remove(McpBreak *b)
{
    CPUState *cpu;
    int i;

    CPU_FOREACH(cpu) {
        if (b->only && cpu != b->only) {
            continue;
        }
        for (i = 0; i < b->n_addrs; i++) {
            CPUBreakpoint *bp = mcp_bp_find(b, b->addrs[i], cpu);

            if (bp) {
                cpu_breakpoint_remove_by_ref(cpu, bp);
            }
        }
        for (i = 0; i < b->n_watches; i++) {
            CPUWatchpoint *wp = mcp_watch_find(&b->watches[i], cpu);

            if (wp) {
                /* a hit that has not stopped the VM yet never will */
                if (cpu->watchpoint_hit == wp) {
                    mcp_watch_clear_hit(cpu, wp);
                }
                cpu_watchpoint_remove_by_ref(cpu, wp);
            }
        }
        if (b->has_excp) {
            g_free(cpu->excp_watch);
            cpu->excp_watch = NULL;
        }
    }
}

/*
 * Take them away at the end, with the vCPUs paused if something else
 * started the VM: a CPU reads its exception watch as it runs
 */
static void mcp_debug_remove_paused(void *opaque)
{
    McpBreak *b = opaque;
    bool running = runstate_is_running();

    if (running) {
        pause_all_vcpus();
    }
    mcp_debug_remove(b);
    if (running) {
        resume_all_vcpus();
    }
}

/*
 * CPUs at an address, that touched the watched memory, or that took an
 * exception
 */
static void mcp_debug_late_hits(McpBreak *b)
{
    McpExcpSeen s;
    CPUState *cpu;

    b->whits = mcp_watch_hits(b, &b->seen);
    b->hits = 0;
    b->ehits = 0;
    CPU_FOREACH(cpu) {
        b->hits += mcp_debug_is_at(b, cpu);
        if (mcp_excp_take(b, cpu, &s) && !b->ehits++) {
            b->eseen = s;
        }
    }
}

/*
 * The time ran out with the VM running: take the breakpoints away with
 * the vCPUs paused.  A CPU may have got to the address just now; then
 * the VM stops there, as for any hit.  A CPU that hit the watchpoint
 * has done the access by now (its last instruction runs with
 * interrupts off) and has asked to stop the VM: it stops.  So does a
 * CPU that took an exception.
 */
static void mcp_debug_remove_running(void *opaque)
{
    McpBreak *b = opaque;

    if (!runstate_is_running()) {
        /* it may have stopped for a hit after the wait gave up */
        if (runstate_check(RUN_STATE_DEBUG)) {
            mcp_debug_late_hits(b);
        }
        mcp_debug_remove(b);
        return;
    }
    pause_all_vcpus();
    mcp_debug_late_hits(b);
    mcp_debug_remove(b);
    if (b->hits || b->whits || b->ehits) {
        qemu_system_debug_request();
    } else {
        resume_all_vcpus();
    }
}

/*
 * The GDB register number of @name on @cpu: a name from its GDB target
 * description, or rN for register N when the description has no names
 * for its core registers (alpha, hppa, mips).  -1 when it has none.
 */
static int mcp_reg_number(CPUState *cpu, const char *name)
{
    g_autoptr(GArray) regs = gdb_get_register_list(cpu);
    const char *end;
    unsigned int i, n;

    for (i = 0; i < regs->len; i++) {
        GDBRegDesc *r = &g_array_index(regs, GDBRegDesc, i);

        if (r->name && !g_ascii_strcasecmp(r->name, name)) {
            return r->gdb_reg;
        }
    }
    if (!cpu->cc->gdb_core_xml_file && (name[0] == 'r' || name[0] == 'R') &&
        qemu_isdigit(name[1]) && !qemu_strtoui(name + 1, &end, 10, &n) &&
        !*end && n < cpu->cc->gdb_num_core_regs) {
        return n;
    }
    return -1;
}

/*
 * GDB's bytes of a register as a number, when it has 1 to 8 bytes: in
 * the byte order the CPU runs in now, as GDB wants it
 */
static bool mcp_reg_decode(CPUState *cpu, const GByteArray *b, uint64_t *val)
{
    bool be = cpu_internal_is_big_endian(cpu);
    unsigned int i;

    if (b->len != 1 && b->len != 2 && b->len != 4 && b->len != 8) {
        return false;
    }
    *val = 0;
    for (i = 0; i < b->len; i++) {
        *val = *val << 8 | b->data[be ? i : b->len - 1 - i];
    }
    return true;
}

/*
 * The value of register @name of @cpu, when it is an integer: pc is
 * where the debug tools say the CPU is, the others are GDB's
 */
static bool mcp_reg_value(CPUState *cpu, const char *name, uint64_t *val)
{
    g_autoptr(GByteArray) buf = NULL;
    int reg;

    if (!g_ascii_strcasecmp(name, "pc")) {
        *val = cpu->cc->get_pc(cpu);
        return true;
    }
    reg = mcp_reg_number(cpu, name);
    if (reg < 0) {
        return false;
    }
    buf = g_byte_array_new();
    gdb_read_register(cpu, buf, reg);
    return mcp_reg_decode(cpu, buf, val);
}

/* The registers an address may name, for an error */
static char *mcp_reg_names(CPUState *cpu)
{
    g_autoptr(GArray) regs = gdb_get_register_list(cpu);
    GString *s = g_string_new("pc");
    unsigned int i;

    for (i = 0; i < regs->len && i < 64; i++) {
        GDBRegDesc *r = &g_array_index(regs, GDBRegDesc, i);

        if (r->name && g_ascii_strcasecmp(r->name, "pc")) {
            g_string_append_printf(s, ", %s", r->name);
        }
    }
    if (i < regs->len) {
        g_string_append(s, " and more");
    }
    if (!cpu->cc->gdb_core_xml_file) {
        g_string_append_printf(s, "%s r0 to r%d (GDB's numbers: on most CPUs"
                               " rN is the general register N)",
                               regs->len ? " and" : ",",
                               cpu->cc->gdb_num_core_regs - 1);
    }
    return g_string_free(s, false);
}

#define REG_NAME_MAX    32
#define ADDR_TERMS_MAX  4
#define STRING_MAX      256

typedef struct McpAddrTerm {
    char reg[REG_NAME_MAX];     /* a register, or "" for @num */
    uint64_t num;
    bool minus;
} McpAddrTerm;

typedef struct McpMem {
    const char *text;           /* as asked */
    McpAddrTerm term[ADDR_TERMS_MAX];
    int n_terms;
    bool has_reg;
    int words;
    bool physical;
    bool string;
} McpMem;

/*
 * A memory address: numbers and registers of the CPU that stops, added
 * and taken away, such as 0x1000, sp, sp+16 or 0x657500000000+r26 (a
 * space or segment base and an offset).  @cpu is one like that CPU.
 */
static bool mcp_parse_mem_address(CPUState *cpu, const char *s, McpMem *m,
                                  Error **errp)
{
    const char *p = s;
    bool minus = false;

    m->text = s;
    for (;;) {
        McpAddrTerm *t = &m->term[m->n_terms];
        size_t len;
        g_autofree char *word = NULL;
        const char *name;

        if (m->n_terms++ == ADDR_TERMS_MAX) {
            error_setg(errp, "memory address '%s' has more than %d parts",
                       s, ADDR_TERMS_MAX);
            return false;
        }
        while (qemu_isspace(*p)) {
            p++;
        }
        len = strcspn(p, "+- ");
        word = g_strndup(p, len);
        p += len;
        t->minus = minus;
        name = word[0] == '$' ? word + 1 : word;
        if (qemu_isdigit(word[0])) {
            if (!mcp_parse_address(word, "memory address", &t->num, errp)) {
                return false;
            }
        } else if (!cpu) {
            error_setg(errp, "memory address '%s': this VM has no CPU", s);
            return false;
        } else if (!*name || strlen(name) >= REG_NAME_MAX ||
                   (mcp_reg_number(cpu, name) < 0 &&
                    g_ascii_strcasecmp(name, "pc"))) {
            g_autofree char *names = mcp_reg_names(cpu);

            error_setg(errp, "memory address '%s': '%s' is not a number or"
                       " a register; the registers are %s", s, word, names);
            return false;
        } else {
            pstrcpy(t->reg, sizeof(t->reg), name);
            m->has_reg = true;
        }
        while (qemu_isspace(*p)) {
            p++;
        }
        if (!*p) {
            return true;
        }
        minus = *p++ == '-';
    }
}

/*
 * The address @m names, with the registers of @cpu now.  *@bad is a
 * register that has no integer value.
 */
static bool mcp_mem_address(CPUState *cpu, McpMem *m, uint64_t *addr,
                            const char **bad)
{
    int i;

    *addr = 0;
    for (i = 0; i < m->n_terms; i++) {
        McpAddrTerm *t = &m->term[i];
        uint64_t v = t->num;

        if (t->reg[0] && !mcp_reg_value(cpu, t->reg, &v)) {
            *bad = t->reg;
            return false;
        }
        *addr += t->minus ? -v : v;
    }
    return true;
}

static McpMem *mcp_parse_memory(CPUState *cpu, McpDebugMemoryList *list,
                                int *count, Error **errp)
{
    g_autofree McpMem *mem = NULL;
    McpDebugMemoryList *l;
    int n = 0;

    for (l = list; l; l = l->next) {
        n++;
    }
    if (n > MEMORY_RANGES_MAX) {
        error_setg(errp, "ask for at most %d memory ranges",
                   MEMORY_RANGES_MAX);
        return NULL;
    }
    /* and the watched memory */
    mem = g_new0(McpMem, n + RUN_WATCHES_MAX);
    for (l = list, n = 0; l; l = l->next, n++) {
        McpDebugMemory *d = l->value;

        if (!mcp_parse_mem_address(cpu, d->address, &mem[n], errp)) {
            return NULL;
        }
        mem[n].words = d->has_words ? d->words : MEMORY_WORDS;
        if (mem[n].words < 1 || mem[n].words > MEMORY_WORDS_MAX) {
            error_setg(errp, "words must be 1 to %d", MEMORY_WORDS_MAX);
            return NULL;
        }
        mem[n].physical = d->has_physical && d->physical;
        mem[n].string = d->has_string && d->string;
    }
    *count = n;
    return g_steal_pointer(&mem);
}

#define WATCH_LEN               4
#define WATCH_LEN_MAX           4096
#define WATCH_WORDS_SHOWN       16
#define WATCH_WORDS_EACH        4       /* shown of each of several */

/*
 * One area of memory to watch into @b.  Registers are read now, from
 * the CPU the tool shows.
 */
static bool mcp_parse_watch(McpWatch *w, McpBreak *b, Error **errp)
{
    static const int flags[] = {
        [MCP_WATCH_ACCESS_WRITE] = BP_MEM_WRITE,
        [MCP_WATCH_ACCESS_READ] = BP_MEM_READ,
        [MCP_WATCH_ACCESS_ACCESS] = BP_MEM_ACCESS,
    };
    CPUState *cpu = b->only ? b->only : mcp_debug_cpu(false, 0, errp);
    McpWatchArea a;
    McpMem m = { 0 };
    const char *bad = NULL;
    int i;

    if (!cpu) {
        return false;
    }
    if (!mcp_parse_mem_address(cpu, w->address, &m, errp)) {
        return false;
    }
    if (m.has_reg && runstate_is_running()) {
        error_setg(errp, "watch address '%s' reads registers, so it needs"
                   " the VM stopped (debug-step or vm-power pause first)",
                   w->address);
        return false;
    }
    if (!mcp_mem_address(cpu, &m, &a.addr, &bad)) {
        error_setg(errp, "watch address '%s': CPU %d has no integer"
                   " register %s", w->address, cpu->cpu_index, bad);
        return false;
    }
    a.len = w->has_length ? w->length : WATCH_LEN;
    if (a.len < 1 || a.len > WATCH_LEN_MAX) {
        error_setg(errp, "watch length must be 1 to %d", WATCH_LEN_MAX);
        return false;
    }
    if (a.addr + a.len - 1 < a.addr) {
        error_setg(errp, "watched memory runs past the end of the address"
                   " space");
        return false;
    }
    a.flags = flags[w->has_access ? w->access : MCP_WATCH_ACCESS_WRITE];

    /* the same area twice is one watchpoint */
    for (i = 0; i < b->n_watches; i++) {
        McpWatchArea *o = &b->watches[i];

        if (o->addr == a.addr && o->len == a.len && o->flags == a.flags) {
            return true;
        }
    }
    if (b->n_watches == RUN_WATCHES_MAX) {
        error_setg(errp, "watch at most %d areas", RUN_WATCHES_MAX);
        return false;
    }
    b->watches[b->n_watches++] = a;
    return true;
}

/*
 * The memory to watch, one area or several, into @b; into @shown, if
 * given, the memory to show when the VM stops, one McpMem per area
 */
static bool mcp_parse_watches(McpWatches *w, McpBreak *b, McpMem *shown,
                              Error **errp)
{
    McpWatchList one = { 0 };
    McpWatchList *l;
    int i, most;

    if (w->type == QTYPE_QDICT) {
        one.value = &w->u.one;
        l = &one;
    } else {
        l = w->u.several;
        if (!l) {
            error_setg(errp, "give at least one area to watch");
            return false;
        }
    }
    for (; l; l = l->next) {
        if (!mcp_parse_watch(l->value, b, errp)) {
            return false;
        }
    }
    if (!shown) {
        return true;
    }
    most = b->n_watches > 1 ? WATCH_WORDS_EACH : WATCH_WORDS_SHOWN;
    for (i = 0; i < b->n_watches; i++) {
        McpWatchArea *a = &b->watches[i];

        shown[i].n_terms = 1;
        shown[i].term[0].num = a->addr & ~3ULL;
        shown[i].words = MIN(DIV_ROUND_UP(a->len + (a->addr & 3), 4), most);
    }
    return true;
}

/*
 * The C string at @addr, quoted, up to STRING_MAX bytes; NULL when not
 * a byte of it can be read
 */
static char *mcp_read_string(CPUState *cpu, uint64_t addr, bool physical)
{
    uint64_t page = qemu_target_page_size();
    uint8_t buf[STRING_MAX];
    GString *s;
    int len = 0, i;
    bool ended = false;

    /* a page at a time: the next one may not be mapped */
    while (len < STRING_MAX && !ended) {
        uint64_t a = addr + len;
        int chunk = MIN(STRING_MAX - len, page - (a & (page - 1)));
        bool ok;

        if (physical) {
            ok = address_space_read(cpu->as, a, MEMTXATTRS_UNSPECIFIED,
                                    buf + len, chunk) == MEMTX_OK;
        } else {
            ok = cpu_memory_rw_debug(cpu, a, buf + len, chunk, false) == 0;
        }
        if (!ok) {
            break;
        }
        ended = memchr(buf + len, 0, chunk);
        len += chunk;
    }
    if (!len) {
        return NULL;
    }
    s = g_string_new("\"");
    for (i = 0; i < len && buf[i]; i++) {
        switch (buf[i]) {
        case '\n':
            g_string_append(s, "\\n");
            break;
        case '\r':
            g_string_append(s, "\\r");
            break;
        case '\t':
            g_string_append(s, "\\t");
            break;
        case '"':
        case '\\':
            g_string_append_printf(s, "\\%c", buf[i]);
            break;
        default:
            if (buf[i] >= 0x20 && buf[i] < 0x7f) {
                g_string_append_c(s, buf[i]);
            } else {
                g_string_append_printf(s, "\\x%02x", buf[i]);
            }
        }
    }
    g_string_append_c(s, '"');
    if (!ended && len < STRING_MAX) {
        g_string_append(s, " (no zero byte; the rest can't be read)");
    } else if (!ended) {
        g_string_append_printf(s, " (no zero byte in the first %d bytes)",
                               STRING_MAX);
    }
    return g_string_free(s, false);
}

static bool mcp_debug_mapped(CPUState *cpu, uint64_t addr)
{
    TranslateForDebugResult t;

    return cpu_translate_for_debug(cpu, addr, &t);
}

/* The instruction at @pc, or why it can't be read */
static char *mcp_debug_insn(CPUState *cpu, uint64_t pc)
{
    if (!mcp_debug_mapped(cpu, pc)) {
        return g_strdup_printf("0x%" PRIx64 ":  (not mapped for CPU %d now,"
                               " so it can't be shown; the CPU may take a"
                               " TLB miss or page fault there)\n", pc,
                               cpu->cpu_index);
    }
    return mcp_cpu_disas(cpu, pc, 1, false);
}

/* The next instruction of @cpu, its registers and the memory asked for */
static void mcp_debug_show(GString *s, CPUState *cpu, McpMem *mem, int n)
{
    g_autofree char *insn = mcp_debug_insn(cpu, cpu->cc->get_pc(cpu));
    g_autofree char *regs = mcp_cpu_registers(cpu);
    int i;

    if (!insn || !regs) {
        g_string_append(s, "\n(no registers: this QEMU has no HMP)");
        return;
    }
    /* one empty line before it */
    g_string_append(s, s->str[s->len - 1] == '\n' ? "\n" : "\n\n");
    g_string_append_printf(s, "Next instruction:\n%s\nRegisters:\n%s\n",
                           insn, g_strchomp(regs));
    for (i = 0; i < n; i++) {
        const char *what = mem[i].string ? "String" : "Memory";
        const char *bad = NULL;
        g_autofree char *dump = NULL;
        g_autofree char *where = NULL;
        uint64_t addr;

        if (!mcp_mem_address(cpu, &mem[i], &addr, &bad)) {
            g_string_append_printf(s, "\n%s at %s: CPU %d has no integer"
                                   " register %s\n", what, mem[i].text,
                                   cpu->cpu_index, bad);
            continue;
        }
        where = g_strdup_printf("%s%s%s0x%" PRIx64,
                                mem[i].has_reg ? mem[i].text : "",
                                mem[i].has_reg ? " = " : "",
                                mem[i].physical ? "physical " : "", addr);
        if (!mem[i].physical && !mcp_debug_mapped(cpu, addr)) {
            g_string_append_printf(s, "\n%s at %s: not mapped for CPU %d now"
                                   " (step on until the guest uses it).%s"
                                   " If it is at the same physical address"
                                   " (kernels often are), ask for it with"
                                   " physical: true.\n", what, where,
                                   cpu->cpu_index, mem[i].has_reg ?
                                   " If a register holds only the offset in"
                                   " a space or segment, add its base." : "");
            continue;
        }
        if (mem[i].string) {
            dump = mcp_read_string(cpu, addr, mem[i].physical);
            g_string_append_printf(s, "\nString at %s: %s\n", where,
                                   dump ? dump : "(it can't be read: no RAM"
                                   " or ROM there)");
            continue;
        }
        dump = mcp_cpu_memory(cpu, addr, mem[i].words, mem[i].physical);
        g_string_append_printf(s, "\nMemory at %s:\n%s", where,
                               dump ? dump : "");
    }
}

/*
 * By CPU index: the instruction that took an interruption (a TLB miss,
 * a page fault) when a debug tool last stepped it.  The CPU usually
 * comes back to run it again, and that is not a new arrival there.
 */
typedef struct McpRetryMark {
    bool set;
    uint64_t pc;
    uint32_t taken;             /* exceptions_taken after it */
} McpRetryMark;

static McpRetryMark *mcp_retry_marks;
static int mcp_n_retry_marks;

/* More interruptions than this since, and it did not come back soon */
#define RETRY_SOON      1000

static McpRetryMark *mcp_retry_mark(CPUState *cpu)
{
    int n = cpu->cpu_index + 1;

    if (n > mcp_n_retry_marks) {
        mcp_retry_marks = g_renew(McpRetryMark, mcp_retry_marks, n);
        memset(mcp_retry_marks + mcp_n_retry_marks, 0,
               (n - mcp_n_retry_marks) * sizeof(McpRetryMark));
        mcp_n_retry_marks = n;
    }
    return &mcp_retry_marks[cpu->cpu_index];
}

/*
 * Take one instruction on @cpu.  *@interrupted tells whether the CPU
 * took an interruption (a fault, a trap) instead of finishing it.
 */
static McpStepResult coroutine_fn mcp_debug_step_one(QemuInputCtx *ctx,
                                                     CPUState *cpu,
                                                     bool *interrupted)
{
    McpRetryMark *m = mcp_retry_mark(cpu);
    uint64_t pc = cpu->cc->get_pc(cpu);
    uint32_t taken = qatomic_read(&cpu->exceptions_taken);
    McpStepResult step = mcp_debug_step(ctx, cpu);

    *interrupted = step == STEP_DONE &&
                   qatomic_read(&cpu->exceptions_taken) != taken;
    if (*interrupted) {
        m->set = true;
        m->pc = pc;
        m->taken = qatomic_read(&cpu->exceptions_taken);
    } else if (step == STEP_DONE && m->pc == pc) {
        m->set = false;         /* it ran this time */
    }
    return step;
}

/*
 * Whether @cpu, at @pc, may be back to run the instruction there again
 * after it took an interruption.  That uses up its mark.
 */
static bool mcp_debug_retry(CPUState *cpu, uint64_t pc)
{
    McpRetryMark *m = mcp_retry_mark(cpu);
    uint32_t since = qatomic_read(&cpu->exceptions_taken) - m->taken;

    if (!m->set || m->pc != pc) {
        return false;
    }
    m->set = false;
    return since <= RETRY_SOON;
}

static void mcp_debug_set_cpu(McpDebugStop *res, CPUState *cpu)
{
    res->has_cpu = true;
    res->cpu = cpu->cpu_index;
    res->pc = g_strdup_printf("0x%" PRIx64, (uint64_t)cpu->cc->get_pc(cpu));
    mcp_debug_last_cpu = cpu->cpu_index;
}

typedef enum {
    RUN_REACHED,
    RUN_TIMEOUT,
    RUN_STOPPED,        /* the VM stopped for another reason */
    RUN_CANCELLED,
} McpRunResult;

/*
 * The watched areas: "wrote to 0x2000", "writes to 0x2000 (4 bytes)",
 * each with the word from @verb for its access
 */
static void mcp_debug_append_watches(GString *s, McpBreak *b,
                                     const char *const verb[],
                                     bool with_len, const char *conj)
{
    GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
    int i;

    for (i = 0; i < b->n_watches; i++) {
        McpWatchArea *a = &b->watches[i];
        g_autofree char *len = with_len ?
            g_strdup_printf(" (%" PRIu64 " byte%s)", a->len,
                            a->len == 1 ? "" : "s") : NULL;

        g_ptr_array_add(items, g_strdup_printf("%s 0x%" PRIx64 "%s",
                                               verb[a->flags], a->addr,
                                               len ? len : ""));
    }
    mcp_append_list(s, items, conj);
}

/*
 * The exceptions of @b: "exception 14" (@one and the number), "exceptions
 * 13 or 14" (@many and the numbers, joined with @conj), or with none
 * given @any, as in "an exception", or "an exception other than 2 or 3"
 */
static void mcp_debug_append_excp(GString *s, McpBreak *b, const char *one,
                                  const char *many, const char *any,
                                  const char *conj)
{
    CPUExcpWatch *w = &b->excp;
    bool skip = w->n_skip;
    int n = skip ? w->n_skip : w->n_only;
    int32_t *num = skip ? w->skip : w->only;
    GPtrArray *items;
    int i;

    if (!n) {
        g_string_append(s, any);
        return;
    }
    items = g_ptr_array_new_with_free_func(g_free);
    for (i = 0; i < n; i++) {
        g_ptr_array_add(items, g_strdup_printf("%" PRId32, num[i]));
    }
    if (skip) {
        g_string_append_printf(s, "%s other than ", any);
        mcp_append_list(s, items, "or");
    } else {
        g_string_append_printf(s, "%s ", n == 1 ? one : many);
        mcp_append_list(s, items, conj);
    }
}

/*
 * "reached 0x1000", "wrote to 0x2000", "took exception 14", or more of
 * them, for the summary
 */
static void mcp_debug_what(GString *s, McpBreak *b)
{
    static const char *const verb[] = {
        [BP_MEM_WRITE] = "wrote to",
        [BP_MEM_READ] = "read",
        [BP_MEM_ACCESS] = "touched",
    };
    const char *sep = "";

    if (b->n_addrs) {
        g_string_append(s, "reached ");
        mcp_debug_append_addrs(s, b, "or");
        sep = " or ";
    }
    if (b->n_watches) {
        g_string_append(s, sep);
        mcp_debug_append_watches(s, b, verb, false, "or");
        sep = " or ";
    }
    if (b->has_excp) {
        g_string_append(s, sep);
        mcp_debug_append_excp(s, b, "took exception", "took exceptions",
                              "took an exception", "or");
    }
}

/* A debug-run or a debug-trace: what it waits for, and what it saw */
typedef struct McpRun McpRun;
struct McpRun {
    McpBreak b;
    QemuInputCtx *ctx;
    int64_t limit;              /* the hits to wait for */
    int64_t end;                /* QEMU_CLOCK_REALTIME, ms */
    int history;                /* blocks to note, 0 for none */
    /*
     * debug-trace: each hit, with the VM stopped there.  @seen is the
     * watched access, @excp the exception, or neither for an arrival.
     */
    void (*log)(McpRun *r, CPUState *cpu, const McpWatchSeen *seen,
                const McpExcpSeen *excp, bool retry);
    int64_t n_hits;
    int64_t n_retries;
    CPUState *at;               /* the last hit */
    bool by_watch;
    bool by_excp;
    bool retry;
    McpWatchSeen seen;
    McpExcpSeen excp;
    bool late;                  /* hits as the time ran out */
    McpRunResult result;
};

static void mcp_run_hit(McpRun *r, CPUState *cpu, const McpWatchSeen *seen,
                        const McpExcpSeen *excp, bool retry)
{
    r->n_hits++;
    r->n_retries += retry;
    r->at = cpu;
    r->by_watch = seen;
    r->by_excp = excp;
    r->retry = retry;
    if (seen) {
        r->seen = *seen;
    }
    if (excp) {
        r->excp = *excp;
    }
    if (r->log) {
        r->log(r, cpu, seen, excp, retry);
    }
}

/* The CPUs stopped at the address have got there */
static void mcp_run_arrivals(McpRun *r)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (mcp_debug_is_at(&r->b, cpu)) {
            mcp_run_hit(r, cpu, NULL, NULL,
                        mcp_debug_retry(cpu, cpu->cc->get_pc(cpu)));
        }
    }
}

#define HISTORY_MAX     1024

/*
 * Note the last @n blocks of code that the CPUs of @b run.  The VM is
 * stopped.  Their blocks no longer chain, so the guest runs slower.
 */
static void mcp_history_on(McpBreak *b, int n)
{
    uint32_t size = pow2ceil(n);
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        CPUTBHistory *h;

        if (b->only && cpu != b->only) {
            continue;
        }
        h = g_malloc0(sizeof(*h) + size * sizeof(h->e[0]));
        h->size = size;
        g_free(cpu->tb_history);
        cpu->tb_history = h;
        tcg_update_cflags(cpu);
    }
}

/* Stop noting them; the vCPUs pause for it if the VM runs */
static void mcp_history_off(void *opaque)
{
    bool running = runstate_is_running();
    CPUState *cpu;

    if (running) {
        pause_all_vcpus();
    }
    CPU_FOREACH(cpu) {
        CPUTBHistory *h = cpu->tb_history;

        cpu->tb_history = NULL;
        tcg_update_cflags(cpu);
        g_free(h);
    }
    if (running) {
        resume_all_vcpus();
    }
}

/* The last @n blocks @cpu ran, oldest first: "0x1000-0x1013 x5" */
static strList *mcp_history_lines(CPUState *cpu, int n)
{
    CPUTBHistory *h = cpu->tb_history;
    strList *list = NULL, **tail = &list;
    uint64_t i;

    if (!h) {
        return NULL;
    }
    for (i = MIN(h->count, (uint64_t)n); i > 0; i--) {
        CPUTBHistoryEntry *e = &h->e[(h->next - i) & (h->size - 1)];
        g_autofree char *end = e->size ?
            g_strdup_printf("-0x%" PRIx64, (uint64_t)e->pc + e->size - 1) :
            NULL;
        g_autofree char *rep = e->repeat > 1 ?
            g_strdup_printf(" x%" PRIu32, e->repeat) : NULL;

        QAPI_LIST_APPEND(tail, g_strdup_printf("0x%" PRIx64 "%s%s",
                                               (uint64_t)e->pc,
                                               end ? end : "",
                                               rep ? rep : ""));
    }
    return list;
}

/* What @cpu stopped for besides an arrival: a watched access, an exception */
static void mcp_run_access(McpRun *r, CPUState *cpu)
{
    McpWatchSeen seen;
    McpExcpSeen excp;

    if (mcp_watch_take(&r->b, cpu, &seen)) {
        mcp_run_hit(r, cpu, &seen, NULL, false);
    }
    if (mcp_excp_take(&r->b, cpu, &excp)) {
        mcp_run_hit(r, cpu, NULL, &excp, false);
    }
}

/*
 * Let the guest run until @r has seen its hits, the time runs out, or
 * the VM stops for another reason.  The breakpoint and the watchpoint
 * are taken away at the end, and the VM is left as it is.
 */
static void coroutine_fn mcp_run(McpRun *r)
{
    McpBreak *b = &r->b;
    QemuInputCtx *ctx = r->ctx;
    RunState paused = RUN_STATE_PAUSED;
    McpStepResult step;
    CPUState *cpu;
    int64_t before;
    bool inserted = false, interrupted;

    r->result = RUN_TIMEOUT;
    /* a CPU there when the running VM stops has got there */
    if (runstate_is_running()) {
        mcp_debug_outside(mcp_debug_stop, &paused);
        if (!runstate_is_running()) {
            mcp_run_arrivals(r);
        }
    }
    if (r->history && !runstate_is_running()) {
        mcp_history_on(b, r->history);
    }
    while (r->n_hits < r->limit) {
        if (runstate_is_running()) {
            r->result = RUN_STOPPED;    /* someone else started it */
            break;
        }
        if (!inserted) {
            mcp_debug_insert(b);
            inserted = true;
        }
        /* a CPU stopped at the address goes on to the next arrival */
        step = STEP_DONE;
        CPU_FOREACH(cpu) {
            if (mcp_debug_is_at(b, cpu)) {
                step = mcp_debug_step_one(ctx, cpu, &interrupted);
                /* that instruction may touch watched memory, or trap */
                mcp_run_access(r, cpu);
                if (step != STEP_DONE && step != STEP_HALTED) {
                    break;
                }
            }
        }
        if (r->n_hits >= r->limit) {
            break;
        }
        if (step == STEP_STOPPED || step == STEP_NOT_STARTED) {
            r->result = RUN_STOPPED;
            break;
        }
        if (ctx->cancelled) {
            r->result = RUN_CANCELLED;
            break;
        }
        if (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) >= r->end) {
            break;
        }

        mcp_debug_outside(mcp_debug_start, NULL);
        if (!mcp_debug_wait_stop(ctx, r->end, NULL)) {
            r->result = ctx->cancelled ? RUN_CANCELLED : RUN_TIMEOUT;
            mcp_debug_outside(mcp_debug_remove_running, b);
            inserted = false;
            if (b->hits || b->whits || b->ehits) {
                /* got there as the time ran out; its stop comes next */
                mcp_debug_wait_stop(ctx, qemu_clock_get_ms(QEMU_CLOCK_REALTIME)
                                    + STEP_WAIT_MS, NULL);
                r->late = true;
                mcp_run_arrivals(r);
                if (b->whits) {
                    mcp_run_hit(r, b->seen.cpu, &b->seen, NULL, false);
                }
                if (b->ehits) {
                    mcp_run_hit(r, b->eseen.cpu, NULL, &b->eseen, false);
                }
                r->result = r->n_hits >= r->limit ? RUN_REACHED : RUN_STOPPED;
            }
            break;
        }
        before = r->n_hits;
        if (runstate_check(RUN_STATE_DEBUG)) {
            mcp_run_arrivals(r);
            CPU_FOREACH(cpu) {
                mcp_run_access(r, cpu);
            }
        }
        if (r->n_hits == before) {
            r->at = NULL;
            r->result = RUN_STOPPED;
            break;
        }
    }
    if (r->n_hits >= r->limit) {
        r->result = RUN_REACHED;
    }
    if (inserted) {
        mcp_debug_outside(mcp_debug_remove_paused, b);
    }
}

/* The addresses to stop at, one or several, into @b */
static bool mcp_parse_addrs(McpAddresses *address, McpBreak *b, Error **errp)
{
    strList one = { 0 };
    strList *l;

    if (address->type == QTYPE_QSTRING) {
        one.value = address->u.one;
        l = &one;
    } else {
        l = address->u.several;
        if (!l) {
            error_setg(errp, "give at least one address");
            return false;
        }
    }
    for (; l; l = l->next) {
        uint64_t a;
        int i;

        if (!mcp_parse_address(l->value, "address", &a, errp)) {
            return false;
        }
        /* the same address twice is one breakpoint */
        for (i = 0; i < b->n_addrs; i++) {
            if (b->addrs[i] == a) {
                break;
            }
        }
        if (i < b->n_addrs) {
            continue;
        }
        if (b->n_addrs == RUN_ADDRS_MAX) {
            error_setg(errp, "give at most %d addresses", RUN_ADDRS_MAX);
            return false;
        }
        b->addrs[b->n_addrs++] = a;
    }
    return true;
}

/* Exception numbers from @l into @num, which has *@n */
static bool mcp_parse_excp_numbers(intList *l, int32_t *num, int *n,
                                   Error **errp)
{
    for (; l; l = l->next) {
        int i;

        /* from EXCP_INTERRUPT on, they are QEMU's, not the CPU's */
        if (l->value < 0 || l->value >= EXCP_INTERRUPT) {
            error_setg(errp, "exception numbers are 0 to %d",
                       EXCP_INTERRUPT - 1);
            return false;
        }
        for (i = 0; i < *n; i++) {
            if (num[i] == l->value) {
                break;
            }
        }
        if (i < *n) {
            continue;
        }
        if (*n == CPU_EXCP_WATCH_MAX) {
            error_setg(errp, "give at most %d exception numbers",
                       CPU_EXCP_WATCH_MAX);
            return false;
        }
        num[(*n)++] = l->value;
    }
    return true;
}

/* The exceptions to stop at, into @b */
static bool mcp_parse_excp(McpExceptions *e, McpBreak *b, Error **errp)
{
    CPUExcpWatch *w = &b->excp;

    if (e->numbers && e->skip) {
        error_setg(errp, "give the exceptions to stop at or the ones to"
                   " skip, not both");
        return false;
    }
    if (!mcp_parse_excp_numbers(e->numbers, w->only, &w->n_only, errp) ||
        !mcp_parse_excp_numbers(e->skip, w->skip, &w->n_skip, errp)) {
        return false;
    }
    b->has_excp = true;
    return true;
}

/*
 * Where to stop or what to log: addresses, watched memory, exceptions,
 * a CPU
 */
static bool mcp_run_parse(McpRun *r, McpAddresses *address,
                          const char *address_mask, McpExceptions *exception,
                          bool has_cpu, int64_t cpu_index, Error **errp)
{
    if (address && !mcp_parse_addrs(address, &r->b, errp)) {
        return false;
    }
    if (exception && !mcp_parse_excp(exception, &r->b, errp)) {
        return false;
    }
    r->b.mask = UINT64_MAX;
    if (address_mask) {
        if (!address) {
            error_setg(errp, "address-mask needs an address");
            return false;
        }
        if (!mcp_parse_address(address_mask, "address-mask", &r->b.mask,
                               errp)) {
            return false;
        }
        if (!r->b.mask) {
            error_setg(errp, "address-mask must keep at least one bit");
            return false;
        }
        r->b.has_mask = true;
    }
    if (has_cpu) {
        r->b.only = mcp_debug_cpu(true, cpu_index, errp);
        if (!r->b.only) {
            return false;
        }
    }
    return true;
}

/*
 * What the last hit of @r was: "reached 0x1000", "wrote to 0x2000",
 * "took exception 14 at 0x1000"
 */
static void mcp_run_append_hit(GString *s, McpRun *r)
{
    if (r->by_watch) {
        g_string_append_printf(s, "%s 0x%" PRIx64,
                               r->seen.write ? "wrote to" : "read",
                               r->seen.addr);
    } else if (r->by_excp) {
        g_string_append_printf(s, "took exception %" PRId32 " at 0x%" PRIx64,
                               r->excp.number, r->excp.from);
    } else {
        g_string_append_printf(s, "reached 0x%" PRIx64,
                               mcp_debug_reached(&r->b, r->at));
    }
}

/* "arrival", "access", "exception", or "hit" when it may be any of them */
static const char *mcp_run_hit_word(McpBreak *b)
{
    if (!!b->n_addrs + !!b->n_watches + b->has_excp > 1) {
        return "hit";
    }
    return b->n_watches ? "access" : b->has_excp ? "exception" : "arrival";
}

McpDebugStop *coroutine_fn qmp_debug_run(McpAddresses *address,
                                         const char *address_mask,
                                         McpWatches *watch,
                                         McpExceptions *exception,
                                         bool has_hits, int64_t hits,
                                         bool has_cpu, int64_t cpu_index,
                                         bool has_memory,
                                         McpDebugMemoryList *memory,
                                         bool has_history, int64_t history,
                                         bool has_timeout_ms,
                                         int64_t timeout_ms, Error **errp)
{
    g_auto(QemuInputCtx) local = { 0 };
    g_autoptr(GString) s = g_string_new(NULL);
    g_autofree McpMem *mem = NULL;
    McpRun r = { .ctx = mcp_current_input_ctx() };
    McpBreak *b = &r.b;
    McpWatchSeen *seen = &r.seen;
    McpDebugStop *res;
    CPUState *at, *cpu;
    strList *hist = NULL, *l;
    int n_mem = 0, n;
    const char *hit_word;

    if (!r.ctx) {
        qemu_input_ctx_init(&local, NULL);
        r.ctx = &local;
    }
    if (!address && !watch && !exception) {
        error_setg(errp, "give an address to stop at, memory to watch,"
                   " exceptions, or more of them");
        return NULL;
    }
    hits = has_hits ? hits : 1;
    if (hits < 1 || hits > RUN_HITS_MAX) {
        error_setg(errp, "hits must be 1 to %d", RUN_HITS_MAX);
        return NULL;
    }
    timeout_ms = has_timeout_ms ? timeout_ms : RUN_DEFAULT_MS;
    timeout_ms = MIN(MAX(timeout_ms, 0), mcp_wait_max_ms());
    if (has_history && (history < 0 || history > HISTORY_MAX)) {
        error_setg(errp, "history must be 0 to %d", HISTORY_MAX);
        return NULL;
    }
    r.history = has_history ? history : 0;
    if (!mcp_run_parse(&r, address, address_mask, exception, has_cpu,
                       cpu_index, errp)) {
        return NULL;
    }
    mem = mcp_parse_memory(b->only ? b->only : first_cpu, memory, &n_mem,
                           errp);
    if (!mem) {
        return NULL;
    }
    if (watch && !mcp_parse_watches(watch, b, &mem[n_mem], errp)) {
        return NULL;
    }
    if (!mcp_debug_begin(r.ctx, errp)) {
        return NULL;
    }
    r.end = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + timeout_ms;
    r.limit = hits;
    hit_word = mcp_run_hit_word(b);

    mcp_run(&r);
    at = r.at;
    cpu = at ? at : b->only ? b->only : first_cpu;
    if (r.history) {
        /* how the CPU shown got there, read while it is stopped */
        if (!runstate_is_running()) {
            hist = mcp_history_lines(cpu, r.history);
        }
        mcp_debug_outside(mcp_history_off, NULL);
    }
    /* the time ran out or the call was cancelled: the guest goes on */
    if ((r.result == RUN_TIMEOUT || r.result == RUN_CANCELLED) &&
        !runstate_is_running()) {
        mcp_debug_outside(mcp_debug_start, NULL);
    }
    mcp_debug_end();
    if (r.result == RUN_CANCELLED) {
        qapi_free_strList(hist);
        error_setg(errp, "cancelled");
        return NULL;
    }

    res = g_new0(McpDebugStop, 1);
    res->reached = r.result == RUN_REACHED;
    res->has_hits = true;
    res->hits = r.n_hits;
    if (at) {
        mcp_debug_set_cpu(res, at);
    }
    res->running = runstate_is_running();
    if (at && r.retry) {
        res->has_retry = true;
        res->retry = true;
    }
    if (at && r.by_watch) {
        res->watch = g_new0(McpWatchHit, 1);
        res->watch->address = g_strdup_printf("0x%" PRIx64, seen->addr);
        res->watch->access = g_strdup(seen->write ? "write" : "read");
        res->watch->pc = g_strdup_printf("0x%" PRIx64, seen->pc);
    }
    if (at && r.by_excp) {
        res->exception = g_new0(McpExceptionHit, 1);
        res->exception->number = r.excp.number;
        res->exception->pc = g_strdup_printf("0x%" PRIx64, r.excp.from);
        res->exception->handler = g_strdup_printf("0x%" PRIx64, r.excp.to);
    }
    switch (r.result) {
    case RUN_REACHED:
        g_string_printf(s, "CPU %d ", at->cpu_index);
        mcp_run_append_hit(s, &r);
        if (hits > 1) {
            g_string_append_printf(s, " (%s %" PRId64 ")", hit_word,
                                   r.n_hits);
        }
        if (r.by_watch) {
            g_autofree char *insn = mcp_debug_insn(at, seen->pc);

            g_string_append_printf(s, "; the VM is stopped after the"
                                   " instruction that did it:\n%s",
                                   insn ? insn : "");
            break;
        }
        if (r.by_excp) {
            g_autofree char *insn = mcp_debug_insn(at, r.excp.from);

            g_string_append_printf(s, "; the VM is stopped at the first"
                                   " instruction of the guest's handler"
                                   " for it, 0x%" PRIx64 ". The"
                                   " instruction that took it:\n%s",
                                   r.excp.to, insn ? insn : "");
            break;
        }
        g_string_append(s, "; the VM is stopped");
        if (r.retry) {
            g_string_append(s, ". This may be the CPU coming back to run"
                            " the instruction again: when it went on from"
                            " here, the instruction took an interruption"
                            " first (such as a TLB miss). Run debug-run"
                            " again to go on");
        }
        if (hits > 1 && r.n_retries) {
            g_string_append_printf(s, " (%" PRId64 " of the arrivals came"
                                   " right after an interruption there)",
                                   r.n_retries);
        }
        break;
    case RUN_TIMEOUT:
        g_string_assign(s, "no CPU ");
        mcp_debug_what(s, b);
        g_string_append_printf(s, " in %.1f s", timeout_ms / 1000.0);
        if (hits > 1) {
            g_string_append_printf(s, " (%" PRId64 " of %" PRId64 " %ss)",
                                   r.n_hits, hits, hit_word);
        }
        g_string_append(s, "; the VM keeps running");
        break;
    case RUN_STOPPED:
        if (at) {
            g_string_printf(s, "CPU %d ", at->cpu_index);
            mcp_run_append_hit(s, &r);
            g_string_append_printf(s, " as the time ran out (%s %" PRId64
                                   " of %" PRId64 "); the VM is stopped",
                                   hit_word, r.n_hits, hits);
        } else {
            if (res->running) {
                g_string_assign(s, "something else started the VM");
            } else {
                g_string_printf(s, "the VM stopped (%s)",
                                RunState_str(runstate_get()));
            }
            g_string_append(s, " before a CPU ");
            mcp_debug_what(s, b);
        }
        break;
    default:
        g_assert_not_reached();
    }
    if (!res->running) {
        if (!at) {
            g_string_append_printf(s, "; CPU %d:", cpu->cpu_index);
        }
        if (hist) {
            for (l = hist, n = 0; l; l = l->next) {
                n++;
            }
            g_string_append_printf(s, "\n\nHow it got here, the last %d"
                                   " block%s of code CPU %d ran, oldest"
                                   " first:", n, n == 1 ? "" : "s",
                                   cpu->cpu_index);
            for (l = hist; l; l = l->next) {
                g_string_append_printf(s, "\n  %s", l->value);
            }
            res->has_history = true;
            res->history = g_steal_pointer(&hist);
        }
        /* and the watched memory, after the memory asked for */
        mcp_debug_show(s, cpu, mem, n_mem + b->n_watches);
    }
    qapi_free_strList(hist);
    res->summary = g_strdup(s->str);
    return res;
}

#define TRACE_COUNT             1000
#define TRACE_COUNT_MAX         100000
#define TRACE_LIST              20
#define TRACE_LIST_MAX          500
#define TRACE_INSNS_MAX         100
#define TRACE_EXCPS_MAX         64
#define TRACE_REGS_MAX          16
#define TRACE_DEFAULT_MS        10000

/* @words 32-bit words at @addr, in hex, in the order hmp's x shows */
static char *mcp_read_words(CPUState *cpu, uint64_t addr, int words,
                            bool physical)
{
    g_autofree uint8_t *buf = g_malloc(words * 4);
    GString *s;
    bool ok;
    int i;

    if (physical) {
        ok = address_space_read(cpu->as, addr, MEMTXATTRS_UNSPECIFIED, buf,
                                words * 4) == MEMTX_OK;
    } else {
        ok = cpu_memory_rw_debug(cpu, addr, buf, words * 4, false) == 0;
    }
    if (!ok) {
        return NULL;
    }
    s = g_string_new(NULL);
    for (i = 0; i < words; i++) {
        uint32_t v = target_big_endian() ? ldl_be_p(buf + 4 * i)
                                         : ldl_le_p(buf + 4 * i);

        g_string_append_printf(s, "%s0x%08" PRIx32, i ? " " : "", v);
    }
    return g_string_free(s, false);
}

/* Memory for a trace line: [sp+4]=0x00001000, or [r26]="text" */
static char *mcp_trace_memory(CPUState *cpu, McpMem *m)
{
    g_autofree char *v = NULL;
    const char *bad = NULL;
    const char *phys = m->physical ? ", physical" : "";
    uint64_t addr;

    if (!mcp_mem_address(cpu, m, &addr, &bad)) {
        return g_strdup_printf("[%s%s]=? (CPU %d has no integer register"
                               " %s)", m->text, phys, cpu->cpu_index, bad);
    }
    if (!m->physical && !mcp_debug_mapped(cpu, addr)) {
        return g_strdup_printf("[%s]=not mapped", m->text);
    }
    v = m->string ? mcp_read_string(cpu, addr, m->physical)
                  : mcp_read_words(cpu, addr, m->words, m->physical);
    return g_strdup_printf("[%s%s]=%s", m->text, phys,
                           v ? v : "? (no RAM or ROM there)");
}

/* An instruction that touched the watched memory, and how often */
typedef struct McpTraceCount {
    uint64_t pc;
    int64_t count;
    char *insn;                 /* the first time it could be read */
    bool mapped;
} McpTraceCount;

static void mcp_trace_count_free(gpointer p)
{
    McpTraceCount *c = p;

    g_free(c->insn);
    g_free(c);
}

static gint mcp_trace_count_cmp(gconstpointer a, gconstpointer b)
{
    const McpTraceCount *x = *(McpTraceCount *const *)a;
    const McpTraceCount *y = *(McpTraceCount *const *)b;

    if (x->count != y->count) {
        return x->count > y->count ? -1 : 1;
    }
    return x->pc < y->pc ? -1 : x->pc > y->pc;
}

/* An exception CPUs took, and how often */
typedef struct McpTraceExcp {
    int32_t number;
    int64_t count;
    uint64_t handler;           /* where the first one went */
} McpTraceExcp;

static gint mcp_trace_excp_cmp(gconstpointer a, gconstpointer b)
{
    const McpTraceExcp *x = *(McpTraceExcp *const *)a;
    const McpTraceExcp *y = *(McpTraceExcp *const *)b;

    if (x->count != y->count) {
        return x->count > y->count ? -1 : 1;
    }
    return x->number < y->number ? -1 : x->number > y->number;
}

typedef struct McpTracer {
    McpRun r;
    McpMem *mem;
    int n_mem;
    strList *regs;
    int64_t list;               /* hits to list in full */
    GString *text;              /* the hits listed */
    McpTraceEntryList *entries;
    McpTraceEntryList **tail;
    int64_t n_listed;
    GHashTable *insns;          /* McpTraceCount by pc */
    int64_t n_other;            /* by instructions past TRACE_INSNS_MAX */
    GHashTable *excps;          /* McpTraceExcp by number */
    int64_t n_other_excps;      /* past TRACE_EXCPS_MAX numbers */
} McpTracer;

/* Count exception @excp by its number */
static void mcp_trace_count_excp(McpTracer *t, const McpExcpSeen *excp)
{
    McpTraceExcp *x = g_hash_table_lookup(t->excps, &excp->number);

    if (!x && g_hash_table_size(t->excps) < TRACE_EXCPS_MAX) {
        x = g_new0(McpTraceExcp, 1);
        x->number = excp->number;
        x->handler = excp->to;
        g_hash_table_insert(t->excps, &x->number, x);
    }
    if (x) {
        x->count++;
    } else {
        t->n_other_excps++;
    }
}

static void mcp_trace_log(McpRun *r, CPUState *cpu, const McpWatchSeen *seen,
                          const McpExcpSeen *excp, bool retry)
{
    McpTracer *t = container_of(r, McpTracer, r);
    uint64_t pc = seen ? seen->pc : excp ? excp->from :
                  mcp_debug_reached(&r->b, cpu);
    McpTraceCount *c = NULL;
    McpTraceEntry *e;
    strList **vtail, *l;
    bool first = false;
    int i;

    if (excp) {
        mcp_trace_count_excp(t, excp);
    }
    /* which instructions touch the watched memory, trap, or are got to */
    if (seen || excp || mcp_debug_by_addr(&r->b)) {
        c = g_hash_table_lookup(t->insns, &pc);
        if (!c && g_hash_table_size(t->insns) < TRACE_INSNS_MAX) {
            c = g_new0(McpTraceCount, 1);
            c->pc = pc;
            g_hash_table_insert(t->insns, &c->pc, c);
            first = true;
        }
        /*
         * A CPU often gets to code before it is mapped, then comes back
         * after the TLB miss: show the instruction once it can be read
         */
        if (c && !c->mapped) {
            bool mapped = mcp_debug_mapped(cpu, pc);

            if (mapped || !c->insn) {
                g_free(c->insn);
                c->insn = mcp_debug_insn(cpu, pc);
                if (c->insn) {
                    g_strchomp(c->insn);
                }
                c->mapped = mapped;
            }
        }
        if (c) {
            c->count++;
        } else {
            t->n_other++;
        }
    }
    if (t->n_listed >= t->list && !first) {
        return;
    }
    t->n_listed++;

    e = g_new0(McpTraceEntry, 1);
    e->hit = r->n_hits;
    e->cpu = cpu->cpu_index;
    e->pc = g_strdup_printf("0x%" PRIx64, pc);
    g_string_append_printf(t->text, "#%" PRId64 " CPU %d at %s", e->hit,
                           cpu->cpu_index, e->pc);
    if (seen) {
        e->watch = g_new0(McpWatchHit, 1);
        e->watch->address = g_strdup_printf("0x%" PRIx64, seen->addr);
        e->watch->access = g_strdup(seen->write ? "write" : "read");
        e->watch->pc = g_strdup(e->pc);
        e->value = mcp_read_words(cpu, seen->addr & ~3ULL, 1, false);
        g_string_append_printf(t->text, " %s %s",
                               seen->write ? "wrote to" : "read",
                               e->watch->address);
        if (e->value) {
            g_string_append_printf(t->text, " (now %s)", e->value);
        }
    }
    if (excp) {
        e->exception = g_new0(McpExceptionHit, 1);
        e->exception->number = excp->number;
        e->exception->pc = g_strdup(e->pc);
        e->exception->handler = g_strdup_printf("0x%" PRIx64, excp->to);
        g_string_append_printf(t->text, " took exception %" PRId32 ", went"
                               " to %s", excp->number,
                               e->exception->handler);
    }
    if (retry) {
        e->has_retry = true;
        e->retry = true;
        g_string_append(t->text, " (again, after an interruption)");
    }
    vtail = &e->values;
    for (l = t->regs; l; l = l->next) {
        uint64_t v;

        QAPI_LIST_APPEND(vtail, mcp_reg_value(cpu, l->value, &v)
                         ? g_strdup_printf("%s=0x%" PRIx64, l->value, v)
                         : g_strdup_printf("%s=?", l->value));
    }
    for (i = 0; i < t->n_mem; i++) {
        QAPI_LIST_APPEND(vtail, mcp_trace_memory(cpu, &t->mem[i]));
    }
    e->has_values = e->values;
    for (l = e->values; l; l = l->next) {
        g_string_append_printf(t->text, "%s %s", l == e->values ? ":" : "",
                               l->value);
    }
    g_string_append_c(t->text, '\n');
    QAPI_LIST_APPEND(t->tail, e);
}

/* "arrivals at 0x1000 and writes to 0x2000 (4 bytes)", "exceptions" */
static void mcp_trace_what(GString *s, McpBreak *b)
{
    static const char *const what[] = {
        [BP_MEM_WRITE] = "writes to",
        [BP_MEM_READ] = "reads of",
        [BP_MEM_ACCESS] = "accesses to",
    };
    const char *sep = "";

    if (b->n_addrs) {
        g_string_append(s, "arrivals at ");
        mcp_debug_append_addrs(s, b, "and");
        sep = " and ";
    }
    if (b->n_watches) {
        g_string_append(s, sep);
        mcp_debug_append_watches(s, b, what, true, "and");
        sep = " and ";
    }
    if (b->has_excp) {
        g_string_append(s, sep);
        mcp_debug_append_excp(s, b, "exception", "exceptions", "exceptions",
                              "and");
    }
}

McpTrace *coroutine_fn qmp_debug_trace(McpAddresses *address,
                                       const char *address_mask,
                                       McpWatches *watch,
                                       McpExceptions *exception,
                                       bool has_count, int64_t count,
                                       bool has_list, int64_t list,
                                       bool has_cpu, int64_t cpu_index,
                                       bool has_registers,
                                       strList *registers,
                                       bool has_memory,
                                       McpDebugMemoryList *memory,
                                       bool has_duration_ms,
                                       int64_t duration_ms, Error **errp)
{
    g_auto(QemuInputCtx) local = { 0 };
    g_autoptr(GString) s = g_string_new(NULL);
    g_autoptr(GString) text = g_string_new(NULL);
    g_autoptr(GHashTable) insns = g_hash_table_new_full(g_int64_hash,
                                                        g_int64_equal, NULL,
                                                        mcp_trace_count_free);
    g_autoptr(GHashTable) excps = g_hash_table_new_full(g_int_hash,
                                                        g_int_equal, NULL,
                                                        g_free);
    g_autoptr(GPtrArray) sorted = NULL;
    g_autofree McpMem *mem = NULL;
    McpTracer t = {
        .r.ctx = mcp_current_input_ctx(),
        .r.log = mcp_trace_log,
        .text = text,
        .insns = insns,
        .excps = excps,
    };
    McpBreak *b = &t.r.b;
    McpTraceInstructionList **itail;
    McpExceptionCountList **xtail;
    McpDebugMemoryList *ml;
    McpTrace *res;
    CPUState *cpu;
    strList *rl;
    int64_t start, ms;
    unsigned int i;
    int n;

    if (!t.r.ctx) {
        qemu_input_ctx_init(&local, NULL);
        t.r.ctx = &local;
    }
    if (!address && !watch && !exception) {
        error_setg(errp, "give an address to trace, memory to watch,"
                   " exceptions, or more of them");
        return NULL;
    }
    count = has_count ? count : TRACE_COUNT;
    if (count < 1 || count > TRACE_COUNT_MAX) {
        error_setg(errp, "count must be 1 to %d", TRACE_COUNT_MAX);
        return NULL;
    }
    list = has_list ? list : TRACE_LIST;
    if (list < 0 || list > TRACE_LIST_MAX) {
        error_setg(errp, "list must be 0 to %d", TRACE_LIST_MAX);
        return NULL;
    }
    duration_ms = has_duration_ms ? duration_ms : TRACE_DEFAULT_MS;
    duration_ms = MIN(MAX(duration_ms, 0), mcp_wait_max_ms());
    if (!mcp_run_parse(&t.r, address, address_mask, exception, has_cpu,
                       cpu_index, errp)) {
        return NULL;
    }
    cpu = b->only ? b->only : first_cpu;
    if (!cpu) {
        error_setg(errp, "this VM has no CPU");
        return NULL;
    }
    for (rl = registers, n = 0; rl; rl = rl->next) {
        if (++n > TRACE_REGS_MAX) {
            error_setg(errp, "ask for at most %d registers", TRACE_REGS_MAX);
            return NULL;
        }
        if (g_ascii_strcasecmp(rl->value, "pc") &&
            mcp_reg_number(cpu, rl->value) < 0) {
            g_autofree char *names = mcp_reg_names(cpu);

            error_setg(errp, "'%s' is not a register; the registers are %s",
                       rl->value, names);
            return NULL;
        }
    }
    mem = mcp_parse_memory(cpu, memory, &t.n_mem, errp);
    if (!mem) {
        return NULL;
    }
    /* one word each, unless asked */
    for (ml = memory, n = 0; ml; ml = ml->next, n++) {
        if (!ml->value->has_words) {
            mem[n].words = 1;
        }
    }
    /* each line shows the watched word */
    if (watch && !mcp_parse_watches(watch, b, NULL, errp)) {
        return NULL;
    }
    t.mem = mem;
    t.regs = registers;
    t.list = list;
    t.tail = &t.entries;
    t.r.limit = count;
    if (!mcp_debug_begin(t.r.ctx, errp)) {
        return NULL;
    }
    start = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    t.r.end = start + duration_ms;

    mcp_run(&t.r);
    ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - start;
    /* the guest goes on, unless something else stopped it */
    if (!runstate_is_running() &&
        (t.r.result != RUN_STOPPED || t.r.late)) {
        mcp_debug_outside(mcp_debug_start, NULL);
    }
    mcp_debug_end();
    if (t.r.result == RUN_CANCELLED) {
        qapi_free_McpTraceEntryList(t.entries);
        error_setg(errp, "cancelled");
        return NULL;
    }

    res = g_new0(McpTrace, 1);
    res->hits = t.r.n_hits;
    res->ms = ms;
    res->running = runstate_is_running();
    res->entries = t.entries;
    g_string_assign(s, "Traced ");
    mcp_trace_what(s, b);
    g_string_append_printf(s, " for %.1f s: ", ms / 1000.0);
    if (t.r.n_hits) {
        g_string_append_printf(s, "%" PRId64 " hit%s", t.r.n_hits,
                               t.r.n_hits == 1 ? "" : "s");
    } else {
        g_string_append(s, "no hits");
    }
    if (t.r.result == RUN_REACHED) {
        g_string_append(s, ", as many as asked for");
    }
    if (t.r.n_retries) {
        g_string_append_printf(s, " (%" PRId64 " right after an interruption"
                               " there)", t.r.n_retries);
    }
    if (res->running) {
        g_string_append(s, "; the guest runs on.");
    } else {
        g_string_append_printf(s, "; then the VM stopped (%s), and it stays"
                               " stopped.", RunState_str(runstate_get()));
    }

    if (b->has_excp) {
        GHashTableIter it;
        gpointer v;

        sorted = g_ptr_array_new();
        g_hash_table_iter_init(&it, excps);
        while (g_hash_table_iter_next(&it, NULL, &v)) {
            g_ptr_array_add(sorted, v);
        }
        g_ptr_array_sort(sorted, mcp_trace_excp_cmp);
        res->has_exceptions = true;
        xtail = &res->exceptions;
        if (sorted->len) {
            g_string_append(s, "\n\nBy exception:\n");
        }
        for (i = 0; i < sorted->len; i++) {
            McpTraceExcp *x = g_ptr_array_index(sorted, i);
            McpExceptionCount *xc = g_new0(McpExceptionCount, 1);

            xc->number = x->number;
            xc->count = x->count;
            xc->handler = g_strdup_printf("0x%" PRIx64, x->handler);
            QAPI_LIST_APPEND(xtail, xc);
            g_string_append_printf(s, "%8" PRId64 " x exception %" PRId32
                                   ", handler %s\n", x->count, x->number,
                                   xc->handler);
        }
        if (t.n_other_excps) {
            g_string_append_printf(s, "%8" PRId64 " x other exceptions"
                                   " (only %d are told apart)\n",
                                   t.n_other_excps, TRACE_EXCPS_MAX);
        }
        g_clear_pointer(&sorted, g_ptr_array_unref);
    }
    if (b->n_watches || b->has_excp || mcp_debug_by_addr(b)) {
        GHashTableIter it;
        gpointer v;

        sorted = g_ptr_array_new();
        g_hash_table_iter_init(&it, insns);
        while (g_hash_table_iter_next(&it, NULL, &v)) {
            g_ptr_array_add(sorted, v);
        }
        g_ptr_array_sort(sorted, mcp_trace_count_cmp);
        res->has_instructions = true;
        itail = &res->instructions;
        if (sorted->len) {
            g_string_append(s, s->str[s->len - 1] == '\n' ? "\n" : "\n\n");
            g_string_append(s, "By instruction:\n");
        }
        for (i = 0; i < sorted->len; i++) {
            McpTraceCount *c = g_ptr_array_index(sorted, i);
            McpTraceInstruction *in = g_new0(McpTraceInstruction, 1);

            in->pc = g_strdup_printf("0x%" PRIx64, c->pc);
            in->count = c->count;
            in->instruction = g_strdup(c->insn);
            QAPI_LIST_APPEND(itail, in);
            g_string_append_printf(s, "%8" PRId64 " x %s\n", c->count,
                                   c->insn ? c->insn : in->pc);
        }
        if (t.n_other) {
            g_string_append_printf(s, "%8" PRId64 " x other instructions"
                                   " (only %d are told apart)\n", t.n_other,
                                   TRACE_INSNS_MAX);
        }
    }
    if (t.n_listed) {
        g_string_append(s, s->str[s->len - 1] == '\n' ? "\n" : "\n\n");
        if (t.n_listed == t.r.n_hits) {
            g_string_append(s, "Hits:\n");
        } else if (b->n_watches || b->has_excp || mcp_debug_by_addr(b)) {
            /* "the first access by each instruction" */
            bool insn = b->n_watches || b->has_excp;
            const char *what = insn && mcp_debug_by_addr(b) ? "hit at" :
                               b->n_watches && b->has_excp ? "hit by" :
                               b->n_watches ? "access by" :
                               b->has_excp ? "exception taken by" :
                               "arrival at";
            const char *where = insn ? "instruction" : "address";

            if (list) {
                g_string_append_printf(s, "Hits (the first %" PRId64 ", then"
                                       " the first %s each new %s):\n",
                                       list, what, where);
            } else {
                g_string_append_printf(s, "Hits (the first %s each %s):\n",
                                       what, where);
            }
        } else {
            g_string_append_printf(s, "Hits (the first %" PRId64 "):\n",
                                   list);
        }
        g_string_append(s, text->str);
    }
    res->summary = g_strdup(g_strchomp(s->str));
    return res;
}

McpDebugStop *coroutine_fn qmp_debug_step(bool has_count, int64_t count,
                                          bool has_cpu, int64_t cpu_index,
                                          bool has_memory,
                                          McpDebugMemoryList *memory,
                                          Error **errp)
{
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_current_input_ctx();
    g_autoptr(GString) s = g_string_new(NULL);
    g_autofree McpMem *mem = NULL;
    g_autofree char **done_insn = NULL;
    RunState paused = RUN_STATE_PAUSED;
    McpStepResult step = STEP_DONE;
    McpDebugStop *res;
    const char *why = NULL;
    CPUState *cpu;
    int64_t done, first, i;
    int n_mem = 0;
    bool interrupted;

    if (!ctx) {
        qemu_input_ctx_init(&local, NULL);
        ctx = &local;
    }
    count = has_count ? count : 1;
    if (count < 1 || count > STEP_MAX) {
        error_setg(errp, "count must be 1 to %d", STEP_MAX);
        return NULL;
    }
    cpu = mcp_debug_cpu(has_cpu, cpu_index, errp);
    if (!cpu) {
        return NULL;
    }
    mem = mcp_parse_memory(cpu, memory, &n_mem, errp);
    if (!mem || !mcp_debug_begin(ctx, errp)) {
        return NULL;
    }
    if (runstate_is_running()) {
        mcp_debug_outside(mcp_debug_stop, &paused);
    }

    done_insn = g_new0(char *, STEP_SHOWN);
    for (done = 0; done < count && !ctx->cancelled; done++) {
        char **slot = &done_insn[done % STEP_SHOWN];
        char *insn;

        if (runstate_is_running()) {
            why = "something else started the VM";
            break;
        }
        if (cpu->halted) {
            why = "it is halted, waiting for an interrupt, and a step keeps "
                  "interrupts away";
            break;
        }
        insn = mcp_debug_insn(cpu, cpu->cc->get_pc(cpu));
        step = mcp_debug_step_one(ctx, cpu, &interrupted);
        if (step == STEP_NOT_STARTED || step == STEP_CANCELLED) {
            g_free(insn);
            why = step == STEP_CANCELLED ? "cancelled" :
                  "the VM did not start";
            break;
        }
        if (interrupted && insn) {
            char *old = insn;

            insn = g_strdup_printf("%s  -> it took an interruption: the CPU"
                                   " went to 0x%" PRIx64 "\n", old,
                                   (uint64_t)cpu->cc->get_pc(cpu));
            g_free(old);
        }
        g_free(*slot);
        *slot = insn;
        if (step == STEP_HALTED) {
            why = "it halted, waiting for an interrupt";
            done++;
            break;
        }
        if (step == STEP_STOPPED) {
            why = "the VM stopped";
            done++;
            break;
        }
    }
    mcp_debug_end();
    if (ctx->cancelled) {
        for (i = 0; i < STEP_SHOWN; i++) {
            g_free(done_insn[i]);
        }
        error_setg(errp, "cancelled");
        return NULL;
    }

    res = g_new0(McpDebugStop, 1);
    res->reached = done == count;
    res->has_steps = true;
    res->steps = done;
    res->running = runstate_is_running();
    mcp_debug_set_cpu(res, cpu);
    g_string_printf(s, "CPU %d did %" PRId64, cpu->cpu_index, done);
    if (done < count) {
        g_string_append_printf(s, " of %" PRId64, count);
    }
    g_string_append_printf(s, " step%s", count == 1 ? "" : "s");
    if (why) {
        g_string_append_printf(s, ": %s", why);
        if (step == STEP_STOPPED) {
            g_string_append_printf(s, " (%s)", RunState_str(runstate_get()));
        }
    }
    g_string_append(s, res->running ? "; the VM is running" :
                    "; the VM is stopped");

    first = MAX(done - STEP_SHOWN, 0);
    if (done && done_insn[first % STEP_SHOWN]) {
        g_string_append_printf(s, "\n\n%s:\n", done > STEP_SHOWN ?
                               "The last instructions done" :
                               done == 1 ? "Instruction done" :
                               "Instructions done");
        for (i = first; i < done; i++) {
            g_string_append(s, done_insn[i % STEP_SHOWN]);
        }
    }
    for (i = 0; i < STEP_SHOWN; i++) {
        g_free(done_insn[i]);
    }
    if (!res->running) {
        mcp_debug_show(s, cpu, mem, n_mem);
    }
    res->summary = g_strdup(s->str);
    return res;
}

#define PC_NO_PHYS      UINT64_MAX

typedef struct McpPcCount {
    uint64_t pc;
    uint64_t pa;                /* where it was then, or PC_NO_PHYS */
    int64_t count;
} McpPcCount;

static guint mcp_pc_hash(gconstpointer key)
{
    const McpPcCount *c = key;

    return g_int64_hash(&c->pc) * 31 + g_int64_hash(&c->pa);
}

static gboolean mcp_pc_equal(gconstpointer a, gconstpointer b)
{
    const McpPcCount *x = a, *y = b;

    return x->pc == y->pc && x->pa == y->pa;
}

/*
 * The samples, shared by the tool and its queued work: the last of
 * them to finish frees it.
 */
typedef struct McpProfileData {
    int refs;
    GHashTable *pcs;            /* set of McpPcCount, by pc and pa */
    int64_t ran, halted;
    bool *pending;              /* by CPU index: a sample is queued */
} McpProfileData;

static void mcp_profile_unref(McpProfileData *p)
{
    if (!--p->refs) {
        g_hash_table_destroy(p->pcs);
        g_free(p->pending);
        g_free(p);
    }
}

/* In the vCPU thread with the BQL, between two blocks of code */
static void mcp_profile_sample(CPUState *cpu, run_on_cpu_data data)
{
    McpProfileData *p = data.host_ptr;

    p->pending[cpu->cpu_index] = false;
    if (!runstate_is_running()) {
        /* paused since the sample was asked for */
    } else if (cpu->halted) {
        p->halted++;
    } else {
        McpPcCount key = { .pc = cpu->cc->get_pc(cpu), .pa = PC_NO_PHYS };
        TranslateForDebugResult t;
        McpPcCount *c;

        /* now, while the CPU runs there: the mapping may be gone later */
        if (cpu_translate_for_debug(cpu, key.pc, &t)) {
            key.pa = t.physaddr;
        }
        c = g_hash_table_lookup(p->pcs, &key);
        if (!c) {
            c = g_memdup2(&key, sizeof(key));
            g_hash_table_add(p->pcs, c);
        }
        c->count++;
        p->ran++;
    }
    mcp_profile_unref(p);
}

static gint mcp_pc_count_cmp(gconstpointer a, gconstpointer b)
{
    const McpPcCount *x = *(McpPcCount * const *)a;
    const McpPcCount *y = *(McpPcCount * const *)b;

    if (x->count != y->count) {
        return x->count < y->count ? 1 : -1;
    }
    if (x->pc != y->pc) {
        return x->pc < y->pc ? -1 : 1;
    }
    return x->pa < y->pa ? -1 : x->pa > y->pa;
}

McpProfile *coroutine_fn qmp_debug_profile(bool has_interval_ms,
                                           int64_t interval_ms,
                                           bool has_cpu, int64_t cpu_index,
                                           bool has_top, int64_t top,
                                           Error **errp)
{
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_current_input_ctx();
    g_autoptr(GString) s = g_string_new(NULL);
    g_autoptr(GPtrArray) all = NULL;
    McpProfileData *p;
    McpProfile *res;
    McpProfileEntryList **tail;
    GHashTableIter iter;
    gpointer value;
    CPUState *only = NULL, *cpu;
    int64_t start, now, end, ms, ticks = 0, paused = 0;
    int ncpus = 0;
    guint i;

    if (!tcg_enabled()) {
        error_setg(errp, "debug-profile needs TCG (-accel tcg)");
        return NULL;
    }
    if (has_cpu) {
        only = mcp_debug_cpu(true, cpu_index, errp);
        if (!only) {
            return NULL;
        }
    }
    if (!ctx) {
        qemu_input_ctx_init(&local, NULL);
        ctx = &local;
    }
    interval_ms = has_interval_ms ? interval_ms : PROFILE_DEFAULT_MS;
    interval_ms = MAX(MIN(interval_ms, mcp_wait_max_ms()), PROFILE_MIN_MS);
    top = has_top ? top : PROFILE_TOP;
    top = MIN(MAX(top, 1), PROFILE_TOP_MAX);
    CPU_FOREACH(cpu) {
        ncpus = MAX(ncpus, cpu->cpu_index + 1);
    }

    p = g_new0(McpProfileData, 1);
    p->refs = 1;
    p->pcs = g_hash_table_new_full(mcp_pc_hash, mcp_pc_equal, g_free, NULL);
    p->pending = g_new0(bool, MAX(ncpus, 1));
    start = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    end = start + interval_ms;
    for (;;) {
        ticks++;
        if (!runstate_is_running()) {
            paused++;
        } else {
            CPU_FOREACH(cpu) {
                if ((only && cpu != only) || cpu->cpu_index >= ncpus ||
                    p->pending[cpu->cpu_index]) {
                    continue;
                }
                p->pending[cpu->cpu_index] = true;
                p->refs++;
                async_run_on_cpu(cpu, mcp_profile_sample,
                                 RUN_ON_CPU_HOST_PTR(p));
            }
        }
        now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        if (now >= end || ctx->cancelled) {
            break;
        }
        /* uneven steps, so that a guest timer can't keep pace with them */
        qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME,
                                  MIN(g_random_int_range(2, 9), end - now)
                                  * SCALE_MS);
    }
    if (ctx->cancelled) {
        mcp_profile_unref(p);
        error_setg(errp, "cancelled");
        return NULL;
    }
    ms = MAX(now - start, 1);

    all = g_ptr_array_new();
    g_hash_table_iter_init(&iter, p->pcs);
    while (g_hash_table_iter_next(&iter, &value, NULL)) {
        g_ptr_array_add(all, value);
    }
    g_ptr_array_sort(all, mcp_pc_count_cmp);

    res = g_new0(McpProfile, 1);
    res->interval_ms = ms;
    res->samples = p->ran + p->halted;
    res->halted_percent = res->samples ? p->halted * 100 / res->samples : 0;
    res->paused_percent = paused * 100 / ticks;
    tail = &res->top;
    cpu = only ? only : first_cpu;
    for (i = 0; i < all->len && i < top; i++) {
        McpPcCount *c = g_ptr_array_index(all, i);
        McpProfileEntry *e = g_new0(McpProfileEntry, 1);
        bool phys = c->pa != PC_NO_PHYS;
        g_autofree char *insn = cpu ? mcp_cpu_disas(cpu, phys ? c->pa : c->pc,
                                                    1, phys) : NULL;

        e->pc = g_strdup_printf("0x%" PRIx64, c->pc);
        if (phys) {
            e->physical = g_strdup_printf("0x%" PRIx64, c->pa);
        }
        e->count = c->count;
        e->percent = (c->count * 100 + p->ran / 2) / p->ran;
        if (insn) {
            /* without the address in front, which is in @pc */
            char *text = strstr(insn, ":  ");
            char *in, *out;

            text = g_strstrip(text ? text + 3 : insn);
            for (in = out = text; *in; in++) {
                char ch = *in == '\t' ? ' ' : *in;

                if (ch != ' ' || (out > text && out[-1] != ' ')) {
                    *out++ = ch;
                }
            }
            *out = '\0';
            e->instruction = g_strdup(text);
        }
        QAPI_LIST_APPEND(tail, e);
    }

    g_string_printf(s, "%.1f s, %" PRId64 " samples; halted %" PRId64 "%%",
                    ms / 1000.0, res->samples, res->halted_percent);
    if (res->paused_percent) {
        g_string_append_printf(s, ", paused %" PRId64 "%%",
                               res->paused_percent);
    }
    if (res->top) {
        g_string_append_printf(s, "; most at %s (%.1f%%)",
                               res->top->value->pc,
                               res->top->value->count * 100.0 / p->ran);
    } else {
        g_string_append(s, "; no CPU ran");
    }
    res->summary = g_strdup(s->str);
    mcp_profile_unref(p);
    return res;
}

/*
 * register-write.  GDB's stub sets a register by name.  The pc goes
 * through the CPU's own set_pc hook, as the GDB stub's "continue at"
 * does, and may change nothing else the debug tools can see: not the
 * CPU's mode (its MMU modes and the flags code is translated with),
 * and no register that does not move as far as the pc.
 */

#define PC_MODE_BITS            4       /* low bits set_pc may take a mode in */

/* What a write may change in a CPU */
typedef struct McpCpuSeen {
    uint64_t pc;
    int mmu_idx[2];                     /* for data, for code */
    uint32_t tb_flags;
    int n_regs;                         /* GDB's registers */
    GByteArray **regs;
} McpCpuSeen;

typedef enum McpRegChange {
    REG_SAME,
    REG_MOVED,                          /* by as much as asked */
    REG_OTHER,
} McpRegChange;

static void mcp_cpu_see(CPUState *cpu, McpCpuSeen *s)
{
    const TCGCPUOps *ops = cpu->cc->tcg_ops;
    int i;

    s->pc = cpu->cc->get_pc(cpu);
    s->mmu_idx[0] = ops->mmu_index(cpu, false);
    s->mmu_idx[1] = ops->mmu_index(cpu, true);
    s->tb_flags = ops->get_tb_cpu_state(cpu).flags;
    /* all of GDB's: the pc may be outside the core ones, as on SPARC */
    s->n_regs = cpu->gdb_num_regs;
    s->regs = g_new(GByteArray *, s->n_regs);
    for (i = 0; i < s->n_regs; i++) {
        s->regs[i] = g_byte_array_new();
        gdb_read_register(cpu, s->regs[i], i);
    }
}

static void mcp_cpu_seen_free(McpCpuSeen *s)
{
    int i;

    for (i = 0; i < s->n_regs; i++) {
        g_byte_array_unref(s->regs[i]);
    }
    g_free(s->regs);
}

static uint64_t mcp_reg_mask(unsigned int len)
{
    return len >= 8 ? UINT64_MAX : (1ULL << (len * 8)) - 1;
}

/* How register @n changed since @was, read into @now */
static McpRegChange mcp_reg_change(CPUState *cpu, const McpCpuSeen *was,
                                   int n, uint64_t delta, GByteArray *now)
{
    const GByteArray *old = was->regs[n];
    uint64_t a, b;

    g_byte_array_set_size(now, 0);
    gdb_read_register(cpu, now, n);
    if (now->len == old->len && !memcmp(now->data, old->data, old->len)) {
        return REG_SAME;
    }
    if (now->len == old->len && mcp_reg_decode(cpu, old, &a) &&
        mcp_reg_decode(cpu, now, &b) &&
        !((b - a - delta) & mcp_reg_mask(now->len))) {
        return REG_MOVED;
    }
    return REG_OTHER;
}

/* GDB's name of register @n, else rN */
static char *mcp_reg_name(GArray *names, int n)
{
    unsigned int i;

    for (i = 0; i < names->len; i++) {
        GDBRegDesc *r = &g_array_index(names, GDBRegDesc, i);

        if (r->gdb_reg == n && r->name) {
            return g_strdup(r->name);
        }
    }
    return g_strdup_printf("r%d", n);
}

/* A register's value: a number, else GDB's bytes in hex */
static char *mcp_reg_show(CPUState *cpu, const GByteArray *b)
{
    uint64_t v;

    if (mcp_reg_decode(cpu, b, &v)) {
        return g_strdup_printf("0x%" PRIx64, v);
    }
    return mcp_hex(b->data, b->len);
}

/*
 * Append the registers but @skip that changed since @was: all of
 * them, or those that did not move by @delta: "npc (0x2000 to 0x123c)".
 * Returns how many.
 */
static int mcp_reg_append_changes(GString *s, CPUState *cpu,
                                  const McpCpuSeen *was, bool all,
                                  uint64_t delta, int skip)
{
    g_autoptr(GArray) names = gdb_get_register_list(cpu);
    g_autoptr(GByteArray) now = g_byte_array_new();
    GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
    int i, n;

    for (i = 0; i < was->n_regs; i++) {
        McpRegChange c = mcp_reg_change(cpu, was, i, delta, now);
        g_autofree char *name = NULL;
        g_autofree char *a = NULL;
        g_autofree char *b = NULL;

        if (i == skip || c == REG_SAME || (c == REG_MOVED && !all)) {
            continue;
        }
        name = mcp_reg_name(names, i);
        a = mcp_reg_show(cpu, was->regs[i]);
        b = mcp_reg_show(cpu, now);
        g_ptr_array_add(items, g_strdup_printf("%s (%s to %s)", name, a, b));
    }
    n = items->len;
    mcp_append_list(s, items, "and");
    return n;
}

/*
 * Whether @cpu is at @want with nothing changed since @was but the
 * registers that moved as far as the pc.  The translation flags come
 * last: some CPUs check them against the rest as they make them.
 */
static bool mcp_pc_alone(CPUState *cpu, const McpCpuSeen *was, uint64_t want)
{
    const TCGCPUOps *ops = cpu->cc->tcg_ops;
    g_autoptr(GByteArray) now = g_byte_array_new();
    int i;

    if (cpu->cc->get_pc(cpu) != want ||
        ops->mmu_index(cpu, false) != was->mmu_idx[0] ||
        ops->mmu_index(cpu, true) != was->mmu_idx[1]) {
        return false;
    }
    for (i = 0; i < was->n_regs; i++) {
        if (mcp_reg_change(cpu, was, i, want - was->pc, now) == REG_OTHER) {
            return false;
        }
    }
    return ops->get_tb_cpu_state(cpu).flags == was->tb_flags;
}

/*
 * Put @cpu at @want through set_pc, with @mode in the low bits: some
 * CPUs take a mode with the pc there (Thumb, MIPS16, PALcode, privilege
 * levels).  When the pc the tools show adds a base, such as the code
 * segment's on x86, set_pc takes the offset.
 */
static void mcp_pc_set(CPUState *cpu, uint64_t want, int mode)
{
    vaddr v = want | mode;
    vaddr got;

    cpu_set_pc(cpu, v);
    got = cpu->cc->get_pc(cpu);
    if (got != want) {
        cpu_set_pc(cpu, v - (got - want));
    }
}

/* Put @cpu back as @was saw it: the pc and the mode, then the registers */
static bool mcp_pc_restore(CPUState *cpu, const McpCpuSeen *was)
{
    const TCGCPUOps *ops = cpu->cc->tcg_ops;
    g_autoptr(GByteArray) now = g_byte_array_new();
    int mode, i;

    for (mode = 0; mode < PC_MODE_BITS; mode++) {
        mcp_pc_set(cpu, was->pc, mode);
        if (cpu->cc->get_pc(cpu) == was->pc &&
            ops->mmu_index(cpu, false) == was->mmu_idx[0] &&
            ops->mmu_index(cpu, true) == was->mmu_idx[1]) {
            break;
        }
    }
    for (i = 0; i < was->n_regs; i++) {
        if (was->regs[i]->len &&
            mcp_reg_change(cpu, was, i, 0, now) != REG_SAME) {
            gdb_write_register(cpu, was->regs[i]->data, i);
        }
    }
    return mcp_pc_alone(cpu, was, was->pc);
}

/*
 * @s as the value of a register of @len bytes: hex with 0x, decimal, or
 * a negative decimal
 */
static bool mcp_parse_reg_value(const char *s, unsigned int len,
                                uint64_t *val, Error **errp)
{
    uint64_t mask = mcp_reg_mask(len);
    int64_t neg;

    if (s[0] == '-') {
        if (qemu_strtoi64(s, NULL, 10, &neg) < 0) {
            error_setg(errp, "value '%s' is not a number: write hex with 0x,"
                       " decimal, or a negative decimal", s);
            return false;
        }
        if (len < 8 && neg < -(int64_t)(mask >> 1) - 1) {
            error_setg(errp, "value %s does not fit in %u byte%s", s, len,
                       len == 1 ? "" : "s");
            return false;
        }
        *val = (uint64_t)neg & mask;
        return true;
    }
    if (!mcp_parse_address(s, "value", val, errp)) {
        return false;
    }
    if (*val & ~mask) {
        error_setg(errp, "value %s does not fit in %u byte%s", s, len,
                   len == 1 ? "" : "s");
        return false;
    }
    return true;
}

static McpRegisterWriteResult *mcp_pc_write(CPUState *cpu, const char *value,
                                            const McpCpuSeen *was,
                                            Error **errp)
{
    g_autoptr(GString) s = g_string_new(NULL);
    g_autoptr(GString) why = g_string_new(NULL);
    McpRegisterWriteResult *res;
    const char *hint = "";
    uint64_t want, pc;
    int mode;

    if (!mcp_parse_reg_value(value, 8, &want, errp)) {
        return NULL;
    }
    for (mode = 0; mode < PC_MODE_BITS; mode++) {
        mcp_pc_set(cpu, want, mode);
        if (mcp_pc_alone(cpu, was, want)) {
            break;
        }
        if (mode) {
            continue;
        }
        /* what the plain value changed, for the error */
        pc = cpu->cc->get_pc(cpu);
        if (pc != want) {
            g_string_printf(why, "put it at 0x%" PRIx64 " instead", pc);
            continue;
        }
        g_string_assign(why, "also change ");
        if (!mcp_reg_append_changes(why, cpu, was, false, want - was->pc,
                                    -1)) {
            g_string_append(why, "its mode");
        }
        hint = ". Set the registers by name instead, or, in a branch delay"
               " slot, debug-step once first";
    }
    if (mode == PC_MODE_BITS) {
        if (!mcp_pc_restore(cpu, was)) {
            error_setg(errp, "setting the pc of CPU %d to 0x%" PRIx64
                       " would %s, and CPU %d could not be put back exactly"
                       " as it was", cpu->cpu_index, want, why->str,
                       cpu->cpu_index);
            return NULL;
        }
        error_setg(errp, "CPU %d stays at 0x%" PRIx64 ": setting its pc to"
                   " 0x%" PRIx64 " would %s%s", cpu->cpu_index, was->pc,
                   want, why->str, hint);
        return NULL;
    }

    res = g_new0(McpRegisterWriteResult, 1);
    res->before = g_strdup_printf("0x%" PRIx64, was->pc);
    res->after = g_strdup_printf("0x%" PRIx64, want);
    g_string_printf(s, "set the pc of CPU %d to 0x%" PRIx64 " (it was 0x%"
                    PRIx64 ")", cpu->cpu_index, want, was->pc);
    if (want != was->pc) {
        g_string_append(s, ", in ");
        mcp_reg_append_changes(s, cpu, was, true, 0, -1);
    }
    res->summary = g_strdup(s->str);
    return res;
}

static McpRegisterWriteResult *mcp_reg_write(CPUState *cpu, int reg,
                                             const char *value,
                                             const McpCpuSeen *was,
                                             Error **errp)
{
    g_autoptr(GArray) names = gdb_get_register_list(cpu);
    g_autoptr(GByteArray) old = g_byte_array_new();
    g_autoptr(GByteArray) now = g_byte_array_new();
    g_autoptr(GString) s = g_string_new(NULL);
    g_autoptr(GString) also = g_string_new(NULL);
    g_autofree char *name = mcp_reg_name(names, reg);
    bool be = cpu_internal_is_big_endian(cpu);
    McpRegisterWriteResult *res;
    uint64_t want, before, after, pc;
    uint8_t buf[8];
    unsigned int i;

    gdb_read_register(cpu, old, reg);
    if (!mcp_reg_decode(cpu, old, &before)) {
        error_setg(errp, "%s has %u bytes; register-write sets registers of"
                   " 1 to 8 bytes", name, old->len);
        return NULL;
    }
    if (!mcp_parse_reg_value(value, old->len, &want, errp)) {
        return NULL;
    }
    /* in the byte order mcp_reg_decode() reads */
    for (i = 0; i < old->len; i++) {
        buf[be ? old->len - 1 - i : i] = want >> (8 * i);
    }
    if (!gdb_write_register(cpu, buf, reg)) {
        error_setg(errp, "CPU %d does not let GDB's stub write %s",
                   cpu->cpu_index, name);
        return NULL;
    }
    gdb_read_register(cpu, now, reg);
    if (!mcp_reg_decode(cpu, now, &after)) {
        after = want;
    }

    res = g_new0(McpRegisterWriteResult, 1);
    res->before = g_strdup_printf("0x%" PRIx64, before);
    res->after = g_strdup_printf("0x%" PRIx64, after);
    g_string_printf(s, "set %s of CPU %d to 0x%" PRIx64 " (it was 0x%" PRIx64
                    ")", name, cpu->cpu_index, want, before);
    if (after != want) {
        g_string_append_printf(s, ", but it reads back 0x%" PRIx64, after);
    }
    pc = cpu->cc->get_pc(cpu);
    if (pc != was->pc) {
        g_string_append_printf(s, "; the pc moved with it, to 0x%" PRIx64
                               " (it was 0x%" PRIx64 ")", pc, was->pc);
    }
    if (mcp_reg_append_changes(also, cpu, was, true, 0, reg)) {
        g_string_append_printf(s, "; it also changed %s", also->str);
    }
    res->summary = g_strdup(s->str);
    return res;
}

McpRegisterWriteResult *qmp_register_write(const char *q_register,
                                           const char *value, bool has_cpu,
                                           int64_t cpu_index, Error **errp)
{
    McpRegisterWriteResult *res;
    McpCpuSeen was;
    CPUState *cpu;
    int reg = -1;

    if (!tcg_enabled()) {
        error_setg(errp, "register-write needs TCG (-accel tcg)");
        return NULL;
    }
    if (replay_mode != REPLAY_MODE_NONE) {
        error_setg(errp, "registers are not written while QEMU records or"
                   " replays");
        return NULL;
    }
    if (mcp_debug.busy) {
        error_setg(errp, "a debug tool is running the VM; wait until it"
                   " ends");
        return NULL;
    }
    if (runstate_is_running()) {
        error_setg(errp, "the VM is running: stop it first with debug-run,"
                   " debug-step or vm-power pause");
        return NULL;
    }
    cpu = mcp_debug_cpu(has_cpu, cpu_index, errp);
    if (!cpu) {
        return NULL;
    }
    if (g_ascii_strcasecmp(q_register, "pc")) {
        reg = mcp_reg_number(cpu, q_register);
        if (reg < 0) {
            g_autofree char *names = mcp_reg_names(cpu);

            error_setg(errp, "'%s' is not a register; the registers are %s",
                       q_register, names);
            return NULL;
        }
    }
    mcp_cpu_see(cpu, &was);
    if (reg < 0) {
        res = mcp_pc_write(cpu, value, &was, errp);
    } else {
        res = mcp_reg_write(cpu, reg, value, &was, errp);
    }
    mcp_cpu_seen_free(&was);
    return res;
}

#endif /* CONFIG_TCG */
