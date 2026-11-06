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
#include "exec/cpu-interrupt.h"
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

/*
 * The watched CPUs that stand at @addr, about to run the instruction
 * there.  A halted CPU there has not got there yet: it goes on with an
 * interrupt.
 */
static int mcp_debug_count_at(CPUState *only, uint64_t addr, CPUState **at)
{
    CPUState *cpu;
    int n = 0;

    CPU_FOREACH(cpu) {
        if ((!only || cpu == only) && !cpu->halted &&
            cpu->cc->get_pc(cpu) == mcp_debug_bp_pc(cpu, addr)) {
            if (!n) {
                *at = cpu;
            }
            n++;
        }
    }
    return n;
}

typedef struct McpWatchSeen {
    CPUState *cpu;
    uint64_t addr;
    uint64_t pc;
    bool write;
} McpWatchSeen;

typedef struct McpBreak {
    CPUState *only;
    bool has_addr;
    uint64_t addr;
    int hits;                   /* CPUs there as the time ran out */
    bool has_watch;             /* the watched memory */
    uint64_t waddr;
    uint64_t wlen;
    int wflags;                 /* BP_MEM_READ, BP_MEM_WRITE or both */
    int whits;                  /* CPUs that touched it as the time ran out */
    McpWatchSeen seen;
} McpBreak;

/*
 * Breakpoints are checked when a CPU looks for the next block of
 * translated code, and chained blocks don't look: flush them all, so
 * that every way to the address looks again.  Watchpoints flush the
 * TLB for their pages themselves.  The VM is stopped.
 */
static void mcp_debug_insert(McpBreak *b)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (b->only && cpu != b->only) {
            continue;
        }
        if (b->has_addr) {
            cpu_breakpoint_insert(cpu, b->addr, BP_GDB, NULL);
        }
        if (b->has_watch) {
            cpu_watchpoint_insert(cpu, b->waddr, b->wlen,
                                  BP_GDB | b->wflags, NULL);
        }
    }
    if (b->has_addr) {
        tb_flush__exclusive_or_serial();
    }
}

/*
 * Our watchpoint on @cpu, found again by what it watches: a GDB client
 * that detaches removes all BP_GDB watchpoints, ours too
 */
static CPUWatchpoint *mcp_watch_find(McpBreak *b, CPUState *cpu)
{
    CPUWatchpoint *wp;

    QTAILQ_FOREACH(wp, &cpu->watchpoints, entry) {
        if (wp->vaddr == b->waddr && wp->len == b->wlen &&
            (wp->flags & ~BP_WATCHPOINT_HIT) == (BP_GDB | b->wflags)) {
            return wp;
        }
    }
    return NULL;
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

    if (!b->has_watch || !wp || wp != mcp_watch_find(b, cpu)) {
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

/* Breakpoints by address, not by reference, for the same reason */
static void mcp_debug_remove(McpBreak *b)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        CPUWatchpoint *wp;

        if (b->only && cpu != b->only) {
            continue;
        }
        if (b->has_addr) {
            cpu_breakpoint_remove(cpu, b->addr, BP_GDB);
        }
        wp = b->has_watch ? mcp_watch_find(b, cpu) : NULL;
        if (wp) {
            /* a hit that has not stopped the VM yet never will */
            if (cpu->watchpoint_hit == wp) {
                mcp_watch_clear_hit(cpu, wp);
            }
            cpu_watchpoint_remove_by_ref(cpu, wp);
        }
    }
}

/* CPUs at the address, or that touched the watched memory */
static void mcp_debug_late_hits(McpBreak *b)
{
    CPUState *dummy;

    b->whits = mcp_watch_hits(b, &b->seen);
    b->hits = b->has_addr ? mcp_debug_count_at(b->only, b->addr, &dummy) : 0;
}

