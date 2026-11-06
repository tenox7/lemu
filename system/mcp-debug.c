/*
 * Debug tools for MCP clients: run to an address, single steps, a
 * profile of where the CPUs run, and memory writes (see qapi/mcp.json).
 * They do what agents used to do with GDB scripts on the GDB stub.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "exec/replay-core.h"
#include "hw/core/cpu.h"
#include "monitor/mcp.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-mcp.h"
#include "qemu/coroutine.h"
#include "qemu/cutils.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/cpus.h"
#include "system/hw_accel.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "system/tcg.h"
#include "ui/input.h"
#ifdef CONFIG_TCG
#include "exec/tb-flush.h"
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

typedef struct McpBreak {
    CPUState *only;
    uint64_t addr;
    int hits;
    CPUState *at;
} McpBreak;

/*
 * Breakpoints are checked when a CPU looks for the next block of
 * translated code, and chained blocks don't look: flush them all, so
 * that every way to the address looks again.  The VM is stopped.
 */
static void mcp_debug_insert(McpBreak *b)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (!b->only || cpu == b->only) {
            cpu_breakpoint_insert(cpu, b->addr, BP_GDB, NULL);
        }
    }
    tb_flush__exclusive_or_serial();
}

/*
 * By address, not by reference: a GDB client that detaches removes
 * all BP_GDB breakpoints, ours too.
 */
static void mcp_debug_remove(McpBreak *b)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (!b->only || cpu == b->only) {
            cpu_breakpoint_remove(cpu, b->addr, BP_GDB);
        }
    }
}

/*
 * The time ran out with the VM running: take the breakpoints away with
 * the vCPUs paused.  A CPU may have got to the address just now; then
 * the VM stops there, as for any hit.
 */
static void mcp_debug_remove_running(void *opaque)
{
    McpBreak *b = opaque;

    if (!runstate_is_running()) {
        mcp_debug_remove(b);
        return;
    }
    pause_all_vcpus();
    mcp_debug_remove(b);
    b->hits = mcp_debug_count_at(b->only, b->addr, &b->at);
    if (b->hits) {
        qemu_system_debug_request();
    } else {
        resume_all_vcpus();
    }
}

typedef struct McpMem {
    uint64_t addr;
    int words;
    bool physical;
} McpMem;

static McpMem *mcp_parse_memory(McpDebugMemoryList *list, int *count,
                                Error **errp)
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
    mem = g_new0(McpMem, MAX(n, 1));
    for (l = list, n = 0; l; l = l->next, n++) {
        McpDebugMemory *d = l->value;

        if (!mcp_parse_address(d->address, "memory address", &mem[n].addr,
                               errp)) {
            return NULL;
        }
        mem[n].words = d->has_words ? d->words : MEMORY_WORDS;
        if (mem[n].words < 1 || mem[n].words > MEMORY_WORDS_MAX) {
            error_setg(errp, "words must be 1 to %d", MEMORY_WORDS_MAX);
            return NULL;
        }
        mem[n].physical = d->has_physical && d->physical;
    }
    *count = n;
    return g_steal_pointer(&mem);
}

/* The next instruction of @cpu, its registers and the memory asked for */
static void mcp_debug_show(GString *s, CPUState *cpu, McpMem *mem, int n)
{
    g_autofree char *insn = mcp_cpu_disas(cpu, cpu->cc->get_pc(cpu), 1);
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
        g_autofree char *dump = mcp_cpu_memory(cpu, mem[i].addr,
                                               mem[i].words,
                                               mem[i].physical);

        g_string_append_printf(s, "\nMemory at %s0x%" PRIx64 ":\n%s",
                               mem[i].physical ? "physical " : "",
                               mem[i].addr, dump ? dump : "");
    }
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

