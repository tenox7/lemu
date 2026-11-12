/*
 * SunOS 4.1.4 on sun4m: halt the CPU in the idle loop
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * SunOS 4 never halts the CPU. With nothing to run it spins in idlework(),
 * at interrupt level 0, so an idle guest keeps a host core busy. One round
 * of that loop is:
 *
 *   top:   klock_exit(); spl0();
 *          cpu_idle = 1; cpu_supv = 0;       what the clock tick counts
 *          if (!cpu_enable) goto top;
 *          if (spin > 0) { spin--; goto top; }
 *          if (Nrunnable || whichqs)         a thread or a process to run
 *              if (klock_knock()) { idle(); spin = 1000; }
 *          if (!qrunflag) goto top;          <- the translator's hook
 *          ...                               run the STREAMS queues
 *
 * The translator finds the end of the loop by its instructions and takes
 * the addresses of the three variables from them (gen_sunos_idle() in
 * translate.c). At the branch back to the top it calls helper_sunos_idle(),
 * which halts the CPU when all three are 0, interrupts are enabled and
 * none is pending. Only an interrupt handler can change that on a machine
 * with one CPU, and the board turns this on only for one CPU. The helper
 * reads the variables again, as an interrupt may have come since the loop
 * loaded them.
 *
 * The CPU goes on at the top of the loop after the interrupt, so the loop
 * sets cpu_idle and cpu_supv again before the next clock tick looks at
 * them, and the guest counts its idle time as before.
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

/*
 * Spin on for this long after the loop first found no work. A round that
 * comes more than SUNOS_IDLE_GAP_NS after the one before means that the
 * guest left the loop between them.
 */
#define SUNOS_IDLE_POLL_NS  (100 * SCALE_US)
#define SUNOS_IDLE_GAP_NS   (20 * SCALE_US)

static bool sunos_idle_polled(CPUSPARCState *env)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (now - env->sunos_idle_last > SUNOS_IDLE_GAP_NS) {
        env->sunos_idle_since = now;
    }
    env->sunos_idle_last = now;
    return now - env->sunos_idle_since >= SUNOS_IDLE_POLL_NS;
}

/* Read a kernel variable of 1 or 4 bytes; false if it is not mapped RAM */
static bool sunos_idle_read(CPUSPARCState *env, target_ulong addr, int size,
                            uint32_t *val)
{
    void *host;
    int flags;

    if (addr & (size - 1)) {
        return false;
    }
    flags = probe_access_flags(env, addr, size, MMU_DATA_LOAD,
                               cpu_mmu_index(env_cpu(env), false), true,
                               &host, 0);
    if ((flags & TLB_INVALID_MASK) || !host) {
        return false;
    }
    *val = size == 1 ? ldub_p(host) : ldl_be_p(host);
    return true;
}

/*
 * SunOS is at the branch from the end of idlework() back to its top.
 * Halt until the next interrupt when the loop has nothing to do, and go
 * on at the top then.
 */
void HELPER(sunos_idle)(CPUSPARCState *env, target_ulong top,
                        target_ulong nrunnable, target_ulong whichqs,
                        target_ulong qrunflag)
{
    CPUState *cs = env_cpu(env);
    uint32_t val;

    /* Interrupts are enabled and none is pending */
    if (!env->psret || env->psrpil ||
        cpu_test_interrupt(cs, CPU_INTERRUPT_HARD)) {
        return;
    }
    if (!sunos_idle_read(env, nrunnable, 4, &val) || val ||
        !sunos_idle_read(env, whichqs, 4, &val) || val ||
        !sunos_idle_read(env, qrunflag, 1, &val) || val) {
        return;
    }
    if (!sunos_idle_polled(env)) {
        return;
    }

    trace_sparc_sunos_idle(top);
    env->pc = top;
    env->npc = top + 4;
    cs->halted = 1;
    cs->exception_index = EXCP_HLT;
    cpu_loop_exit(cs);
}
