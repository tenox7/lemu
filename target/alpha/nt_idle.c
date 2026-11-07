/*
 * Alpha: halt the vCPU in the Windows NT idle loop
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Windows NT for Alpha spins when it is idle. KiIdleLoop checks its DPC
 * queue and its next thread over and over; in NT 3.51 and 4.0 it also calls
 * HalProcessorIdle, which just enables interrupts and returns. The
 * translator finds the loop (gen_nt_idle()) and calls helper_nt_idle() with
 * interrupts enabled, which halts the vCPU until the next interrupt when
 * there is no work. With one CPU only an interrupt can bring work.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "cpu.h"
#include "internals.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/helper-proto.h"
#include "system/memory.h"
#include "trace.h"

/*
 * Spin on for NT_IDLE_POLL_NS after the idle loop was entered, as an I/O
 * often completes that soon and waking a halted vCPU takes longer (KVM's
 * halt polling does the same). A call more than NT_IDLE_GAP_NS after the
 * previous one means the loop was left between.
 */
#define NT_IDLE_POLL_NS     (100 * SCALE_US)
#define NT_IDLE_GAP_NS      (20 * SCALE_US)

/*
 * Read a longword of NT kernel data straight from guest memory. NT keeps
 * it in KSEG0, the superpage that maps the first GB of physical memory.
 */
static bool nt_idle_read(CPUAlphaState *env, uint64_t va, uint32_t *val)
{
    MemTxResult res;

    if ((va >> 30) != 0x3fffffffeULL || (va & 3)) {
        return false;
    }
    *val = address_space_ldl_le(env_cpu(env)->as, va & 0x3fffffff,
                                MEMTXATTRS_UNSPECIFIED, &res);
    return res == MEMTX_OK;
}

/* The address that the ldl instruction insn loads from */
static uint64_t nt_idle_addr(CPUAlphaState *env, uint32_t insn)
{
    return env->gpregs[extract32(insn, 16, 5)] + sextract32(insn, 0, 16);
}

/*
 * The idle loop runs with interrupts enabled at this instruction. If
 * there is no work, halt until the next interrupt: no interrupt pending,
 * the DPC queue that the ldl dpc loads from is empty and the next thread
 * that the ldl next loads is 0. With list, dpc loads the head of the DPC
 * list, which is empty when it points to itself; otherwise dpc loads a
 * count. The vCPU resumes at this instruction and the loop looks for
 * work again.
 */
void HELPER(nt_idle)(CPUAlphaState *env, uint32_t dpc, uint32_t next,
                     uint32_t list)
{
    AlphaCPU *cpu = env_archcpu(env);
    CPUState *cs = env_cpu(env);
    uint64_t dpc_addr = nt_idle_addr(env, dpc);
    uint32_t val;
    int64_t now;

    /* Any IRQ line wakes a halted vCPU, even a masked one */
    if (!alpha_get_eien(env) || alpha_get_eir(env) ||
        (alpha_get_sir(env) & alpha_get_sien(env)) ||
        alpha_pending_ast(env)) {
        return;
    }
    if (!nt_idle_read(env, dpc_addr, &val) ||
        val != (list ? (uint32_t)dpc_addr : 0)) {
        return;
    }
    if (!nt_idle_read(env, nt_idle_addr(env, next), &val) || val) {
        return;
    }

    /* With -icount, the translator made this the last insn of its TB */
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now - cpu->nt_idle_last > NT_IDLE_GAP_NS) {
        cpu->nt_idle_since = now;
    }
    cpu->nt_idle_last = now;
    if (now - cpu->nt_idle_since < NT_IDLE_POLL_NS) {
        return;
    }

    cs->halted = 1;
    cs->exception_index = EXCP_HLT;
    cpu_restore_state(cs, GETPC());
    trace_alpha_nt_idle(cs->cpu_index, env->pc);
    cpu_loop_exit(cs);
}