McpDebugStop *coroutine_fn qmp_debug_run(const char *address,
                                         bool has_hits, int64_t hits,
                                         bool has_cpu, int64_t cpu_index,
                                         bool has_memory,
                                         McpDebugMemoryList *memory,
                                         bool has_timeout_ms,
                                         int64_t timeout_ms, Error **errp)
{
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_current_input_ctx();
    g_autoptr(GString) s = g_string_new(NULL);
    g_autofree McpMem *mem = NULL;
    McpBreak b = { 0 };
    RunState paused = RUN_STATE_PAUSED;
    McpRunResult result = RUN_TIMEOUT;
    McpStepResult step;
    McpDebugStop *res;
    CPUState *at = NULL, *cpu;
    int64_t end, n_hits = 0;
    int n_mem = 0, k;
    bool inserted = false;

    if (!ctx) {
        qemu_input_ctx_init(&local, NULL);
        ctx = &local;
    }
    hits = has_hits ? hits : 1;
    if (hits < 1 || hits > RUN_HITS_MAX) {
        error_setg(errp, "hits must be 1 to %d", RUN_HITS_MAX);
        return NULL;
    }
    timeout_ms = has_timeout_ms ? timeout_ms : RUN_DEFAULT_MS;
    timeout_ms = MIN(MAX(timeout_ms, 0), mcp_wait_max_ms());
    if (!mcp_parse_address(address, "address", &b.addr, errp)) {
        return NULL;
    }
    mem = mcp_parse_memory(memory, &n_mem, errp);
    if (!mem) {
        return NULL;
    }
    if (has_cpu) {
        b.only = mcp_debug_cpu(true, cpu_index, errp);
        if (!b.only) {
            return NULL;
        }
    }
    if (!mcp_debug_begin(ctx, errp)) {
        return NULL;
    }
    end = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + timeout_ms;

    /* a CPU there when the running VM stops has got there */
    if (runstate_is_running()) {
        mcp_debug_outside(mcp_debug_stop, &paused);
        if (!runstate_is_running()) {
            n_hits = mcp_debug_count_at(b.only, b.addr, &at);
        }
    }
    while (n_hits < hits) {
        if (runstate_is_running()) {
            result = RUN_STOPPED;       /* someone else started it */
            break;
        }
        if (!inserted) {
            mcp_debug_insert(&b);
            inserted = true;
        }
        /* a CPU stopped at the address goes on to the next arrival */
        step = STEP_DONE;
        CPU_FOREACH(cpu) {
            CPUState *dummy;

            if ((!b.only || cpu == b.only) &&
                mcp_debug_count_at(cpu, b.addr, &dummy)) {
                step = mcp_debug_step(ctx, cpu);
                if (step != STEP_DONE && step != STEP_HALTED) {
                    break;
                }
            }
        }
        if (step == STEP_STOPPED || step == STEP_NOT_STARTED) {
            result = RUN_STOPPED;
            break;
        }
        if (ctx->cancelled) {
            result = RUN_CANCELLED;
            break;
        }
        if (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) >= end) {
            break;
        }

        mcp_debug_outside(mcp_debug_start, NULL);
        if (!mcp_debug_wait_stop(ctx, end, NULL)) {
            result = ctx->cancelled ? RUN_CANCELLED : RUN_TIMEOUT;
            mcp_debug_outside(mcp_debug_remove_running, &b);
            inserted = false;
            if (b.hits) {
                /* got there as the time ran out; its stop comes next */
                n_hits += b.hits;
                at = b.at;
                result = n_hits >= hits ? RUN_REACHED : RUN_STOPPED;
                mcp_debug_wait_stop(ctx, qemu_clock_get_ms(QEMU_CLOCK_REALTIME)
                                    + STEP_WAIT_MS, NULL);
            }
            break;
        }
        k = runstate_check(RUN_STATE_DEBUG) ?
            mcp_debug_count_at(b.only, b.addr, &at) : 0;
        if (!k) {
            at = NULL;
            result = RUN_STOPPED;
            break;
        }
        n_hits += k;
    }
    if (n_hits >= hits) {
        result = RUN_REACHED;
    }
    if (inserted) {
        mcp_debug_remove(&b);
    }
    /* the time ran out or the call was cancelled: the guest goes on */
    if ((result == RUN_TIMEOUT || result == RUN_CANCELLED) &&
        !runstate_is_running()) {
        mcp_debug_outside(mcp_debug_start, NULL);
    }
    mcp_debug_end();
    if (result == RUN_CANCELLED) {
        error_setg(errp, "cancelled");
        return NULL;
    }

    res = g_new0(McpDebugStop, 1);
    res->reached = result == RUN_REACHED;
    res->has_hits = true;
    res->hits = n_hits;
    if (at) {
        mcp_debug_set_cpu(res, at);
    }
    res->running = runstate_is_running();
    switch (result) {
    case RUN_REACHED:
        g_string_printf(s, "CPU %d reached 0x%" PRIx64, at->cpu_index,
                        b.addr);
        if (hits > 1) {
            g_string_append_printf(s, " (arrival %" PRId64 ")", n_hits);
        }
        g_string_append(s, "; the VM is stopped");
        break;
    case RUN_TIMEOUT:
        g_string_printf(s, "no CPU reached 0x%" PRIx64 " in %.1f s", b.addr,
                        timeout_ms / 1000.0);
        if (hits > 1) {
            g_string_append_printf(s, " (%" PRId64 " of %" PRId64
                                   " arrivals)", n_hits, hits);
        }
        g_string_append(s, "; the VM keeps running");
        break;
    case RUN_STOPPED:
        if (at) {
            g_string_printf(s, "CPU %d reached 0x%" PRIx64 " as the time ran "
                            "out (arrival %" PRId64 " of %" PRId64 "); "
                            "the VM is stopped", at->cpu_index, b.addr,
                            n_hits, hits);
        } else if (res->running) {
            g_string_printf(s, "something else started the VM before a CPU "
                            "reached 0x%" PRIx64, b.addr);
        } else {
            g_string_printf(s, "the VM stopped (%s) before a CPU reached 0x%"
                            PRIx64, RunState_str(runstate_get()), b.addr);
        }
        break;
    default:
        g_assert_not_reached();
    }
    if (!res->running) {
        cpu = at ? at : b.only ? b.only : first_cpu;
        if (!at) {
            g_string_append_printf(s, "; CPU %d:", cpu->cpu_index);
        }
        mcp_debug_show(s, cpu, mem, n_mem);
    }
    res->summary = g_strdup(s->str);
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

    if (!ctx) {
        qemu_input_ctx_init(&local, NULL);
        ctx = &local;
    }
    count = has_count ? count : 1;
    if (count < 1 || count > STEP_MAX) {
        error_setg(errp, "count must be 1 to %d", STEP_MAX);
        return NULL;
    }
    mem = mcp_parse_memory(memory, &n_mem, errp);
    if (!mem) {
        return NULL;
    }
    cpu = mcp_debug_cpu(has_cpu, cpu_index, errp);
    if (!cpu || !mcp_debug_begin(ctx, errp)) {
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
        insn = mcp_cpu_disas(cpu, cpu->cc->get_pc(cpu), 1);
        step = mcp_debug_step(ctx, cpu);
        if (step == STEP_NOT_STARTED || step == STEP_CANCELLED) {
            g_free(insn);
            why = step == STEP_CANCELLED ? "cancelled" :
                  "the VM did not start";
            break;
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

typedef struct McpPcCount {
    uint64_t pc;
    int64_t count;
} McpPcCount;

/*
 * The samples, shared by the tool and its queued work: the last of
 * them to finish frees it.
 */
typedef struct McpProfileData {
    int refs;
    GHashTable *pcs;            /* &McpPcCount.pc -> McpPcCount */
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
        uint64_t pc = cpu->cc->get_pc(cpu);
        McpPcCount *c = g_hash_table_lookup(p->pcs, &pc);

        if (!c) {
            c = g_new0(McpPcCount, 1);
            c->pc = pc;
            g_hash_table_insert(p->pcs, &c->pc, c);
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
    return x->pc < y->pc ? -1 : x->pc > y->pc;
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
    p->pcs = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL,
                                   g_free);
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
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
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
        g_autofree char *insn = cpu ? mcp_cpu_disas(cpu, c->pc, 1) : NULL;

        e->pc = g_strdup_printf("0x%" PRIx64, c->pc);
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
