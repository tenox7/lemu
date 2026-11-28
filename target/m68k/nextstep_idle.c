/*
 * NeXTSTEP on the NeXT machines: halt the CPU in Mach's idle loop
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The 68030 and 68040 have STOP, but Mach on the NeXT never uses it. With
 * nothing to run, idle_thread() spins at interrupt level 0 until a run
 * queue has a thread, so an idle guest keeps a host core busy. NeXTSTEP
 * 2.2 tests three counts:
 *
 *   top:   tst.l (a4)          the processor's run queue
 *          bne.s out
 *          tst.l (a5)          the default processor set's
 *          bne.s out
 *          tst.l (a3)          the global one
 *          beq.s top           <- the translator's hook
 *
 * NeXTSTEP 3.x and OPENSTEP 4.x first look at the CPU's pending ASTs, a
 * word of flags at need_ast[cpu], and load the addresses of the counts
 * into an address register from data registers:
 *
 *   top:   lea need_ast,a5     (4.x: movea.l d3,a2)
 *          move.l (0,a5,d3.l*4),d0
 *          moveq #-8,d7
 *          and.l d7,d0
 *          beq.s counts
 *          ...                 clear those flags at spl3
 *   counts: move.l (a4),d0
 *          bne.s out
 *          movea.l d6,a5
 *          move.l (a5),d0
 *          bne.s out
 *          movea.l d5,a5
 *          move.l (a5),d0
 *          beq.s top           <- the translator's hook
 *
 * The compilers chose other registers in each release, so the translator
 * reads the loop back from the branch and accepts only these forms
 * (gen_nextstep_idle() in translate.c). What it found is a list of up to
 * four words that the loop needs to be 0, each given as a register that
 * holds its address, or a constant, an index register scaled by 4 and a
 * mask. The helper reads them again: when all are 0, interrupts are open
 * and none is pending, it halts the CPU until the next interrupt, and the
 * CPU goes on at the top of the loop then. On a machine with one CPU only
 * an interrupt handler can put a thread on a run queue.
 *
 * Waking a host thread takes longer than a gap in the work often lasts, as
 * when a disk request completes at once. So the loop spins on for 100 us
 * before the first halt, as KVM's halt polling does.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"
#include "cpu.h"
#include "accel/tcg/cpu-loop.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "accel/tcg/probe.h"
#include "exec/cpu-common.h"
#include "exec/helper-proto.h"
#include "exec/tlb-flags.h"
#include "trace.h"

#define NEXTSTEP_IDLE_POLL_NS   (100 * SCALE_US)
#define NEXTSTEP_IDLE_GAP_NS    (20 * SCALE_US)

static bool nextstep_idle_polled(CPUM68KState *env)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (now - env->nextstep_idle_last > NEXTSTEP_IDLE_GAP_NS) {
        env->nextstep_idle_since = now;
    }
    env->nextstep_idle_last = now;
    return now - env->nextstep_idle_since >= NEXTSTEP_IDLE_POLL_NS;
}

/* Read a kernel long word; false if it is not mapped RAM */
static bool nextstep_idle_read(CPUM68KState *env, uint32_t addr,
                               uint32_t *val)
{
    void *host;
    int flags;

    if (addr & 1) {
        return false;
    }
    flags = probe_access_flags(env, addr, 4, MMU_DATA_LOAD,
                               cpu_mmu_index(env_cpu(env), false), true,
                               &host, 0);
    if ((flags & TLB_INVALID_MASK) || !host) {
        return false;
    }
    *val = ldl_be_p(host);
    return true;
}

/* The register a check names: 0-7 are D0-D7, 8-15 are A0-A7 */
static uint32_t nextstep_idle_reg(CPUM68KState *env, unsigned reg)
{
    return reg < 8 ? env->dregs[reg] : env->aregs[reg - 8];
}

/*
 * Mach is at the branch from the end of idle_thread()'s loop back to its
 * top. Each byte of checks, from the lowest, is one word the loop tests:
 * bit 7 set for a used one, bits 0-3 the register with its address, or 15
 * for the constant addr; bits 4-6 an index register, D0 to D7, scaled by
 * 4, when the mask has bits outside it.
 */
void HELPER(nextstep_idle)(CPUM68KState *env, uint32_t top, uint32_t checks,
                           uint32_t addr, uint32_t mask)
{
    CPUState *cs = env_cpu(env);
    int i;

    /* Interrupts are open and none is pending */
    if ((env->sr & SR_I) || cpu_test_interrupt(cs, CPU_INTERRUPT_HARD)) {
        return;
    }
    for (i = 0; i < 4; i++) {
        unsigned c = extract32(checks, i * 8, 8);
        unsigned reg = extract32(c, 0, 4);
        uint32_t a, val, m = 0xffffffff;

        if (!(c & 0x80)) {
            continue;
        }
        a = reg == 15 ? addr : nextstep_idle_reg(env, reg);
        if (i == 0 && mask != 0xffffffff) {
            a += env->dregs[extract32(c, 4, 3)] * 4;
            m = mask;
        }
        if (!nextstep_idle_read(env, a, &val) || (val & m)) {
            return;
        }
    }
    if (!nextstep_idle_polled(env)) {
        return;
    }

    trace_m68k_nextstep_idle(top);
    env->pc = top;
    cs->halted = 1;
    cs->exception_index = EXCP_HLT;
    cpu_loop_exit(cs);
}