/*
 * The time ran out with the VM running: take the breakpoints away with
 * the vCPUs paused.  A CPU may have got to the address just now; then
 * the VM stops there, as for any hit.  A CPU that hit the watchpoint
 * has done the access by now (its last instruction runs with
 * interrupts off) and has asked to stop the VM: it stops.
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
    if (b->hits || b->whits) {
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
 * The value of register @name of @cpu, when it is an integer: pc is
 * where the debug tools say the CPU is, the others are GDB's
 */
static bool mcp_reg_value(CPUState *cpu, const char *name, uint64_t *val)
{
    g_autoptr(GByteArray) buf = NULL;
    int reg, len, i;
    bool be;

    if (!g_ascii_strcasecmp(name, "pc")) {
        *val = cpu->cc->get_pc(cpu);
        return true;
    }
    reg = mcp_reg_number(cpu, name);
    if (reg < 0) {
        return false;
    }
    buf = g_byte_array_new();
    len = gdb_read_register(cpu, buf, reg);
    if (len != 1 && len != 2 && len != 4 && len != 8) {
        return false;
    }
    /* in the byte order the CPU runs in now, as GDB wants it */
    be = cpu_internal_is_big_endian(cpu);
    *val = 0;
    for (i = 0; i < len; i++) {
        *val = *val << 8 | buf->data[be ? i : len - 1 - i];
    }
    return true;
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
    /* one more, for the watched memory */
    mem = g_new0(McpMem, n + 1);
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

/*
 * The memory to watch into @b, and into @shown the memory to show when
 * the VM stops.  Registers are read now, from the CPU the tool shows.
 */
static bool mcp_parse_watch(McpWatch *w, McpBreak *b, McpMem *shown,
                            Error **errp)
{
    static const int flags[] = {
        [MCP_WATCH_ACCESS_WRITE] = BP_MEM_WRITE,
        [MCP_WATCH_ACCESS_READ] = BP_MEM_READ,
        [MCP_WATCH_ACCESS_ACCESS] = BP_MEM_ACCESS,
    };
    CPUState *cpu = b->only ? b->only : mcp_debug_cpu(false, 0, errp);
    McpMem m = { 0 };
    const char *bad = NULL;

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
    if (!mcp_mem_address(cpu, &m, &b->waddr, &bad)) {
        error_setg(errp, "watch address '%s': CPU %d has no integer"
                   " register %s", w->address, cpu->cpu_index, bad);
        return false;
    }
    b->wlen = w->has_length ? w->length : WATCH_LEN;
    if (b->wlen < 1 || b->wlen > WATCH_LEN_MAX) {
        error_setg(errp, "watch length must be 1 to %d", WATCH_LEN_MAX);
        return false;
    }
    if (b->waddr + b->wlen - 1 < b->waddr) {
        error_setg(errp, "watched memory runs past the end of the address"
                   " space");
        return false;
    }
    b->wflags = flags[w->has_access ? w->access : MCP_WATCH_ACCESS_WRITE];
    b->has_watch = true;

    shown->n_terms = 1;
    shown->term[0].num = b->waddr;
    shown->words = MIN(DIV_ROUND_UP(b->wlen + (b->waddr & 3), 4),
                       WATCH_WORDS_SHOWN);
    shown->term[0].num &= ~3ULL;
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

/* "reached 0x1000", "wrote to 0x2000", or both, for the summary */
static void mcp_debug_what(GString *s, McpBreak *b)
{
    static const char *const verb[] = {
        [BP_MEM_WRITE] = "wrote to",
        [BP_MEM_READ] = "read",
        [BP_MEM_ACCESS] = "touched",
    };

    if (b->has_addr) {
        g_string_append_printf(s, "reached 0x%" PRIx64, b->addr);
    }
    if (b->has_addr && b->has_watch) {
        g_string_append(s, " or ");
    }
    if (b->has_watch) {
        g_string_append_printf(s, "%s 0x%" PRIx64, verb[b->wflags],
                               b->waddr);
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
    /* debug-trace: each hit, with the VM stopped there */
    void (*log)(McpRun *r, CPUState *cpu, const McpWatchSeen *seen,
                bool retry);
    int64_t n_hits;
    int64_t n_retries;
    CPUState *at;               /* the last hit */
    bool by_watch;
    bool retry;
    McpWatchSeen seen;
    bool late;                  /* hits as the time ran out */
    McpRunResult result;
};

static void mcp_run_hit(McpRun *r, CPUState *cpu, const McpWatchSeen *seen,
                        bool retry)
{
    r->n_hits++;
    r->n_retries += retry;
    r->at = cpu;
    r->by_watch = seen;
    r->retry = retry;
    if (seen) {
        r->seen = *seen;
    }
    if (r->log) {
        r->log(r, cpu, seen, retry);
    }
}

/* The CPUs stopped at the address have got there */
static void mcp_run_arrivals(McpRun *r)
{
    CPUState *cpu, *dummy;

    if (!r->b.has_addr) {
        return;
    }
    CPU_FOREACH(cpu) {
        if ((!r->b.only || cpu == r->b.only) &&
            mcp_debug_count_at(cpu, r->b.addr, &dummy)) {
            mcp_run_hit(r, cpu, NULL, mcp_debug_retry(cpu, r->b.addr));
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

static void mcp_run_access(McpRun *r, CPUState *cpu)
{
    McpWatchSeen seen;

    if (mcp_watch_take(&r->b, cpu, &seen)) {
        mcp_run_hit(r, cpu, &seen, false);
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
            CPUState *dummy;

            if (b->has_addr && (!b->only || cpu == b->only) &&
                mcp_debug_count_at(cpu, b->addr, &dummy)) {
                step = mcp_debug_step_one(ctx, cpu, &interrupted);
                /* that instruction may touch the watched memory */
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
            if (b->hits || b->whits) {
                /* got there as the time ran out; its stop comes next */
                mcp_debug_wait_stop(ctx, qemu_clock_get_ms(QEMU_CLOCK_REALTIME)
                                    + STEP_WAIT_MS, NULL);
                r->late = true;
                mcp_run_arrivals(r);
                if (b->whits) {
                    mcp_run_hit(r, b->seen.cpu, &b->seen, false);
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
        mcp_debug_remove(b);
    }
}

/* Where to stop or what to log: an address, watched memory, a CPU */
static bool mcp_run_parse(McpRun *r, const char *address, bool has_cpu,
                          int64_t cpu_index, Error **errp)
{
    if (address && !mcp_parse_address(address, "address", &r->b.addr,
                                      errp)) {
        return false;
    }
    r->b.has_addr = address;
    if (has_cpu) {
        r->b.only = mcp_debug_cpu(true, cpu_index, errp);
        if (!r->b.only) {
            return false;
        }
    }
    return true;
}

McpDebugStop *coroutine_fn qmp_debug_run(const char *address,
                                         McpWatch *watch,
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
    if (!address && !watch) {
        error_setg(errp, "give an address to stop at, memory to watch, or"
                   " both");
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
    if (!mcp_run_parse(&r, address, has_cpu, cpu_index, errp)) {
        return NULL;
    }
    mem = mcp_parse_memory(b->only ? b->only : first_cpu, memory, &n_mem,
                           errp);
    if (!mem) {
        return NULL;
    }
    if (watch && !mcp_parse_watch(watch, b, &mem[n_mem], errp)) {
        return NULL;
    }
    if (!mcp_debug_begin(r.ctx, errp)) {
        return NULL;
    }
    r.end = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + timeout_ms;
    r.limit = hits;
    hit_word = !b->has_watch ? "arrival" : b->has_addr ? "hit" : "access";

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
    switch (r.result) {
    case RUN_REACHED:
        if (r.by_watch) {
            g_autofree char *insn = mcp_debug_insn(at, seen->pc);

            g_string_printf(s, "CPU %d %s 0x%" PRIx64, at->cpu_index,
                            seen->write ? "wrote to" : "read", seen->addr);
            if (hits > 1) {
                g_string_append_printf(s, " (%s %" PRId64 ")", hit_word,
                                       r.n_hits);
            }
            g_string_append_printf(s, "; the VM is stopped after the"
                                   " instruction that did it:\n%s",
                                   insn ? insn : "");
            break;
        }
        g_string_printf(s, "CPU %d reached 0x%" PRIx64, at->cpu_index,
                        b->addr);
        if (hits > 1) {
            g_string_append_printf(s, " (%s %" PRId64 ")", hit_word,
                                   r.n_hits);
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
            g_string_printf(s, "CPU %d %s 0x%" PRIx64 " as the time ran out"
                            " (%s %" PRId64 " of %" PRId64 "); the VM is"
                            " stopped", at->cpu_index,
                            r.by_watch ? (seen->write ? "wrote to" : "read") :
                            "reached", r.by_watch ? seen->addr : b->addr,
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
        mcp_debug_show(s, cpu, mem, n_mem + b->has_watch);
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
    char *insn;                 /* as it was the first time */
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
} McpTracer;

static void mcp_trace_log(McpRun *r, CPUState *cpu, const McpWatchSeen *seen,
                          bool retry)
{
    McpTracer *t = container_of(r, McpTracer, r);
    uint64_t pc = seen ? seen->pc : r->b.addr;
    McpTraceCount *c = NULL;
    McpTraceEntry *e;
    strList **vtail, *l;
    bool first = false;
    int i;

    /* which instructions touch the watched memory */
    if (seen) {
        c = g_hash_table_lookup(t->insns, &pc);
        if (!c && g_hash_table_size(t->insns) < TRACE_INSNS_MAX) {
            c = g_new0(McpTraceCount, 1);
            c->pc = pc;
            c->insn = mcp_debug_insn(cpu, pc);
            if (c->insn) {
                g_strchomp(c->insn);
            }
            g_hash_table_insert(t->insns, &c->pc, c);
            first = true;
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

/* "arrivals at 0x1000 and writes to 0x2000 (4 bytes)" */
static void mcp_trace_what(GString *s, McpBreak *b)
{
    static const char *const what[] = {
        [BP_MEM_WRITE] = "writes to",
        [BP_MEM_READ] = "reads of",
        [BP_MEM_ACCESS] = "accesses to",
    };

    if (b->has_addr) {
        g_string_append_printf(s, "arrivals at 0x%" PRIx64, b->addr);
    }
    if (b->has_addr && b->has_watch) {
        g_string_append(s, " and ");
    }
    if (b->has_watch) {
        g_string_append_printf(s, "%s 0x%" PRIx64 " (%" PRIu64 " byte%s)",
                               what[b->wflags], b->waddr, b->wlen,
                               b->wlen == 1 ? "" : "s");
    }
}

McpTrace *coroutine_fn qmp_debug_trace(const char *address, McpWatch *watch,
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
    g_autoptr(GPtrArray) sorted = NULL;
    g_autofree McpMem *mem = NULL;
    McpTracer t = {
        .r.ctx = mcp_current_input_ctx(),
        .r.log = mcp_trace_log,
        .text = text,
        .insns = insns,
    };
    McpBreak *b = &t.r.b;
    McpMem shown = { 0 };       /* each line shows the watched word */
    McpTraceInstructionList **itail;
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
    if (!address && !watch) {
        error_setg(errp, "give an address to trace, memory to watch, or"
                   " both");
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
    if (!mcp_run_parse(&t.r, address, has_cpu, cpu_index, errp)) {
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
    if (watch && !mcp_parse_watch(watch, b, &shown, errp)) {
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

    if (b->has_watch) {
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
            g_string_append(s, "\n\nBy instruction:\n");
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
        } else if (b->has_watch && !list) {
            g_string_append(s, "Hits (the first access by each"
                            " instruction):\n");
        } else if (b->has_watch) {
            g_string_append_printf(s, "Hits (the first %" PRId64 ", then the"
                                   " first access by each new"
                                   " instruction):\n", list);
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

#endif /* CONFIG_TCG */
