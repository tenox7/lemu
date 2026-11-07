/*
 * MIPS: halt the vCPU in the Windows NT idle loop
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * NT 4.0 for MIPS spins when it is idle: KiIdleLoop enables interrupts for
 * a few nops, checks its DPC list and its next thread, and calls
 * HalProcessorIdle, which returns without waiting. The translator finds
 * the loop (gen_nt_idle()) and calls helper_nt_idle() after the mtc0 that
 * enables interrupts, which halts the vCPU as WAIT would when there is no
 * work. In NT 3.51 HalProcessorIdle enables interrupts, so the translator
 * (gen_nt351_idle()) calls it at the top of the loop, before the mtc0 that
 * disables them again. NT 3.1 keeps interrupts on in its idle loop and
 * does not call the HAL there: the translator (gen_nt31_idle()) ends a TB
 * after the load at the top of the loop and calls helper_nt31_idle().
 *
 * With more CPUs, NT gives an idle CPU a thread by writing its next thread,
 * and may queue a DPC for it, without an interrupt. So the other vCPUs
 * watch these two words of an idle vCPU and wake it after a write.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "cpu.h"
#include "internal.h"
#include "exec/breakpoint.h"
#include "exec/helper-proto.h"
#include "exec/icount.h"
#include "exec/watchpoint.h"
#include "trace.h"

/*
 * Spin on while an interrupt is likely to come soon, as waking a halted
 * vCPU takes longer: for NT_IDLE_POLL_NS after the idle loop was entered,
 * where an I/O often completes (KVM's halt polling does the same), and with
 * -icount while a timer is due within NT_IDLE_SOON_NS. A call more than
 * NT_IDLE_GAP_NS after the previous one means the loop was left between.
 */
#define NT_IDLE_POLL_NS     (100 * SCALE_US)
#define NT_IDLE_SOON_NS     SCALE_MS
#define NT_IDLE_GAP_NS      (20 * SCALE_US)

/* Read a word of NT kernel data in KSEG0 straight from guest memory */
static bool nt_idle_read(CPUMIPSState *env, target_ulong va, uint32_t *val)
{
    AddressSpace *as = env_cpu(env)->as;
    hwaddr pa = (uint32_t)va & 0x1fffffff;
    MemTxResult res;

    if ((target_long)va != (int32_t)va || ((uint32_t)va >> 29) != 4 ||
        (va & 3)) {
        return false;
    }
    if (mips_env_is_bigendian(env)) {
        *val = address_space_ldl_be(as, pa, MEMTXATTRS_UNSPECIFIED, &res);
    } else {
        *val = address_space_ldl_le(as, pa, MEMTXATTRS_UNSPECIFIED, &res);
    }
    return res == MEMTX_OK;
}

/*
 * On vCPU cs, with the BQL: watch the words of every vCPU that asked for
 * it, and nothing else. The CPU reset drops these watchpoints.
 */
static void nt_idle_watch(CPUState *cs, run_on_cpu_data data)
{
    CPUState *other;

    cpu_watchpoint_remove_all(cs, BP_CPU);
    CPU_FOREACH(other) {
        MIPSCPU *cpu = MIPS_CPU(other);

        if (other != cs && cpu->nt_idle_next) {
            cpu_watchpoint_insert(cs, cpu->nt_idle_next, 4,
                                  BP_CPU | BP_MEM_WRITE, NULL);
            cpu_watchpoint_insert(cs, cpu->nt_idle_dpc, 4,
                                  BP_CPU | BP_MEM_WRITE, NULL);
            qatomic_or(&cpu->nt_idle_watched, 1u << cs->cpu_index);
            trace_mips_nt_idle_watch(cs->cpu_index, other->cpu_index,
                                     cpu->nt_idle_next, cpu->nt_idle_dpc);
        }
    }
}

/* On the vCPU that wrote, once the write is done: wake the idle vCPU */
static void nt_idle_kick(CPUState *cs, run_on_cpu_data data)
{
    MIPSCPU *cpu = data.host_ptr;

    trace_mips_nt_idle_kick(cs->cpu_index, CPU(cpu)->cpu_index);
    qatomic_store_release(&cpu->nt_idle_wake, true);
    qemu_cpu_kick(CPU(cpu));
}

/*
 * A store to a watched word is about to happen. Wake the vCPU it belongs
 * to after the store: then the vCPU sees the new value, or it has not
 * halted yet and finds the wake-up when it checks for work again. The
 * store goes on, as MIPS has no BP_CPU watchpoints of its own.
 */
bool mips_cpu_debug_check_watchpoint(CPUState *cs, CPUWatchpoint *wp)
{
    CPUState *other;

    CPU_FOREACH(other) {
        MIPSCPU *cpu = MIPS_CPU(other);

        if (other != cs && (wp->vaddr == qatomic_read(&cpu->nt_idle_next) ||
                            wp->vaddr == qatomic_read(&cpu->nt_idle_dpc))) {
            async_run_on_cpu(cs, nt_idle_kick, RUN_ON_CPU_HOST_PTR(cpu));
        }
    }
    return false;
}

/*
 * Have all other vCPUs watch this vCPU's next thread and DPC list, before
 * it reads them. Returns true once they all do.
 */
static bool nt_idle_watched(CPUState *cs, vaddr next, vaddr dpc)
{
    MIPSCPU *cpu = MIPS_CPU(cs);
    uint32_t others = 0;
    CPUState *other;

    CPU_FOREACH(other) {
        if (other != cs) {
            others |= 1u << other->cpu_index;
        }
    }
    if (!others) {
        return true;
    }
    if (cpu->nt_idle_next != next || cpu->nt_idle_dpc != dpc) {
        BQL_LOCK_GUARD();

        qatomic_set(&cpu->nt_idle_next, next);
        qatomic_set(&cpu->nt_idle_dpc, dpc);
        qatomic_set(&cpu->nt_idle_watched, 0);
        CPU_FOREACH(other) {
            if (other != cs) {
                async_run_on_cpu(other, nt_idle_watch, RUN_ON_CPU_NULL);
            }
        }
        return false;
    }
    return qatomic_load_acquire(&cpu->nt_idle_watched) == others;
}

/*
 * Interrupts are on in the idle loop. If there is no work, halt
 * until the next interrupt or watched write: no interrupt pending, the DPC
 * list head at s3 points to itself and the processor block at prcb has no
 * next thread at offset next. The vCPU resumes at resume and the loop
 * looks for work again.
 */
static void nt_idle_halt(CPUMIPSState *env, target_ulong resume,
                         target_ulong prcb, int32_t next)
{
    MIPSCPU *cpu = env_archcpu(env);
    CPUState *cs = env_cpu(env);
    target_ulong dpc = env->active_tc.gpr[19];
    int64_t now, deadline;
    uint32_t val;

    /* A watched write from here on wakes this vCPU again */
    qatomic_xchg(&cpu->nt_idle_wake, false);

    if (!cpu->nt_idle || !cpu_mips_hw_interrupts_enabled(env) ||
        cpu_mips_hw_interrupts_pending(env)) {
        return;
    }
    if (!nt_idle_watched(cs, prcb + next, dpc)) {
        return;
    }
    if (!nt_idle_read(env, dpc, &val) || val != (uint32_t)dpc) {
        return;
    }
    if (!nt_idle_read(env, prcb + next, &val) || val) {
        return;
    }

    /* The mtc0 ends its TB, so it may read the clock with -icount */
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now - cpu->nt_idle_last > NT_IDLE_GAP_NS) {
        cpu->nt_idle_since = now;
    }
    cpu->nt_idle_last = now;
    if (now - cpu->nt_idle_since < NT_IDLE_POLL_NS) {
        return;
    }
    if (icount_enabled()) {
        deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                              QEMU_TIMER_ATTR_ALL);
        if (deadline >= 0 && deadline < NT_IDLE_SOON_NS) {
            return;
        }
    }

    trace_mips_nt_idle(cs->cpu_index, resume);
    env->active_tc.PC = resume;
    cs->halted = 1;
    do_raise_exception(env, EXCP_HLT, 0);
}

void HELPER(nt_idle)(CPUMIPSState *env, target_ulong resume, int32_t next)
{
    nt_idle_halt(env, resume, env->active_tc.gpr[16], next);
}

/*
 * The top of the NT 3.51 idle loop, before its mtc0 s6,psr. The idle
 * thread can come here with all interrupts masked, as when NT shuts down,
 * and only that mtc0 unmasks them. So halt only when the mask is already
 * the one in s6, or no interrupt could wake the vCPU.
 */
void HELPER(nt351_idle)(CPUMIPSState *env, target_ulong resume, int32_t next)
{
    if ((env->CP0_Status ^ env->active_tc.gpr[22]) & (0xff << CP0St_IM)) {
        return;
    }
    nt_idle_halt(env, resume, env->active_tc.gpr[16], next);
}

/*
 * After the load of KdDebuggerEnabled into t0 at the top of the NT 3.1
 * idle loop. Its processor block is in s2. While the kernel debugger is on,
 * the loop polls it, so spin then. Also spin while all hardware interrupts
 * are masked, as nothing could wake the vCPU.
 */
void HELPER(nt31_idle)(CPUMIPSState *env, target_ulong resume, int32_t next)
{
    if (env->active_tc.gpr[8] || !(env->CP0_Status & (0xfc << CP0St_IM))) {
        return;
    }
    nt_idle_halt(env, resume, env->active_tc.gpr[18], next);
}
